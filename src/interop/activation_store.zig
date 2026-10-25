//! Activation token validation, environment selection and a bounded store of the
//! host's own pending tokens, plus a ring of recently spent tokens. Each token is
//! refused while it is pending or spent within the last 30 s. Fail-closed clock
//! rollback handling. Caller-owned storage, no allocation, no I/O.
const std = @import("std");

/// Maximum token length in bytes; the wire field limit is 255.
pub const MaxTokenBytes: usize = 255;
/// Caller buffer size for take: the token followed by a NUL terminator.
pub const TokenBufferBytes: usize = MaxTokenBytes + 1;
/// Capacity of the pending array and of the spent ring.
pub const MaxEntries: usize = 32;
/// Lifetime in milliseconds: an entry is live while now < inserted + TtlMs.
pub const TtlMs: u64 = 30_000;
/// Byte size of one token slot, including unused tail bytes.
const TokenSlot: usize = 256;

/// Put refusals and fatal argument errors. Every error leaves the store unchanged.
pub const error_t = error{ InvalidToken, InvalidProcess, Duplicate, TokenSpent, SpentRingFull };

/// One slot of the pending array or the spent ring. Extern layout (280 bytes).
pub const entry_t = extern struct {
    /// Token bytes; only the first token_length bytes are meaningful.
    token: [TokenSlot]u8,
    /// Token length, 1..255 while occupied; 0 when free.
    token_length: u8,
    /// 1 while the slot holds an entry (possibly expired but not yet removed), else 0.
    occupied: u8,
    /// Zero padding that aligns process_id to four bytes.
    reserved: [2]u8,
    /// Owning process identifier for a pending entry; 0 in a spent entry.
    process_id: u32,
    /// Expiry instant: inserted (or spent) + TtlMs, saturating at the u64 maximum.
    deadline_ms: u64,
    /// Insertion order; a larger value is newer. Used for eviction and newest-wins.
    sequence: u64,
};

/// Store of pending tokens and spent tokens. Extern layout (17960 bytes); an all-zero
/// value is a valid empty store. Not thread-safe; one owner thread.
pub const store_t = extern struct {
    /// Tokens the host itself obtained and has not yet taken.
    pending: [MaxEntries]entry_t,
    /// Tokens returned by take_for_process, refused by put until their deadline.
    spent: [MaxEntries]entry_t,
    /// Sequence given to the next insertion; the first insertion gets 0.
    next_sequence: u64,
    /// Most recent accepted clock value; meaningful when clock_started is 1.
    last_now: u64,
    /// Occupied pending slots.
    count: u32,
    /// Occupied spent slots.
    spent_count: u32,
    /// 0 before the first timed operation, 1 afterwards.
    clock_started: u32,
    /// Number of detected clock rollbacks; each one cleared pending and spent.
    rollbacks: u32,
    /// Zero padding to an eight-byte multiple.
    reserved: [2]u32,

    /// Insert a pending token. Refusals are decided before the clock is observed,
    /// so a refused put changes nothing at all.
    /// @param self Store, mutated on success only.
    /// @param token Borrowed token bytes; must satisfy token_valid.
    /// @param process_id Nonzero process identifier that will take the token.
    /// @param now Monotonic milliseconds from the caller clock.
    /// @return error.InvalidToken, error.InvalidProcess, error.Duplicate (live pending
    ///   copy exists), error.TokenSpent (live spent copy exists), or error.SpentRingFull
    ///   (all spent slots are live, so a spent token could not be recognized).
    pub fn put(self: *store_t, token: []const u8, process_id: u32, now: u64) error_t!void {
        if (!token_valid(token)) return error.InvalidToken;
        if (process_id == 0) return error.InvalidProcess;
        if (!self.rolled_back(now)) {
            if (live_index(&self.pending, token, now) != null) return error.Duplicate;
            if (live_index(&self.spent, token, now) != null) return error.TokenSpent;
            if (live_count(&self.spent, now) == MaxEntries) return error.SpentRingFull;
        }
        self.observe(now);
        _ = self.expire_stale(now);
        if (self.count == MaxEntries) {
            self.pending[oldest(&self.pending)] = std.mem.zeroes(entry_t);
            self.count -= 1;
        }
        self.fill(&self.pending, first_free(&self.pending), token, process_id, now +| TtlMs);
        self.count += 1;
    }

    /// Remove and return the newest live pending token for a process identifier.
    /// The token is moved to the spent ring, so put refuses it for the next 30 s.
    /// @param self Store, mutated on success.
    /// @param process_id Nonzero process identifier.
    /// @param now Monotonic milliseconds from the caller clock.
    /// @param out Caller buffer of TokenBufferBytes (256): the token, then a NUL at index length.
    ///   Bytes after the NUL are not written.
    /// @return Token length, or null when no live pending token matches.
    /// @note When the spent ring is full, the oldest spent entry is evicted to make room. This
    ///   is safe: while the ring is full put refuses every token with SpentRingFull, so the
    ///   evicted token cannot be re-accepted before its own 30 s window ends.
    pub fn take_for_process(self: *store_t, process_id: u32, now: u64, out: *[TokenBufferBytes]u8) error{InvalidProcess}!?usize {
        if (process_id == 0) return error.InvalidProcess;
        self.observe(now);
        _ = self.expire_stale(now);
        const index = newest_for(&self.pending, process_id) orelse return null;
        const source = self.pending[index];
        const length: usize = source.token_length;
        @memcpy(out[0..length], source.token[0..length]);
        out[length] = 0;
        self.pending[index] = std.mem.zeroes(entry_t);
        self.count -= 1;
        if (self.spent_count == MaxEntries) {
            self.spent[oldest(&self.spent)] = std.mem.zeroes(entry_t);
            self.spent_count -= 1;
        }
        self.fill(&self.spent, first_free(&self.spent), source.token[0..length], 0, now +| TtlMs);
        self.spent_count += 1;
        return length;
    }

    /// Remove every expired pending and spent entry. Returns the pending count removed.
    /// @param self Store, mutated.
    /// @param now Monotonic milliseconds from the caller clock.
    /// @return Number of pending entries removed by this call.
    pub fn expire(self: *store_t, now: u64) usize {
        self.observe(now);
        return self.expire_stale(now);
    }

    /// Remove every pending entry. Spent entries and clock state are kept, so a
    /// token taken before clear is still refused until its deadline.
    /// @param self Store, mutated.
    pub fn clear(self: *store_t) void {
        for (&self.pending) |*entry| entry.* = std.mem.zeroes(entry_t);
        self.count = 0;
    }

    fn rolled_back(self: *const store_t, now: u64) bool {
        return self.clock_started != 0 and now < self.last_now;
    }

    /// Record the clock. A value below the last accepted one is a rollback: pending and
    /// spent entries are both dropped, so no earlier token is refused or returned from them.
    fn observe(self: *store_t, now: u64) void {
        if (self.rolled_back(now)) {
            self.clear();
            for (&self.spent) |*entry| entry.* = std.mem.zeroes(entry_t);
            self.spent_count = 0;
            self.rollbacks += 1;
        }
        self.clock_started = 1;
        self.last_now = now;
    }

    fn expire_stale(self: *store_t, now: u64) usize {
        for (&self.spent) |*entry| {
            if (entry.occupied != 0 and now >= entry.deadline_ms) {
                entry.* = std.mem.zeroes(entry_t);
                self.spent_count -= 1;
            }
        }
        var removed: usize = 0;
        for (&self.pending) |*entry| {
            if (entry.occupied != 0 and now >= entry.deadline_ms) {
                entry.* = std.mem.zeroes(entry_t);
                self.count -= 1;
                removed += 1;
            }
        }
        return removed;
    }

    fn fill(self: *store_t, slots: []entry_t, index: usize, token: []const u8, process_id: u32, deadline: u64) void {
        const entry = &slots[index];
        @memcpy(entry.token[0..token.len], token);
        entry.token_length = @intCast(token.len);
        entry.occupied = 1;
        entry.process_id = process_id;
        entry.deadline_ms = deadline;
        entry.sequence = self.next_sequence;
        self.next_sequence += 1;
    }
};

