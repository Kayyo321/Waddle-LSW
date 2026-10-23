const std = @import("std");
const layout = @import("interop_layout.zig");

/// Number of fixed 2 MiB allocation pages in the guest-owned heap.
pub const PageCount: usize = @intCast(layout.HeapBytes / layout.PageBytes);

/// Errors returned by the caller-owned heap allocator.
pub const heap_error_t = error{
    InvalidSessionEpoch,
    InvalidCapacity,
    HeapFull,
    TicketExhausted,
    InvalidRun,
    StaleRun,
    HeapStateCorrupt,
};

/// Exact identity and range of one allocation returned by `allocate`.
///
/// @note The caller owns this value. It contains no pointer and does not own
///       mapped memory. Preserve the complete value for the matching release.
pub const heap_run_t = struct {
    /// Nonzero session epoch supplied when the allocator was initialized.
    session_epoch: u64,
    /// Unique nonzero ticket within the allocator session.
    ticket: u64,
    /// First page in the guest-owned heap.
    start_page: u32,
    /// Number of contiguous pages in this allocation.
    page_count: u32,
    /// Mapping-relative byte offset of the first page.
    data_offset: u64,
    /// Exact allocation size in bytes.
    capacity_bytes: u64,
};

const allocation_record_t = struct {
    active: bool = false,
    ticket: u64 = 0,
    start_page: u32 = 0,
    page_count: u32 = 0,
};

