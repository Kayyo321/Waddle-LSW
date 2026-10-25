//! Activation token validation, environment selection and a bounded
//! (token, process_id) store with 30 s expiry and fail-closed clock rollback.
//! Caller-owned storage, no allocation, no I/O.
const std = @import("std");

/// Maximum token length in bytes; the wire field limit is 255.
pub const MaxTokenBytes: usize = 255;
/// Maximum stored (token, process_id) pairs.
pub const MaxEntries: usize = 32;
/// Entry lifetime in milliseconds: live while now < inserted + TtlMs.
pub const TtlMs: u64 = 30_000;
/// Byte size of one token slot, including unused tail bytes.
const TokenSlot: usize = 256;

/// Token or process rejection. Every error leaves the store unchanged.
pub const error_t = error{ InvalidToken, InvalidProcess };

/// One stored pair. Extern layout (280 bytes) shared with activation_store.h.
pub const entry_t = extern struct {
    /// Token bytes; only the first token_length bytes are meaningful.
    token: [TokenSlot]u8,
    /// Token length, 1..255 while occupied; 0 when free.
    token_length: u8,
    /// 1 while the slot holds a pair (possibly expired but not yet removed), else 0.
    occupied: u8,
    /// Zero padding that aligns process_id to four bytes.
    reserved: [2]u8,
    /// Nonzero process identifier while occupied.
    process_id: u32,
    /// Expiry instant: inserted + TtlMs, saturating at the u64 maximum.
    deadline_ms: u64,
    /// Insertion or replacement order; a larger value is newer.
    sequence: u64,
};

/// Fixed store of at most MaxEntries pairs. Extern layout is 8992 bytes; an
/// all-zero value is a valid empty store. Not thread-safe; one owner thread.
pub const store_t = extern struct {
    /// Slot array; free slots have occupied 0.
    entries: [MaxEntries]entry_t,
    /// Sequence value given to the next insertion or replacement; starts at 0.
    next_sequence: u64,
    /// Most recent accepted clock value; meaningful when clock_started is 1.
    last_now: u64,
    /// Number of occupied slots.
    count: u32,
    /// 0 before the first timed operation, 1 afterwards.
    clock_started: u32,
    /// Number of detected clock rollbacks; each one cleared the store.
    rollbacks: u32,
    /// Zero padding to an eight-byte multiple.
    reserved: u32,

    /// Insert or replace a (token, process_id) pair.
    /// @param self Store, mutated on success only.
    /// @param token Borrowed token bytes; must satisfy token_valid.
    /// @param process_id Nonzero process identifier.
    /// @param now Monotonic milliseconds from the caller clock.
    /// @return error.InvalidToken or error.InvalidProcess without state change.
    pub fn put(self: *store_t, token: []const u8, process_id: u32, now: u64) error_t!void {
        if (!token_valid(token)) return error.InvalidToken;
        if (process_id == 0) return error.InvalidProcess;
        self.observe(now);
        _ = self.expire_stale(now);
        if (self.find_token(token)) |index| {
            self.fill(index, token, process_id, now);
            return;
        }
        if (self.count == MaxEntries) self.remove(self.oldest());
        self.fill(self.first_free(), token, process_id, now);
    }

    /// Remove and return the newest live token for a process identifier.
    /// @param self Store, mutated on success.
    /// @param process_id Nonzero process identifier.
    /// @param now Monotonic milliseconds from the caller clock.
    /// @param out Caller buffer receiving the token bytes (up to 255 bytes).
    /// @return Token length, or null when no live token matches.
    /// @note A returned token cannot be returned again.
    pub fn take_for_process(self: *store_t, process_id: u32, now: u64, out: *[MaxTokenBytes]u8) error{InvalidProcess}!?usize {
        if (process_id == 0) return error.InvalidProcess;
        self.observe(now);
        _ = self.expire_stale(now);
        const index = self.newest_for(process_id) orelse return null;
        const length: usize = self.entries[index].token_length;
        @memcpy(out[0..length], self.entries[index].token[0..length]);
        self.remove(index);
        return length;
    }

    /// Remove every expired pair and return how many were removed.
    /// @param self Store, mutated.
    /// @param now Monotonic milliseconds from the caller clock.
    /// @return Count of pairs removed by this call.
    pub fn expire(self: *store_t, now: u64) usize {
        self.observe(now);
        return self.expire_stale(now);
    }

    /// Remove every pair. Clock state and the rollback counter are preserved.
    /// @param self Store, mutated.
    pub fn clear(self: *store_t) void {
        for (&self.entries) |*entry| entry.* = std.mem.zeroes(entry_t);
        self.count = 0;
    }

    /// Record the clock. A value below the previous one is a rollback: every
    /// pair is dropped so that no earlier token can be returned.
    fn observe(self: *store_t, now: u64) void {
        if (self.clock_started != 0 and now < self.last_now) {
            self.clear();
            self.rollbacks += 1;
        }
        self.clock_started = 1;
        self.last_now = now;
    }

    fn expire_stale(self: *store_t, now: u64) usize {
        var removed: usize = 0;
        for (&self.entries, 0..) |*entry, index| {
            if (entry.occupied != 0 and now >= entry.deadline_ms) {
                self.remove(index);
                removed += 1;
            }
        }
        return removed;
    }

    fn remove(self: *store_t, index: usize) void {
        self.entries[index] = std.mem.zeroes(entry_t);
        self.count -= 1;
    }

    fn fill(self: *store_t, index: usize, token: []const u8, process_id: u32, now: u64) void {
        const entry = &self.entries[index];
        if (entry.occupied == 0) self.count += 1;
        @memcpy(entry.token[0..token.len], token);
        entry.token_length = @intCast(token.len);
        entry.occupied = 1;
        entry.process_id = process_id;
        entry.deadline_ms = now +| TtlMs;
        entry.sequence = self.next_sequence;
        self.next_sequence += 1;
    }

    fn find_token(self: *const store_t, token: []const u8) ?usize {
        for (&self.entries, 0..) |*entry, index| {
            if (entry.occupied != 0 and std.mem.eql(u8, entry.token[0..entry.token_length], token)) return index;
        }
        return null;
    }

    fn oldest(self: *const store_t) usize {
        var best: ?usize = null;
        for (&self.entries, 0..) |*entry, index| {
            if (entry.occupied == 0) continue;
            if (best == null or entry.sequence < self.entries[best.?].sequence) best = index;
        }
        return best orelse unreachable;
    }

    fn newest_for(self: *const store_t, process_id: u32) ?usize {
        var best: ?usize = null;
        for (&self.entries, 0..) |*entry, index| {
            if (entry.occupied == 0 or entry.process_id != process_id) continue;
            if (best == null or entry.sequence > self.entries[best.?].sequence) best = index;
        }
        return best;
    }

    fn first_free(self: *const store_t) usize {
        for (&self.entries, 0..) |*entry, index| {
            if (entry.occupied == 0) return index;
        }
        unreachable;
    }
};

