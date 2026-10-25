//! Keyboard shortcut-inhibit policy for zwp_keyboard_shortcuts_inhibit_v1.
//! Pure, allocation-free, single owner-thread state. It decides which inhibitor
//! object to create or destroy and whether compositor shortcuts are forwarded.
//! It creates no Wayland objects and performs no I/O.
const std = @import("std");

/// Half of the 32-bit generation space; a forward distance at or above this is older.
pub const HalfGenerationSpace: u32 = 0x8000_0000;
/// Maximum actions one event can produce: destroy the previous inhibitor, then create the new one.
pub const MaxActions: usize = 2;

/// Explicit rejections. No state is changed when one is returned.
/// InvalidIdentity: window identity is zero, or generation is zero.
pub const error_t = error{InvalidIdentity};

/// Shortcut inhibit policy from the `input.inhibit_shortcuts` configuration.
/// Never: no inhibitor is ever requested. Fullscreen: requested only while the
/// focused surface is fullscreen or pointer-locked. Focused: requested whenever
/// the window holds input focus.
pub const mode_t = enum(u8) {
    /// Never request an inhibitor.
    Never = 0,
    /// Request only while the focused window is fullscreen or pointer-locked.
    Fullscreen = 1,
    /// Request whenever the window holds input focus.
    Focused = 2,
};

/// Window identity. Both fields are opaque and compared exactly, except that
/// generations are ordered with wrap-safe distance inside one window.
pub const identity_t = struct {
    /// Window identity; zero is never valid.
    window: u64,
    /// Window generation; zero is never valid.
    generation: u32,
};

/// Compositor-reported inhibitor state for the live object. None means no object exists.
pub const phase_t = enum(u8) {
    /// No inhibitor object exists for the focused window.
    None = 0,
    /// Object created; the compositor has not yet reported it active.
    Pending = 1,
    /// Compositor reported the inhibitor active; shortcuts go to the guest.
    Active = 2,
};

/// Kind of inhibitor object action.
pub const action_kind_t = enum(u8) {
    /// Create one zwp_keyboard_shortcuts_inhibitor_v1 for the identity.
    Create = 1,
    /// Destroy the live inhibitor for the identity exactly once.
    Destroy = 2,
};

/// One ordered object action.
pub const action_t = struct {
    /// Create or Destroy.
    kind: action_kind_t,
    /// Identity the object belongs to.
    identity: identity_t,
};

/// Result of one apply call. Actions are in the order to execute; index 0 is
/// first. Entries beyond `count` are undefined and must not be read.
pub const result_t = struct {
    /// Ordered actions; valid entries are actions[0] through actions[count - 1].
    actions: [MaxActions]action_t,
    /// Number of valid actions; 0 through MaxActions.
    count: u8,
    /// True when Super and Alt-Tab style shortcuts must be forwarded to the guest.
    forward_shortcuts: bool,
};

/// Surface state change payload: whether the focused window is fullscreen or pointer-locked.
pub const surface_t = struct {
    /// Window the change applies to.
    identity: identity_t,
    /// True when the surface is fullscreen or pointer-locked.
    fullscreen: bool,
};

/// Input to the policy reducer. Events are applied in arrival order.
pub const event_t = union(enum) {
    /// The window gained keyboard focus (wl_keyboard.enter).
    focus_enter: identity_t,
    /// The window lost keyboard focus (wl_keyboard.leave).
    focus_leave: identity_t,
    /// The compositor reported the live inhibitor active.
    inhibitor_active: identity_t,
    /// The compositor reported the live inhibitor inactive.
    inhibitor_inactive: identity_t,
    /// The focused surface became or stopped being fullscreen or pointer-locked.
    surface_fullscreen: surface_t,
    /// The user's escape chord toggled the local shortcut override.
    user_toggle: identity_t,
    /// The window was destroyed.
    window_destroy: identity_t,
    /// The inhibit policy configuration changed.
    configure: mode_t,
};