fn live_index(slots: []const entry_t, token: []const u8, now: u64) ?usize {
    for (slots, 0..) |entry, index| {
        if (entry.occupied != 0 and now < entry.deadline_ms and std.mem.eql(u8, entry.token[0..entry.token_length], token)) return index;
    }
    return null;
}

fn live_count(slots: []const entry_t, now: u64) usize {
    var count: usize = 0;
    for (slots) |entry| count += @intFromBool(entry.occupied != 0 and now < entry.deadline_ms);
    return count;
}

/// Index of the occupied slot with the smallest sequence. Callers guarantee one exists.
fn oldest(slots: []const entry_t) usize {
    var best: ?usize = null;
    for (slots, 0..) |entry, index| {
        if (entry.occupied == 0) continue;
        if (best == null or entry.sequence < slots[best.?].sequence) best = index;
    }
    return best orelse unreachable;
}

fn newest_for(slots: []const entry_t, process_id: u32) ?usize {
    var best: ?usize = null;
    for (slots, 0..) |entry, index| {
        if (entry.occupied == 0 or entry.process_id != process_id) continue;
        if (best == null or entry.sequence > slots[best.?].sequence) best = index;
    }
    return best;
}

fn first_free(slots: []const entry_t) usize {
    for (slots, 0..) |entry, index| {
        if (entry.occupied == 0) return index;
    }
    unreachable;
}

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
/// @param[out] store Nonnull; InteropActivationStoreBytes writable bytes, 8-byte aligned.
/// @return 0 on success, 1 when store is NULL.
export fn interop_activation_store_init(store: ?*store_t) c_int {
    const target = store orelse return 1;
    target.* = std.mem.zeroes(store_t);
    return 0;
}

/// C ABI: insert a pending token. Maps store_t.put errors to status codes 2..6.
/// @param[in,out] store Nonnull store.
/// @param[in] token Readable token bytes; NULL is allowed only when token_length is 0.
/// @param[in] token_length Byte count, 0..255 valid.
/// @param[in] process_id Nonzero process identifier.
/// @param[in] now_ms Monotonic milliseconds.
/// @return 0 Ok; 1 InvalidArgument (NULL store, or NULL token with length above 0);
///         2 InvalidToken; 3 InvalidProcess; 4 Duplicate; 5 TokenSpent; 6 SpentRingFull.
export fn interop_activation_put(store: ?*store_t, token: ?[*]const u8, token_length: usize, process_id: u32, now_ms: u64) c_int {
    const target = store orelse return 1;
    const bytes: []const u8 = if (token_length == 0) &.{} else (token orelse return 1)[0..token_length];
    target.put(bytes, process_id, now_ms) catch |err| return switch (err) {
        error.InvalidToken => 2,
        error.InvalidProcess => 3,
        error.Duplicate => 4,
        error.TokenSpent => 5,
        error.SpentRingFull => 6,
    };
    return 0;
}

