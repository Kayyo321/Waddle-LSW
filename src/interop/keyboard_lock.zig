//! Keyboard lock-state reducers: host lock mask and send decision, guest toggle plan.
//! Owner-thread state only. No allocation, OS calls, timers, or wire parsing.
const std = @import("std");

/// Caps Lock bit of the three-bit lock mask (protocol tag Locks 0x0021, bit 0).
pub const LockCaps: u8 = 1 << 0;
/// Num Lock bit of the lock mask (bit 1).
pub const LockNum: u8 = 1 << 1;
/// Scroll Lock bit of the lock mask (bit 2).
pub const LockScroll: u8 = 1 << 2;
/// Union of all admitted lock bits; a mask with any other bit is invalid.
pub const LockAll: u8 = LockCaps | LockNum | LockScroll;
/// Highest XKB modifier index accepted from a keymap; indices are 0 through 31.
pub const MaxModifierIndex: u8 = 31;
/// Sentinel index meaning the active keymap defines no such lock modifier.
pub const ModifierAbsent: u8 = 0xFF;
/// Windows virtual-key code of Caps Lock (VK_CAPITAL).
pub const VkCapital: u16 = 0x14;
/// Windows virtual-key code of Num Lock (VK_NUMLOCK).
pub const VkNumlock: u16 = 0x90;
/// Windows virtual-key code of Scroll Lock (VK_SCROLL).
pub const VkScroll: u16 = 0x91;
/// Upper bound on toggle actions: three lock keys, one press and one release each.
pub const MaxToggleActions: usize = 6;
/// Half of the 32-bit serial space; a forward distance at or above this is older.
pub const HalfSerialSpace: u32 = 0x8000_0000;

/// Explicit rejections. No state is mutated when any of these is returned.
/// InvalidIndex: index is neither ModifierAbsent nor in 0 through 31.
/// InvalidMask: a lock mask has a bit outside LockAll.
/// InvalidSerial: protocol serial is zero (a value the codec also rejects).
/// InvalidEpoch: lease epoch, or the guest's current epoch, is zero (the codec rejects zero).
/// EpochMismatch: the update's lease epoch differs from the guest's current epoch,
/// in either direction. Such an update belongs to another lease and is never applied.
pub const error_t = error{ InvalidIndex, InvalidMask, InvalidSerial, InvalidEpoch, EpochMismatch };

/// Wayland wl_keyboard.modifiers masks, copied by value. Only `locked` selects
/// lock bits; `depressed` and `latched` are carried for diagnostics only.
pub const modifiers_t = extern struct {
    /// Physically held modifier bits. Never selects a lock bit.
    depressed: u32,
    /// One-shot latched modifier bits. Never selects a lock bit.
    latched: u32,
    /// Locked modifier bits; bit n set means XKB modifier index n is locked.
    locked: u32,
};

/// Keymap modifier indices for the three lock modifiers. Each value is either
/// ModifierAbsent or in 0 through MaxModifierIndex. Copied by value.
pub const indices_t = extern struct {
    /// XKB index of the Caps Lock modifier, or ModifierAbsent.
    caps: u8,
    /// XKB index of the Num Lock modifier (Mod2 in common keymaps), or ModifierAbsent.
    num: u8,
    /// XKB index of the Scroll Lock modifier, or ModifierAbsent.
    scroll: u8,
};

/// Host owner state for one wl_keyboard. Zero is the valid initial value
/// (unfocused). Owned by the single host event-loop thread; never copied while
/// in use. No allocation; no field is freed.
pub const host_state_t = extern struct {
    /// True between a focus enter and the next leave.
    focused: bool,
    /// Lock mask last sent to the guest for the current focus.
    last_mask: u8,
};

/// Host decision for one observation. When `send` is true the caller queues
/// exactly one InputLockState carrying `mask`; otherwise it queues nothing.
pub const host_decision_t = extern struct {
    /// True when an InputLockState must be queued.
    send: bool,
    /// Lock mask computed from this observation, valid even when `send` is false.
    mask: u8,
};

/// Guest owner state for accepted InputLockState updates. Zero is the valid
/// initial value (no update accepted yet). Owned by the guest owner thread.
pub const guest_state_t = extern struct {
    /// True once the first update has been accepted.
    accepted: bool,
    /// Lease epoch of the last accepted update; meaningful only when `accepted`.
    epoch: u64,
    /// Protocol serial of the last accepted update; meaningful only when `accepted`.
    serial: u32,
};

/// One guest VK toggle action. Each lock key yields a press then a release.
pub const toggle_action_t = extern struct {
    /// Windows virtual-key code (VkCapital, VkNumlock or VkScroll).
    vk: u16,
    /// True for the press, false for the release.
    pressed: bool,
};

/// Ordered toggle actions. Entries 0 through `count` - 1 are valid; the rest are zero.
pub const toggle_plan_t = extern struct {
    /// Number of valid entries in `actions`; 0 through MaxToggleActions.
    count: u8,
    /// Actions in injection order.
    actions: [MaxToggleActions]toggle_action_t,
};

/// Guest result for one InputLockState. When `applied` is false the update was
/// stale and the plan is empty; state was not changed.
pub const update_t = extern struct {
    /// True when the update was newer than the last accepted update.
    applied: bool,
    /// Toggle actions to inject before forwarding any other key.
    plan: toggle_plan_t,
};

