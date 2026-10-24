const std_t = @import("std");

/// Request identifier occupancy for one serialized connected-peer direction.
/// @ownership Caller owns this fixed-size value; no allocation or external resource is retained.
/// @thread_safety Calls on one instance must be serialized by its connection owner.
pub const request_ids_t = struct {
    /// Fixed occupied-ID storage; zero denotes an unused entry.
    slots: [RequestCapacity]u32 = [_]u32{0} ** RequestCapacity,
    /// Number of occupied entries, always at most RequestCapacity.
    count: usize = 0,
    /// Next nonzero candidate used by reserve; production initialization starts at 1.
    next_candidate: u32 = 1,

    /// Initialize a fresh allocator for one connected-peer direction.
    /// @return Empty state with the first candidate set to 1.
    /// @ownership Returned value is caller-owned and allocation-free.
    /// @thread_safety Serialize calls on the returned instance.
    pub fn init() request_ids_t {
        return .{};
    }

    /// Reserve the next available nonzero identifier, preserving sequence order.
    /// @param[in,out] self Allocator whose occupied set and cursor may change on success.
    /// @return A unique occupied nonzero request identifier.
    /// @errors Full when all RequestCapacity entries are occupied; the complete
    /// state is unchanged on failure.
    /// @ownership Returned ID is a copied scalar; the caller must eventually
    /// release it or retire the whole allocator after all users have stopped.
    /// @thread_safety Caller serializes reservation, release and retirement.
    pub fn reserve(self: *request_ids_t) request_ids_error_t!u32 {
        if (self.count == RequestCapacity) return error.Full;

        var candidate = self.next_candidate;
        var attempts: usize = 0;
        while (attempts <= self.count) : (attempts += 1) {
            if (!self.contains(candidate)) {
                const free_index = self.find_free_slot() orelse return error.Full;
                self.slots[free_index] = candidate;
                self.count += 1;
                self.next_candidate = following_id(candidate);
                return candidate;
            }
            candidate = following_id(candidate);
        }
        return error.Full;
    }

    /// Release exactly one currently occupied request identifier.
    /// @param[in,out] self Allocator whose matching slot is cleared on success.
    /// @param[in] request_id Nonzero identifier previously returned by reserve.
    /// @return No value on successful release.
    /// @errors InvalidRequestId for zero; UnknownRequestId for an absent ID.
    /// Both errors preserve the complete allocator state.
    /// @ownership No memory is transferred or freed.
    /// @thread_safety Caller serializes release with all other operations.
    pub fn release(self: *request_ids_t, request_id: u32) request_ids_error_t!void {
        if (request_id == 0) return error.InvalidRequestId;
        for (&self.slots) |*slot| {
            if (slot.* == request_id) {
                slot.* = 0;
                self.count -= 1;
                return;
            }
        }
        return error.UnknownRequestId;
    }

    /// Check whether a nonzero request identifier is currently occupied.
    /// @param[in] self Allocator whose occupied set is inspected.
    /// @param[in] request_id Identifier to query; zero always returns false.
    /// @return True only if request_id is present in an occupied slot.
    /// @ownership No memory is transferred and state is unchanged.
    /// @thread_safety Caller serializes reads with writes to this instance.
    pub fn contains(self: *const request_ids_t, request_id: u32) bool {
        if (request_id == 0) return false;
        for (self.slots) |slot| {
            if (slot == request_id) return true;
        }
        return false;
    }

    /// Retire all request IDs after every request user for the connection has stopped.
    /// @param[in,out] self Allocator whose occupied slots and count are cleared.
    /// @return No value; next_candidate is preserved to prevent sequence reset.
    /// @ownership Does not release feature resources, cancel work, or prove mapping drain.
    /// @thread_safety Caller serializes this operation and must first stop all users.
    pub fn retire_all(self: *request_ids_t) void {
        @memset(&self.slots, 0);
        self.count = 0;
    }

    fn find_free_slot(self: *const request_ids_t) ?usize {
        for (self.slots, 0..) |slot, index| {
            if (slot == 0) return index;
        }
        return null;
    }
};

/// Fixed local admission capacity for one request-ID direction.
/// @ownership Compile-time value; no runtime storage.
pub const RequestCapacity: usize = 4096;

/// Errors returned by request identifier occupancy operations.
/// @ownership Compile-time error set; no runtime storage.
pub const request_ids_error_t = error{
    /// Every fixed slot is occupied, so no identifier can be reserved.
    Full,
    /// Release was called with the reserved sentinel identifier zero.
    InvalidRequestId,
    /// Release was called for an identifier that is not occupied.
    UnknownRequestId,
};

fn following_id(request_id: u32) u32 {
    return if (request_id == std_t.math.maxInt(u32)) 1 else request_id + 1;
}

// Test-only fixtures.
test "reserve starts at one and release creates a hole without resetting order" {
    var ids = request_ids_t.init();
    try std_t.testing.expectEqual(@as(u32, 1), try ids.reserve());
    try std_t.testing.expectEqual(@as(u32, 2), try ids.reserve());
    try std_t.testing.expect(ids.contains(1));
    try std_t.testing.expect(ids.contains(2));
    try ids.release(1);
    try std_t.testing.expect(!ids.contains(1));
    try std_t.testing.expectEqual(@as(u32, 3), try ids.reserve());
    try std_t.testing.expectEqual(@as(usize, 2), ids.count);
}