/// C ABI: remove and copy the newest live pending token for a process identifier.
/// The output is the token followed by a NUL terminator; bytes after it are not written.
/// @param[in,out] store Nonnull store.
/// @param[in] process_id Nonzero process identifier.
/// @param[in] now_ms Monotonic milliseconds.
/// @param[out] out Nonnull buffer of 256 bytes: the token, then a NUL at index length.
/// @param[out] length_out Nonnull; receives the token length on success.
/// @return 0 Found; 1 InvalidArgument (NULL pointer); 2 NotFound; 3 InvalidProcess.
export fn interop_activation_take(store: ?*store_t, process_id: u32, now_ms: u64, out: ?*[TokenBufferBytes]u8, length_out: ?*usize) c_int {
    const target = store orelse return 1;
    const destination = out orelse return 1;
    const length_ref = length_out orelse return 1;
    const found = target.take_for_process(process_id, now_ms, destination) catch return 3;
    length_ref.* = found orelse return 2;
    return 0;
}

/// C ABI: remove expired pending and spent entries.
/// @param[in,out] store Store; NULL returns 0.
/// @param[in] now_ms Monotonic milliseconds.
/// @return Number of pending entries removed.
export fn interop_activation_expire(store: ?*store_t, now_ms: u64) usize {
    const target = store orelse return 0;
    return target.expire(now_ms);
}

/// C ABI: remove every pending entry; NULL is a no-op. Spent entries are kept.
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

fn opaque_value(comptime value_t: type, value: value_t) value_t {
    var copy = value;
    std.mem.doNotOptimizeAway(&copy);
    return copy;
}

fn opaque_bytes(bytes: []const u8) []const u8 {
    return opaque_value([]const u8, bytes);
}

fn opaque_candidate(bytes: []const u8) ?[]const u8 {
    return opaque_value(?[]const u8, bytes);
}

fn put_at(store: *store_t, token: []const u8, process_id: u32, now: u64) error_t!void {
    return store.put(opaque_value([]const u8, token), opaque_value(u32, process_id), opaque_value(u64, now));
}

fn take_at(store: *store_t, process_id: u32, now: u64, out: *[TokenBufferBytes]u8) error{InvalidProcess}!?usize {
    return store.take_for_process(opaque_value(u32, process_id), opaque_value(u64, now), out);
}

fn expire_at(store: *store_t, now: u64) usize {
    return store.expire(opaque_value(u64, now));
}

fn absent_store() ?*store_t {
    return opaque_value(?*store_t, null);
}

fn absent_bytes() ?[*]const u8 {
    return opaque_value(?[*]const u8, null);
}

fn absent_out() ?*[TokenBufferBytes]u8 {
    return opaque_value(?*[TokenBufferBytes]u8, null);
}

fn absent_length() ?*usize {
    return opaque_value(?*usize, null);
}

fn absent_string() ?[*:0]const u8 {
    return opaque_value(?[*:0]const u8, null);
}

fn absent_token_out() ?*[*]const u8 {
    return opaque_value(?*[*]const u8, null);
}

fn absent_slice() ?[]const u8 {
    return opaque_value(?[]const u8, null);
}

fn c_put(store: ?*store_t, token: ?[*]const u8, token_length: usize, process_id: u32, now: u64) c_int {
    return interop_activation_put(opaque_value(?*store_t, store), opaque_value(?[*]const u8, token), opaque_value(usize, token_length), opaque_value(u32, process_id), opaque_value(u64, now));
}

fn c_take(store: ?*store_t, process_id: u32, now: u64, out: ?*[TokenBufferBytes]u8, length_out: ?*usize) c_int {
    return interop_activation_take(opaque_value(?*store_t, store), opaque_value(u32, process_id), opaque_value(u64, now), opaque_value(?*[TokenBufferBytes]u8, out), opaque_value(?*usize, length_out));
}

test "extern layouts match activation_store.h byte for byte" {
    try testing.expectEqual(@as(usize, 280), @sizeOf(entry_t));
    try testing.expectEqual(@as(usize, 17960), @sizeOf(store_t));
    try testing.expectEqual(@as(usize, 8), @alignOf(store_t));
    try testing.expectEqual(@as(usize, 256), @offsetOf(entry_t, "token_length"));
    try testing.expectEqual(@as(usize, 257), @offsetOf(entry_t, "occupied"));
    try testing.expectEqual(@as(usize, 260), @offsetOf(entry_t, "process_id"));
    try testing.expectEqual(@as(usize, 264), @offsetOf(entry_t, "deadline_ms"));
    try testing.expectEqual(@as(usize, 272), @offsetOf(entry_t, "sequence"));
    try testing.expectEqual(@as(usize, 8960), @offsetOf(store_t, "spent"));
    try testing.expectEqual(@as(usize, 17920), @offsetOf(store_t, "next_sequence"));
    try testing.expectEqual(@as(usize, 17928), @offsetOf(store_t, "last_now"));
    try testing.expectEqual(@as(usize, 17936), @offsetOf(store_t, "count"));
    try testing.expectEqual(@as(usize, 17940), @offsetOf(store_t, "spent_count"));
    try testing.expectEqual(@as(usize, 17944), @offsetOf(store_t, "clock_started"));
    try testing.expectEqual(@as(usize, 17948), @offsetOf(store_t, "rollbacks"));
    try testing.expectEqual(@as(usize, 17952), @offsetOf(store_t, "reserved"));
}