/// Fixed-size bitmap allocator for the layout-v3 guest heap.
///
/// @note Initialize with `init`; the caller owns this value for its entire
///       lifetime. No mapping or allocation is performed. It is not internally
///       synchronized; all calls and token access require external
///       serialization. Reinitialization is permitted only after the caller
///       has ended the prior session and guarantees a fresh, unique epoch.
pub const heap_allocator_t = struct {
    /// Immutable nonzero session identity; caller guarantees uniqueness on reinit.
    session_epoch: u64,
    /// Next nonzero ticket, never wrapped or reused within this session.
    next_ticket: u64,
    /// Sticky after issuing the maximum u64 ticket; allocation then fails.
    tickets_exhausted: bool,
    /// One ownership bit per heap page, stored as fixed-size bool values.
    page_bitmap: [PageCount]bool,
    /// Fixed reservation ledger; only active entries own the recorded pages.
    allocations: [PageCount]allocation_record_t,

    /// Initialize an empty fixed-size allocator for one unique session epoch.
    ///
    /// @param[in] session_epoch Nonzero epoch unique across allocator sessions.
    /// @return Initialized caller-owned allocator, or InvalidSessionEpoch for 0.
    /// @note Allocation-free and thread-compatible only under external
    ///       serialization. The caller owns the returned value.
    pub fn init(session_epoch: u64) heap_error_t!heap_allocator_t {
        if (session_epoch == 0) return error.InvalidSessionEpoch;
        return .{
            .session_epoch = session_epoch,
            .next_ticket = 1,
            .tickets_exhausted = false,
            .page_bitmap = [_]bool{false} ** PageCount,
            .allocations = [_]allocation_record_t{.{}} ** PageCount,
        };
    }

    /// Reserve the first contiguous run whose capacity exactly matches `capacity_bytes`.
    ///
    /// @param[in,out] self Initialized allocator, externally serialized.
    /// @param[in] capacity_bytes Nonzero size that is an exact multiple of PageBytes.
    /// @param[out] run Non-NULL caller-owned token output; unchanged on error.
    /// @return void on success, otherwise InvalidCapacity, HeapFull, or
    ///         TicketExhausted. On every error the allocator and output are unchanged.
    /// @note Allocation-free. The token describes a reservation only; callers
    ///       manage mapping access and any external pins separately.
    pub fn allocate(
        self: *heap_allocator_t,
        capacity_bytes: u64,
        run: *heap_run_t,
    ) heap_error_t!void {
        const page_bytes: u64 = layout.PageBytes;
        if (capacity_bytes == 0 or capacity_bytes % page_bytes != 0 or capacity_bytes > layout.HeapBytes)
            return error.InvalidCapacity;
        if (self.tickets_exhausted) return error.TicketExhausted;

        const page_count: usize = @intCast(capacity_bytes / page_bytes);
        var record_index: ?usize = null;
        for (&self.allocations, 0..) |*record, index| {
            if (!record.active) {
                record_index = index;
                break;
            }
        }
        const free_record = record_index orelse return error.HeapFull;

        var first_page: ?usize = null;
        var candidate: usize = 0;
        while (candidate + page_count <= PageCount) : (candidate += 1) {
            var page = candidate;
            while (page < candidate + page_count and !self.page_bitmap[page]) : (page += 1) {}
            if (page == candidate + page_count) {
                first_page = candidate;
                break;
            }
            candidate = page;
        }
        const start_page = first_page orelse return error.HeapFull;

        const ticket = self.next_ticket;
        const token = heap_run_t{
            .session_epoch = self.session_epoch,
            .ticket = ticket,
            .start_page = @intCast(start_page),
            .page_count = @intCast(page_count),
            .data_offset = layout.HeapOffset + @as(u64, @intCast(start_page)) * page_bytes,
            .capacity_bytes = capacity_bytes,
        };

        // All possible failure checks precede these state changes.
        for (self.page_bitmap[start_page .. start_page + page_count]) |*used| used.* = true;
        self.allocations[free_record] = .{
            .active = true,
            .ticket = ticket,
            .start_page = @intCast(start_page),
            .page_count = @intCast(page_count),
        };
        if (ticket == std.math.maxInt(u64)) {
            self.tickets_exhausted = true;
        } else {
            self.next_ticket = ticket + 1;
        }
        run.* = token;
    }

    /// Release exactly the active reservation represented by `run`.
    ///
    /// @param[in,out] self Initialized allocator, externally serialized.
    /// @param[in] run Complete unmodified token previously returned by allocate.
    /// @return void on success, otherwise InvalidRun, StaleRun, or
    ///         HeapStateCorrupt. All errors leave allocator state unchanged.
    /// @note Allocation-free. A token from a different epoch or a prior
    ///       allocation cannot release a current reservation.
    pub fn release(self: *heap_allocator_t, run: heap_run_t) heap_error_t!void {
        if (run.session_epoch == 0 or run.ticket == 0 or run.page_count == 0)
            return error.InvalidRun;
        if (run.session_epoch != self.session_epoch) return error.StaleRun;

        var record_index: ?usize = null;
        for (&self.allocations, 0..) |*record, index| {
            if (record.active and record.ticket == run.ticket) {
                record_index = index;
                break;
            }
        }
        const index = record_index orelse return error.StaleRun;
        const record = self.allocations[index];
        const expected_offset = layout.HeapOffset + @as(u64, record.start_page) * layout.PageBytes;
        const expected_capacity = @as(u64, record.page_count) * layout.PageBytes;
        if (run.start_page != record.start_page or run.page_count != record.page_count or
            run.data_offset != expected_offset or run.capacity_bytes != expected_capacity)
            return error.InvalidRun;

        const start: usize = record.start_page;
        const count: usize = record.page_count;
        if (start >= PageCount or count > PageCount - start) return error.HeapStateCorrupt;
        for (self.page_bitmap[start .. start + count]) |used| {
            if (!used) return error.HeapStateCorrupt;
        }

        // Validation is complete; mutation below cannot fail.
        for (self.page_bitmap[start .. start + count]) |*used| used.* = false;
        self.allocations[index] = .{};
    }
};

// Test-only fixtures.
fn call_allocate_dynamic(
    allocator: *heap_allocator_t,
    capacity_bytes: u64,
    output: *heap_run_t,
) heap_error_t!void {
    return @call(.never_inline, heap_allocator_t.allocate, .{ allocator, capacity_bytes, output });
}

