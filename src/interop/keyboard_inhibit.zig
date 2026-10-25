//! Keyboard shortcut-inhibit policy for zwp_keyboard_shortcuts_inhibit_v1.
//! Pure, allocation-free, single owner-thread state. It decides which inhibitor
//! object to create or destroy and whether compositor shortcuts are forwarded.
//! It creates no Wayland objects and performs no I/O.
const std = @import("std");

/// Bounded number of windows whose fullscreen and pointer-lock flags are remembered.
pub const MaxTrackedWindows: usize = 16;
/// Half of the 32-bit generation space; a forward distance at or above this is older.
pub const HalfGenerationSpace: u32 = 0x8000_0000;
/// Maximum actions one event can produce: destroy the previous inhibitor, then create the new one.
pub const MaxActions: usize = 2;

/// Explicit rejections. No state is changed when one is returned.
/// InvalidIdentity: window identity is zero, or generation is zero.
/// WindowTableFull: a new window cannot be remembered until another window is destroyed.
pub const error_t = error{ InvalidIdentity, WindowTableFull };

/// Shortcut inhibit policy from the `input.inhibit_shortcuts` configuration.
/// Fullscreen is the spec default and is the zero value, so zero-filled state
/// starts with the default policy. Never requests no inhibitor. Focused requests
/// one whenever the window holds input focus.
pub const mode_t = enum(u8) {
    /// Request only while the focused window is fullscreen or pointer-locked (default).
    Fullscreen = 0,
    /// Never request an inhibitor.
    Never = 1,
    /// Request whenever the window holds input focus.
    Focused = 2,
};

/// Window identity. Window and generation are compared exactly, except that
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

/// Result of one apply call. Valid actions are actions[0] through actions[count - 1];
/// entries beyond `count` are undefined and must not be read.
pub const result_t = struct {
    /// Ordered actions in execution order.
    actions: [MaxActions]action_t,
    /// Number of valid actions; 0 through MaxActions.
    count: u8,
    /// True when Super and Alt-Tab style shortcuts must be forwarded to the guest.
    forward_shortcuts: bool,
};

/// Surface state of one window generation. The two flags come from independent
/// sources (the xdg fullscreen state and the pointer-constraints lock) and are
/// ORed for the policy.
pub const surface_t = struct {
    /// Window generation the flags describe.
    identity: identity_t,
    /// True while the surface is fullscreen.
    fullscreen: bool,
    /// True while the surface has an active pointer lock.
    pointer_lock: bool,
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
    /// Fullscreen or pointer-lock state of a window generation changed. Accepted
    /// whether or not the window is focused.
    surface_state: surface_t,
    /// The user's escape chord toggled the local shortcut override.
    user_toggle: identity_t,
    /// The window was destroyed.
    window_destroy: identity_t,
    /// The inhibit policy configuration changed.
    configure: mode_t,
};

/// Remembered surface state of one window. Window zero marks a free slot.
/// Flags are stored as bytes so that any caller-visible value is defined to read.
pub const entry_t = extern struct {
    /// Window identity; zero for a free slot.
    window: u64,
    /// Newest generation seen for this window.
    generation: u32,
    /// 1 when fullscreen, 0 otherwise.
    fullscreen: u8,
    /// 1 when pointer-locked, 0 otherwise.
    pointer_lock: u8,
};

/// Owner-thread policy state. Zero-filled storage is valid: policy Fullscreen,
/// unfocused, no object, no remembered windows. Caller-owned storage of 280 bytes.
/// Fields are private to the module. Bytes are used for every field so that the
/// C ABI can validate them before use; the C header marks them private.
pub const state_t = extern struct {
    /// mode_t value of the configured policy.
    config: u8,
    /// phase_t value of the live inhibitor for the focused window.
    phase: u8,
    /// 1 while a window holds keyboard focus, 0 otherwise.
    focused: u8,
    /// 1 while the user override suppresses inhibition for this focus, 0 otherwise.
    override: u8,
    /// Focused window identity; meaningful only when `focused` is 1.
    window: u64,
    /// Focused window generation; meaningful only when `focused` is 1.
    generation: u32,
    /// Bounded per-window memory of fullscreen and pointer-lock flags.
    table: [MaxTrackedWindows]entry_t,
};

/// Apply one event. Returns the ordered actions and the forwarding decision.
/// `state` is in/out and owner-serialized. Zero identities are rejected. Events
/// for a window or generation other than the focused one are no-ops, except that
/// surface_state is remembered for any window so focus can recompute later.
/// Ordering rule: when focus moves, the old inhibitor is destroyed before the new
/// one is created, so at most one object exists at any time.
/// Errors: InvalidIdentity, or WindowTableFull for an untracked window when the
/// table is full. State is not changed on error.
/// Thread safety: caller serializes calls on one state.
pub fn apply(state: *state_t, event: event_t) error_t!result_t {
    var out = result_t{ .actions = undefined, .count = 0, .forward_shortcuts = false };
    switch (event) {
        .focus_enter => |id| {
            try validate(id);
            if (is_focused(state) and state.window == id.window) {
                if (state.generation == id.generation) return finish(state, &out);
                if (!generation_newer(id.generation, state.generation)) return finish(state, &out);
            }
            release(state, &out);
            state.focused = 1;
            state.window = id.window;
            state.generation = id.generation;
            state.override = 0;
            reconcile(state, &out);
        },
        .focus_leave => |id| {
            try validate(id);
            if (matches(state, id)) release(state, &out);
        },
        .window_destroy => |id| {
            try validate(id);
            forget(state, id);
            if (matches(state, id)) {
                release(state, &out);
            } else if (is_focused(state) and state.window == id.window) {
                reconcile(state, &out);
            }
        },
        .inhibitor_active => |id| {
            try validate(id);
            if (matches(state, id) and phase_of(state.phase) == .Pending) state.phase = @intFromEnum(phase_t.Active);
        },
        .inhibitor_inactive => |id| {
            try validate(id);
            if (matches(state, id) and phase_of(state.phase) == .Active) state.phase = @intFromEnum(phase_t.Pending);
        },
        .surface_state => |change| {
            try validate(change.identity);
            try remember(state, change);
            if (is_focused(state) and state.window == change.identity.window) reconcile(state, &out);
        },
        .user_toggle => |id| {
            try validate(id);
            if (matches(state, id)) {
                state.override ^= 1;
                reconcile(state, &out);
            }
        },
        .configure => |mode| {
            state.config = @intFromEnum(mode);
            reconcile(state, &out);
        },
    }
    return finish(state, &out);
}