/// Check the frozen token rule: 1..255 bytes, each in 0x21..0x7E.
/// @param token Borrowed candidate bytes.
/// @return true when the token may be stored or sent on the wire.
pub fn token_valid(token: []const u8) bool {
    if (token.len == 0 or token.len > MaxTokenBytes) return false;
    for (token) |byte| {
        if (byte < 0x21 or byte > 0x7e) return false;
    }
    return true;
}

/// Select the activation token: the first valid of XDG_ACTIVATION_TOKEN, then
/// DESKTOP_STARTUP_ID. Absent or invalid candidates are skipped.
/// @param xdg Borrowed XDG_ACTIVATION_TOKEN value, or null when unset.
/// @param desktop_startup_id Borrowed DESKTOP_STARTUP_ID value, or null when unset.
/// @return Borrowed token bytes from one candidate, or null when neither is valid.
pub fn pick_token(xdg: ?[]const u8, desktop_startup_id: ?[]const u8) ?[]const u8 {
    if (xdg) |candidate| {
        if (token_valid(candidate)) return candidate;
    }
    if (desktop_startup_id) |candidate| {
        if (token_valid(candidate)) return candidate;
    }
    return null;
}

/// C ABI: initialize caller storage to an empty store.
/// @param[out] store Nonnull; 8992 writable bytes, 8-byte aligned.
/// @return 0 on success, 1 when store is NULL.
/// @note Zeroed storage is already valid; this call is for clarity.
export fn interop_activation_store_init(store: ?*store_t) c_int {
    const target = store orelse return 1;
    target.* = std.mem.zeroes(store_t);
    return 0;
}