test "token rule accepts exact printable bounds and rejects everything else" {
    try testing.expect(!token_valid(opaque_bytes("")));
    try testing.expect(token_valid(opaque_bytes("a")));
    try testing.expect(token_valid(opaque_bytes("!~")));
    try testing.expect(!token_valid(opaque_bytes(" a")));
    try testing.expect(!token_valid(opaque_bytes("a b")));
    try testing.expect(!token_valid(opaque_bytes("\x20")));
    try testing.expect(!token_valid(opaque_bytes("\x7f")));
    try testing.expect(!token_valid(opaque_bytes("\x00")));
    try testing.expect(!token_valid(opaque_bytes("\xc3\xa9")));
    const max = [_]u8{'t'} ** MaxTokenBytes;
    try testing.expect(token_valid(opaque_bytes(&max)));
    const over = [_]u8{'t'} ** (MaxTokenBytes + 1);
    try testing.expect(!token_valid(opaque_bytes(&over)));
}

test "pick_token prefers XDG then DESKTOP_STARTUP_ID and skips invalid candidates" {
    try testing.expectEqualSlices(u8, "xdg-1", pick_token(opaque_candidate("xdg-1"), opaque_candidate("startup-1")).?);
    try testing.expectEqualSlices(u8, "startup-1", pick_token(opaque_candidate("bad token"), opaque_candidate("startup-1")).?);
    try testing.expectEqualSlices(u8, "startup-1", pick_token(absent_slice(), opaque_candidate("startup-1")).?);
    try testing.expectEqualSlices(u8, "startup-1", pick_token(opaque_candidate(""), opaque_candidate("startup-1")).?);
    try testing.expect(pick_token(opaque_candidate("bad token"), opaque_candidate("")) == null);
    try testing.expect(pick_token(absent_slice(), absent_slice()) == null);
    const over = [_]u8{'x'} ** (MaxTokenBytes + 1);
    try testing.expectEqualSlices(u8, "fallback", pick_token(opaque_candidate(&over), opaque_candidate("fallback")).?);
}

test "put and take return a token exactly once to the matching process" {
    var store = std.mem.zeroes(store_t);
    var out: [TokenBufferBytes]u8 = undefined;
    try put_at(&store, "token-a", 42, 1000);
    try testing.expectEqual(@as(?usize, null), try take_at(&store, 43, 1001, &out));
    try testing.expectEqual(@as(u32, 1), store.count);
    const length = (try take_at(&store, 42, 1001, &out)).?;
    try testing.expectEqualSlices(u8, "token-a", out[0..length]);
    try testing.expectEqual(@as(u32, 0), store.count);
    try testing.expectEqual(@as(u32, 1), store.spent_count);
    try testing.expectEqual(@as(?usize, null), try take_at(&store, 42, 1002, &out));
}

test "invalid token or process leaves the store and clock untouched" {
    var store = std.mem.zeroes(store_t);
    var out: [TokenBufferBytes]u8 = undefined;
    try testing.expectError(error.InvalidToken, put_at(&store, "", 1, 500));
    try testing.expectError(error.InvalidToken, put_at(&store, "bad token", 1, 500));
    try testing.expectError(error.InvalidProcess, put_at(&store, "ok", 0, 500));
    try testing.expectError(error.InvalidProcess, take_at(&store, 0, 400, &out));
    try testing.expectEqual(@as(u32, 0), store.clock_started);
    try testing.expectEqual(@as(u32, 0), store.count);
    try put_at(&store, "ok", 1, 100);
    try testing.expectEqual(@as(u64, 100), store.last_now);
}

test "a refused put changes nothing, including the clock baseline" {
    var store = std.mem.zeroes(store_t);
    try put_at(&store, "dup", 1, 0);
    try testing.expectError(error.Duplicate, put_at(&store, "dup", 2, 20_000));
    try testing.expectEqual(@as(u64, 0), store.last_now);
    try testing.expectEqual(@as(u32, 1), store.count);
    var out: [TokenBufferBytes]u8 = undefined;
    const length = (try take_at(&store, 1, 20_001, &out)).?;
    try testing.expectEqualSlices(u8, "dup", out[0..length]);
}

test "a taken token is refused as TokenSpent for exactly 30 s and then accepted again" {
    var store = std.mem.zeroes(store_t);
    var out: [TokenBufferBytes]u8 = undefined;
    try put_at(&store, "spent-token", 1, 0);
    _ = (try take_at(&store, 1, 1000, &out)).?;
    try testing.expectError(error.TokenSpent, put_at(&store, "spent-token", 2, 1000));
    try testing.expectError(error.TokenSpent, put_at(&store, "spent-token", 2, 1000 + TtlMs - 1));
    try testing.expectEqual(@as(u32, 1), store.spent_count);
    try put_at(&store, "spent-token", 2, 1000 + TtlMs);
    try testing.expectEqual(@as(u32, 0), store.spent_count);
}