/// Set state to the zero value: policy Fullscreen, unfocused, no object, no
/// remembered windows. `state` is out. No error.
/// Thread safety: caller serializes calls on one state.
pub fn init(state: *state_t) void {
    state.* = std.mem.zeroes(state_t);
}

noinline fn validate(id: identity_t) error_t!void {
    if (id.window == 0 or id.generation == 0) return error.InvalidIdentity;
}

/// Wrap-safe generation order (RFC 1982 style): candidate is newer when its
/// forward distance from current is in 1 through HalfGenerationSpace - 1.
/// Parameters are by value. Pure function.
pub fn generation_newer(candidate: u32, current: u32) bool {
    return candidate != current and (candidate -% current) < HalfGenerationSpace;
}

fn phase_of(raw: u8) phase_t {
    return @enumFromInt(raw);
}

fn mode_of(raw: u8) mode_t {
    return @enumFromInt(raw);
}

fn is_focused(state: *const state_t) bool {
    return state.focused != 0;
}

noinline fn matches(state: *const state_t, id: identity_t) bool {
    return is_focused(state) and state.window == id.window and state.generation == id.generation;
}

/// Find the table slot that holds `window`, or null.
noinline fn find_window(state: *const state_t, window: u64) ?usize {
    for (state.table, 0..) |entry, index| {
        if (entry.window == window) return index;
    }
    return null;
}

/// Fullscreen and pointer-lock of exactly this window generation. Any other
/// generation of the window reads as neither flag.
noinline fn flags_for(state: *const state_t, id: identity_t) [2]bool {
    const index = find_window(state, id.window) orelse return .{ false, false };
    const entry = state.table[index];
    if (entry.generation != id.generation) return .{ false, false };
    return .{ entry.fullscreen != 0, entry.pointer_lock != 0 };
}

/// Store the flags of a window generation, whether or not it is focused.
/// A generation older than the remembered one is ignored. A newer or equal
/// generation replaces the remembered flags. A new window needs a free slot.
noinline fn remember(state: *state_t, change: surface_t) error_t!void {
    const fullscreen: u8 = @intFromBool(change.fullscreen);
    const pointer_lock: u8 = @intFromBool(change.pointer_lock);
    if (find_window(state, change.identity.window)) |index| {
        const entry = &state.table[index];
        if (change.identity.generation == entry.generation or
            generation_newer(change.identity.generation, entry.generation))
        {
            entry.* = .{
                .window = change.identity.window,
                .generation = change.identity.generation,
                .fullscreen = fullscreen,
                .pointer_lock = pointer_lock,
            };
        }
        return;
    }
    for (&state.table) |*entry| {
        if (entry.window == 0) {
            entry.* = .{
                .window = change.identity.window,
                .generation = change.identity.generation,
                .fullscreen = fullscreen,
                .pointer_lock = pointer_lock,
            };
            return;
        }
    }
    return error.WindowTableFull;
}

/// Forget a window when its destroyed generation is not older than the remembered one.
noinline fn forget(state: *state_t, id: identity_t) void {
    const index = find_window(state, id.window) orelse return;
    const entry = &state.table[index];
    if (entry.generation == id.generation or generation_newer(id.generation, entry.generation)) {
        entry.* = std.mem.zeroes(entry_t);
    }
}

fn wanted(mode: mode_t, locked_or_fullscreen: bool) bool {
    return switch (mode) {
        .Fullscreen => locked_or_fullscreen,
        .Never => false,
        .Focused => true,
    };
}

/// Destroy the live object if one exists, then clear focus and per-focus fields.
noinline fn release(state: *state_t, out: *result_t) void {
    if (phase_of(state.phase) != .None) {
        push(out, .Destroy, .{ .window = state.window, .generation = state.generation });
    }
    state.phase = @intFromEnum(phase_t.None);
    state.focused = 0;
    state.override = 0;
    state.window = 0;
    state.generation = 0;
}

/// Create or destroy the object so it exists exactly when the policy wants it.
/// The fullscreen and pointer-lock flags of the focused generation are read from
/// the table, so focus recomputes them without a new surface event.
noinline fn reconcile(state: *state_t, out: *result_t) void {
    const id = identity_t{ .window = state.window, .generation = state.generation };
    const flags = if (is_focused(state)) flags_for(state, id) else [2]bool{ false, false };
    const desired = is_focused(state) and state.override == 0 and
        wanted(mode_of(state.config), flags[0] or flags[1]);
    if (desired and phase_of(state.phase) == .None) {
        push(out, .Create, id);
        state.phase = @intFromEnum(phase_t.Pending);
    } else if (!desired and phase_of(state.phase) != .None) {
        push(out, .Destroy, id);
        state.phase = @intFromEnum(phase_t.None);
    }
}