/// C ABI: insert or replace a pair.
/// @param[in,out] store Nonnull store.
/// @param[in] token Readable token bytes; NULL is allowed only when token_length is 0.
/// @param[in] token_length Byte count, 0..255 valid.
/// @param[in] process_id Nonzero process identifier.
/// @param[in] now_ms Monotonic milliseconds.
/// @return 0 Ok; 1 InvalidArgument (NULL store, or NULL token with length above 0);
///         2 InvalidToken; 3 InvalidProcess.
/// @note Not thread-safe; the caller serializes calls on one store.
export fn interop_activation_put(store: ?*store_t, token: ?[*]const u8, token_length: usize, process_id: u32, now_ms: u64) c_int {
    const target = store orelse return 1;
    const bytes: []const u8 = if (token_length == 0) &.{} else (token orelse return 1)[0..token_length];
    target.put(bytes, process_id, now_ms) catch |err| return switch (err) {
        error.InvalidToken => 2,
        error.InvalidProcess => 3,
    };
    return 0;
}

/// C ABI: remove and copy the newest live token for a process identifier.
/// @param[in,out] store Nonnull store.
/// @param[in] process_id Nonzero process identifier.
/// @param[in] now_ms Monotonic milliseconds.
/// @param[out] out Nonnull buffer of 255 bytes receiving the token.
/// @param[out] length_out Nonnull; receives the token length on success.
/// @return 0 Found; 1 InvalidArgument (NULL pointer); 2 NotFound; 3 InvalidProcess.
export fn interop_activation_take(store: ?*store_t, process_id: u32, now_ms: u64, out: ?*[MaxTokenBytes]u8, length_out: ?*usize) c_int {
    const target = store orelse return 1;
    const destination = out orelse return 1;
    const length_ref = length_out orelse return 1;
    const found = target.take_for_process(process_id, now_ms, destination) catch return 3;
    length_ref.* = found orelse return 2;
    return 0;
}

/// C ABI: remove every expired pair.
/// @param[in,out] store Store; NULL returns 0.
/// @param[in] now_ms Monotonic milliseconds.
/// @return Number of pairs removed.
export fn interop_activation_expire(store: ?*store_t, now_ms: u64) usize {
    const target = store orelse return 0;
    return target.expire(now_ms);
}

/// C ABI: remove every pair; NULL is a no-op.
/// @param[in,out] store Store.
export fn interop_activation_clear(store: ?*store_t) void {
    const target = store orelse return;
    target.clear();
}

/// C ABI: pick the activation token from two environment strings.
/// @param[in] xdg NUL-terminated XDG_ACTIVATION_TOKEN, or NULL. At most 256 bytes are examined.
/// @param[in] startup NUL-terminated DESKTOP_STARTUP_ID, or NULL. At most 256 bytes are examined.
/// @param[out] token_out Nonnull; receives a pointer into the chosen input on success.
/// @param[out] length_out Nonnull; receives the token length on success.
/// @return 0 Found; 1 no valid candidate (outputs unchanged); 2 InvalidArgument (NULL output).
export fn interop_activation_pick_token(xdg: ?[*:0]const u8, startup: ?[*:0]const u8, token_out: ?*[*]const u8, length_out: ?*usize) c_int {
    const token_ref = token_out orelse return 2;
    const length_ref = length_out orelse return 2;
    const picked = pick_token(bounded_candidate(xdg), bounded_candidate(startup)) orelse return 1;
    token_ref.* = picked.ptr;
    length_ref.* = picked.len;
    return 0;
}

/// Read a candidate up to its NUL, examining at most TokenSlot bytes; a longer
/// candidate yields a 256-byte slice, which token_valid rejects.
fn bounded_candidate(candidate: ?[*:0]const u8) ?[]const u8 {
    const pointer = candidate orelse return null;
    var length: usize = 0;
    while (length < TokenSlot and pointer[length] != 0) : (length += 1) {}
    return pointer[0..length];
}

// Test-only fixtures.
const testing = std.testing;