/// Owner-thread policy state. Zero-initialized storage is valid: no focus,
/// policy Never, no inhibitor. Enum fields hold only values from their declarations;
/// callers must not write them directly.
pub const state_t = extern struct {
    /// Configured policy.
    config: mode_t,
    /// Live inhibitor phase for the focused window.
    phase: phase_t,
    /// True while a window holds keyboard focus.
    focused: bool,
    /// True while the focused surface is fullscreen or pointer-locked.
    fullscreen: bool,
    /// True while the user override suppresses inhibition for this focus.
    override: bool,
    /// Focused window identity; meaningful only when `focused`.
    window: u64,
    /// Focused window generation; meaningful only when `focused`.
    generation: u32,
};

/// Apply one event. Returns the ordered actions and forwarding decision.
/// `state` is in/out and owner-serialized. Events with a zero identity are rejected.
/// Events that refer to a window or generation other than the focused one are no-ops.
/// Ordering rule: when focus moves, the old inhibitor is destroyed before the new
/// one is created, so at most one object exists at any time.
/// Errors: InvalidIdentity; state is not changed on error.
/// Thread safety: caller serializes calls on one state.
pub fn apply(state: *state_t, event: event_t) error_t!result_t {
    var out = result_t{ .actions = undefined, .count = 0, .forward_shortcuts = false };
    switch (event) {
        .focus_enter => |id| {
            try validate(id);
            if (state.focused and state.window == id.window) {
                if (state.generation == id.generation) return finish(state, &out);
                if (!generation_newer(id.generation, state.generation)) return finish(state, &out);
            }
            release(state, &out);
            state.focused = true;
            state.window = id.window;
            state.generation = id.generation;
            state.fullscreen = false;
            state.override = false;
            reconcile(state, &out);
        },
        .focus_leave => |id| {
            try validate(id);
            if (matches(state, id)) release(state, &out);
        },
        .window_destroy => |id| {
            try validate(id);
            if (matches(state, id)) release(state, &out);
        },
        .inhibitor_active => |id| {
            try validate(id);
            if (matches(state, id) and state.phase == .Pending) state.phase = .Active;
        },
        .inhibitor_inactive => |id| {
            try validate(id);
            if (matches(state, id) and state.phase == .Active) state.phase = .Pending;
        },
        .surface_fullscreen => |change| {
            try validate(change.identity);
            if (matches(state, change.identity)) {
                state.fullscreen = change.fullscreen;
                reconcile(state, &out);
            }
        },
        .user_toggle => |id| {
            try validate(id);
            if (matches(state, id)) {
                state.override = !state.override;
                reconcile(state, &out);
            }
        },
        .configure => |mode| {
            state.config = mode;
            reconcile(state, &out);
        },
    }
    return finish(state, &out);
}

/// Initialize state to the zero value: unfocused, policy Never, no object.
/// `state` is out. No error. Thread safety: caller serializes calls on one state.
pub fn init(state: *state_t) void {
    state.* = std.mem.zeroes(state_t);
}

fn validate(id: identity_t) error_t!void {
    if (id.window == 0 or id.generation == 0) return error.InvalidIdentity;
}

/// Wrap-safe generation order (RFC 1982 style): candidate is newer when its
/// forward distance from current is in 1 through HalfGenerationSpace - 1.
fn generation_newer(candidate: u32, current: u32) bool {
    return candidate != current and (candidate -% current) < HalfGenerationSpace;
}

fn matches(state: *const state_t, id: identity_t) bool {
    return state.focused and state.window == id.window and state.generation == id.generation;
}

fn wanted(mode: mode_t, fullscreen: bool) bool {
    return switch (mode) {
        .Never => false,
        .Fullscreen => fullscreen,
        .Focused => true,
    };
}

/// Destroy the live object if one exists, then clear focus and all per-focus flags.
fn release(state: *state_t, out: *result_t) void {
    if (state.phase != .None) {
        push(out, .Destroy, .{ .window = state.window, .generation = state.generation });
    }
    state.phase = .None;
    state.focused = false;
    state.fullscreen = false;
    state.override = false;
    state.window = 0;
    state.generation = 0;
}

/// Create or destroy the object so it exists exactly when the policy wants it.
fn reconcile(state: *state_t, out: *result_t) void {
    const desired = state.focused and !state.override and wanted(state.config, state.fullscreen);
    const id = identity_t{ .window = state.window, .generation = state.generation };
    if (desired and state.phase == .None) {
        push(out, .Create, id);
        state.phase = .Pending;
    } else if (!desired and state.phase != .None) {
        push(out, .Destroy, id);
        state.phase = .None;
    }
}