fn finish(state: *const state_t, out: *result_t) result_t {
    out.forward_shortcuts = phase_of(state.phase) == .Active;
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
    /// A flag byte of an event is neither 0 nor 1.
    InvalidFlag = 5,
    /// A state byte is outside its range; the state was not changed.
    InvalidState = 6,
    /// The remembered-window table is full; the event was not applied.
    WindowTableFull = 7,
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
    /// surface_state with identity and both flag bytes.
    SurfaceState = 4,
    /// user_toggle with identity (window, generation).
    UserToggle = 5,
    /// window_destroy with identity (window, generation).
    WindowDestroy = 6,
    /// configure with the `mode` byte.
    Configure = 7,
};

/// C ABI event. Both flag bytes must be 0 or 1 for every kind; other bytes are
/// rejected with InvalidFlag. Fields a kind does not use must be zero.
pub const event_abi_t = extern struct {
    /// event_kind_t value.
    kind: u8,
    /// mode_t value, used only by Configure.
    mode: u8,
    /// 0 or 1: the surface is fullscreen. Used only by SurfaceState.
    fullscreen: u8,
    /// 0 or 1: the surface is pointer-locked. Used only by SurfaceState.
    pointer_lock: u8,
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

const abi_error_t = error{ NullArgument, InvalidIdentity, InvalidKind, InvalidMode, InvalidFlag, InvalidState, WindowTableFull };

noinline fn status_of(err: abi_error_t) status_t {
    return switch (err) {
        error.NullArgument => .NullArgument,
        error.InvalidIdentity => .InvalidIdentity,
        error.InvalidKind => .InvalidKind,
        error.InvalidMode => .InvalidMode,
        error.InvalidFlag => .InvalidFlag,
        error.InvalidState => .InvalidState,
        error.WindowTableFull => .WindowTableFull,
    };
}

noinline fn decode(raw: event_abi_t) abi_error_t!event_t {
    if (raw.fullscreen > 1 or raw.pointer_lock > 1) return error.InvalidFlag;
    const kind = std.meta.intToEnum(event_kind_t, raw.kind) catch return error.InvalidKind;
    const id = identity_t{ .window = raw.window, .generation = raw.generation };
    return switch (kind) {
        .FocusEnter => .{ .focus_enter = id },
        .FocusLeave => .{ .focus_leave = id },
        .InhibitorActive => .{ .inhibitor_active = id },
        .InhibitorInactive => .{ .inhibitor_inactive = id },
        .SurfaceState => .{ .surface_state = .{
            .identity = id,
            .fullscreen = raw.fullscreen != 0,
            .pointer_lock = raw.pointer_lock != 0,
        } },
        .UserToggle => .{ .user_toggle = id },
        .WindowDestroy => .{ .window_destroy = id },
        .Configure => .{ .configure = std.meta.intToEnum(mode_t, raw.mode) catch return error.InvalidMode },
    };
}

/// Reject any state byte that cannot come from this module, before it is read as an enum.
noinline fn validate_state(state: *const state_t) abi_error_t!void {
    if (state.config > 2 or state.phase > 2 or state.focused > 1 or state.override > 1) return error.InvalidState;
    for (state.table) |entry| {
        if (entry.fullscreen > 1 or entry.pointer_lock > 1) return error.InvalidState;
    }
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

/// C ABI: set the state to the zero value (policy Fullscreen, unfocused).
/// `state` is out, non-NULL. Returns Ok or NullArgument.
/// Thread safety: caller serializes calls on one state.
export fn waddle_keyboard_inhibit_init(state: ?*state_t) callconv(.C) status_t {
    const s = state orelse return .NullArgument;
    init(s);
    return .Ok;
}

/// C ABI: apply one event. `state` is inout, `event` is in, `out` is out.
/// The event is decoded and validated, then the state bytes are validated, before
/// any change. Writes `out` and state only on Ok.
/// Returns Ok, NullArgument, InvalidIdentity, InvalidKind, InvalidMode, InvalidFlag,
/// InvalidState, or WindowTableFull.
/// Thread safety: caller serializes calls on one state.
export fn waddle_keyboard_inhibit_apply(state: ?*state_t, event: ?*const event_abi_t, out: ?*result_abi_t) callconv(.C) status_t {
    const s = state orelse return .NullArgument;
    const e = event orelse return .NullArgument;
    const o = out orelse return .NullArgument;
    const decoded = decode(e.*) catch |err| return status_of(err);
    validate_state(s) catch |err| return status_of(err);
    const result = apply(s, decoded) catch |err| return status_of(err);
    o.* = encode(result);
    return .Ok;
}

test "a zero state is the fullscreen policy and creates nothing until a window is fullscreen" {
    var state: state_t = undefined;
    init(&state);
    try std.testing.expectEqual(mode_t.Fullscreen, mode_of(state.config));
    const result = try apply(&state, .{ .focus_leave = .{ .window = 1, .generation = 1 } });
    try std.testing.expectEqual(@as(u8, 0), result.count);
    try std.testing.expect(!result.forward_shortcuts);
    const id = identity_t{ .window = 1, .generation = 1 };
    _ = try apply(&state, .{ .focus_enter = id });
    try std.testing.expectEqual(phase_t.None, phase_of(state.phase));
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
    try std.testing.expectEqual(phase_t.None, phase_of(state.phase));
    const again = try apply(&state, .{ .focus_leave = .{ .window = 7, .generation = 2 } });
    try std.testing.expectEqual(@as(u8, 0), again.count);
}

test "fullscreen flag set while unfocused creates the object on the next focus enter" {
    var state: state_t = undefined;
    init(&state);
    const id = identity_t{ .window = 3, .generation = 1 };
    const unfocused = try apply(&state, .{ .surface_state = .{ .identity = id, .fullscreen = true, .pointer_lock = false } });
    try std.testing.expectEqual(@as(u8, 0), unfocused.count);

    const entered = try apply(&state, .{ .focus_enter = id });
    try std.testing.expectEqual(@as(u8, 1), entered.count);
    try std.testing.expectEqual(action_kind_t.Create, entered.actions[0].kind);
    try std.testing.expectEqual(phase_t.Pending, phase_of(state.phase));

    const windowed = try apply(&state, .{ .surface_state = .{ .identity = id, .fullscreen = false, .pointer_lock = false } });
    try std.testing.expectEqual(action_kind_t.Destroy, windowed.actions[0].kind);
    try std.testing.expectEqual(phase_t.None, phase_of(state.phase));
}

test "refocus of a fullscreen window recreates the inhibitor" {
    var state: state_t = undefined;
    init(&state);
    const id = identity_t{ .window = 4, .generation = 2 };
    _ = try apply(&state, .{ .surface_state = .{ .identity = id, .fullscreen = true, .pointer_lock = false } });
    _ = try apply(&state, .{ .focus_enter = id });
    _ = try apply(&state, .{ .inhibitor_active = id });
    const left = try apply(&state, .{ .focus_leave = id });
    try std.testing.expectEqual(action_kind_t.Destroy, left.actions[0].kind);
    try std.testing.expect(!left.forward_shortcuts);
    const back = try apply(&state, .{ .focus_enter = id });
    try std.testing.expectEqual(@as(u8, 1), back.count);
    try std.testing.expectEqual(action_kind_t.Create, back.actions[0].kind);
    try std.testing.expectEqual(phase_t.Pending, phase_of(state.phase));
}

test "pointer lock alone triggers the fullscreen policy and the two flags are ORed" {
    var state: state_t = undefined;
    init(&state);
    const id = identity_t{ .window = 5, .generation = 1 };
    _ = try apply(&state, .{ .focus_enter = id });
    const locked = try apply(&state, .{ .surface_state = .{ .identity = id, .fullscreen = false, .pointer_lock = true } });
    try std.testing.expectEqual(action_kind_t.Create, locked.actions[0].kind);

    const both = try apply(&state, .{ .surface_state = .{ .identity = id, .fullscreen = true, .pointer_lock = true } });
    try std.testing.expectEqual(@as(u8, 0), both.count);
    const unlocked = try apply(&state, .{ .surface_state = .{ .identity = id, .fullscreen = true, .pointer_lock = false } });
    try std.testing.expectEqual(@as(u8, 0), unlocked.count);
    const cleared = try apply(&state, .{ .surface_state = .{ .identity = id, .fullscreen = false, .pointer_lock = false } });
    try std.testing.expectEqual(action_kind_t.Destroy, cleared.actions[0].kind);
    try std.testing.expectEqual(phase_t.None, phase_of(state.phase));
}

test "never policy requests nothing and forwards nothing" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Never });
    const id = identity_t{ .window = 4, .generation = 1 };
    _ = try apply(&state, .{ .surface_state = .{ .identity = id, .fullscreen = true, .pointer_lock = true } });
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
    try std.testing.expectEqual(phase_t.Active, phase_of(state.phase));
    const repeated = try apply(&state, .{ .inhibitor_active = id });
    try std.testing.expect(repeated.forward_shortcuts);

    const inactive = try apply(&state, .{ .inhibitor_inactive = id });
    try std.testing.expectEqual(@as(u8, 0), inactive.count);
    try std.testing.expect(!inactive.forward_shortcuts);
    try std.testing.expectEqual(phase_t.Pending, phase_of(state.phase));
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