test "extern layouts match activation_store.h byte for byte" {
    try testing.expectEqual(@as(usize, 280), @sizeOf(entry_t));
    try testing.expectEqual(@as(usize, 8992), @sizeOf(store_t));
    try testing.expectEqual(@as(usize, 8), @alignOf(store_t));
    try testing.expectEqual(@as(usize, 256), @offsetOf(entry_t, "token_length"));
    try testing.expectEqual(@as(usize, 257), @offsetOf(entry_t, "occupied"));
    try testing.expectEqual(@as(usize, 260), @offsetOf(entry_t, "process_id"));
    try testing.expectEqual(@as(usize, 264), @offsetOf(entry_t, "deadline_ms"));
    try testing.expectEqual(@as(usize, 272), @offsetOf(entry_t, "sequence"));
    try testing.expectEqual(@as(usize, 8960), @offsetOf(store_t, "next_sequence"));
    try testing.expectEqual(@as(usize, 8968), @offsetOf(store_t, "last_now"));
    try testing.expectEqual(@as(usize, 8976), @offsetOf(store_t, "count"));
    try testing.expectEqual(@as(usize, 8980), @offsetOf(store_t, "clock_started"));
    try testing.expectEqual(@as(usize, 8984), @offsetOf(store_t, "rollbacks"));
}

test "token rule accepts exact printable bounds and rejects everything else" {
    try testing.expect(!token_valid(""));
    try testing.expect(token_valid("a"));
    try testing.expect(token_valid("!~"));
    try testing.expect(!token_valid(" a"));
    try testing.expect(!token_valid("a b"));
    try testing.expect(!token_valid("\x20"));
    try testing.expect(!token_valid("\x7f"));
    try testing.expect(!token_valid("\x00"));
    try testing.expect(!token_valid("\xc3\xa9"));
    const max = [_]u8{'t'} ** MaxTokenBytes;
    try testing.expect(token_valid(&max));
    const over = [_]u8{'t'} ** (MaxTokenBytes + 1);
    try testing.expect(!token_valid(&over));
}

test "pick_token prefers XDG then DESKTOP_STARTUP_ID and skips invalid candidates" {
    try testing.expectEqualSlices(u8, "xdg-1", pick_token("xdg-1", "startup-1").?);
    try testing.expectEqualSlices(u8, "startup-1", pick_token("bad token", "startup-1").?);
    try testing.expectEqualSlices(u8, "startup-1", pick_token(null, "startup-1").?);
    try testing.expectEqualSlices(u8, "startup-1", pick_token("", "startup-1").?);
    try testing.expect(pick_token("bad token", "") == null);
    try testing.expect(pick_token(null, null) == null);
    const over = [_]u8{'x'} ** (MaxTokenBytes + 1);
    try testing.expectEqualSlices(u8, "fallback", pick_token(&over, "fallback").?);
}

test "put and take return a token exactly once to the matching process" {
    var store = std.mem.zeroes(store_t);
    var out: [MaxTokenBytes]u8 = undefined;
    try store.put("token-a", 42, 1000);
    try testing.expectEqual(@as(?usize, null), try store.take_for_process(43, 1001, &out));
    try testing.expectEqual(@as(u32, 1), store.count);
    const length = (try store.take_for_process(42, 1001, &out)).?;
    try testing.expectEqualSlices(u8, "token-a", out[0..length]);
    try testing.expectEqual(@as(?usize, null), try store.take_for_process(42, 1002, &out));
    try testing.expectEqual(@as(u32, 0), store.count);
}

test "invalid token or process leaves the store and clock untouched" {
    var store = std.mem.zeroes(store_t);
    var out: [MaxTokenBytes]u8 = undefined;
    try testing.expectError(error.InvalidToken, store.put("", 1, 500));
    try testing.expectError(error.InvalidToken, store.put("bad token", 1, 500));
    try testing.expectError(error.InvalidProcess, store.put("ok", 0, 500));
    try testing.expectError(error.InvalidProcess, store.take_for_process(0, 400, &out));
    try testing.expectEqual(@as(u32, 0), store.clock_started);
    try testing.expectEqual(@as(u32, 0), store.count);
    try store.put("ok", 1, 100);
    try testing.expectEqual(@as(u64, 100), store.last_now);
}

test "entries are live strictly before inserted plus ttl and expire at the boundary" {
    var out: [MaxTokenBytes]u8 = undefined;
    var live = std.mem.zeroes(store_t);
    try live.put("edge", 7, 5000);
    try testing.expect((try live.take_for_process(7, 5000 + TtlMs - 1, &out)) != null);
    var expiring = std.mem.zeroes(store_t);
    try expiring.put("edge", 7, 5000);
    try testing.expectEqual(@as(?usize, null), try expiring.take_for_process(7, 5000 + TtlMs, &out));
    try testing.expectEqual(@as(u32, 0), expiring.count);
    try testing.expectEqual(@as(u32, 0), expiring.rollbacks);
}