test "a full spent ring refuses new tokens with SpentRingFull until an entry expires" {
    var store = std.mem.zeroes(store_t);
    var out: [TokenBufferBytes]u8 = undefined;
    var name_buffer: [16]u8 = undefined;
    for (0..MaxEntries) |index| {
        const name = try std.fmt.bufPrint(&name_buffer, "s{d}", .{index});
        try put_at(&store, name, @intCast(index + 1), 0);
        _ = (try take_at(&store, @intCast(index + 1), 0, &out)).?;
    }
    try testing.expectEqual(@as(u32, MaxEntries), store.spent_count);
    try testing.expectError(error.SpentRingFull, put_at(&store, "fresh", 99, 0));
    try testing.expectError(error.SpentRingFull, put_at(&store, "fresh", 99, TtlMs - 1));
    try testing.expectEqual(@as(u32, 0), store.count);
    try put_at(&store, "fresh", 99, TtlMs);
    try testing.expectEqual(@as(u32, 0), store.spent_count);
}

test "take evicts the oldest spent entry when full, and the evicted token stays refused" {
    var store = std.mem.zeroes(store_t);
    var out: [TokenBufferBytes]u8 = undefined;
    var name_buffer: [16]u8 = undefined;
    try put_at(&store, "pending", 7, 0);
    for (0..MaxEntries) |index| {
        const name = try std.fmt.bufPrint(&name_buffer, "s{d}", .{index});
        try put_at(&store, name, @intCast(index + 1), 0);
        _ = (try take_at(&store, @intCast(index + 1), 0, &out)).?;
    }
    const length = (try take_at(&store, 7, 0, &out)).?;
    try testing.expectEqualSlices(u8, "pending", out[0..length]);
    try testing.expectEqual(@as(u32, MaxEntries), store.spent_count);
    try testing.expectError(error.TokenSpent, put_at(&store, "pending", 8, 0));
    try testing.expectError(error.SpentRingFull, put_at(&store, "s0", 1, 0));
}

test "entries are live strictly before inserted plus ttl and expire at the boundary" {
    var out: [TokenBufferBytes]u8 = undefined;
    var live = std.mem.zeroes(store_t);
    try put_at(&live, "edge", 7, 5000);
    try testing.expect((try take_at(&live, 7, 5000 + TtlMs - 1, &out)) != null);
    var expiring = std.mem.zeroes(store_t);
    try put_at(&expiring, "edge", 7, 5000);
    try testing.expectEqual(@as(?usize, null), try take_at(&expiring, 7, 5000 + TtlMs, &out));
    try testing.expectEqual(@as(u32, 0), expiring.count);
    try testing.expectEqual(@as(u32, 0), expiring.rollbacks);
}

test "expire removes only stale pending entries and reports the removed count" {
    var store = std.mem.zeroes(store_t);
    try put_at(&store, "old-1", 1, 0);
    try put_at(&store, "old-2", 2, 10);
    try put_at(&store, "new", 3, 20_000);
    try testing.expectEqual(@as(usize, 0), expire_at(&store, 29_999));
    try testing.expectEqual(@as(usize, 2), expire_at(&store, 30_010));
    try testing.expectEqual(@as(u32, 1), store.count);
    store.clear();
    try testing.expectEqual(@as(u32, 0), store.count);
    try testing.expectEqual(@as(u32, 0), store.rollbacks);
}

test "full pending store expires stale entries before evicting and evicts the oldest" {
    var store = std.mem.zeroes(store_t);
    var name_buffer: [16]u8 = undefined;
    for (0..MaxEntries) |index| {
        const name = try std.fmt.bufPrint(&name_buffer, "t{d}", .{index});
        try put_at(&store, name, @intCast(index + 1), 100);
    }
    try testing.expectEqual(@as(u32, MaxEntries), store.count);
    try put_at(&store, "fresh-a", 99, 100);
    try testing.expectEqual(@as(u32, MaxEntries), store.count);
    var out: [TokenBufferBytes]u8 = undefined;
    try testing.expectEqual(@as(?usize, null), try take_at(&store, 1, 100, &out));
    try testing.expect((try take_at(&store, 2, 100, &out)) != null);

    var expired = std.mem.zeroes(store_t);
    for (0..MaxEntries) |index| {
        const name = try std.fmt.bufPrint(&name_buffer, "e{d}", .{index});
        try put_at(&expired, name, @intCast(index + 1), 0);
    }
    try put_at(&expired, "after", 77, 40_000);
    try testing.expectEqual(@as(u32, 1), expired.count);
    try testing.expectEqual(@as(u32, 0), expired.rollbacks);
}

test "newest token wins when one process holds several live tokens" {
    var store = std.mem.zeroes(store_t);
    var out: [TokenBufferBytes]u8 = undefined;
    try put_at(&store, "first", 5, 0);
    try put_at(&store, "second", 5, 1);
    const length = (try take_at(&store, 5, 2, &out)).?;
    try testing.expectEqualSlices(u8, "second", out[0..length]);
    const next = (try take_at(&store, 5, 3, &out)).?;
    try testing.expectEqualSlices(u8, "first", out[0..next]);
}

test "clock rollback clears pending and spent entries and never returns an earlier token" {
    var store = std.mem.zeroes(store_t);
    var out: [TokenBufferBytes]u8 = undefined;
    try put_at(&store, "before", 9, 1000);
    _ = (try take_at(&store, 9, 1000, &out)).?;
    try put_at(&store, "pending", 4, 1000);
    try testing.expectEqual(@as(?usize, null), try take_at(&store, 9, 500, &out));
    try testing.expectEqual(@as(u32, 1), store.rollbacks);
    try testing.expectEqual(@as(u32, 0), store.spent_count);
    try testing.expectEqual(@as(u32, 0), store.count);
    try put_at(&store, "before", 9, 600);
    const length = (try take_at(&store, 9, 601, &out)).?;
    try testing.expectEqualSlices(u8, "before", out[0..length]);
    try testing.expectEqual(@as(usize, 0), expire_at(&store, 601));
    try put_at(&store, "again", 9, 30_700);
    try testing.expectEqual(@as(usize, 1), expire_at(&store, 30_700 + TtlMs));
    try testing.expectEqual(@as(u32, 1), store.rollbacks);
}