/// C ABI status. Ok is 0; every other value names one rejection in error_t.
pub const status_t = enum(c_int) {
    /// Operation succeeded.
    Ok = 0,
    /// A required pointer argument was NULL.
    NullArgument = 1,
    /// A modifier index is outside 0 through 31 and is not ModifierAbsent.
    InvalidIndex = 2,
    /// A lock mask has a bit outside LockAll.
    InvalidMask = 3,
    /// The protocol serial is zero.
    InvalidSerial = 4,
    /// The lease epoch, or the guest's current epoch, is zero.
    InvalidEpoch = 5,
    /// The update's lease epoch differs from the guest's current epoch.
    EpochMismatch = 6,
};

const toggle_entry_t = struct { bit: u8, vk: u16 };
const toggle_table = [_]toggle_entry_t{
    .{ .bit = LockCaps, .vk = VkCapital },
    .{ .bit = LockNum, .vk = VkNumlock },
    .{ .bit = LockScroll, .vk = VkScroll },
};

/// Derive the three-bit lock mask from the locked modifier bits.
/// Bit 0 is Caps, bit 1 Num, bit 2 Scroll. An absent index contributes zero.
/// Parameters are by value (in); no ownership transfer.
/// Returns the lock mask, a subset of LockAll.
/// Errors: InvalidIndex when any index is neither ModifierAbsent nor 0..31.
/// Thread safety: pure function, callable from any thread.
pub noinline fn lock_mask(mods: modifiers_t, indices: indices_t) error_t!u8 {
    try validate_index(indices.caps);
    try validate_index(indices.num);
    try validate_index(indices.scroll);
    var mask: u8 = 0;
    if (locked_bit(mods.locked, indices.caps)) mask |= LockCaps;
    if (locked_bit(mods.locked, indices.num)) mask |= LockNum;
    if (locked_bit(mods.locked, indices.scroll)) mask |= LockScroll;
    return mask;
}

/// Host focus enter. Always decides to send one InputLockState, because every
/// wl_keyboard enter requires a fresh guest sync. Marks the state focused and
/// records the mask as sent.
/// `state` is in/out and owner-serialized; `mods` and `indices` are in by value.
/// Returns the decision with send=true.
/// Errors: InvalidIndex; state is not mutated on error.
/// Thread safety: caller serializes all calls on one state.
pub noinline fn host_enter(state: *host_state_t, mods: modifiers_t, indices: indices_t) error_t!host_decision_t {
    const mask = try lock_mask(mods, indices);
    state.focused = true;
    state.last_mask = mask;
    return .{ .send = true, .mask = mask };
}

/// Host modifier observation. Sends only on change while focused, so an
/// unchanged mask is never duplicated. While unfocused nothing is sent and the
/// recorded mask is left unchanged.
/// `state` is in/out; `mods` and `indices` are in by value.
/// Returns the decision; send=true means exactly one InputLockState is due.
/// Errors: InvalidIndex; state is not mutated on error, and nothing is sent.
/// Thread safety: caller serializes all calls on one state.
pub noinline fn host_observe(state: *host_state_t, mods: modifiers_t, indices: indices_t) error_t!host_decision_t {
    const mask = try lock_mask(mods, indices);
    if (!state.focused or mask == state.last_mask) return .{ .send = false, .mask = mask };
    state.last_mask = mask;
    return .{ .send = true, .mask = mask };
}

/// Host focus leave. Clears focus so later observations send nothing.
/// `state` is in/out. No error. Thread safety: caller serializes.
pub noinline fn host_leave(state: *host_state_t) void {
    state.focused = false;
}

/// Wrap-safe serial ordering (RFC 1982 style). `candidate` is newer than `last`
/// when its forward distance (candidate - last mod 2^32) lies in 1 through
/// HalfSerialSpace - 1. Equal values are never newer. Distances at or above
/// HalfSerialSpace are treated as older, so a jump of exactly 2^31 is stale.
/// Parameters are by value. Pure function.
pub fn serial_newer(candidate: u32, last: u32) bool {
    return candidate != last and (candidate -% last) < HalfSerialSpace;
}

/// Build the ordered VK toggle plan that turns the current guest toggle state
/// into the desired lock mask. Keys are emitted in bit order (Caps, Num,
/// Scroll), each as press then release. Differing bits only; equal masks give
/// an empty plan.
/// Parameters are by value. `current` is the GetKeyState-derived toggle bits.
/// Returns the plan with count of 0, 2, 4 or 6.
/// Errors: InvalidMask when either mask has a bit outside LockAll.
/// Thread safety: pure function.
pub noinline fn plan_toggles(desired: u8, current: u8) error_t!toggle_plan_t {
    try validate_mask(desired);
    try validate_mask(current);
    var plan = empty_plan();
    const differing = desired ^ current;
    for (toggle_table) |entry| {
        if ((differing & entry.bit) == 0) continue;
        append(&plan, entry.vk, true);
        append(&plan, entry.vk, false);
    }
    return plan;
}