test "expire removes only stale pairs and reports the removed count" {
    var store = std.mem.zeroes(store_t);
    try store.put("old-1", 1, 0);
    try store.put("old-2", 2, 10);
    try store.put("new", 3, 20_000);
    try testing.expectEqual(@as(usize, 0), store.expire(29_999));
    try testing.expectEqual(@as(usize, 2), store.expire(30_010));
    try testing.expectEqual(@as(u32, 1), store.count);
    store.clear();
    try testing.expectEqual(@as(u32, 0), store.count);
    try testing.expectEqual(@as(u32, 0), store.rollbacks);
}

test "duplicate token replaces the pid and refreshes the deadline" {
    var store = std.mem.zeroes(store_t);
    var out: [MaxTokenBytes]u8 = undefined;
    try store.put("dup", 1, 0);
    try store.put("dup", 2, 20_000);
    try testing.expectEqual(@as(u32, 1), store.count);
    try testing.expectEqual(@as(?usize, null), try store.take_for_process(1, 20_001, &out));
    try testing.expect((try store.take_for_process(2, 40_000, &out)) != null);
}

test "full store expires stale pairs before evicting and evicts the oldest" {
    var store = std.mem.zeroes(store_t);
    var name_buffer: [16]u8 = undefined;
    for (0..MaxEntries) |index| {
        const name = try std.fmt.bufPrint(&name_buffer, "t{d}", .{index});
        try store.put(name, @intCast(index + 1), 100);
    }
    try testing.expectEqual(@as(u32, MaxEntries), store.count);
    try store.put("fresh-a", 99, 100);
    try testing.expectEqual(@as(u32, MaxEntries), store.count);
    var out: [MaxTokenBytes]u8 = undefined;
    try testing.expectEqual(@as(?usize, null), try store.take_for_process(1, 100, &out));
    try testing.expect((try store.take_for_process(2, 100, &out)) != null);

    var expired = std.mem.zeroes(store_t);
    for (0..MaxEntries) |index| {
        const name = try std.fmt.bufPrint(&name_buffer, "e{d}", .{index});
        try expired.put(name, @intCast(index + 1), 0);
    }
    try expired.put("after", 77, 40_000);
    try testing.expectEqual(@as(u32, 1), expired.count);
    try testing.expectEqual(@as(u32, 0), expired.rollbacks);
}

test "newest token wins when one process holds several live tokens" {
    var store = std.mem.zeroes(store_t);
    var out: [MaxTokenBytes]u8 = undefined;
    try store.put("first", 5, 0);
    try store.put("second", 5, 1);
    const length = (try store.take_for_process(5, 2, &out)).?;
    try testing.expectEqualSlices(u8, "second", out[0..length]);
    const next = (try store.take_for_process(5, 3, &out)).?;
    try testing.expectEqualSlices(u8, "first", out[0..next]);
}

test "clock rollback clears every pair and never returns an earlier token" {
    var store = std.mem.zeroes(store_t);
    var out: [MaxTokenBytes]u8 = undefined;
    try store.put("before", 9, 1000);
    try testing.expectEqual(@as(?usize, null), try store.take_for_process(9, 500, &out));
    try testing.expectEqual(@as(u32, 1), store.rollbacks);
    try testing.expectEqual(@as(u32, 0), store.count);
    try store.put("after", 9, 600);
    try testing.expectEqual(@as(usize, 0), store.expire(601));
    try testing.expectEqual(@as(u32, 1), store.count);
    try testing.expectEqual(@as(usize, 1), store.expire(600 + TtlMs));
    try store.put("again", 9, 30_700);
    try testing.expectEqual(@as(u32, 1), store.count);
    try testing.expectEqual(@as(usize, 1), store.expire(30_700 + TtlMs));
    try testing.expectEqual(@as(u32, 1), store.rollbacks);
}