test "rollback during put clears old entries and still inserts the new token" {
    var store = std.mem.zeroes(store_t);
    var out: [TokenBufferBytes]u8 = undefined;
    try put_at(&store, "old", 1, 9000);
    try put_at(&store, "new", 2, 3000);
    try testing.expectEqual(@as(u32, 1), store.rollbacks);
    try testing.expectEqual(@as(u32, 1), store.count);
    try testing.expectEqual(@as(?usize, null), try take_at(&store, 1, 3001, &out));
    try testing.expect((try take_at(&store, 2, 3001, &out)) != null);
}

test "clock may move forward by any amount without rollback" {
    var store = std.mem.zeroes(store_t);
    try put_at(&store, "far", 3, 0);
    try testing.expectEqual(@as(usize, 1), expire_at(&store, std.math.maxInt(u64)));
    try testing.expectEqual(@as(u32, 0), store.rollbacks);
    try put_at(&store, "saturated", 4, std.math.maxInt(u64) - 5);
    var out: [TokenBufferBytes]u8 = undefined;
    try testing.expect((try take_at(&store, 4, std.math.maxInt(u64) - 1, &out)) != null);
}

test "a deadline saturated at the u64 maximum expires at the last representable instant" {
    var store = std.mem.zeroes(store_t);
    try put_at(&store, "last", 5, std.math.maxInt(u64));
    try testing.expectEqual(std.math.maxInt(u64), store.pending[0].deadline_ms);
    var before_end = std.mem.zeroes(store_t);
    try put_at(&before_end, "last", 5, std.math.maxInt(u64) - 1);
    try testing.expectEqual(@as(usize, 0), expire_at(&before_end, std.math.maxInt(u64) - 2));
    try testing.expectEqual(@as(usize, 1), expire_at(&store, std.math.maxInt(u64)));
}

test "oldest scans past free slots and prefers the smallest sequence" {
    var store = std.mem.zeroes(store_t);
    try put_at(&store, "x", 1, 0);
    try put_at(&store, "y", 2, 0);
    var out: [TokenBufferBytes]u8 = undefined;
    _ = try take_at(&store, 1, 0, &out);
    try put_at(&store, "z", 3, 0);
    try testing.expectEqual(@as(usize, 1), oldest(&store.pending));
}

test "take writes a NUL terminator after the token and leaves the tail alone" {
    var store = std.mem.zeroes(store_t);
    var out: [TokenBufferBytes]u8 = [_]u8{0xaa} ** TokenBufferBytes;
    try put_at(&store, "abc", 4, 0);
    const length = (try take_at(&store, 4, 1, &out)).?;
    try testing.expectEqual(@as(usize, 3), length);
    try testing.expectEqualSlices(u8, "abc", out[0..length]);
    try testing.expectEqual(@as(u8, 0), out[length]);
    try testing.expectEqual(@as(u8, 0xaa), out[length + 1]);
    try testing.expectEqual(@as(usize, 256), TokenBufferBytes);
}