test "policy change while focused reconciles the object in both directions" {
    var state: state_t = undefined;
    init(&state);
    const id = identity_t{ .window = 8, .generation = 1 };
    _ = try apply(&state, .{ .focus_enter = id });
    const on = try apply(&state, .{ .configure = .Focused });
    try std.testing.expectEqual(action_kind_t.Create, on.actions[0].kind);
    const off = try apply(&state, .{ .configure = .Never });
    try std.testing.expectEqual(action_kind_t.Destroy, off.actions[0].kind);
    const fullscreen_again = try apply(&state, .{ .configure = .Fullscreen });
    try std.testing.expectEqual(@as(u8, 0), fullscreen_again.count);
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
    try std.testing.expectEqual(phase_t.Pending, phase_of(state.phase));
    const stale_leave = try apply(&state, .{ .focus_leave = first });
    try std.testing.expectEqual(@as(u8, 0), stale_leave.count);
}

test "flags of an unfocused window are kept for its later focus and not applied to others" {
    var state: state_t = undefined;
    init(&state);
    const fullscreen_window = identity_t{ .window = 1, .generation = 1 };
    const plain_window = identity_t{ .window = 2, .generation = 1 };
    _ = try apply(&state, .{ .surface_state = .{ .identity = fullscreen_window, .fullscreen = true, .pointer_lock = false } });
    const entered_plain = try apply(&state, .{ .focus_enter = plain_window });
    try std.testing.expectEqual(@as(u8, 0), entered_plain.count);
    const left_plain = try apply(&state, .{ .focus_leave = plain_window });
    try std.testing.expectEqual(@as(u8, 0), left_plain.count);
    const entered = try apply(&state, .{ .focus_enter = fullscreen_window });
    try std.testing.expectEqual(action_kind_t.Create, entered.actions[0].kind);
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
}