test "rollback during put clears old pairs and still inserts the new token" {
    var store = std.mem.zeroes(store_t);
    var out: [MaxTokenBytes]u8 = undefined;
    try store.put("old", 1, 9000);
    try store.put("new", 2, 3000);
    try testing.expectEqual(@as(u32, 1), store.rollbacks);
    try testing.expectEqual(@as(u32, 1), store.count);
    try testing.expectEqual(@as(?usize, null), try store.take_for_process(1, 3001, &out));
    try testing.expect((try store.take_for_process(2, 3001, &out)) != null);
}

test "clock may move forward by any amount without rollback" {
    var store = std.mem.zeroes(store_t);
    try store.put("far", 3, 0);
    try testing.expectEqual(@as(usize, 1), store.expire(std.math.maxInt(u64)));
    try testing.expectEqual(@as(u32, 0), store.rollbacks);
    try store.put("saturated", 4, std.math.maxInt(u64) - 5);
    var out: [MaxTokenBytes]u8 = undefined;
    try testing.expect((try store.take_for_process(4, std.math.maxInt(u64) - 1, &out)) != null);
}

test "seeded operation streams match a reference model step by step" {
    var prng = std.Random.DefaultPrng.init(0x61637476_73746f72);
    const random = prng.random();
    var store = std.mem.zeroes(store_t);

    // Reference model: an unordered list. Liveness is judged by the elapsed
    // time since insertion, not by stored deadlines, and eviction by an
    // explicit order counter, so neither shares code with store_t.
    const Record = struct { token: [8]u8 = undefined, token_length: usize = 0, process_id: u32 = 0, inserted: u64 = 0, order: u64 = 0, live: bool = false };
    var records = [_]Record{.{}} ** 48;
    var model_started = false;
    var model_last: u64 = 0;
    var model_rollbacks: u32 = 0;
    var model_order: u64 = 0;
    var now: u64 = 1000;
    var name_buffer: [8]u8 = undefined;
    var out: [MaxTokenBytes]u8 = undefined;
    var model_out: [8]u8 = undefined;

    for (0..20000) |_| {
        if (random.uintLessThan(u32, 25) == 0) {
            now -|= random.uintLessThan(u64, now + 1);
        } else {
            now += random.uintLessThan(u64, 12_000);
        }
        if (model_started and now < model_last) {
            for (&records) |*record| record.live = false;
            model_rollbacks += 1;
        }
        model_started = true;
        model_last = now;
        var expired_now: usize = 0;
        for (&records) |*record| {
            if (record.live and now - record.inserted >= TtlMs) {
                record.live = false;
                expired_now += 1;
            }
        }

        switch (random.uintLessThan(u32, 10)) {
            0...5 => {
                const index = random.uintLessThan(u32, 40);
                const token = try std.fmt.bufPrint(&name_buffer, "s{d}", .{index});
                const pid: u32 = 1 + random.uintLessThan(u32, 5);
                try store.put(token, pid, now);
                var found: ?*Record = null;
                var live_count: usize = 0;
                for (&records) |*record| {
                    if (!record.live) continue;
                    live_count += 1;
                    if (std.mem.eql(u8, record.token[0..record.token_length], token)) found = record;
                }
                if (found) |record| {
                    record.process_id = pid;
                    record.inserted = now;
                    model_order += 1;
                    record.order = model_order;
                } else {
                    if (live_count == MaxEntries) {
                        var victim: ?*Record = null;
                        for (&records) |*record| {
                            if (record.live and (victim == null or record.order < victim.?.order)) victim = record;
                        }
                        victim.?.live = false;
                    }
                    for (&records) |*record| {
                        if (record.live) continue;
                        @memcpy(record.token[0..token.len], token);
                        record.token_length = token.len;
                        record.process_id = pid;
                        record.inserted = now;
                        model_order += 1;
                        record.order = model_order;
                        record.live = true;
                        break;
                    }
                }
            },
            6, 7 => {
                const pid: u32 = 1 + random.uintLessThan(u32, 5);
                var best: ?*Record = null;
                for (&records) |*record| {
                    if (record.live and record.process_id == pid and (best == null or record.order > best.?.order)) best = record;
                }
                const actual = try store.take_for_process(pid, now, &out);
                if (best) |chosen| {
                    @memcpy(model_out[0..chosen.token_length], chosen.token[0..chosen.token_length]);
                    try testing.expectEqual(@as(?usize, chosen.token_length), actual);
                    try testing.expectEqualSlices(u8, model_out[0..chosen.token_length], out[0..chosen.token_length]);
                    chosen.live = false;
                } else {
                    try testing.expectEqual(@as(?usize, null), actual);
                }
            },
            else => {
                try testing.expectEqual(expired_now, store.expire(now));
            },
        }
        var live_total: usize = 0;
        for (records) |record| live_total += @intFromBool(record.live);
        try testing.expectEqual(@as(u32, @intCast(live_total)), store.count);
        try testing.expectEqual(model_rollbacks, store.rollbacks);
    }
    try testing.expect(model_rollbacks > 0);
}