fn finish(state: *const state_t, out: *result_t) result_t {
    out.forward_shortcuts = state.phase == .Active;
    return out.*;
}

fn push(out: *result_t, kind: action_kind_t, id: identity_t) void {
    out.actions[out.count] = .{ .kind = kind, .identity = id };
    out.count += 1;
}

/// C ABI status. Ok is 0; every other value names one rejection.
pub const status_t = enum(c_int) {
    /// Operation succeeded; output arguments were written.
    Ok = 0,
    /// A required pointer argument was NULL.
    NullArgument = 1,
    /// The event identity has a zero window or a zero generation.
    InvalidIdentity = 2,
    /// The event kind byte is not an event_kind_t value.
    InvalidKind = 3,
    /// The mode byte of a Configure event is not a mode_t value.
    InvalidMode = 4,
};

/// C ABI event kind; one value per event_t variant.
pub const event_kind_t = enum(u8) {
    /// focus_enter with identity (window, generation).
    FocusEnter = 0,
    /// focus_leave with identity (window, generation).
    FocusLeave = 1,
    /// inhibitor_active with identity (window, generation).
    InhibitorActive = 2,
    /// inhibitor_inactive with identity (window, generation).
    InhibitorInactive = 3,
    /// surface_fullscreen with identity and the `fullscreen` byte.
    SurfaceFullscreen = 4,
    /// user_toggle with identity (window, generation).
    UserToggle = 5,
    /// window_destroy with identity (window, generation).
    WindowDestroy = 6,
    /// configure with the `mode` byte.
    Configure = 7,
};

/// C ABI event. Fields that a given kind does not use are ignored.
pub const event_abi_t = extern struct {
    /// event_kind_t value.
    kind: u8,
    /// mode_t value, used only by Configure.
    mode: u8,
    /// Fullscreen flag, used only by SurfaceFullscreen.
    fullscreen: bool,
    /// Window identity.
    window: u64,
    /// Window generation.
    generation: u32,
};

/// C ABI object action.
pub const action_abi_t = extern struct {
    /// action_kind_t value: 1 Create, 2 Destroy.
    kind: u8,
    /// Identity the object belongs to.
    window: u64,
    /// Generation the object belongs to.
    generation: u32,
};

/// C ABI result. Entries at index `count` and beyond are zero.
pub const result_abi_t = extern struct {
    /// Number of valid actions; 0 through MaxActions.
    count: u8,
    /// Actions in execution order.
    actions: [MaxActions]action_abi_t,
    /// True when Super and Alt-Tab style shortcuts must be forwarded to the guest.
    forward_shortcuts: bool,
};

const abi_error_t = error{ NullArgument, InvalidIdentity, InvalidKind, InvalidMode };

fn status_of(err: abi_error_t) status_t {
    return switch (err) {
        error.NullArgument => .NullArgument,
        error.InvalidIdentity => .InvalidIdentity,
        error.InvalidKind => .InvalidKind,
        error.InvalidMode => .InvalidMode,
    };
}

fn decode(raw: event_abi_t) abi_error_t!event_t {
    const kind = std.meta.intToEnum(event_kind_t, raw.kind) catch return error.InvalidKind;
    const id = identity_t{ .window = raw.window, .generation = raw.generation };
    return switch (kind) {
        .FocusEnter => .{ .focus_enter = id },
        .FocusLeave => .{ .focus_leave = id },
        .InhibitorActive => .{ .inhibitor_active = id },
        .InhibitorInactive => .{ .inhibitor_inactive = id },
        .SurfaceFullscreen => .{ .surface_fullscreen = .{ .identity = id, .fullscreen = raw.fullscreen } },
        .UserToggle => .{ .user_toggle = id },
        .WindowDestroy => .{ .window_destroy = id },
        .Configure => .{ .configure = std.meta.intToEnum(mode_t, raw.mode) catch return error.InvalidMode },
    };
}

fn encode(result: result_t) result_abi_t {
    var out = std.mem.zeroes(result_abi_t);
    out.count = result.count;
    out.forward_shortcuts = result.forward_shortcuts;
    for (result.actions[0..result.count], 0..) |action, index| {
        out.actions[index] = .{
            .kind = @intFromEnum(action.kind),
            .window = action.identity.window,
            .generation = action.identity.generation,
        };
    }
    return out;
}