test "stale surface state for an older generation is not remembered" {
    var state: state_t = undefined;
    init(&state);
    const current = identity_t{ .window = 12, .generation = 5 };
    _ = try apply(&state, .{ .surface_state = .{ .identity = current, .fullscreen = false, .pointer_lock = false } });
    _ = try apply(&state, .{ .surface_state = .{ .identity = .{ .window = 12, .generation = 4 }, .fullscreen = true, .pointer_lock = true } });
    const index = find_window(&state, 12) orelse return error.TestUnexpectedResult;
    try std.testing.expectEqual(@as(u32, 5), state.table[index].generation);
    try std.testing.expectEqual(@as(u8, 0), state.table[index].fullscreen);
    try std.testing.expectEqual(@as(u8, 0), state.table[index].pointer_lock);
    const newer = try apply(&state, .{ .surface_state = .{ .identity = .{ .window = 12, .generation = 6 }, .fullscreen = true, .pointer_lock = false } });
    try std.testing.expectEqual(@as(u32, 6), state.table[index].generation);
    try std.testing.expectEqual(@as(u8, 1), state.table[index].fullscreen);
    try std.testing.expectEqual(@as(u8, 0), newer.count);
}

test "a newer generation of the focused window recomputes the focused generation's flags" {
    var state: state_t = undefined;
    init(&state);
    const focused = identity_t{ .window = 16, .generation = 2 };
    _ = try apply(&state, .{ .surface_state = .{ .identity = focused, .fullscreen = true, .pointer_lock = false } });
    _ = try apply(&state, .{ .focus_enter = focused });
    const replaced = try apply(&state, .{ .surface_state = .{ .identity = .{ .window = 16, .generation = 3 }, .fullscreen = false, .pointer_lock = false } });
    try std.testing.expectEqual(action_kind_t.Destroy, replaced.actions[0].kind);
    try std.testing.expectEqual(phase_t.None, phase_of(state.phase));
}

test "destroy of a newer generation of the focused window releases the inhibitor" {
    var state: state_t = undefined;
    init(&state);
    const focused = identity_t{ .window = 17, .generation = 2 };
    _ = try apply(&state, .{ .surface_state = .{ .identity = focused, .fullscreen = true, .pointer_lock = false } });
    _ = try apply(&state, .{ .focus_enter = focused });
    const destroyed = try apply(&state, .{ .window_destroy = .{ .window = 17, .generation = 3 } });
    try std.testing.expectEqual(@as(u8, 1), destroyed.count);
    try std.testing.expectEqual(action_kind_t.Destroy, destroyed.actions[0].kind);
    try std.testing.expectEqual(phase_t.None, phase_of(state.phase));
}

test "generation order is wrap safe across uint32 and half space is older" {
    try std.testing.expect(generation_newer(1, 0xFFFF_FFFF));
    try std.testing.expect(!generation_newer(0xFFFF_FFFF, 1));
    try std.testing.expect(!generation_newer(4, 4));
    try std.testing.expect(!generation_newer(HalfGenerationSpace, 0));
    try std.testing.expect(generation_newer(HalfGenerationSpace - 1, 0));
}

test "destroying a window destroys its inhibitor exactly once and forgets its flags" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Focused });
    const id = identity_t{ .window = 10, .generation = 1 };
    _ = try apply(&state, .{ .focus_enter = id });
    const destroyed = try apply(&state, .{ .window_destroy = id });
    try std.testing.expectEqual(@as(u8, 1), destroyed.count);
    try std.testing.expectEqual(action_kind_t.Destroy, destroyed.actions[0].kind);
    try std.testing.expectEqual(phase_t.None, phase_of(state.phase));
    const again = try apply(&state, .{ .window_destroy = id });
    try std.testing.expectEqual(@as(u8, 0), again.count);
    const leave = try apply(&state, .{ .focus_leave = id });
    try std.testing.expectEqual(@as(u8, 0), leave.count);
}

test "destroying an unfocused window forgets its remembered fullscreen flag" {
    var state: state_t = undefined;
    init(&state);
    const id = identity_t{ .window = 14, .generation = 1 };
    _ = try apply(&state, .{ .surface_state = .{ .identity = id, .fullscreen = true, .pointer_lock = false } });
    _ = try apply(&state, .{ .window_destroy = id });
    try std.testing.expect(find_window(&state, 14) == null);
    const reused = try apply(&state, .{ .focus_enter = .{ .window = 14, .generation = 2 } });
    try std.testing.expectEqual(@as(u8, 0), reused.count);
}

test "destroy of an older generation leaves the remembered newer generation" {
    var state: state_t = undefined;
    init(&state);
    const newer = identity_t{ .window = 15, .generation = 7 };
    _ = try apply(&state, .{ .surface_state = .{ .identity = newer, .fullscreen = true, .pointer_lock = false } });
    _ = try apply(&state, .{ .window_destroy = .{ .window = 15, .generation = 6 } });
    try std.testing.expect(find_window(&state, 15) != null);
    _ = try apply(&state, .{ .window_destroy = .{ .window = 15, .generation = 8 } });
    try std.testing.expect(find_window(&state, 15) == null);
}