test "seeded operation streams match a reference model step by step, including spent refusals" {
    var prng = std.Random.DefaultPrng.init(0x61637476_73746f72);
    const random = prng.random();
    var store = std.mem.zeroes(store_t);

    // Reference model: two unordered lists. Liveness is judged from elapsed time since
    // insertion or spend, and eviction by an explicit order counter. No code is shared
    // with store_t. A refused put leaves the model clock untouched, as in the store.
    const model_t = struct { token: [8]u8 = undefined, token_length: usize = 0, process_id: u32 = 0, at: u64 = 0, order: u64 = 0, live: bool = false };
    var pending = [_]model_t{.{}} ** 48;
    var spent = [_]model_t{.{}} ** 48;
    var model_started = false;
    var model_last: u64 = 0;
    var model_rollbacks: u32 = 0;
    var model_order: u64 = 0;
    var now: u64 = 1000;
    var name_buffer: [8]u8 = undefined;
    var out: [TokenBufferBytes]u8 = undefined;
    var model_out: [8]u8 = undefined;
    var outcomes = [_]usize{0} ** 4;

    for (0..30000) |_| {
        if (random.uintLessThan(u32, 25) == 0) {
            now -|= random.uintLessThan(u64, now + 1);
        } else {
            now += random.uintLessThan(u64, 9_000);
        }
        const rolled = model_started and now < model_last;
        const choice = random.uintLessThan(u32, 10);
        const index = random.uintLessThan(u32, 40);
        const token = try std.fmt.bufPrint(&name_buffer, "s{d}", .{index});
        const pid: u32 = 1 + random.uintLessThan(u32, 5);

        if (choice <= 3) {
            // Put: refusals are decided on the pre-rollback state, and change nothing.
            var expected_error: ?error_t = null;
            if (!rolled) {
                var pending_live: ?usize = null;
                var spent_live: ?usize = null;
                var spent_count: usize = 0;
                for (&pending, 0..) |*entry, slot| {
                    if (entry.live and now - entry.at < TtlMs and std.mem.eql(u8, entry.token[0..entry.token_length], token)) pending_live = slot;
                }
                for (&spent, 0..) |*entry, slot| {
                    if (entry.live and now - entry.at < TtlMs) spent_count += 1;
                    if (entry.live and now - entry.at < TtlMs and std.mem.eql(u8, entry.token[0..entry.token_length], token)) spent_live = slot;
                }
                if (pending_live != null) expected_error = error.Duplicate else if (spent_live != null) expected_error = error.TokenSpent else if (spent_count == MaxEntries) expected_error = error.SpentRingFull;
            }
            if (expected_error == null) {
                model_started = true;
                if (rolled) {
                    for (&pending) |*entry| entry.live = false;
                    for (&spent) |*entry| entry.live = false;
                    model_rollbacks += 1;
                }
                model_last = now;
                for (&pending) |*entry| {
                    if (entry.live and now - entry.at >= TtlMs) entry.live = false;
                }
                for (&spent) |*entry| {
                    if (entry.live and now - entry.at >= TtlMs) entry.live = false;
                }
                var live_pending: usize = 0;
                for (&pending) |*entry| live_pending += @intFromBool(entry.live);
                if (live_pending == MaxEntries) {
                    var victim: ?*model_t = null;
                    for (&pending) |*entry| {
                        if (entry.live and (victim == null or entry.order < victim.?.order)) victim = entry;
                    }
                    victim.?.live = false;
                }
                for (&pending) |*entry| {
                    if (entry.live) continue;
                    @memcpy(entry.token[0..token.len], token);
                    entry.token_length = token.len;
                    entry.process_id = pid;
                    entry.at = now;
                    model_order += 1;
                    entry.order = model_order;
                    entry.live = true;
                    break;
                }
            }
            const actual = put_at(&store, token, pid, now);
            if (expected_error) |expected| {
                try testing.expectError(expected, actual);
                outcomes[1 + @as(usize, switch (expected) {
                    error.Duplicate => 0,
                    error.TokenSpent => 1,
                    else => 2,
                })] += 1;
            } else {
                try actual;
                outcomes[0] += 1;
            }
        } else if (choice <= 6) {
            // Take: observe the clock, expire, newest live pending for the process, then spend it.
            if (rolled) {
                for (&pending) |*entry| entry.live = false;
                for (&spent) |*entry| entry.live = false;
                model_rollbacks += 1;
            }
            model_started = true;
            model_last = now;
            for (&pending) |*entry| {
                if (entry.live and now - entry.at >= TtlMs) entry.live = false;
            }
            for (&spent) |*entry| {
                if (entry.live and now - entry.at >= TtlMs) entry.live = false;
            }
            var best: ?*model_t = null;
            for (&pending) |*entry| {
                if (entry.live and entry.process_id == pid and (best == null or entry.order > best.?.order)) best = entry;
            }
            const actual = try take_at(&store, pid, now, &out);
            if (best) |chosen| {
                @memcpy(model_out[0..chosen.token_length], chosen.token[0..chosen.token_length]);
                try testing.expectEqual(@as(?usize, chosen.token_length), actual);
                try testing.expectEqualSlices(u8, model_out[0..chosen.token_length], out[0..chosen.token_length]);
                try testing.expectEqual(@as(u8, 0), out[chosen.token_length]);
                var live_spent: usize = 0;
                for (&spent) |*entry| live_spent += @intFromBool(entry.live);
                if (live_spent == MaxEntries) {
                    var victim: ?*model_t = null;
                    for (&spent) |*entry| {
                        if (entry.live and (victim == null or entry.order < victim.?.order)) victim = entry;
                    }
                    victim.?.live = false;
                }
                const moved_token = chosen.token[0..chosen.token_length];
                const moved_pid = chosen.process_id;
                _ = moved_pid;
                chosen.live = false;
                for (&spent) |*entry| {
                    if (entry.live) continue;
                    @memcpy(entry.token[0..moved_token.len], moved_token);
                    entry.token_length = moved_token.len;
                    entry.at = now;
                    model_order += 1;
                    entry.order = model_order;
                    entry.live = true;
                    break;
                }
            } else {
                try testing.expectEqual(@as(?usize, null), actual);
            }
        } else {
            // Expire: observe, then count the live pending entries that lapse.
            if (rolled) {
                for (&pending) |*entry| entry.live = false;
                for (&spent) |*entry| entry.live = false;
                model_rollbacks += 1;
            }
            model_started = true;
            model_last = now;
            var lapsed: usize = 0;
            for (&pending) |*entry| {
                if (entry.live and now - entry.at >= TtlMs) {
                    entry.live = false;
                    lapsed += 1;
                }
            }
            for (&spent) |*entry| {
                if (entry.live and now - entry.at >= TtlMs) entry.live = false;
            }
            try testing.expectEqual(lapsed, expire_at(&store, now));
        }

        // Liveness is judged by elapsed time, as in the store: a refused put does not run
        // expiry, so an expired entry may keep its flag until the next expiring operation.
        var live_pending: usize = 0;
        var live_spent: usize = 0;
        for (&pending) |*entry| live_pending += @intFromBool(entry.live and now - entry.at < TtlMs);
        for (&spent) |*entry| live_spent += @intFromBool(entry.live and now - entry.at < TtlMs);
        try testing.expectEqual(live_pending, live_count(&store.pending, now));
        try testing.expectEqual(live_spent, live_count(&store.spent, now));
        try testing.expectEqual(model_rollbacks, store.rollbacks);
    }
    try testing.expect(model_rollbacks > 0);
    try testing.expect(outcomes[0] > 1000);
    try testing.expect(outcomes[1] > 0);
    try testing.expect(outcomes[2] > 0);
}

