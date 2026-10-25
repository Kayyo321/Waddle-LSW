//! C ABI for the host key-repeat reducer. Caller-owned opaque storage; no allocation.
//! The reducer's policy and timing rules are documented in keyboard_repeat.zig and
//! keyboard_repeat_contract.md. This file only maps C arguments and status codes.
const std = @import("std");
const repeat = @import("keyboard_repeat.zig");

/// Bytes of caller storage required by one reducer. Mirrors WaddleKeyboardRepeatStorageBytes.
pub const StorageBytes: usize = 1024;

comptime {
    if (@sizeOf(repeat.state_t) > StorageBytes) @compileError("keyboard repeat state exceeds ABI storage");
    if (@alignOf(repeat.state_t) > 8) @compileError("keyboard repeat state alignment exceeds the ABI guarantee");
}

/// C ABI status. Ok is 0; every other value names one rejection. No state is
/// changed when a non-Ok status is returned.
pub const status_t = enum(c_int) {
    /// Operation succeeded; output arguments were written.
    Ok = 0,
    /// A required pointer argument was NULL.
    NullArgument = 1,
    /// A flag argument (enabled, pressed, repeatable) was neither 0 nor 1.
    InvalidFlag = 2,
    /// Rate outside 0..1000 or delay outside 0..60000 milliseconds.
    InvalidTiming = 3,
    /// Window or generation is zero.
    InvalidFocus = 4,
    /// Key code is zero or above 767.
    InvalidKey = 5,
    /// The monotonic clock moved backward or a deadline overflowed; repeat was disarmed.
    ClockDiscontinuity = 6,
};

noinline fn map_error(err: anyerror) status_t {
    return switch (err) {
        error.InvalidTiming => .InvalidTiming,
        error.InvalidFocus => .InvalidFocus,
        error.InvalidKey => .InvalidKey,
        error.ClockDiscontinuity => .ClockDiscontinuity,
        else => unreachable,
    };
}

noinline fn state_of(storage: ?*anyopaque) ?*repeat.state_t {
    const raw = storage orelse return null;
    return @ptrCast(@alignCast(raw));
}

noinline fn flag(value: u8) ?bool {
    return switch (value) {
        0 => false,
        1 => true,
        else => null,
    };
}

/// C ABI: initialize caller storage to the reducer's zero state (disabled, unfocused, no candidate).
/// `storage` is out, must be non-NULL, 8-byte aligned, and at least StorageBytes long.
/// Thread safety: caller serializes calls on one storage.
export fn waddle_keyboard_repeat_init(storage: ?*anyopaque) callconv(.C) status_t {
    const state = state_of(storage) orelse return .NullArgument;
    state.* = .{};
    return .Ok;
}

/// C ABI: set trusted timing. `enabled` is in and must be 0 or 1. Rate is 0..1000 Hz;
/// delay is 0..60000 ms. A successful call disarms any active candidate.
/// Thread safety: caller serializes calls on one storage.
export fn waddle_keyboard_repeat_configure(storage: ?*anyopaque, enabled: u8, rate: i32, delay_ms: i32) callconv(.C) status_t {
    const state = state_of(storage) orelse return .NullArgument;
    const on = flag(enabled) orelse return .InvalidFlag;
    state.configure(on, rate, delay_ms) catch |err| return map_error(err);
    return .Ok;
}

/// C ABI: replace focus with (window, generation) and discard inherited held keys.
/// Both values must be nonzero. Thread safety: caller serializes calls on one storage.
export fn waddle_keyboard_repeat_focus(storage: ?*anyopaque, window: u64, generation: u32) callconv(.C) status_t {
    const state = state_of(storage) orelse return .NullArgument;
    state.focus(window, generation) catch |err| return map_error(err);
    return .Ok;
}

/// C ABI: cancel repeat, clear focus and held observations, keep configured timing.
/// Idempotent. Call on focus loss, disconnect, failed delivery, or lease revocation.
/// Thread safety: caller serializes calls on one storage.
export fn waddle_keyboard_repeat_cancel(storage: ?*anyopaque) callconv(.C) status_t {
    const state = state_of(storage) orelse return .NullArgument;
    state.cancel();
    return .Ok;
}

/// C ABI: observe one physical key event at monotonic time `now_ns`.
/// `code` is the evdev code (1..767). `pressed` and `repeatable` must each be 0 or 1.
/// `repeatable` comes from trusted keymap policy; the reducer also excludes modifier,
/// lock, Pause, and suppressed keys by itself.
/// Thread safety: caller serializes calls on one storage.
export fn waddle_keyboard_repeat_key(storage: ?*anyopaque, code: u16, pressed: u8, repeatable: u8, now_ns: u64) callconv(.C) status_t {
    const state = state_of(storage) orelse return .NullArgument;
    const down = flag(pressed) orelse return .InvalidFlag;
    const can_repeat = flag(repeatable) orelse return .InvalidFlag;
    state.key(code, down, can_repeat, now_ns) catch |err| return map_error(err);
    return .Ok;
}

/// C ABI: consume at most one due repeat. `out_code` is out and non-NULL; it receives
/// the evdev code to send as a key down, or 0 when no repeat is due. A returned repeat
/// is consumed and must be queued by the caller; it is never retried.
/// Thread safety: caller serializes calls on one storage.
export fn waddle_keyboard_repeat_take_due(storage: ?*anyopaque, now_ns: u64, out_code: ?*u16) callconv(.C) status_t {
    const state = state_of(storage) orelse return .NullArgument;
    const code_out = out_code orelse return .NullArgument;
    const due = state.take_due(now_ns) catch |err| return map_error(err);
    code_out.* = due orelse 0;
    return .Ok;
}