/// C ABI: set the state to the zero value (no focus, policy Never).
/// `state` is out, non-NULL. Returns Ok or NullArgument.
/// Thread safety: caller serializes calls on one state.
export fn waddle_keyboard_inhibit_init(state: ?*state_t) callconv(.C) status_t {
    const s = state orelse return .NullArgument;
    init(s);
    return .Ok;
}

/// C ABI: apply one event. `state` is inout, `event` is in, `out` is out.
/// Decodes and validates the event before touching state. Writes `out` and state
/// only on Ok. Returns Ok, NullArgument, InvalidIdentity, InvalidKind, or InvalidMode.
/// Thread safety: caller serializes calls on one state.
export fn waddle_keyboard_inhibit_apply(state: ?*state_t, event: ?*const event_abi_t, out: ?*result_abi_t) callconv(.C) status_t {
    const s = state orelse return .NullArgument;
    const e = event orelse return .NullArgument;
    const o = out orelse return .NullArgument;
    const decoded = decode(e.*) catch |err| return status_of(err);
    const result = apply(s, decoded) catch |err| return status_of(err);
    o.* = encode(result);
    return .Ok;
}

test "a fresh state creates nothing and forwards nothing" {
    var state: state_t = undefined;
    init(&state);
    const result = try apply(&state, .{ .focus_leave = .{ .window = 1, .generation = 1 } });
    try std.testing.expectEqual(@as(u8, 0), result.count);
    try std.testing.expect(!result.forward_shortcuts);
    try std.testing.expectEqual(mode_t.Never, state.config);
}

test "focused policy creates on enter, destroys on leave, and never duplicates" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Focused });
    const entered = try apply(&state, .{ .focus_enter = .{ .window = 7, .generation = 2 } });
    try std.testing.expectEqual(@as(u8, 1), entered.count);
    try std.testing.expectEqual(action_kind_t.Create, entered.actions[0].kind);
    try std.testing.expectEqual(@as(u64, 7), entered.actions[0].identity.window);
    try std.testing.expect(!entered.forward_shortcuts);

    const duplicate = try apply(&state, .{ .focus_enter = .{ .window = 7, .generation = 2 } });
    try std.testing.expectEqual(@as(u8, 0), duplicate.count);

    const left = try apply(&state, .{ .focus_leave = .{ .window = 7, .generation = 2 } });
    try std.testing.expectEqual(@as(u8, 1), left.count);
    try std.testing.expectEqual(action_kind_t.Destroy, left.actions[0].kind);
    try std.testing.expectEqual(phase_t.None, state.phase);
    const again = try apply(&state, .{ .focus_leave = .{ .window = 7, .generation = 2 } });
    try std.testing.expectEqual(@as(u8, 0), again.count);
}

test "fullscreen policy follows the surface flag and never creates while unfocused" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Fullscreen });
    const id = identity_t{ .window = 3, .generation = 1 };
    const unfocused = try apply(&state, .{ .surface_fullscreen = .{ .identity = id, .fullscreen = true } });
    try std.testing.expectEqual(@as(u8, 0), unfocused.count);

    _ = try apply(&state, .{ .focus_enter = id });
    try std.testing.expectEqual(phase_t.None, state.phase);
    const windowed = try apply(&state, .{ .surface_fullscreen = .{ .identity = id, .fullscreen = false } });
    try std.testing.expectEqual(@as(u8, 0), windowed.count);

    const full = try apply(&state, .{ .surface_fullscreen = .{ .identity = id, .fullscreen = true } });
    try std.testing.expectEqual(@as(u8, 1), full.count);
    try std.testing.expectEqual(action_kind_t.Create, full.actions[0].kind);
    const exit = try apply(&state, .{ .surface_fullscreen = .{ .identity = id, .fullscreen = false } });
    try std.testing.expectEqual(action_kind_t.Destroy, exit.actions[0].kind);
    try std.testing.expectEqual(phase_t.None, state.phase);
}