/// Guest handling of one InputLockState for the guest's live lease.
/// Validation order: zero epochs, zero serial, epoch mismatch, masks. Only then the
/// serial gate applies. An update is accepted only when its lease epoch equals
/// `current_epoch`, in either direction. Within that epoch, an update whose serial
/// is not wrap-newer than the last accepted serial is stale: applied = false, empty
/// plan, state unchanged. A change of epoch starts a fresh serial order, because
/// the last accepted serial belonged to an older lease.
/// `state` is in/out and owner-serialized; scalars are by value.
/// Returns the update result.
/// Errors: InvalidEpoch (either epoch zero), InvalidSerial, EpochMismatch,
/// InvalidMask. State is not mutated on error.
/// Thread safety: caller serializes all calls on one state.
pub noinline fn guest_update(state: *guest_state_t, current_epoch: u64, epoch: u64, serial: u32, desired: u8, current: u8) error_t!update_t {
    if (current_epoch == 0 or epoch == 0) return error.InvalidEpoch;
    if (serial == 0) return error.InvalidSerial;
    if (epoch != current_epoch) return error.EpochMismatch;
    const plan = try plan_toggles(desired, current);
    if (state.accepted and state.epoch == epoch and !serial_newer(serial, state.serial)) {
        return .{ .applied = false, .plan = empty_plan() };
    }
    state.* = .{ .accepted = true, .epoch = epoch, .serial = serial };
    return .{ .applied = true, .plan = plan };
}

noinline fn validate_index(index: u8) error_t!void {
    if (index != ModifierAbsent and index > MaxModifierIndex) return error.InvalidIndex;
}

noinline fn validate_mask(mask: u8) error_t!void {
    if ((mask & ~LockAll) != 0) return error.InvalidMask;
}

noinline fn locked_bit(locked: u32, index: u8) bool {
    if (index == ModifierAbsent) return false;
    return ((locked >> @as(u5, @intCast(index))) & 1) == 1;
}

noinline fn empty_plan() toggle_plan_t {
    return std.mem.zeroes(toggle_plan_t);
}

noinline fn append(plan: *toggle_plan_t, vk: u16, pressed: bool) void {
    plan.actions[plan.count] = .{ .vk = vk, .pressed = pressed };
    plan.count += 1;
}

noinline fn status_of(err: error_t) status_t {
    return switch (err) {
        error.InvalidIndex => .InvalidIndex,
        error.InvalidMask => .InvalidMask,
        error.InvalidSerial => .InvalidSerial,
        error.InvalidEpoch => .InvalidEpoch,
        error.EpochMismatch => .EpochMismatch,
    };
}

/// C ABI: compute the lock mask. All pointers are in; `out` is out.
/// Returns Ok, NullArgument, or InvalidIndex. Writes `out` only on Ok.
/// Thread safety: pure; any thread.
export fn waddle_keyboard_lock_mask(mods: ?*const modifiers_t, indices: ?*const indices_t, out: ?*u8) callconv(.C) status_t {
    const m = mods orelse return .NullArgument;
    const i = indices orelse return .NullArgument;
    const o = out orelse return .NullArgument;
    o.* = lock_mask(m.*, i.*) catch |err| return status_of(err);
    return .Ok;
}

/// C ABI: host focus enter. `state` is inout; `mods`/`indices` are in; `out` is out.
/// Returns Ok, NullArgument, or InvalidIndex. State and `out` change only on Ok.
/// Thread safety: caller serializes calls on one state.
export fn waddle_keyboard_lock_host_enter(state: ?*host_state_t, mods: ?*const modifiers_t, indices: ?*const indices_t, out: ?*host_decision_t) callconv(.C) status_t {
    const s = state orelse return .NullArgument;
    const m = mods orelse return .NullArgument;
    const i = indices orelse return .NullArgument;
    const o = out orelse return .NullArgument;
    o.* = host_enter(s, m.*, i.*) catch |err| return status_of(err);
    return .Ok;
}

/// C ABI: host modifier observation. `state` is inout; `mods`/`indices` are in;
/// `out` is out. Returns Ok, NullArgument, or InvalidIndex. Writes state and
/// `out` only on Ok.
/// Thread safety: caller serializes calls on one state.
export fn waddle_keyboard_lock_host_observe(state: ?*host_state_t, mods: ?*const modifiers_t, indices: ?*const indices_t, out: ?*host_decision_t) callconv(.C) status_t {
    const s = state orelse return .NullArgument;
    const m = mods orelse return .NullArgument;
    const i = indices orelse return .NullArgument;
    const o = out orelse return .NullArgument;
    o.* = host_observe(s, m.*, i.*) catch |err| return status_of(err);
    return .Ok;
}

/// C ABI: host focus leave. `state` is inout. Returns Ok or NullArgument.
/// Thread safety: caller serializes calls on one state.
export fn waddle_keyboard_lock_host_leave(state: ?*host_state_t) callconv(.C) status_t {
    const s = state orelse return .NullArgument;
    host_leave(s);
    return .Ok;
}

/// C ABI: guest InputLockState handling. `state` is inout; `current_epoch`,
/// `epoch`, `serial`, `desired`, `current` are in; `out` is out. Returns Ok,
/// NullArgument, InvalidEpoch, InvalidSerial, EpochMismatch, or InvalidMask.
/// State and `out` change only on Ok.
/// Thread safety: caller serializes calls on one state.
export fn waddle_keyboard_lock_guest_update(state: ?*guest_state_t, current_epoch: u64, epoch: u64, serial: u32, desired: u8, current: u8, out: ?*update_t) callconv(.C) status_t {
    const s = state orelse return .NullArgument;
    const o = out orelse return .NullArgument;
    o.* = guest_update(s, current_epoch, epoch, serial, desired, current) catch |err| return status_of(err);
    return .Ok;
}