test "the remembered-window table is bounded and a full table rejects only new windows" {
    var state: state_t = undefined;
    init(&state);
    for (0..MaxTrackedWindows) |index| {
        _ = try apply(&state, .{ .surface_state = .{ .identity = .{ .window = @intCast(index + 1), .generation = 1 }, .fullscreen = false, .pointer_lock = false } });
    }
    const before = state;
    try std.testing.expectError(error.WindowTableFull, apply(&state, .{ .surface_state = .{ .identity = .{ .window = 100, .generation = 1 }, .fullscreen = true, .pointer_lock = false } }));
    try std.testing.expectEqualDeep(before, state);
    _ = try apply(&state, .{ .surface_state = .{ .identity = .{ .window = 1, .generation = 2 }, .fullscreen = true, .pointer_lock = false } });
    _ = try apply(&state, .{ .window_destroy = .{ .window = 2, .generation = 1 } });
    _ = try apply(&state, .{ .surface_state = .{ .identity = .{ .window = 100, .generation = 1 }, .fullscreen = true, .pointer_lock = false } });
    try std.testing.expect(find_window(&state, 100) != null);
}

test "zero identities are rejected without changing state" {
    var state: state_t = undefined;
    init(&state);
    _ = try apply(&state, .{ .configure = .Focused });
    _ = try apply(&state, .{ .focus_enter = .{ .window = 13, .generation = 1 } });
    const before = state;
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .focus_enter = .{ .window = 0, .generation = 1 } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .focus_enter = .{ .window = 14, .generation = 0 } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .focus_leave = .{ .window = 0, .generation = 1 } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .window_destroy = .{ .window = 13, .generation = 0 } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .inhibitor_active = .{ .window = 0, .generation = 1 } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .inhibitor_inactive = .{ .window = 13, .generation = 0 } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .surface_state = .{ .identity = .{ .window = 0, .generation = 1 }, .fullscreen = true, .pointer_lock = false } }));
    try std.testing.expectError(error.InvalidIdentity, apply(&state, .{ .user_toggle = .{ .window = 13, .generation = 0 } }));
    try std.testing.expectEqualDeep(before, state);
}

/// Reference model for the randomized test. It tracks the same facts as the reducer
/// through a separate, simpler update rule and checks each step against them.
const reference_t = struct {
    const record_t = struct { generation: u32, fullscreen: bool, pointer_lock: bool };
    focus: ?identity_t = null,
    override: bool = false,
    config: mode_t = .Fullscreen,
    live: ?identity_t = null,
    active: bool = false,
    records: [4]?record_t = [_]?record_t{null} ** 4,

    fn is_focus(self: reference_t, id: identity_t) bool {
        const focus = self.focus orelse return false;
        return focus.window == id.window and focus.generation == id.generation;
    }

    fn flags(self: reference_t, id: identity_t) [2]bool {
        const record = self.records[@intCast(id.window)] orelse return .{ false, false };
        if (record.generation != id.generation) return .{ false, false };
        return .{ record.fullscreen, record.pointer_lock };
    }

    fn wants(self: reference_t) bool {
        const focus = self.focus orelse return false;
        if (self.override) return false;
        const locked_or_full = self.flags(focus);
        return switch (self.config) {
            .Fullscreen => locked_or_full[0] or locked_or_full[1],
            .Never => false,
            .Focused => true,
        };
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
            },
            .focus_leave => |id| {
                if (self.is_focus(id)) {
                    self.focus = null;
                    self.override = false;
                }
            },
            .window_destroy => |id| {
                if (self.records[@intCast(id.window)]) |record| {
                    if (@as(i32, @bitCast(id.generation -% record.generation)) >= 0) {
                        self.records[@intCast(id.window)] = null;
                    }
                }
                if (self.is_focus(id)) {
                    self.focus = null;
                    self.override = false;
                }
            },
            .inhibitor_active => |id| {
                if (self.is_focus(id) and self.live != null) self.active = true;
            },
            .inhibitor_inactive => |id| {
                if (self.is_focus(id)) self.active = false;
            },
            .surface_state => |change| {
                const window: usize = @intCast(change.identity.window);
                if (self.records[window]) |record| {
                    if (change.identity.generation == record.generation or
                        @as(i32, @bitCast(change.identity.generation -% record.generation)) > 0)
                    {
                        self.records[window] = .{ .generation = change.identity.generation, .fullscreen = change.fullscreen, .pointer_lock = change.pointer_lock };
                    }
                } else {
                    self.records[window] = .{ .generation = change.identity.generation, .fullscreen = change.fullscreen, .pointer_lock = change.pointer_lock };
                }
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
            23...29 => .{ .surface_state = .{ .identity = random_identity(random), .fullscreen = random.boolean(), .pointer_lock = random.boolean() } },
            30...34 => .{ .user_toggle = random_identity(random) },
            35...37 => .{ .configure = @enumFromInt(random.uintLessThan(u8, 3)) },
            else => .{ .focus_enter = .{ .window = 0, .generation = 1 } },
        };
        const before = state;
        const rejected = switch (event) {
            .focus_enter, .focus_leave, .window_destroy, .inhibitor_active, .inhibitor_inactive, .user_toggle => |id| id.window == 0 or id.generation == 0,
            .surface_state => |change| change.identity.window == 0 or change.identity.generation == 0,
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
        if (reference.live == null) reference.active = false;

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
        try std.testing.expectEqual(reference.config, mode_of(state.config));
        try std.testing.expect(!(phase_of(state.phase) != .None and !is_focused(&state)));
    }
}