test "never policy requests nothing and forwards nothing" {
    var state: state_t = undefined;
    init(&state);
    const id = identity_t{ .window = 4, .generation = 1 };
    _ = try apply(&state, .{ .surface_fullscreen = .{ .identity = id, .fullscreen = true } });
    const entered = try apply(&state, .{ .focus_enter = id });
    try std.testing.expectEqual(@as(u8, 0), entered.count);
    const active = try apply(&state, .{ .inhibitor_active = id });
    try std.testing.expect(!active.forward_shortcuts);
    try std.testing.expectEqual(@as(u8, 0), active.count);
}

test "compositor active and inactive reports toggle forwarding without objects changing" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Focused });
    const id = identity_t{ .window = 5, .generation = 9 };
    _ = try apply(&state, .{ .focus_enter = id });

    const active = try apply(&state, .{ .inhibitor_active = id });
    try std.testing.expectEqual(@as(u8, 0), active.count);
    try std.testing.expect(active.forward_shortcuts);
    try std.testing.expectEqual(phase_t.Active, state.phase);
    const repeated = try apply(&state, .{ .inhibitor_active = id });
    try std.testing.expect(repeated.forward_shortcuts);

    const inactive = try apply(&state, .{ .inhibitor_inactive = id });
    try std.testing.expectEqual(@as(u8, 0), inactive.count);
    try std.testing.expect(!inactive.forward_shortcuts);
    try std.testing.expectEqual(phase_t.Pending, state.phase);
    const inactive_again = try apply(&state, .{ .inhibitor_inactive = id });
    try std.testing.expect(!inactive_again.forward_shortcuts);
}

test "user override destroys the object and restores creation when cleared" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Focused });
    const id = identity_t{ .window = 6, .generation = 1 };
    _ = try apply(&state, .{ .focus_enter = id });
    _ = try apply(&state, .{ .inhibitor_active = id });

    const overridden = try apply(&state, .{ .user_toggle = id });
    try std.testing.expectEqual(@as(u8, 1), overridden.count);
    try std.testing.expectEqual(action_kind_t.Destroy, overridden.actions[0].kind);
    try std.testing.expect(!overridden.forward_shortcuts);

    const restored = try apply(&state, .{ .user_toggle = id });
    try std.testing.expectEqual(@as(u8, 1), restored.count);
    try std.testing.expectEqual(action_kind_t.Create, restored.actions[0].kind);
    try std.testing.expect(!restored.forward_shortcuts);
}

test "policy change while focused reconciles the object" {
    var state: state_t = undefined;
    init(&state);
    const id = identity_t{ .window = 8, .generation = 1 };
    _ = try apply(&state, .{ .focus_enter = id });
    const off = try apply(&state, .{ .configure = .Never });
    try std.testing.expectEqual(@as(u8, 0), off.count);
    const on = try apply(&state, .{ .configure = .Focused });
    try std.testing.expectEqual(@as(u8, 1), on.count);
    try std.testing.expectEqual(action_kind_t.Create, on.actions[0].kind);
    const back = try apply(&state, .{ .configure = .Never });
    try std.testing.expectEqual(action_kind_t.Destroy, back.actions[0].kind);
}

test "focus moving between windows destroys the old object before creating the new one" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Focused });
    const first = identity_t{ .window = 1, .generation = 1 };
    const second = identity_t{ .window = 2, .generation = 1 };
    _ = try apply(&state, .{ .focus_enter = first });
    _ = try apply(&state, .{ .inhibitor_active = first });

    const moved = try apply(&state, .{ .focus_enter = second });
    try std.testing.expectEqual(@as(u8, 2), moved.count);
    try std.testing.expectEqual(action_kind_t.Destroy, moved.actions[0].kind);
    try std.testing.expectEqual(@as(u64, 1), moved.actions[0].identity.window);
    try std.testing.expectEqual(action_kind_t.Create, moved.actions[1].kind);
    try std.testing.expectEqual(@as(u64, 2), moved.actions[1].identity.window);
    try std.testing.expect(!moved.forward_shortcuts);

    const stale_first = try apply(&state, .{ .inhibitor_active = first });
    try std.testing.expect(!stale_first.forward_shortcuts);
    try std.testing.expectEqual(phase_t.Pending, state.phase);
    const stale_leave = try apply(&state, .{ .focus_leave = first });
    try std.testing.expectEqual(@as(u8, 0), stale_leave.count);
}