test "C ABI put take expire and clear map every status" {
    var store: store_t = undefined;
    try testing.expectEqual(@as(c_int, 0), interop_activation_store_init(&store));
    try testing.expectEqual(@as(c_int, 1), interop_activation_store_init(null));
    try testing.expectEqual(@as(c_int, 1), interop_activation_put(null, "x", 1, 1, 0));
    try testing.expectEqual(@as(c_int, 1), interop_activation_put(&store, null, 1, 1, 0));
    try testing.expectEqual(@as(c_int, 2), interop_activation_put(&store, "", 0, 1, 0));
    try testing.expectEqual(@as(c_int, 3), interop_activation_put(&store, "x", 1, 0, 0));
    try testing.expectEqual(@as(c_int, 0), interop_activation_put(&store, "abc", 3, 11, 100));
    var out: [MaxTokenBytes]u8 = undefined;
    var length: usize = 0;
    try testing.expectEqual(@as(c_int, 1), interop_activation_take(null, 11, 101, &out, &length));
    try testing.expectEqual(@as(c_int, 1), interop_activation_take(&store, 11, 101, null, &length));
    try testing.expectEqual(@as(c_int, 1), interop_activation_take(&store, 11, 101, &out, null));
    try testing.expectEqual(@as(c_int, 3), interop_activation_take(&store, 0, 101, &out, &length));
    try testing.expectEqual(@as(c_int, 2), interop_activation_take(&store, 12, 101, &out, &length));
    try testing.expectEqual(@as(c_int, 0), interop_activation_take(&store, 11, 101, &out, &length));
    try testing.expectEqualSlices(u8, "abc", out[0..length]);
    try testing.expectEqual(@as(usize, 0), interop_activation_expire(null, 1));
    try testing.expectEqual(@as(c_int, 0), interop_activation_put(&store, "gone", 4, 5, 200));
    try testing.expectEqual(@as(usize, 1), interop_activation_expire(&store, 200 + TtlMs));
    try testing.expectEqual(@as(c_int, 0), interop_activation_put(&store, "clr", 3, 6, 30_300));
    interop_activation_clear(null);
    interop_activation_clear(&store);
    try testing.expectEqual(@as(u32, 0), store.count);
}

test "C ABI pick reports found, none and invalid output arguments" {
    var pointer: [*]const u8 = undefined;
    var length: usize = 0;
    try testing.expectEqual(@as(c_int, 0), interop_activation_pick_token("xdg-1", "startup", &pointer, &length));
    try testing.expectEqualSlices(u8, "xdg-1", pointer[0..length]);
    try testing.expectEqual(@as(c_int, 0), interop_activation_pick_token("bad tok", "startup", &pointer, &length));
    try testing.expectEqualSlices(u8, "startup", pointer[0..length]);
    try testing.expectEqual(@as(c_int, 0), interop_activation_pick_token(null, "s2", &pointer, &length));
    try testing.expectEqual(@as(c_int, 1), interop_activation_pick_token(null, null, &pointer, &length));
    try testing.expectEqual(@as(c_int, 1), interop_activation_pick_token("", "bad tok", &pointer, &length));
    try testing.expectEqual(@as(c_int, 2), interop_activation_pick_token("x", "y", null, &length));
    try testing.expectEqual(@as(c_int, 2), interop_activation_pick_token("x", "y", &pointer, null));
    var long: [MaxTokenBytes + 1:0]u8 = [_:0]u8{0} ** (MaxTokenBytes + 1);
    @memset(long[0 .. MaxTokenBytes + 1], 'q');
    try testing.expectEqual(@as(c_int, 1), interop_activation_pick_token(&long, null, &pointer, &length));
    try testing.expectEqual(@as(c_int, 1), interop_activation_pick_token(null, &long, &pointer, &length));
}