/// C ABI: build the toggle plan without guest state. `out` is out.
/// Returns Ok, NullArgument, or InvalidMask. Writes `out` only on Ok.
/// Thread safety: pure; any thread.
export fn waddle_keyboard_lock_plan_toggles(desired: u8, current: u8, out: ?*toggle_plan_t) callconv(.C) status_t {
    const o = out orelse return .NullArgument;
    o.* = plan_toggles(desired, current) catch |err| return status_of(err);
    return .Ok;
}

test "lock mask reads only locked bits at admitted indices" {
    const mods = modifiers_t{ .depressed = 1 << 0, .latched = 1 << 1, .locked = (1 << 1) | (1 << 3) | (1 << 16) };
    const by_num = indices_t{ .caps = 0, .num = 3, .scroll = ModifierAbsent };
    try std.testing.expectEqual(@as(u8, LockNum), try lock_mask(mods, by_num));
    const all = indices_t{ .caps = 16, .num = 3, .scroll = 1 };
    try std.testing.expectEqual(@as(u8, LockAll), try lock_mask(mods, all));
    const boundary = indices_t{ .caps = MaxModifierIndex, .num = ModifierAbsent, .scroll = 0 };
    const high = modifiers_t{ .depressed = 0, .latched = 0, .locked = 1 << 31 };
    try std.testing.expectEqual(@as(u8, LockCaps), try lock_mask(high, boundary));
    const absent = indices_t{ .caps = ModifierAbsent, .num = ModifierAbsent, .scroll = ModifierAbsent };
    try std.testing.expectEqual(@as(u8, 0), try lock_mask(.{ .depressed = 0xFFFF_FFFF, .latched = 0xFFFF_FFFF, .locked = 0xFFFF_FFFF }, absent));
    try std.testing.expectEqual(@as(u8, LockAll), try lock_mask(.{ .depressed = 0, .latched = 0, .locked = 0b111 }, .{ .caps = 0, .num = 1, .scroll = 2 }));
}

test "lock mask rejects indices outside 0 through 31 at every position" {
    const mods = rt(modifiers_t, .{ .depressed = 0, .latched = 0, .locked = 0 });
    const good = rt(indices_t, .{ .caps = 0, .num = 1, .scroll = 2 });
    for ([_]u8{ 32, 33, 254 }) |bad_value| {
        const bad = rt(u8, bad_value);
        try std.testing.expectError(error.InvalidIndex, lock_mask(mods, .{ .caps = bad, .num = 1, .scroll = 2 }));
        try std.testing.expectError(error.InvalidIndex, lock_mask(mods, .{ .caps = 0, .num = bad, .scroll = 2 }));
        try std.testing.expectError(error.InvalidIndex, lock_mask(mods, .{ .caps = 0, .num = 1, .scroll = bad }));
    }
    try std.testing.expectEqual(@as(u8, 0), try lock_mask(mods, good));
}

test "host enter always sends and observe sends only on change while focused" {
    var state = host_state_t{ .focused = false, .last_mask = 0 };
    const caps_on = modifiers_t{ .depressed = 0, .latched = 0, .locked = 1 << 1 };
    const indices = indices_t{ .caps = 1, .num = 2, .scroll = ModifierAbsent };
    const unchanged = try host_observe(&state, caps_on, indices);
    try std.testing.expect(!unchanged.send);
    try std.testing.expect(!state.focused);

    const entered = try host_enter(&state, caps_on, indices);
    try std.testing.expectEqualDeep(host_decision_t{ .send = true, .mask = LockCaps }, entered);
    try std.testing.expect(state.focused);
    try std.testing.expectEqual(@as(u8, LockCaps), state.last_mask);

    const duplicate = try host_observe(&state, caps_on, indices);
    try std.testing.expect(!duplicate.send);
    try std.testing.expectEqual(@as(u8, LockCaps), duplicate.mask);

    const changed = try host_observe(&state, .{ .depressed = 0, .latched = 0, .locked = 1 << 2 }, indices);
    try std.testing.expectEqualDeep(host_decision_t{ .send = true, .mask = LockNum }, changed);
    try std.testing.expectEqual(@as(u8, LockNum), state.last_mask);

    host_leave(&state);
    try std.testing.expect(!state.focused);
    const after_leave = try host_observe(&state, .{ .depressed = 0, .latched = 0, .locked = 0 }, indices);
    try std.testing.expect(!after_leave.send);
    try std.testing.expectEqual(@as(u8, LockNum), state.last_mask);

    const reentered = try host_enter(&state, .{ .depressed = 0, .latched = 0, .locked = 0 }, indices);
    try std.testing.expectEqualDeep(host_decision_t{ .send = true, .mask = 0 }, reentered);
}

test "host enter repeated without leave still sends one message per enter" {
    var state = host_state_t{ .focused = false, .last_mask = 0 };
    const indices = indices_t{ .caps = 0, .num = 1, .scroll = 2 };
    const mods = modifiers_t{ .depressed = 0, .latched = 0, .locked = 0b100 };
    try std.testing.expect((try host_enter(&state, mods, indices)).send);
    try std.testing.expect((try host_enter(&state, mods, indices)).send);
    try std.testing.expect(!(try host_observe(&state, mods, indices)).send);
}