test "stale generations are no-ops and a newer generation replaces the focus" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Focused });
    const current = identity_t{ .window = 9, .generation = 5 };
    _ = try apply(&state, .{ .focus_enter = current });
    const older = try apply(&state, .{ .focus_enter = .{ .window = 9, .generation = 4 } });
    try std.testing.expectEqual(@as(u8, 0), older.count);
    try std.testing.expectEqual(@as(u32, 5), state.generation);

    const newer = try apply(&state, .{ .focus_enter = .{ .window = 9, .generation = 6 } });
    try std.testing.expectEqual(@as(u8, 2), newer.count);
    try std.testing.expectEqual(@as(u32, 6), state.generation);

    const stale_active = try apply(&state, .{ .inhibitor_active = current });
    try std.testing.expect(!stale_active.forward_shortcuts);
    const stale_toggle = try apply(&state, .{ .user_toggle = current });
    try std.testing.expectEqual(@as(u8, 0), stale_toggle.count);
    const stale_fullscreen = try apply(&state, .{ .surface_fullscreen = .{ .identity = current, .fullscreen = true } });
    try std.testing.expectEqual(@as(u8, 0), stale_fullscreen.count);
    try std.testing.expect(!state.fullscreen);
}

test "generation order is wrap safe across uint32 and half space is older" {
    try std.testing.expect(generation_newer(1, 0xFFFF_FFFF));
    try std.testing.expect(!generation_newer(0xFFFF_FFFF, 1));
    try std.testing.expect(!generation_newer(4, 4));
    try std.testing.expect(!generation_newer(HalfGenerationSpace, 0));
    try std.testing.expect(generation_newer(HalfGenerationSpace - 1, 0));
}

test "destroying a window destroys its inhibitor exactly once" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Focused });
    const id = identity_t{ .window = 10, .generation = 1 };
    _ = try apply(&state, .{ .focus_enter = id });
    const destroyed = try apply(&state, .{ .window_destroy = id });
    try std.testing.expectEqual(@as(u8, 1), destroyed.count);
    try std.testing.expectEqual(action_kind_t.Destroy, destroyed.actions[0].kind);
    try std.testing.expectEqual(phase_t.None, state.phase);
    const again = try apply(&state, .{ .window_destroy = id });
    try std.testing.expectEqual(@as(u8, 0), again.count);
    const leave = try apply(&state, .{ .focus_leave = id });
    try std.testing.expectEqual(@as(u8, 0), leave.count);
}

test "destroying an unfocused window does nothing" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Focused });
    _ = try apply(&state, .{ .focus_enter = .{ .window = 11, .generation = 1 } });
    const other = try apply(&state, .{ .window_destroy = .{ .window = 12, .generation = 1 } });
    try std.testing.expectEqual(@as(u8, 0), other.count);
    try std.testing.expectEqual(@as(u64, 11), state.window);
    try std.testing.expectEqual(phase_t.Pending, state.phase);
}

test "zero identities are rejected without changing state" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Focused });
    _ = try apply(&state, .{ .focus_enter = .{ .window = 13, .generation = 1 } });
    const before = state;
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .focus_enter = .{ .window = 0, .generation = 1 } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .focus_enter = .{ .window = 14, .generation = 0 } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .inhibitor_active = .{ .window = 0, .generation = 1 } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .inhibitor_inactive = .{ .window = 13, .generation = 0 } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .surface_fullscreen = .{ .identity = .{ .window = 0, .generation = 1 }, .fullscreen = true } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .user_toggle = .{ .window = 13, .generation = 0 } }));
    try std.testing.expectEqualDeep(before, state);
}