test "reservation skips occupied identifiers across uint32 wrap" {
    var ids = request_ids_t.init();
    try std_t.testing.expectEqual(@as(u32, 1), try ids.reserve());
    ids.next_candidate = std_t.math.maxInt(u32);

    try std_t.testing.expectEqual(std_t.math.maxInt(u32), try ids.reserve());
    try std_t.testing.expectEqual(@as(u32, 1), ids.next_candidate);
    try std_t.testing.expectEqual(@as(u32, 2), try ids.reserve());
    try std_t.testing.expect(ids.contains(1));
    try std_t.testing.expect(ids.contains(std_t.math.maxInt(u32)));
    try std_t.testing.expect(ids.contains(2));
}

test "full capacity and invalid releases preserve complete state" {
    var ids = request_ids_t.init();
    for (0..RequestCapacity) |index| {
        try std_t.testing.expectEqual(@as(u32, @intCast(index + 1)), try ids.reserve());
    }
    try std_t.testing.expectEqual(RequestCapacity, ids.count);
    const full_state = ids;
    try std_t.testing.expectError(error.Full, ids.reserve());
    try std_t.testing.expectEqualDeep(full_state, ids);

    try std_t.testing.expectError(error.InvalidRequestId, ids.release(0));
    try std_t.testing.expectEqualDeep(full_state, ids);
    try std_t.testing.expectError(error.UnknownRequestId, ids.release(@as(u32, 1) + @as(u32, @intCast(RequestCapacity))));
    try std_t.testing.expectEqualDeep(full_state, ids);

    try ids.release(2048);
    try std_t.testing.expectEqual(RequestCapacity - 1, ids.count);
    try std_t.testing.expectEqual(@as(u32, RequestCapacity + 1), try ids.reserve());
    try std_t.testing.expectEqual(RequestCapacity, ids.count);
    try std_t.testing.expect(ids.contains(2048) == false);
}

test "unknown and repeated release are unchanged" {
    var ids = request_ids_t.init();
    try std_t.testing.expectError(error.UnknownRequestId, ids.release(1));
    try std_t.testing.expectEqualDeep(request_ids_t.init(), ids);
    const request_id = try ids.reserve();
    try ids.release(request_id);
    const released = ids;
    try std_t.testing.expectError(error.UnknownRequestId, ids.release(request_id));
    try std_t.testing.expectEqualDeep(released, ids);
}

test "runtime sentinel input is never an occupied or releasable request" {
    var ids = request_ids_t.init();
    _ = try ids.reserve();
    const before = ids;
    // Exercise the public runtime boundary, keeping the zero guard in the
    // callee rather than folding a constant caller's error at compile time.
    try std_t.testing.expect(!@call(.never_inline, request_ids_t.contains, .{ &ids, 0 }));
    try std_t.testing.expectError(error.InvalidRequestId, @call(.never_inline, request_ids_t.release, .{ &ids, 0 }));
    try std_t.testing.expectEqualDeep(before, ids);
}

test "retire clears occupancy and preserves sequence cursor" {
    var ids = request_ids_t.init();
    _ = try ids.reserve();
    _ = try ids.reserve();
    _ = try ids.reserve();
    const cursor = ids.next_candidate;
    ids.retire_all();
    try std_t.testing.expectEqual(@as(usize, 0), ids.count);
    try std_t.testing.expect(!ids.contains(1));
    try std_t.testing.expect(!ids.contains(2));
    try std_t.testing.expect(!ids.contains(3));
    try std_t.testing.expectEqual(cursor, ids.next_candidate);
    try std_t.testing.expectEqual(@as(u32, 4), try ids.reserve());
}

test "bounded deterministic reserve release stress preserves uniqueness" {
    var ids = request_ids_t.init();
    var live_ids = [_]u32{0} ** StressPoolCapacity;
    live_ids[0] = try ids.reserve();
    try std_t.testing.expectEqual(@as(u32, 1), live_ids[0]);
    ids.next_candidate = std_t.math.maxInt(u32) - 5_000;
    for (1..StressPoolCapacity) |index| {
        live_ids[index] = try ids.reserve();
    }
    try std_t.testing.expect(ids.contains(1));
    try std_t.testing.expect(ids.contains(std_t.math.maxInt(u32) - 4_938));
    try std_t.testing.expectEqual(StressPoolCapacity, ids.count);

    for (0..10_000) |iteration| {
        const index = (iteration * 17 + 7) % (StressPoolCapacity - 1) + 1;
        const released_id = live_ids[index];
        try ids.release(released_id);
        try std_t.testing.expect(!ids.contains(released_id));

        const request_id = try ids.reserve();
        live_ids[index] = request_id;
        try std_t.testing.expect(request_id != 0);
        try std_t.testing.expect(ids.contains(request_id));
        try std_t.testing.expectEqual(StressPoolCapacity, ids.count);
        try std_t.testing.expectEqual(ids.count, occupied_count(&ids));

        for (live_ids, 0..) |live_id, live_index| {
            try std_t.testing.expect(ids.contains(live_id));
            for (live_ids[live_index + 1 ..]) |other_id| {
                try std_t.testing.expect(live_id != other_id);
            }
        }
        try std_t.testing.expect(ids.contains(1));
    }
    try std_t.testing.expect(ids.next_candidate < 10_000);
}

/// Fixed live-ID count used only by the deterministic stress fixture.
const StressPoolCapacity: usize = 64;

fn occupied_count(ids: *const request_ids_t) usize {
    var count: usize = 0;
    for (ids.slots) |slot| {
        if (slot != 0) count += 1;
    }
    return count;
}