fn call_release_dynamic(allocator: *heap_allocator_t, run: heap_run_t) heap_error_t!void {
    return @call(.never_inline, heap_allocator_t.release, .{ allocator, run });
}

test "initialization and capacity errors preserve outputs" {
    try std.testing.expectError(error.InvalidSessionEpoch, heap_allocator_t.init(0));
    var allocator = try heap_allocator_t.init(17);
    var output = heap_run_t{
        .session_epoch = 91,
        .ticket = 92,
        .start_page = 93,
        .page_count = 94,
        .data_offset = 95,
        .capacity_bytes = 96,
    };
    const before = allocator;
    const output_before = output;
    try std.testing.expectError(error.InvalidCapacity, allocator.allocate(0, &output));
    try std.testing.expectEqualDeep(before, allocator);
    try std.testing.expectEqualDeep(output_before, output);
    try std.testing.expectError(error.InvalidCapacity, allocator.allocate(layout.PageBytes + 1, &output));
    try std.testing.expectEqualDeep(before, allocator);
    try std.testing.expectEqualDeep(output_before, output);
    const over_heap = layout.HeapBytes + layout.PageBytes;
    try std.testing.expectError(error.InvalidCapacity, call_allocate_dynamic(&allocator, over_heap, &output));
    try std.testing.expectEqualDeep(before, allocator);
    try std.testing.expectEqualDeep(output_before, output);
}

test "first fit skips a partially occupied candidate" {
    var allocator = try heap_allocator_t.init(7);
    var page_zero: heap_run_t = undefined;
    var page_one: heap_run_t = undefined;
    try allocator.allocate(layout.PageBytes, &page_zero);
    try allocator.allocate(layout.PageBytes, &page_one);
    try allocator.release(page_zero);

    var two_pages: heap_run_t = undefined;
    try allocator.allocate(2 * layout.PageBytes, &two_pages);
    try std.testing.expectEqual(@as(u32, 2), two_pages.start_page);
    try allocator.release(two_pages);
    try allocator.release(page_one);
}

test "all single-page records can be reserved and ledger exhaustion is atomic" {
    var allocator = try heap_allocator_t.init(8);
    var runs: [PageCount]heap_run_t = undefined;
    for (&runs, 0..) |*run, index| {
        try allocator.allocate(layout.PageBytes, run);
        try std.testing.expectEqual(@as(u32, @intCast(index)), run.start_page);
    }

    var output = runs[0];
    output.ticket = 0xfeed;
    const state_before = allocator;
    const output_before = output;
    try std.testing.expectError(error.HeapFull, call_allocate_dynamic(&allocator, layout.PageBytes, &output));
    try std.testing.expectEqualDeep(state_before, allocator);
    try std.testing.expectEqualDeep(output_before, output);
    for (runs) |run| try allocator.release(run);
}

test "full heap allocates all pages then rejects another reservation unchanged" {
    var allocator = try heap_allocator_t.init(1);
    var full_run: heap_run_t = undefined;
    try allocator.allocate(layout.HeapBytes, &full_run);
    try std.testing.expectEqual(@as(u32, @intCast(PageCount)), full_run.page_count);
    try std.testing.expectEqual(layout.HeapOffset, full_run.data_offset);

    var output = full_run;
    output.ticket = 777;
    const before = allocator;
    const output_before = output;
    try std.testing.expectError(error.HeapFull, allocator.allocate(layout.PageBytes, &output));
    try std.testing.expectEqualDeep(before, allocator);
    try std.testing.expectEqualDeep(output_before, output);
    try allocator.release(full_run);
    var expected_empty = try heap_allocator_t.init(1);
    expected_empty.next_ticket = 2;
    try std.testing.expectEqualDeep(expected_empty, allocator);
}