/// Reference model for the randomized test. It tracks the same facts as the reducer
/// through a separate, simpler update rule and checks each step against them.
const reference_t = struct {
    focus: ?identity_t = null,
    override: bool = false,
    fullscreen: bool = false,
    config: mode_t = .Never,
    live: ?identity_t = null,
    active: bool = false,

    fn wants(self: reference_t) bool {
        if (self.focus == null or self.override) return false;
        return switch (self.config) {
            .Never => false,
            .Fullscreen => self.fullscreen,
            .Focused => true,
        };
    }

    fn is_focus(self: reference_t, id: identity_t) bool {
        const focus = self.focus orelse return false;
        return focus.window == id.window and focus.generation == id.generation;
    }

    fn update(self: *reference_t, event: event_t) void {
        switch (event) {
            .focus_enter => |id| {
                if (self.focus) |focus| {
                    if (focus.window == id.window and
                        (focus.generation == id.generation or
                        !(@as(i32, @bitCast(id.generation -% focus.generation)) > 0))) return;
                }
                self.focus = id;
                self.override = false;
                self.fullscreen = false;
            },
            .focus_leave, .window_destroy => |id| {
                if (self.is_focus(id)) {
                    self.focus = null;
                    self.override = false;
                    self.fullscreen = false;
                }
            },
            .inhibitor_active => |id| {
                if (self.is_focus(id) and self.live != null) self.active = true;
            },
            .inhibitor_inactive => |id| {
                if (self.is_focus(id)) self.active = false;
            },
            .surface_fullscreen => |change| {
                if (self.is_focus(change.identity)) self.fullscreen = change.fullscreen;
            },
            .user_toggle => |id| {
                if (self.is_focus(id)) self.override = !self.override;
            },
            .configure => |mode| self.config = mode,
        }
    }
};

fn random_identity(random: std.Random) identity_t {
    return .{
        .window = 1 + random.uintLessThan(u64, 3),
        .generation = 1 + random.uintLessThan(u32, 3),
    };
}

test "randomized long sequences keep one object at most and match the reference" {
    var prng = std.Random.DefaultPrng.init(0x696e686962);
    const random = prng.random();
    var state: state_t = undefined;
    init(&state);
    var reference = reference_t{};
    for (0..20000) |_| {
        const roll = random.uintLessThan(u8, 40);
        const event: event_t = switch (roll) {
            0...7 => .{ .focus_enter = random_identity(random) },
            8...11 => .{ .focus_leave = random_identity(random) },
            12...14 => .{ .window_destroy = random_identity(random) },
            15...19 => .{ .inhibitor_active = random_identity(random) },
            20...22 => .{ .inhibitor_inactive = random_identity(random) },
            23...29 => .{ .surface_fullscreen = .{ .identity = random_identity(random), .fullscreen = random.boolean() } },
            30...34 => .{ .user_toggle = random_identity(random) },
            35...37 => .{ .configure = @enumFromInt(random.uintLessThan(u8, 3)) },
            else => .{ .focus_enter = .{ .window = 0, .generation = 1 } },
        };
        const before = state;
        const rejected = switch (event) {
            .focus_enter, .focus_leave, .window_destroy, .inhibitor_active, .inhibitor_inactive, .user_toggle => |id| id.window == 0 or id.generation == 0,
            .surface_fullscreen => |change| change.identity.window == 0 or change.identity.generation == 0,
            .configure => false,
        };
        const result = apply(&state, event) catch |err| {
            try std.testing.expect(rejected);
            try std.testing.expectEqual(error.InvalidIdentity, err);
            try std.testing.expectEqualDeep(before, state);
            continue;
        };
        try std.testing.expect(!rejected);
        reference.update(event);
        if (reference.active and reference.live == null) reference.active = false;

        for (result.actions[0..result.count]) |action| {
            switch (action.kind) {
                .Destroy => {
                    const live = reference.live orelse return error.TestUnexpectedResult;
                    try std.testing.expectEqualDeep(live, action.identity);
                    reference.live = null;
                    reference.active = false;
                },
                .Create => {
                    try std.testing.expect(reference.live == null);
                    const focus = reference.focus orelse return error.TestUnexpectedResult;
                    try std.testing.expectEqualDeep(focus, action.identity);
                    reference.live = focus;
                    reference.active = false;
                },
            }
        }
        if (reference.wants()) {
            try std.testing.expectEqualDeep(reference.focus, reference.live);
        } else {
            try std.testing.expect(reference.live == null);
        }
        try std.testing.expectEqual(reference.live != null and reference.active, result.forward_shortcuts);
        try std.testing.expect(result.count <= MaxActions);
        try std.testing.expectEqual(reference.config, state.config);
        try std.testing.expect(!(state.phase != .None and !state.focused));
    }
}

test "ABI struct sizes match the C header layouts" {
    try std.testing.expectEqual(@as(usize, 24), @sizeOf(state_t));
    try std.testing.expectEqual(@as(usize, 24), @sizeOf(event_abi_t));
    try std.testing.expectEqual(@as(usize, 24), @sizeOf(action_abi_t));
    try std.testing.expectEqual(@as(usize, 64), @sizeOf(result_abi_t));
}

