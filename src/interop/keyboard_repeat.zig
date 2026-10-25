//! Allocation-free host repeat timing reducer; event-loop/injection owners are external.
const std = @import("std");
/// Maximum admitted evdev numeric code; zero is never a key.
pub const MaxKey: u16 = 767;
/// Nanoseconds per second; integer periods round upward to avoid excess rate.
pub const NanosecondsPerSecond: u64 = 1_000_000_000;
/// Explicit timing errors; invalid configuration preserves the complete state.
pub const error_t = error{ InvalidTiming, InvalidFocus, InvalidKey, ClockDiscontinuity };

/// Caller-owned, zero-initialized state. Never copy between active sessions.
/// All fields are implementation-owned after zero initialization; callers use
/// methods and must not forge state or change fields. All mutation and reads
/// require one serialized owner thread; no allocation.
pub const state_t = struct {
    /// Current focused window identity; zero means no focus.
    window: u64 = 0,
    /// Current focused generation; zero means no focus.
    generation: u32 = 0,
    /// Physical down observations for admitted keys only.
    held: [MaxKey + 1]bool = [_]bool{false} ** (MaxKey + 1),
    /// Configured host repeat enable policy.
    enabled: bool = false,
    /// Requested rate, bounded0..1000Hz.
    rate: u16 = 0,
    /// Requested delay, bounded0..60000ms.
    delay_ms: u16 = 0,
    /// Active repeat candidate, or zero.
    candidate: u16 = 0,
    /// Next due time, meaningful only when candidate is nonzero.
    deadline_ns: u64 = 0,
    /// Last accepted monotonic time; null before the first timed operation.
    last_now: ?u64 = null,

    fn clock(self: *state_t, now: u64) error_t!void {
        if (self.last_now) |prior| if (now < prior) {
            self.candidate = 0;
            self.last_now = now;
            return error.ClockDiscontinuity;
        };
        self.last_now = now;
    }
    fn schedule(self: *state_t, now: u64, delta: u64) error_t!void {
        self.deadline_ns = std.math.add(u64, now, delta) catch {
            self.candidate = 0;
            return error.ClockDiscontinuity;
        };
    }
    /// Set trusted timing scalars, borrowing no memory. InvalidTiming preserves
    /// state; successful reconfiguration disarms repeat until a fresh down.
    /// No value returned; caller serializes and keeps ownership of self.
    pub fn configure(self: *state_t, enabled: bool, rate: i32, delay_ms: i32) error_t!void {
        if (rate < 0 or rate > 1000 or delay_ms < 0 or delay_ms > 60000) return error.InvalidTiming;
        self.enabled = enabled;
        self.rate = @intCast(rate);
        self.delay_ms = @intCast(delay_ms);
        self.candidate = 0;
    }
    /// Replace focus and discard all inherited held keys/candidate. Both IDs
    /// must be nonzero; invalid inputs preserve state. Borrows no memory.
    pub fn focus(self: *state_t, window: u64, generation: u32) error_t!void {
        if (window == 0 or generation == 0) return error.InvalidFocus;
        self.cancel();
        self.window = window;
        self.generation = generation;
    }
    /// Cancel on focus loss, disconnect, or failed delivery. Clears all held
    /// observations and identity; retains configured timing. Idempotent, no error.
    pub fn cancel(self: *state_t) void {
        self.window = 0;
        self.generation = 0;
        @memset(&self.held, false);
        self.candidate = 0;
        self.deadline_ns = 0;
        self.last_now = null;
    }
    /// Observe one already-admitted physical event. Numeric key must be1..767;
    /// repeatable comes from trusted translation/keymap policy. Inputs are by
    /// value, no ownership transfer. InvalidKey preserves state; clock errors
    /// disarm repeat. No focus means no held/candidate mutation after clock check.
    pub fn key(self: *state_t, code: u16, pressed: bool, repeatable: bool, now: u64) error_t!void {
        if (code == 0 or code > MaxKey) return error.InvalidKey;
        try self.clock(now);
        if (self.window == 0) return;
        if (!pressed) {
            self.held[code] = false;
            if (self.candidate == code) self.candidate = 0;
            return;
        }
        if (self.held[code]) return;
        self.held[code] = true;
        self.candidate = 0;
        if (!self.enabled or self.rate == 0 or !repeatable or !policy_repeatable(code)) return;
        self.candidate = code;
        try self.schedule(now, @as(u64, self.delay_ms) * 1_000_000);
    }
    /// Consume at most one due repeat, returned by value. Null means none due.
    /// No native input is injected. Caller queues pressed=true or cancels focus
    /// on failure; consumed repeat is never retried. Clock errors disarm state.
    pub fn take_due(self: *state_t, now: u64) error_t!?u16 {
        try self.clock(now);
        if (self.candidate == 0 or now < self.deadline_ns) return null;
        const code = self.candidate;
        const period = (NanosecondsPerSecond + self.rate - 1) / self.rate;
        try self.schedule(now, period);
        return code;
    }
    /// Return the borrowed scalar deadline, or null if no candidate is active.
    /// Does not mutate state; caller serializes reads with mutation.
    pub fn next_deadline(self: *const state_t) ?u64 {
        return if (self.candidate == 0) null else self.deadline_ns;
    }
};

/// Host repeat exclusions (IMPL_DESC 3.6.3): modifiers, lock keys, Pause, Hangul and
/// Hanja (make-only on real keyboards), and the suppressed power and sleep keys.
/// Mirrored in av_input.c NonRepeatKeys; keyboard_repeat_abi exports it for the parity test.
pub fn policy_repeatable(code: u16) bool {
    return switch (code) {
        29, 42, 54, 56, 58, 69, 70, 97, 100, 116, 119, 122, 123, 125, 126, 142 => false,
        else => true,
    };
}