/// C ABI: report the next repeat deadline. `out_present` is set to 1 with `out_deadline`
/// written when a repeat is armed, and to 0 otherwise. Both pointers are out, non-NULL.
/// Does not mutate state. Thread safety: caller serializes reads with mutation.
export fn waddle_keyboard_repeat_next_deadline(storage: ?*anyopaque, out_present: ?*u8, out_deadline: ?*u64) callconv(.C) status_t {
    const state = state_of(storage) orelse return .NullArgument;
    const present = out_present orelse return .NullArgument;
    const deadline = out_deadline orelse return .NullArgument;
    if (state.next_deadline()) |value| {
        present.* = 1;
        deadline.* = value;
    } else {
        present.* = 0;
        deadline.* = 0;
    }
    return .Ok;
}

test "storage holds the reducer state and the zero bytes are the initial state" {
    try std.testing.expect(@sizeOf(repeat.state_t) <= StorageBytes);
    var storage = [_]u64{0} ** (StorageBytes / 8);
    const zero_state = state_of(@ptrCast(&storage)) orelse return error.TestUnexpectedResult;
    try std.testing.expect(zero_state.next_deadline() == null);
    try std.testing.expect(zero_state.last_now == null);
}

test "every reducer error maps to its status" {
    try std.testing.expectEqual(status_t.InvalidTiming, map_error(error.InvalidTiming));
    try std.testing.expectEqual(status_t.InvalidFocus, map_error(error.InvalidFocus));
    try std.testing.expectEqual(status_t.InvalidKey, map_error(error.InvalidKey));
    try std.testing.expectEqual(status_t.ClockDiscontinuity, map_error(error.ClockDiscontinuity));
}

test "ABI rejects NULL storage and invalid flags without changing state" {
    var storage = [_]u64{0} ** (StorageBytes / 8);
    const raw: *anyopaque = rt(*anyopaque, @ptrCast(&storage));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_init(raw));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_repeat_init(rt(?*anyopaque, null)));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_repeat_configure(rt(?*anyopaque, null), 1, 30, 100));
    try std.testing.expectEqual(status_t.InvalidFlag, waddle_keyboard_repeat_configure(raw, rt(u8, 2), rt(i32, 30), rt(i32, 100)));
    try std.testing.expectEqual(status_t.InvalidTiming, waddle_keyboard_repeat_configure(raw, 1, -1, 100));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_configure(raw, rt(u8, 1), rt(i32, 30), rt(i32, 100)));
    try std.testing.expectEqual(status_t.InvalidFocus, waddle_keyboard_repeat_focus(raw, 0, 1));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_focus(raw, 1, 1));
    try std.testing.expectEqual(status_t.InvalidKey, waddle_keyboard_repeat_key(raw, 0, 1, 1, 0));
    try std.testing.expectEqual(status_t.InvalidFlag, waddle_keyboard_repeat_key(raw, 30, 3, 1, 0));
    try std.testing.expectEqual(status_t.InvalidFlag, waddle_keyboard_repeat_key(raw, 30, 1, 9, 0));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_key(raw, 30, 1, 1, 100));

    var present: u8 = 9;
    var deadline: u64 = 9;
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_next_deadline(raw, rt(?*u8, &present), rt(?*u64, &deadline)));
    try std.testing.expectEqual(@as(u8, 1), present);
    try std.testing.expectEqual(@as(u64, 100 + 100_000_000), deadline);
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_repeat_next_deadline(raw, rt(?*u8, null), &deadline));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_repeat_next_deadline(raw, &present, rt(?*u64, null)));

    var code: u16 = 7;
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_repeat_take_due(raw, 0, rt(?*u16, null)));
    try std.testing.expectEqual(status_t.ClockDiscontinuity, waddle_keyboard_repeat_take_due(raw, 50, &code));
    try std.testing.expectEqual(@as(u16, 7), code);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_cancel(rt(?*anyopaque, raw)));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_cancel(rt(?*anyopaque, raw)));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_repeat_cancel(rt(?*anyopaque, null)));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_repeat_key(rt(?*anyopaque, null), 30, 1, 1, 0));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_repeat_focus(rt(?*anyopaque, null), 1, 1));
}

test "ABI repeat delivers the first repeat at delay and then one per period" {
    var storage = [_]u64{0} ** (StorageBytes / 8);
    const raw: *anyopaque = rt(*anyopaque, @ptrCast(&storage));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_init(raw));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_configure(raw, rt(u8, 1), rt(i32, 4), rt(i32, 20)));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_focus(raw, 5, 2));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_key(raw, 30, 1, 1, 1_000_000_000));
    var code: u16 = 0;
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_take_due(raw, 1_019_999_999, &code));
    try std.testing.expectEqual(@as(u16, 0), code);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_take_due(raw, 1_020_000_000, &code));
    try std.testing.expectEqual(@as(u16, 30), code);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_take_due(raw, 1_020_000_001, &code));
    try std.testing.expectEqual(@as(u16, 0), code);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_key(raw, 30, 0, 1, 1_100_000_000));
    var present: u8 = 1;
    var deadline: u64 = 0;
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_repeat_next_deadline(raw, rt(?*u8, &present), rt(?*u64, &deadline)));
    try std.testing.expectEqual(@as(u8, 0), present);
}

/// Launders a value through memory the optimizer cannot see, so every error and
/// null path stays a live runtime branch instead of being folded out of the test.
fn rt(comptime T: type, value: T) T {
    var copy = value;
    std.mem.doNotOptimizeAway(&copy);
    return copy;
}