/// Launders a value through memory the optimizer cannot see, so every error and
/// null path stays a live runtime branch instead of being folded out of the test.
fn rt(comptime T: type, value: T) T {
    var copy = value;
    std.mem.doNotOptimizeAway(&copy);
    return copy;
}

test "every ABI error maps to its status when the error value is opaque" {
    try std.testing.expectEqual(status_t.NullArgument, status_of(rt(abi_error_t, error.NullArgument)));
    try std.testing.expectEqual(status_t.InvalidIdentity, status_of(rt(abi_error_t, error.InvalidIdentity)));
    try std.testing.expectEqual(status_t.InvalidKind, status_of(rt(abi_error_t, error.InvalidKind)));
    try std.testing.expectEqual(status_t.InvalidMode, status_of(rt(abi_error_t, error.InvalidMode)));
    try std.testing.expectEqual(status_t.InvalidFlag, status_of(rt(abi_error_t, error.InvalidFlag)));
    try std.testing.expectEqual(status_t.InvalidState, status_of(rt(abi_error_t, error.InvalidState)));
    try std.testing.expectEqual(status_t.WindowTableFull, status_of(rt(abi_error_t, error.WindowTableFull)));
}

test "each state byte outside its range is rejected on its own" {
    var state: state_t = undefined;
    init(&state);
    try validate_state(&state);
    const out_of_range = rt(u8, 3);
    const flag_out_of_range = rt(u8, 2);
    state.config = out_of_range;
    try std.testing.expectError(error.InvalidState, validate_state(&state));
    init(&state);
    state.phase = out_of_range;
    try std.testing.expectError(error.InvalidState, validate_state(&state));
    init(&state);
    state.focused = flag_out_of_range;
    try std.testing.expectError(error.InvalidState, validate_state(&state));
    init(&state);
    state.override = flag_out_of_range;
    try std.testing.expectError(error.InvalidState, validate_state(&state));
    init(&state);
    state.table[rt(usize, 0)].fullscreen = flag_out_of_range;
    try std.testing.expectError(error.InvalidState, validate_state(&state));
    init(&state);
    state.table[rt(usize, 1)].pointer_lock = flag_out_of_range;
    try std.testing.expectError(error.InvalidState, validate_state(&state));
    init(&state);
    try validate_state(&state);
}

test "encode writes exactly the valid actions and zeroes the unused slots" {
    var result = result_t{ .actions = undefined, .count = 0, .forward_shortcuts = rt(bool, true) };
    result.count = rt(u8, 0);
    const none = encode(result);
    try std.testing.expectEqual(@as(u8, 0), none.count);
    try std.testing.expectEqual(@as(u8, 0), none.actions[0].kind);
    try std.testing.expect(none.forward_shortcuts);

    result.count = rt(u8, 1);
    result.actions[0] = .{ .kind = .Create, .identity = .{ .window = 3, .generation = 4 } };
    const one = encode(result);
    try std.testing.expectEqual(@intFromEnum(action_kind_t.Create), one.actions[0].kind);
    try std.testing.expectEqual(@as(u8, 0), one.actions[1].kind);

    result.count = rt(u8, 2);
    result.actions[1] = .{ .kind = .Destroy, .identity = .{ .window = 5, .generation = 6 } };
    const two = encode(result);
    try std.testing.expectEqual(@intFromEnum(action_kind_t.Destroy), two.actions[1].kind);
    try std.testing.expectEqual(@as(u64, 5), two.actions[1].window);
}

test "NULL pointers are rejected when they are opaque at run time" {
    var state: state_t = undefined;
    init(&state);
    const event = event_abi_t{ .kind = @intFromEnum(event_kind_t.Configure), .mode = 0, .fullscreen = 0, .pointer_lock = 0, .window = 0, .generation = 0 };
    var out = std.mem.zeroes(result_abi_t);
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_init(rt(?*state_t, null)));
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_init(&state));
    try std.testing.expectEqual(mode_t.Fullscreen, mode_of(state.config));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_apply(rt(?*state_t, null), &event, &out));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_apply(&state, rt(?*const event_abi_t, null), &out));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_apply(&state, &event, rt(?*result_abi_t, null)));
}

test "ABI struct sizes match the C header layouts" {
    try std.testing.expectEqual(@as(usize, 16), @sizeOf(entry_t));
    try std.testing.expectEqual(@as(usize, 280), @sizeOf(state_t));
    try std.testing.expectEqual(@as(usize, 24), @sizeOf(event_abi_t));
    try std.testing.expectEqual(@as(usize, 24), @sizeOf(action_abi_t));
    try std.testing.expectEqual(@as(usize, 64), @sizeOf(result_abi_t));
}