test "first fit respects fragmentation and coalesces released neighbors" {
    var allocator = try heap_allocator_t.init(2);
    var runs: [3]heap_run_t = undefined;
    try allocator.allocate(510 * layout.PageBytes, &runs[0]);
    try allocator.allocate(2 * layout.PageBytes, &runs[1]);
    try allocator.allocate(510 * layout.PageBytes, &runs[2]);
    try std.testing.expectEqual(@as(u32, 0), runs[0].start_page);
    try std.testing.expectEqual(@as(u32, 510), runs[1].start_page);
    try std.testing.expectEqual(@as(u32, 512), runs[2].start_page);

    try allocator.release(runs[1]);
    var next: heap_run_t = undefined;
    try std.testing.expectError(error.HeapFull, allocator.allocate(3 * layout.PageBytes, &next));
    try allocator.release(runs[0]);
    try allocator.allocate(512 * layout.PageBytes, &next);
    try std.testing.expectEqual(@as(u32, 0), next.start_page);
    try allocator.release(next);
    try allocator.release(runs[2]);
}

test "release rejects altered, stale, interior, and duplicate tokens atomically" {
    var allocator = try heap_allocator_t.init(33);
    var run: heap_run_t = undefined;
    try allocator.allocate(2 * layout.PageBytes, &run);
    var forged = run;
    forged.data_offset += layout.PageBytes;
    const before = allocator;
    try std.testing.expectError(error.InvalidRun, allocator.release(forged));
    try std.testing.expectEqualDeep(before, allocator);
    forged = run;
    forged.capacity_bytes -= layout.PageBytes;
    try std.testing.expectError(error.InvalidRun, allocator.release(forged));
    try std.testing.expectEqualDeep(before, allocator);
    forged = run;
    forged.start_page += 1;
    try std.testing.expectError(error.InvalidRun, allocator.release(forged));
    try std.testing.expectEqualDeep(before, allocator);
    forged = run;
    forged.page_count = 0;
    try std.testing.expectError(error.InvalidRun, allocator.release(forged));
    try std.testing.expectEqualDeep(before, allocator);
    forged = run;
    forged.session_epoch += 1;
    try std.testing.expectError(error.StaleRun, allocator.release(forged));
    try std.testing.expectEqualDeep(before, allocator);
    forged = run;
    forged.ticket += 100;
    try std.testing.expectError(error.StaleRun, allocator.release(forged));
    try std.testing.expectEqualDeep(before, allocator);
    forged.ticket = 0;
    try std.testing.expectError(error.InvalidRun, allocator.release(forged));
    try std.testing.expectEqualDeep(before, allocator);
    forged = run;
    forged.session_epoch = 0;
    try std.testing.expectError(error.InvalidRun, call_release_dynamic(&allocator, forged));
    try std.testing.expectEqualDeep(before, allocator);

    try allocator.release(run);
    const empty = allocator;
    try std.testing.expectError(error.StaleRun, allocator.release(run));
    try std.testing.expectEqualDeep(empty, allocator);
    var reused: heap_run_t = undefined;
    try allocator.allocate(2 * layout.PageBytes, &reused);
    try std.testing.expectEqual(run.start_page, reused.start_page);
    try std.testing.expect(reused.ticket != run.ticket);
    try std.testing.expectError(error.StaleRun, allocator.release(run));
    try allocator.release(reused);
}

test "release detects corrupt stored range and missing bitmap pages atomically" {
    var allocator = try heap_allocator_t.init(34);
    var run: heap_run_t = undefined;
    try allocator.allocate(layout.PageBytes, &run);

    const original_record = allocator.allocations[0];
    allocator.allocations[0].start_page = @intCast(PageCount);
    var corrupt_range_token = run;
    corrupt_range_token.start_page = @intCast(PageCount);
    corrupt_range_token.data_offset = layout.HeapEndOffset;
    const range_before = allocator;
    try std.testing.expectError(error.HeapStateCorrupt, call_release_dynamic(&allocator, corrupt_range_token));
    try std.testing.expectEqualDeep(range_before, allocator);

    allocator.allocations[0] = original_record;
    allocator.page_bitmap[run.start_page] = false;
    const bitmap_before = allocator;
    try std.testing.expectError(error.HeapStateCorrupt, allocator.release(run));
    try std.testing.expectEqualDeep(bitmap_before, allocator);
}