test "host rejections preserve state and never send" {
    var state = host_state_t{ .focused = false, .last_mask = 0 };
    const mods = rt(modifiers_t, .{ .depressed = 0, .latched = 0, .locked = 1 });
    const bad = rt(indices_t, .{ .caps = 40, .num = 1, .scroll = 2 });
    const before = state;
    try std.testing.expectError(error.InvalidIndex, host_enter(&state, mods, bad));
    try std.testing.expectEqualDeep(before, state);

    _ = try host_enter(&state, mods, .{ .caps = 0, .num = 1, .scroll = 2 });
    const focused = state;
    try std.testing.expectError(error.InvalidIndex, host_observe(&state, mods, bad));
    try std.testing.expectEqualDeep(focused, state);
}

test "serial ordering is wrap safe and rejects equal and half-space distances" {
    try std.testing.expect(serial_newer(2, 1));
    try std.testing.expect(!serial_newer(1, 2));
    try std.testing.expect(!serial_newer(7, 7));
    try std.testing.expect(serial_newer(1, 0xFFFF_FFFF));
    try std.testing.expect(!serial_newer(0xFFFF_FFFF, 1));
    try std.testing.expect(serial_newer(HalfSerialSpace - 1, 0));
    try std.testing.expect(!serial_newer(HalfSerialSpace, 0));
    try std.testing.expect(!serial_newer(0, HalfSerialSpace));
    try std.testing.expect(serial_newer(0, HalfSerialSpace + 1));
}

test "toggle plan is empty for equal masks and ordered press-release for differences" {
    const none = try plan_toggles(LockNum, LockNum);
    try std.testing.expectEqual(@as(u8, 0), none.count);

    const one = try plan_toggles(LockNum, 0);
    try std.testing.expectEqual(@as(u8, 2), one.count);
    try std.testing.expectEqualDeep(toggle_action_t{ .vk = VkNumlock, .pressed = true }, one.actions[0]);
    try std.testing.expectEqualDeep(toggle_action_t{ .vk = VkNumlock, .pressed = false }, one.actions[1]);

    const all = try plan_toggles(LockAll, 0);
    try std.testing.expectEqual(@as(u8, 6), all.count);
    try std.testing.expectEqual(VkCapital, all.actions[0].vk);
    try std.testing.expectEqual(VkCapital, all.actions[1].vk);
    try std.testing.expectEqual(VkNumlock, all.actions[2].vk);
    try std.testing.expectEqual(VkNumlock, all.actions[3].vk);
    try std.testing.expectEqual(VkScroll, all.actions[4].vk);
    try std.testing.expect(all.actions[4].pressed);
    try std.testing.expect(!all.actions[5].pressed);

    const scroll_only = try plan_toggles(0, LockScroll);
    try std.testing.expectEqual(@as(u8, 2), scroll_only.count);
    try std.testing.expectEqual(VkScroll, scroll_only.actions[0].vk);
}

test "toggle plan rejects unknown bits in either mask" {
    try std.testing.expectError(error.InvalidMask, plan_toggles(rt(u8, 8), 0));
    try std.testing.expectError(error.InvalidMask, plan_toggles(0, rt(u8, 0x80)));
    try std.testing.expectError(error.InvalidMask, plan_toggles(rt(u8, 0xFF), 0));
    try std.testing.expectEqual(@as(u8, 0), (try plan_toggles(LockAll, LockAll)).count);
}

test "guest accepts the first update of the current epoch and orders serials within it" {
    var state = guest_state_t{ .accepted = false, .epoch = 0, .serial = 0 };
    const first = try guest_update(&state, 5, 5, 10, LockCaps, 0);
    try std.testing.expect(first.applied);
    try std.testing.expectEqual(@as(u8, 2), first.plan.count);

    const same = try guest_update(&state, 5, 5, 10, 0, LockCaps);
    try std.testing.expect(!same.applied);
    try std.testing.expectEqual(@as(u8, 0), same.plan.count);

    const older = try guest_update(&state, 5, 5, 9, 0, LockCaps);
    try std.testing.expect(!older.applied);

    const newer = try guest_update(&state, 5, 5, 11, LockNum, 0);
    try std.testing.expect(newer.applied);
    try std.testing.expectEqual(@as(u64, 5), state.epoch);
    try std.testing.expectEqual(@as(u32, 11), state.serial);
}

test "guest rejects every epoch other than the current one in both directions without mutation" {
    var state = guest_state_t{ .accepted = false, .epoch = 0, .serial = 0 };
    _ = try guest_update(&state, 5, 5, 10, LockCaps, 0);
    const before = state;
    try std.testing.expectError(error.EpochMismatch, guest_update(&state, 5, 4, 99, 0, LockCaps));
    try std.testing.expectError(error.EpochMismatch, guest_update(&state, 5, 6, 99, 0, LockCaps));
    try std.testing.expectError(error.EpochMismatch, guest_update(&state, 5, 6, 1, LockAll, rt(u8, 8)));
    try std.testing.expectEqualDeep(before, state);
}