test "C ABI decodes every event kind and rejects bad kinds, modes, flags and state" {
    var state: state_t = undefined;
    init(&state);
    var out = result_abi_t{ .count = 9, .actions = undefined, .forward_shortcuts = true };
    const configure_default = event_abi_t{ .kind = @intFromEnum(event_kind_t.Configure), .mode = @intFromEnum(mode_t.Focused), .fullscreen = 0, .pointer_lock = 0, .window = 0, .generation = 0 };
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_init(null));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_apply(null, &configure_default, &out));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_apply(&state, null, &out));
    try std.testing.expectEqual(status_t.NullArgument, waddle_keyboard_inhibit_apply(&state, &configure_default, null));
    try std.testing.expectEqual(@as(u8, 9), out.count);

    const bad_kind = event_abi_t{ .kind = 200, .mode = 0, .fullscreen = 0, .pointer_lock = 0, .window = 1, .generation = 1 };
    try std.testing.expectEqual(status_t.InvalidKind, waddle_keyboard_inhibit_apply(&state, &bad_kind, &out));
    const bad_mode = event_abi_t{ .kind = @intFromEnum(event_kind_t.Configure), .mode = 3, .fullscreen = 0, .pointer_lock = 0, .window = 0, .generation = 0 };
    try std.testing.expectEqual(status_t.InvalidMode, waddle_keyboard_inhibit_apply(&state, &bad_mode, &out));
    const bad_flag = event_abi_t{ .kind = @intFromEnum(event_kind_t.SurfaceState), .mode = 0, .fullscreen = 2, .pointer_lock = 0, .window = 1, .generation = 1 };
    try std.testing.expectEqual(status_t.InvalidFlag, waddle_keyboard_inhibit_apply(&state, &bad_flag, &out));
    const bad_lock_flag = event_abi_t{ .kind = @intFromEnum(event_kind_t.SurfaceState), .mode = 0, .fullscreen = 0, .pointer_lock = 255, .window = 1, .generation = 1 };
    try std.testing.expectEqual(status_t.InvalidFlag, waddle_keyboard_inhibit_apply(&state, &bad_lock_flag, &out));
    try std.testing.expectEqual(@as(u8, 9), out.count);
    try std.testing.expectEqual(@as(u8, 0), state.focused);

    state.config = 3;
    try std.testing.expectEqual(status_t.InvalidState, waddle_keyboard_inhibit_apply(&state, &configure_default, &out));
    init(&state);
    state.table[0].fullscreen = 7;
    state.table[0].window = 1;
    try std.testing.expectEqual(status_t.InvalidState, waddle_keyboard_inhibit_apply(&state, &configure_default, &out));
    init(&state);

    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &configure_default, &out));
    try std.testing.expectEqual(@as(u8, 0), out.count);
    try std.testing.expect(!out.forward_shortcuts);

    const enter = event_abi_t{ .kind = @intFromEnum(event_kind_t.FocusEnter), .mode = 0, .fullscreen = 0, .pointer_lock = 0, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &enter, &out));
    try std.testing.expectEqual(@as(u8, 1), out.count);
    try std.testing.expectEqual(@intFromEnum(action_kind_t.Create), out.actions[0].kind);
    try std.testing.expectEqual(@as(u64, 21), out.actions[0].window);
    try std.testing.expectEqual(@as(u32, 3), out.actions[0].generation);
    try std.testing.expectEqual(@as(u8, 0), out.actions[1].kind);
    const zero_identity = event_abi_t{ .kind = @intFromEnum(event_kind_t.FocusEnter), .mode = 0, .fullscreen = 0, .pointer_lock = 0, .window = 0, .generation = 3 };
    const before = state;
    try std.testing.expectEqual(status_t.InvalidIdentity, waddle_keyboard_inhibit_apply(&state, &zero_identity, &out));
    try std.testing.expectEqualDeep(before, state);
    try std.testing.expectEqual(@as(u8, 1), out.count);

    const active = event_abi_t{ .kind = @intFromEnum(event_kind_t.InhibitorActive), .mode = 0, .fullscreen = 0, .pointer_lock = 0, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &active, &out));
    try std.testing.expect(out.forward_shortcuts);
    const inactive = event_abi_t{ .kind = @intFromEnum(event_kind_t.InhibitorInactive), .mode = 0, .fullscreen = 0, .pointer_lock = 0, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &inactive, &out));
    try std.testing.expect(!out.forward_shortcuts);
    const toggle = event_abi_t{ .kind = @intFromEnum(event_kind_t.UserToggle), .mode = 0, .fullscreen = 0, .pointer_lock = 0, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &toggle, &out));
    try std.testing.expectEqual(@intFromEnum(action_kind_t.Destroy), out.actions[0].kind);
    const surface = event_abi_t{ .kind = @intFromEnum(event_kind_t.SurfaceState), .mode = 0, .fullscreen = 1, .pointer_lock = 0, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &surface, &out));
    try std.testing.expectEqual(@as(u8, 0), out.count);
    const destroy = event_abi_t{ .kind = @intFromEnum(event_kind_t.WindowDestroy), .mode = 0, .fullscreen = 0, .pointer_lock = 0, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &destroy, &out));
    try std.testing.expectEqual(@as(u8, 0), out.count);
    const leave = event_abi_t{ .kind = @intFromEnum(event_kind_t.FocusLeave), .mode = 0, .fullscreen = 0, .pointer_lock = 0, .window = 21, .generation = 3 };
    try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &leave, &out));
    try std.testing.expectEqual(@as(u8, 0), out.count);
}

test "C ABI maps a full remembered-window table to WindowTableFull" {
    var state: state_t = undefined;
    init(&state);
    var out = std.mem.zeroes(result_abi_t);
    for (0..MaxTrackedWindows) |index| {
        const event = event_abi_t{ .kind = @intFromEnum(event_kind_t.SurfaceState), .mode = 0, .fullscreen = 1, .pointer_lock = 0, .window = @intCast(index + 1), .generation = 1 };
        try std.testing.expectEqual(status_t.Ok, waddle_keyboard_inhibit_apply(&state, &event, &out));
    }
    const overflow = event_abi_t{ .kind = @intFromEnum(event_kind_t.SurfaceState), .mode = 0, .fullscreen = 1, .pointer_lock = 0, .window = 99, .generation = 1 };
    try std.testing.expectEqual(status_t.WindowTableFull, waddle_keyboard_inhibit_apply(&state, &overflow, &out));
}