test "ticket exhaustion does not wrap or partially allocate" {
    var allocator = try heap_allocator_t.init(44);
    allocator.next_ticket = std.math.maxInt(u64);
    var last: heap_run_t = undefined;
    try allocator.allocate(layout.PageBytes, &last);
    try std.testing.expectEqual(std.math.maxInt(u64), last.ticket);
    try std.testing.expect(allocator.tickets_exhausted);
    const before = allocator;
    var output = last;
    output.ticket = 123;
    const output_before = output;
    try std.testing.expectError(error.TicketExhausted, allocator.allocate(layout.PageBytes, &output));
    try std.testing.expectEqualDeep(before, allocator);
    try std.testing.expectEqualDeep(output_before, output);
    try allocator.release(last);
    try std.testing.expectError(error.TicketExhausted, allocator.allocate(layout.PageBytes, &output));
}

test "deterministic randomized operations match independent page oracle" {
    const token_slot_t = struct {
        active: bool = false,
        token: heap_run_t = undefined,
    };
    var allocator = try heap_allocator_t.init(55);
    var oracle = [_]bool{false} ** PageCount;
    var tokens = [_]token_slot_t{.{}} ** PageCount;
    var prng = std.Random.DefaultPrng.init(0x5eed_cafe_1234_5678);
    const random = prng.random();

    for (0..4000) |_| {
        if (random.boolean()) {
            const count: usize = random.intRangeAtMost(usize, 1, 12);
            var expected_start: ?usize = null;
            var candidate: usize = 0;
            while (candidate + count <= PageCount) : (candidate += 1) {
                var page = candidate;
                while (page < candidate + count and !oracle[page]) : (page += 1) {}
                if (page == candidate + count) {
                    expected_start = candidate;
                    break;
                }
                candidate = page;
            }

            var free_token_slot: ?usize = null;
            for (&tokens, 0..) |*slot, index| {
                if (!slot.active) {
                    free_token_slot = index;
                    break;
                }
            }
            var output = heap_run_t{
                .session_epoch = 99,
                .ticket = 98,
                .start_page = 97,
                .page_count = 96,
                .data_offset = 95,
                .capacity_bytes = 94,
            };
            const output_before = output;
            const can_allocate = expected_start != null and free_token_slot != null;
            if (can_allocate) {
                try allocator.allocate(@as(u64, @intCast(count)) * layout.PageBytes, &output);
                const start = expected_start.?;
                try std.testing.expectEqual(@as(u32, @intCast(start)), output.start_page);
                try std.testing.expectEqual(@as(u32, @intCast(count)), output.page_count);
                for (oracle[start .. start + count]) |*used| used.* = true;
                const slot = &tokens[free_token_slot.?];
                slot.active = true;
                slot.token = output;
            } else {
                const state_before = allocator;
                try std.testing.expectError(error.HeapFull, allocator.allocate(@as(u64, @intCast(count)) * layout.PageBytes, &output));
                try std.testing.expectEqualDeep(state_before, allocator);
                try std.testing.expectEqualDeep(output_before, output);
            }
        } else {
            var active_count: usize = 0;
            for (tokens) |slot| if (slot.active) {
                active_count += 1;
            };
            if (active_count != 0) {
                var selected = random.intRangeAtMost(usize, 0, active_count - 1);
                for (&tokens) |*slot| {
                    if (!slot.active) continue;
                    if (selected != 0) {
                        selected -= 1;
                        continue;
                    }
                    const start: usize = slot.token.start_page;
                    const count: usize = slot.token.page_count;
                    try allocator.release(slot.token);
                    for (oracle[start .. start + count]) |*used| used.* = false;
                    slot.active = false;
                    break;
                }
            }
        }

        for (oracle, 0..) |expected, page| {
            try std.testing.expectEqual(expected, allocator.page_bitmap[page]);
        }
    }
}