test "a new epoch starts a fresh serial order and replaces the older lease state" {
    var state = guest_state_t{ .accepted = false, .epoch = 0, .serial = 0 };
    _ = try guest_update(&state, 5, 5, 900, LockCaps, 0);
    const next = try guest_update(&state, 6, 6, 3, LockNum, 0);
    try std.testing.expect(next.applied);
    try std.testing.expectEqual(@as(u64, 6), state.epoch);
    try std.testing.expectEqual(@as(u32, 3), state.serial);
    const stale = try guest_update(&state, 6, 6, 3, 0, LockNum);
    try std.testing.expect(!stale.applied);
    try std.testing.expectError(error.EpochMismatch, guest_update(&state, 6, 5, 901, 0, LockCaps));
    try std.testing.expectEqual(@as(u32, 3), state.serial);
}

test "guest serial gate survives uint32 wrap and rejects half-space jumps" {
    var state = guest_state_t{ .accepted = true, .epoch = 1, .serial = 0xFFFF_FFFE };
    try std.testing.expect((try guest_update(&state, 1, 1, 0xFFFF_FFFF, 0, 0)).applied);
    try std.testing.expect((try guest_update(&state, 1, 1, 1, 0, 0)).applied);
    try std.testing.expect(!(try guest_update(&state, 1, 1, 0xFFFF_FFFF, 0, 0)).applied);
    try std.testing.expect(!(try guest_update(&state, 1, 1, 1 +% HalfSerialSpace, 0, 0)).applied);
    try std.testing.expectEqual(@as(u32, 1), state.serial);
}

test "guest rejects zero epochs, zero serial and unknown bits without mutation" {
    var state = guest_state_t{ .accepted = false, .epoch = 0, .serial = 0 };
    try std.testing.expectError(error.InvalidEpoch, guest_update(&state, rt(u64, 0), 0, 1, 0, 0));
    try std.testing.expectError(error.InvalidEpoch, guest_update(&state, 1, rt(u64, 0), 1, 0, 0));
    try std.testing.expectError(error.InvalidSerial, guest_update(&state, 1, 1, rt(u32, 0), 0, 0));
    try std.testing.expectError(error.InvalidMask, guest_update(&state, 1, 1, 1, rt(u8, 8), 0));
    try std.testing.expectError(error.InvalidMask, guest_update(&state, 1, 1, 1, 0, rt(u8, 8)));
    try std.testing.expectEqualDeep(guest_state_t{ .accepted = false, .epoch = 0, .serial = 0 }, state);
}

test "C ABI maps every status and writes outputs only on success" {
    var sentinel_update = update_t{ .applied = true, .plan = std.mem.zeroes(toggle_plan_t) };
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_guest_update(rt(?*guest_state_t, null), 1, 1, 1, 0, 0, &sentinel_update));
    var guest = guest_state_t{ .accepted = false, .epoch = 0, .serial = 0 };
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_guest_update(&guest, 1, 1, 1, 0, 0, rt(?*update_t, null)));
    try std.testing.expectEqual(status_t.InvalidEpoch, waddle_keyboard_lock_guest_update(&guest, 1, rt(u64, 0), 1, 0, 0, &sentinel_update));
    try std.testing.expectEqual(status_t.InvalidSerial, waddle_keyboard_lock_guest_update(&guest, 1, 1, rt(u32, 0), 0, 0, &sentinel_update));
    try std.testing.expectEqual(status_t.EpochMismatch, waddle_keyboard_lock_guest_update(&guest, 2, 1, 1, 0, 0, &sentinel_update));
    try std.testing.expectEqual(status_t.InvalidMask, waddle_keyboard_lock_guest_update(&guest, 1, 1, 1, rt(u8, 9), 0, &sentinel_update));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_lock_guest_update(&guest, 1, 1, 1, LockCaps, 0, &sentinel_update));
    try std.testing.expect(sentinel_update.applied);
    try std.testing.expectEqual(@as(u8, 2), sentinel_update.plan.count);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_lock_guest_update(&guest, 1, 1, 1, 0, 0, &sentinel_update));
    try std.testing.expect(!sentinel_update.applied);

    const mods = rt(modifiers_t, .{ .depressed = 0, .latched = 0, .locked = 1 });
    const indices = rt(indices_t, .{ .caps = 0, .num = 1, .scroll = 2 });
    const bad_indices = rt(indices_t, .{ .caps = 0, .num = 99, .scroll = 2 });
    var mask: u8 = 0xEE;
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_mask(rt(?*const modifiers_t, null), &indices, &mask));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_mask(&mods, rt(?*const indices_t, null), &mask));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_mask(&mods, &indices, rt(?*u8, null)));
    try std.testing.expectEqual(status_t.InvalidIndex, waddle_keyboard_lock_mask(&mods, &bad_indices, &mask));
    try std.testing.expectEqual(@as(u8, 0xEE), mask);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_lock_mask(&mods, &indices, &mask));
    try std.testing.expectEqual(@as(u8, LockCaps), mask);

    var host = host_state_t{ .focused = false, .last_mask = 0 };
    var decision = host_decision_t{ .send = true, .mask = 0xEE };
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_host_enter(rt(?*host_state_t, null), &mods, &indices, &decision));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_host_enter(&host, rt(?*const modifiers_t, null), &indices, &decision));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_host_enter(&host, &mods, rt(?*const indices_t, null), &decision));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_host_enter(&host, &mods, &indices, rt(?*host_decision_t, null)));
    try std.testing.expectEqual(status_t.InvalidIndex, waddle_keyboard_lock_host_enter(&host, &mods, &bad_indices, &decision));
    try std.testing.expect(!host.focused);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_lock_host_enter(&host, &mods, &indices, &decision));
    try std.testing.expectEqualDeep(host_decision_t{ .send = true, .mask = LockCaps }, decision);
    decision = .{ .send = true, .mask = 0xEE };
    try std.testing.expectEqual(status_t.InvalidIndex, waddle_keyboard_lock_host_observe(&host, &mods, &bad_indices, &decision));
    try std.testing.expectEqual(@as(u8, 0xEE), decision.mask);
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_host_observe(rt(?*host_state_t, null), &mods, &indices, &decision));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_host_observe(&host, rt(?*const modifiers_t, null), &indices, &decision));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_host_observe(&host, &mods, rt(?*const indices_t, null), &decision));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_host_observe(&host, &mods, &indices, rt(?*host_decision_t, null)));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_lock_host_observe(&host, &mods, &indices, &decision));
    try std.testing.expect(!decision.send);
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_host_leave(rt(?*host_state_t, null)));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_lock_host_leave(&host));
    try std.testing.expect(!host.focused);

    var plan = std.mem.zeroes(toggle_plan_t);
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_lock_plan_toggles(LockAll, 0, rt(?*toggle_plan_t, null)));
    try std.testing.expectEqual(status_t.InvalidMask, waddle_keyboard_lock_plan_toggles(rt(u8, 16), 0, &plan));
    try std.testing.expectEqual(@as(u8, 0), plan.count);
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_lock_plan_toggles(LockAll, 0, &plan));
    try std.testing.expectEqual(@as(u8, 6), plan.count);
}