test "C ABI decodes every event kind and rejects bad kinds and modes without mutation" {
    var state: state_t = undefined;
    init(&state);
    var out = result_abi_t{ .count = 9, .actions = undefined, .forward_shortcuts = true };
    const id_one = event_abi_t{ .kind = @intFromEnum(event_kind_t.Configure), .mode = @intFromEnum(mode_t.Focused), .fullscreen = false, .window = 0, .generation = 0 };
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_init(null));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_apply(null, &id_one, &out));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_apply(&state, null, &out));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_apply(&state, &id_one, null));
    try std.testing.expectEqual(@as(u8, 9), out.count);

    const bad_kind = event_abi_t{ .kind = 200, .mode = 0, .fullscreen = false, .window = 1, .generation = 1 };
    try std.testing.expectEqual(status_t.InvalidKind, waddle_keyboard_inhibit_apply(&state, &bad_kind, &out));
    const bad_mode = event_abi_t{ .kind = @intFromEnum(event_kind_t.Configure), .mode = 3, .fullscreen = false, .window = 0, .generation = 0 };
    try std.testing.expectEqual(status_t.InvalidMode, waddle_keyboard_inhibit_apply(&state, &bad_mode, &out));
    try std.testing.expectEqual(@as(u8, 9), out.count);
    try std.testing.expectEqualDeep(std.mem.zeroes(state_t), state);

    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &id_one, &out));
    try std.testing.expectEqual(@as(u8, 0), out.count);
    try std.testing.expect(!out.forward_shortcuts);
    try std.testing.expectEqual(@as(u8, 0), out.actions[0].kind);

    const enter = event_abi_t{ .kind = @intFromEnum(event_kind_t.FocusEnter), .mode = 0, .fullscreen = false, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &enter, &out));
    try std.testing.expectEqual(@as(u8, 1), out.count);
    try std.testing.expectEqual(@intFromEnum(action_kind_t.Create), out.actions[0].kind);
    try std.testing.expectEqual(@as(u64, 21), out.actions[0].window);
    try std.testing.expectEqual(@as(u32, 3), out.actions[0].generation);
    try std.testing.expectEqual(@as(u8, 0), out.actions[1].kind);
    const zero_identity = event_abi_t{ .kind = @intFromEnum(event_kind_t.FocusEnter), .mode = 0, .fullscreen = false, .window = 0, .generation = 3 };
    const before = state;
    try std.testing.expectEqual(status_t.InvalidIdentity, waddle_keyboard_inhibit_apply(&state, &zero_identity, &out));
    try std.testing.expectEqualDeep(before, state);
    try std.testing.expectEqual(@as(u8, 1), out.count);

    const fullscreen_on = event_abi_t{ .kind = @intFromEnum(event_kind_t.SurfaceFullscreen), .mode = 0, .fullscreen = true, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &fullscreen_on, &out));
    try std.testing.expectEqual(@as(u8, 0), out.count);
    const active = event_abi_t{ .kind = @intFromEnum(event_kind_t.InhibitorActive), .mode = 0, .fullscreen = false, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &active, &out));
    try std.testing.expect(out.forward_shortcuts);
    const inactive = event_abi_t{ .kind = @intFromEnum(event_kind_t.InhibitorInactive), .mode = 0, .fullscreen = false, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &inactive, &out));
    try std.testing.expect(!out.forward_shortcuts);
    const toggle = event_abi_t{ .kind = @intFromEnum(event_kind_t.UserToggle), .mode = 0, .fullscreen = false, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &toggle, &out));
    try std.testing.expectEqual(@intFromEnum(action_kind_t.Destroy), out.actions[0].kind);
    const destroy = event_abi_t{ .kind = @intFromEnum(event_kind_t.WindowDestroy), .mode = 0, .fullscreen = false, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &destroy, &out));
    try std.testing.expectEqual(@as(u8, 0), out.count);
    const leave = event_abi_t{ .kind = @intFromEnum(event_kind_t.FocusLeave), .mode = 0, .fullscreen = false, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &leave, &out));
    try std.testing.expectEqual(@as(u8, 0), out.count);
}