test "C ABI put maps Duplicate, TokenSpent and SpentRingFull to statuses 4, 5 and 6" {
    var store: store_t = undefined;
    try testing.expectEqual(@as(c_int, 0), interop_activation_store_init(&store));
    try testing.expectEqual(@as(c_int, 0), c_put(&store, "abc", 3, 11, 100));
    try testing.expectEqual(@as(c_int, 4), c_put(&store, "abc", 3, 12, 100));
    var out: [TokenBufferBytes]u8 = undefined;
    var length: usize = 0;
    try testing.expectEqual(@as(c_int, 0), c_take(&store, 11, 100, &out, &length));
    try testing.expectEqual(@as(c_int, 5), c_put(&store, "abc", 3, 13, 101));
    var name_buffer: [16]u8 = undefined;
    for (1..MaxEntries) |index| {
        const name = try std.fmt.bufPrint(&name_buffer, "f{d}", .{index});
        try testing.expectEqual(@as(c_int, 0), c_put(&store, name.ptr, name.len, @intCast(index), 101));
        try testing.expectEqual(@as(c_int, 0), c_take(&store, @intCast(index), 101, &out, &length));
    }
    try testing.expectEqual(@as(c_int, 6), c_put(&store, "late", 4, 40, 102));
}

test "C ABI put take expire and clear map every status" {
    var store: store_t = undefined;
    try testing.expectEqual(@as(c_int, 0), interop_activation_store_init(opaque_value(?*store_t, &store)));
    try testing.expectEqual(@as(c_int, 1), interop_activation_store_init(absent_store()));
    try testing.expectEqual(@as(c_int, 1), c_put(absent_store(), "x", 1, 1, 0));
    try testing.expectEqual(@as(c_int, 1), c_put(&store, absent_bytes(), 1, 1, 0));
    try testing.expectEqual(@as(c_int, 2), c_put(&store, "", 0, 1, 0));
    try testing.expectEqual(@as(c_int, 3), c_put(&store, "x", 1, 0, 0));
    try testing.expectEqual(@as(c_int, 0), c_put(&store, "abc", 3, 11, 100));
    var out: [TokenBufferBytes]u8 = undefined;
    var length: usize = 0;
    try testing.expectEqual(@as(c_int, 1), c_take(absent_store(), 11, 101, &out, &length));
    try testing.expectEqual(@as(c_int, 1), c_take(&store, 11, 101, absent_out(), &length));
    try testing.expectEqual(@as(c_int, 1), c_take(&store, 11, 101, &out, absent_length()));
    try testing.expectEqual(@as(c_int, 3), c_take(&store, 0, 101, &out, &length));
    try testing.expectEqual(@as(c_int, 2), c_take(&store, 12, 101, &out, &length));
    try testing.expectEqual(@as(c_int, 0), c_take(&store, 11, 101, &out, &length));
    try testing.expectEqualSlices(u8, "abc", out[0..length]);
    try testing.expectEqual(@as(usize, 0), interop_activation_expire(absent_store(), 1));
    try testing.expectEqual(@as(c_int, 0), c_put(&store, "gone", 4, 5, 200));
    try testing.expectEqual(@as(usize, 1), interop_activation_expire(opaque_value(?*store_t, &store), opaque_value(u64, 200 + TtlMs)));
    try testing.expectEqual(@as(c_int, 0), c_put(&store, "clr", 3, 6, 30_300));
    interop_activation_clear(absent_store());
    interop_activation_clear(opaque_value(?*store_t, &store));
    try testing.expectEqual(@as(u32, 0), store.count);
}

test "C ABI pick reports found, none and invalid output arguments" {
    var pointer: [*]const u8 = undefined;
    var length: usize = 0;
    try testing.expectEqual(@as(c_int, 0), interop_activation_pick_token("xdg-1", "startup", &pointer, &length));
    try testing.expectEqualSlices(u8, "xdg-1", pointer[0..length]);
    try testing.expectEqual(@as(c_int, 0), interop_activation_pick_token("bad tok", "startup", &pointer, &length));
    try testing.expectEqualSlices(u8, "startup", pointer[0..length]);
    try testing.expectEqual(@as(c_int, 0), interop_activation_pick_token(absent_string(), "s2", &pointer, &length));
    try testing.expectEqualSlices(u8, "s2", pointer[0..length]);
    try testing.expectEqual(@as(c_int, 1), interop_activation_pick_token(absent_string(), absent_string(), &pointer, &length));
    try testing.expectEqual(@as(c_int, 1), interop_activation_pick_token("", "bad tok", &pointer, &length));
    try testing.expectEqual(@as(c_int, 2), interop_activation_pick_token("x", "y", absent_token_out(), &length));
    try testing.expectEqual(@as(c_int, 2), interop_activation_pick_token("x", "y", &pointer, absent_length()));
    var long: [MaxTokenBytes + 1:0]u8 = [_:0]u8{0} ** (MaxTokenBytes + 1);
    @memset(long[0 .. MaxTokenBytes + 1], 'q');
    try testing.expectEqual(@as(c_int, 1), interop_activation_pick_token(&long, absent_string(), &pointer, &length));
    try testing.expectEqual(@as(c_int, 0), interop_activation_pick_token(&long, "ok", &pointer, &length));
    try testing.expectEqual(@as(usize, 2), length);
}