// Test-only fixtures.
/// Launders a value through memory the optimizer cannot see, so every error and
/// null path stays a live runtime branch instead of being folded out of the test.
fn rt(comptime T: type, value: T) T {
    var copy = value;
    std.mem.doNotOptimizeAway(&copy);
    return copy;
}

// Independent references: pure arithmetic written differently from the reducer.
const reference_t = struct {
    focused: bool = false,
    last_mask: u8 = 0,
    accepted: bool = false,
    epoch: u64 = 0,
    serial: u32 = 0,
};

fn reference_bit(locked: u32, index: u8, bit: u8) u8 {
    if (index == ModifierAbsent) return 0;
    return if (((locked >> @intCast(index)) & 1) != 0) bit else 0;
}

fn reference_mask(mods: modifiers_t, indices: indices_t) u8 {
    return reference_bit(mods.locked, indices.caps, LockCaps) |
        reference_bit(mods.locked, indices.num, LockNum) |
        reference_bit(mods.locked, indices.scroll, LockScroll);
}

fn reference_index_valid(index: u8) bool {
    return index == ModifierAbsent or index <= MaxModifierIndex;
}

fn random_index(random: std.Random) u8 {
    const value = random.uintLessThan(u8, 40);
    if (value == 39) return ModifierAbsent;
    return value;
}

test "randomized host and guest sequences match independent references" {
    var prng = std.Random.DefaultPrng.init(0x6c6f636b);
    const random = prng.random();
    var host = host_state_t{ .focused = false, .last_mask = 0 };
    var guest = guest_state_t{ .accepted = false, .epoch = 0, .serial = 0 };
    var reference = reference_t{};
    var wrap_base: u32 = 0;
    for (0..20000) |_| {
        const mods = modifiers_t{ .depressed = random.int(u32), .latched = random.int(u32), .locked = random.int(u32) };
        const indices = indices_t{ .caps = random_index(random), .num = random_index(random), .scroll = random_index(random) };
        const valid = reference_index_valid(indices.caps) and reference_index_valid(indices.num) and reference_index_valid(indices.scroll);
        switch (random.uintLessThan(u8, 4)) {
            0 => {
                const before = host;
                if (!valid) {
                    try std.testing.expectError(error.InvalidIndex, host_enter(&host, mods, indices));
                    try std.testing.expectEqualDeep(before, host);
                } else {
                    const mask = reference_mask(mods, indices);
                    const decision = try host_enter(&host, mods, indices);
                    try std.testing.expect(decision.send);
                    try std.testing.expectEqual(mask, decision.mask);
                    reference.focused = true;
                    reference.last_mask = mask;
                }
            },
            1 => {
                if (!valid) {
                    const before = host;
                    try std.testing.expectError(error.InvalidIndex, host_observe(&host, mods, indices));
                    try std.testing.expectEqualDeep(before, host);
                } else {
                    const mask = reference_mask(mods, indices);
                    const decision = try host_observe(&host, mods, indices);
                    const expect_send = reference.focused and mask != reference.last_mask;
                    try std.testing.expectEqual(expect_send, decision.send);
                    try std.testing.expectEqual(mask, decision.mask);
                    if (expect_send) reference.last_mask = mask;
                }
            },
            2 => {
                host_leave(&host);
                reference.focused = false;
            },
            else => {
                const current_epoch: u64 = if (random.uintLessThan(u8, 16) == 0) 0 else 1 + random.uintLessThan(u64, 3);
                const epoch: u64 = if (random.uintLessThan(u8, 8) == 0) random.uintLessThan(u64, 4) else current_epoch;
                wrap_base = if (random.boolean()) 0 else 0xFFFF_FFF0;
                const serial = random.uintLessThan(u32, 16) +% wrap_base;
                const desired = random.uintLessThan(u8, 9);
                const current = random.uintLessThan(u8, 16);
                const before = guest;
                const expected_error: ?anyerror = if (current_epoch == 0 or epoch == 0)
                    error.InvalidEpoch
                else if (serial == 0)
                    error.InvalidSerial
                else if (epoch != current_epoch)
                    error.EpochMismatch
                else if (desired > LockAll or current > LockAll)
                    error.InvalidMask
                else
                    null;
                if (expected_error) |expected| {
                    try std.testing.expectError(expected, guest_update(&guest, current_epoch, epoch, serial, desired, current));
                    try std.testing.expectEqualDeep(before, guest);
                    continue;
                }
                const update = try guest_update(&guest, current_epoch, epoch, serial, desired, current);
                const expect_applied = !reference.accepted or reference.epoch != epoch or
                    @as(i32, @bitCast(serial -% reference.serial)) > 0;
                try std.testing.expectEqual(expect_applied, update.applied);
                if (expect_applied) {
                    reference.accepted = true;
                    reference.epoch = epoch;
                    reference.serial = serial;
                    const differing = @popCount(desired ^ current);
                    try std.testing.expectEqual(@as(u8, @intCast(differing * 2)), update.plan.count);
                } else {
                    try std.testing.expectEqual(@as(u8, 0), update.plan.count);
                    try std.testing.expectEqualDeep(before, guest);
                }
            },
        }
        try std.testing.expectEqual(reference.focused, host.focused);
    }
}