test "repeat obeys delay period skip policy and physical key lifecycle" {
    var state = state_t{};
    try state.configure(true, 3, 100);
    try state.focus(10, 2);
    try state.key(30, true, true, 10);
    try std.testing.expectEqual(@as(?u64, 100_000_010), state.next_deadline());
    try std.testing.expectEqual(@as(?u16, null), try state.take_due(100_000_009));
    try std.testing.expectEqual(@as(?u16, 30), try state.take_due(100_000_010));
    try std.testing.expectEqual(@as(?u64, 433_333_344), state.next_deadline());
    try state.key(30, true, true, 100_000_011);
    try std.testing.expectEqual(@as(?u64, 433_333_344), state.next_deadline());
    try state.key(31, false, true, 100_000_012);
    try std.testing.expectEqual(@as(?u16, 30), try state.take_due(2_000_000_000));
    try std.testing.expectEqual(@as(?u16, null), try state.take_due(2_000_000_000));
    try state.key(30, false, true, 2_000_000_001);
    try std.testing.expectEqual(@as(?u64, null), state.next_deadline());
    try state.key(31, true, true, 2_000_000_002);
    try state.key(32, true, false, 2_000_000_003);
    try std.testing.expectEqual(@as(?u64, null), state.next_deadline());
    try state.key(32, false, true, 2_000_000_004);
    try std.testing.expectEqual(@as(?u64, null), state.next_deadline());
    state.cancel();
    state.cancel();
    try std.testing.expect(!state.held[31]);
}

test "excluded keys timing configuration and focus cannot start stale repeat" {
    var state = state_t{};
    try state.key(30, true, true, 0);
    try std.testing.expect(!state.held[30]);
    try state.configure(false, 100, 0);
    try state.focus(1, 1);
    try state.key(30, true, true, 0);
    try state.configure(true, 0, 0);
    try state.key(31, true, true, 0);
    try state.configure(true, 1000, 0);
    try std.testing.expectEqual(@as(?u16, null), try state.take_due(0));
    for ([_]u16{ 29, 42, 54, 56, 58, 69, 70, 97, 100, 116, 119, 122, 123, 125, 126, 142 }) |code| {
        try state.key(code, true, true, 0);
        try std.testing.expectEqual(@as(?u64, null), state.next_deadline());
    }
    try state.key(40, true, true, 0);
    try std.testing.expectEqual(@as(?u16, 40), try state.take_due(0));
    try state.focus(2, 3);
    try std.testing.expect(!state.held[40]);
    try std.testing.expectEqual(@as(?u64, null), state.next_deadline());
    const before = state;
    for ([_][2]i32{ .{ -1, 0 }, .{ 1001, 0 }, .{ 1, -1 }, .{ 1, 60001 } }) |pair|
        try std.testing.expectError(error.InvalidTiming, state.configure(true, pair[0], pair[1]));
    try std.testing.expectError(error.InvalidFocus, state.focus(0, 1));
    try std.testing.expectError(error.InvalidFocus, state.focus(1, 0));
    try std.testing.expectError(error.InvalidKey, state.key(0, true, true, 0));
    try std.testing.expectError(error.InvalidKey, state.key(768, true, true, 0));
    try std.testing.expectEqualDeep(before, state);
}

test "clock rollback and deadline overflow disarm without phantom repeats" {
    var state = state_t{};
    try state.configure(true, 1, 60000);
    try state.focus(1, 1);
    try state.key(30, true, true, 100);
    try std.testing.expectError(error.ClockDiscontinuity, state.take_due(99));
    try std.testing.expectEqual(@as(?u64, null), state.next_deadline());
    try state.key(30, false, true, 100);
    try std.testing.expectError(error.ClockDiscontinuity, state.key(30, true, true, std.math.maxInt(u64)));
    try std.testing.expectEqual(@as(?u64, null), state.next_deadline());
    state.cancel();
    try state.configure(true, 1000, 0);
    try state.focus(2, 1);
    try state.key(31, true, true, std.math.maxInt(u64));
    try std.testing.expectError(error.ClockDiscontinuity, state.take_due(std.math.maxInt(u64)));
    try std.testing.expectEqual(@as(?u64, null), state.next_deadline());
}

test "long deterministic repeat lifecycle stays bounded" {
    var state = state_t{};
    try state.configure(true, 25, 2);
    var now: u64 = 0;
    for (0..2000) |index| {
        try state.focus(@intCast(index + 1), 1);
        try state.key(30, true, true, now);
        now += 2_000_000;
        try std.testing.expectEqual(@as(?u16, 30), try state.take_due(now));
        try state.key(30, false, true, now);
        now += 1;
        try std.testing.expectEqual(@as(?u16, null), try state.take_due(now));
    }
}

test "runtime event corpus covers changing press values and clock rejection" {
    var state = state_t{};
    try state.configure(true, 30, 4);
    try state.focus(1, 1);
    var random_state = std.Random.DefaultPrng.init(0x726570656174);
    const random = random_state.random();
    for (0..3000) |index| {
        const code = random.uintLessThan(u16, MaxKey + 2);
        const pressed = random.boolean();
        state.key(code, pressed, random.boolean(), @intCast(index)) catch |err| {
            try std.testing.expectEqual(error.InvalidKey, err);
        };
    }
    try std.testing.expectError(error.ClockDiscontinuity, state.key(30, false, true, 0));
    try std.testing.expectEqual(@as(?u64, null), state.next_deadline());
}