test "every index and guest epoch, serial and mask combination is classified exactly" {
    // Bounds are runtime-opaque so every error and accept edge stays live.
    const index_limit = rt(u8, 40);
    var index: u8 = 0;
    while (index < index_limit) : (index += 1) {
        const valid_index = index == ModifierAbsent or index <= MaxModifierIndex;
        const mods = modifiers_t{ .depressed = 0, .latched = 0, .locked = rt(u32, 0xFFFF_FFFF) };
        for ([_]indices_t{
            .{ .caps = index, .num = 1, .scroll = 2 },
            .{ .caps = 0, .num = index, .scroll = 2 },
            .{ .caps = 0, .num = 1, .scroll = index },
        }) |probe| {
            const result = lock_mask(mods, probe);
            if (valid_index) {
                _ = try result;
            } else {
                try std.testing.expectError(error.InvalidIndex, result);
            }
        }
    }
    var current_epoch: u64 = 0;
    const epoch_limit = rt(u64, 3);
    while (current_epoch <= epoch_limit) : (current_epoch += 1) {
        var epoch: u64 = 0;
        while (epoch <= epoch_limit) : (epoch += 1) {
            var serial: u32 = 0;
            const serial_limit = rt(u32, 2);
            while (serial <= serial_limit) : (serial += 1) {
                var desired: u8 = 0;
                const mask_limit = rt(u8, 9);
                while (desired <= mask_limit) : (desired += 1) {
                    var current_mask: u8 = 0;
                    const current_limit = rt(u8, 9);
                    while (current_mask <= current_limit) : (current_mask += 1) {
                        var state = guest_state_t{ .accepted = false, .epoch = 0, .serial = 0 };
                        const expected_error: ?anyerror = if (current_epoch == 0 or epoch == 0)
                            error.InvalidEpoch
                        else if (serial == 0)
                            error.InvalidSerial
                        else if (epoch != current_epoch)
                            error.EpochMismatch
                        else if (desired > LockAll or current_mask > LockAll)
                            error.InvalidMask
                        else
                            null;
                        const outcome = guest_update(&state, current_epoch, epoch, serial, desired, current_mask);
                        if (expected_error) |expected| {
                            try std.testing.expectError(expected, outcome);
                            try std.testing.expectEqualDeep(guest_state_t{ .accepted = false, .epoch = 0, .serial = 0 }, state);
                        } else {
                            const update = try outcome;
                            try std.testing.expect(update.applied);
                            try std.testing.expectEqual(@as(u8, @intCast(@popCount(desired ^ current_mask) * 2)), update.plan.count);
                        }
                    }
                }
            }
        }
    }
    try std.testing.expectError(error.InvalidMask, plan_toggles(rt(u8, 8), 0));
    try std.testing.expectEqual(@as(u8, 0), (try plan_toggles(rt(u8, 0), 0)).count);
}

test "ABI struct sizes match the C header layouts" {
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(modifiers_t));
    try std.testing.expectEqual(@as(usize, 3), @sizeOf(indices_t));
    try std.testing.expectEqual(@as(usize, 2), @sizeOf(host_state_t));
    try std.testing.expectEqual(@as(usize, 2), @sizeOf(host_decision_t));
    try std.testing.expectEqual(@as(usize, 24), @sizeOf(guest_state_t));
    try std.testing.expectEqual(@as(usize, 4), @sizeOf(toggle_action_t));
    try std.testing.expectEqual(@as(usize, 26), @sizeOf(toggle_plan_t));
    try std.testing.expectEqual(@as(usize, 28), @sizeOf(update_t));
}
