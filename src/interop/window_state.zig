//! Bounded portable reducer for generation-safe window state transitions.
//! The module owns only fixed-size values; native platform calls remain in
//! adapters and must report their completed ordered action prefix.
const std = @import("std");
const graph = @import("window_graph.zig");

/// Frozen protocol Minimized bit.
pub const StateMinimized: u32 = graph.StateMinimized;
/// Frozen protocol Maximized bit.
pub const StateMaximized: u32 = graph.StateMaximized;
/// Frozen protocol Fullscreen bit.
pub const StateFullscreen: u32 = graph.StateFullscreen;
/// Frozen protocol host-to-guest Activated bit.
pub const StateActivated: u32 = graph.StateActivated;
/// Frozen protocol guest-to-host Resizable capability bit.
pub const StateResizable: u32 = graph.StateResizable;
/// Frozen protocol guest-to-host Maximizable capability bit.
pub const StateMaximizable: u32 = graph.StateMaximizable;
/// Frozen protocol guest-to-host Minimizable capability bit.
pub const StateMinimizable: u32 = graph.StateMinimizable;
/// Mask of every defined state or capability bit.
pub const StateMask: u32 = graph.StateMask;
/// Mask of minimized, maximized, and fullscreen state bits.
pub const WindowStateMask: u32 = StateMinimized | StateMaximized | StateFullscreen;
/// Mask of guest-derived window capabilities.
pub const CapabilityMask: u32 = StateResizable | StateMaximizable | StateMinimizable;
/// Maximum number of actions in one ordered plan; current reducer paths emit at most five.
pub const MaxActions: usize = 8;
/// Maximum accepted geometry extent in protocol physical pixels.
pub const MaxExtent: u32 = 16384;

/// Exact protocol identity for one window incarnation.
pub const window_ref_t = struct {
    /// Nonzero protocol window handle.
    window_id: u64,
    /// Nonzero incarnation generation paired with window_id.
    generation: u32,
};

/// Caller-prepared guest physical rectangle for a configure action.
pub const geometry_t = struct {
    /// Signed caller-prepared x coordinate, passed through unchanged.
    x: i32,
    /// Signed caller-prepared y coordinate, passed through unchanged.
    y: i32,
    /// Width; zero only when height is also zero, otherwise 1..16384.
    width: u32,
    /// Height; zero only when width is also zero, otherwise 1..16384.
    height: u32,
};

/// State and guest capability bits observed from the guest.
pub const guest_observation_t = struct {
    /// Frozen state plus capability bitmask; Activated is forbidden.
    state: u32,
};

/// Host configure state and optional geometry supplied by the compositor.
pub const configure_t = struct {
    /// Frozen state bitmask; capability bits are forbidden.
    state: u32,
    /// Caller-prepared geometry; paired zero extent means no resize.
    geometry: geometry_t,
};

/// One event consumed by the portable reducer.
pub const input_t = union(enum) {
    /// Fresh guest observation; capabilities are carried in guest state bits.
    GuestObservation: guest_observation_t,
    /// Host compositor configure destined for the guest native adapter.
    HostConfigure: configure_t,
    /// Guest request for host maximize policy; true means maximize.
    HostStateRequest: bool,
    /// Host close request; never implies retirement by itself.
    CloseRequest,
    /// Guest hide observation, which retires this exact generation.
    HideObserved,
    /// Guest destroy observation, which retires this exact generation.
    DestroyObserved,
};

/// Geometry action payload. maximized tells the adapter to use the frozen
/// maximized configure size policy for this rectangle.
pub const geometry_action_t = struct {
    /// Exact caller-prepared rectangle from the configure.
    rectangle: geometry_t,
    /// Whether the host configure is maximized.
    maximized: bool,
};

/// One ordered host or guest adapter operation in a plan.
pub const action_t = union(enum) {
    /// Ask the host compositor to minimize the toplevel.
    HostSetMinimized,
    /// Ask the host compositor to maximize the toplevel.
    HostSetMaximized,
    /// Ask the host compositor to unmaximize the toplevel.
    HostUnsetMaximized,
    /// Ask the host compositor to enter fullscreen.
    HostSetFullscreen,
    /// Ask the host compositor to leave fullscreen.
    HostUnsetFullscreen,
    /// Restore the guest window from minimized or maximized state.
    GuestRestore,
    /// Maximize the guest window.
    GuestMaximize,
    /// Minimize the guest window after preceding geometry actions.
    GuestMinimize,
    /// Enter guest fullscreen before applying geometry.
    GuestEnterFullscreen,
    /// Leave guest fullscreen before applying geometry.
    GuestLeaveFullscreen,
    /// Activate the guest window.
    GuestActivate,
    /// Apply configure geometry after state transitions.
    GuestSetGeometry: geometry_action_t,
    /// Post a guest WM_CLOSE request; not proof of retirement.
    GuestPostClose,
};

const pending_t = union(enum) {
    GuestObservation: struct { state: u32, capabilities: u32, host_state: u32 },
    HostConfigure: u32,
    HostStateRequest: bool,
    CloseRequest,
    Retire,
};

/// Fixed-size, generation-bound action plan. The caller owns this value and
/// returns it to complete() after reporting the executed action prefix.
pub const plan_t = struct {
    /// Identity to which every action and completion applies.
    reference: window_ref_t,
    /// Exact machine revision read during planning.
    base_revision: u64,
    /// Ordered actions; only the first action_count entries are initialized.
    actions: [MaxActions]action_t,
    /// Count of initialized ordered actions.
    action_count: usize,
    /// Internal reducer completion payload; callers must preserve this field
    /// verbatim and must not construct or edit plans.
    pending: pending_t,

    /// Return action at an in-range index, or InvalidActionIndex.
    pub fn action_at(self: *const plan_t, index: usize) error{InvalidActionIndex}!action_t {
        if (index >= self.action_count) return error.InvalidActionIndex;
        return self.actions[index];
    }

    fn append(self: *plan_t, action: action_t) void {
        // The reducer's complete action grammar is bounded to five actions;
        // MaxActions is deliberately larger to leave room for compatible
        // additions without turning normal planning into a dynamic path.
        self.actions[self.action_count] = action;
        self.action_count += 1;
    }
};

/// Owner-thread state for one exact window generation. No pointers, native
/// resources, or allocator-owned memory are retained.
pub const machine_t = struct {
    /// Bound protocol identity, fixed for the lifetime of the machine.
    reference: window_ref_t,
    /// Last guest-reported minimized/maximized/fullscreen state, not policy.
    observed_guest_state: u32,
    /// Last guest-reported capability bits.
    guest_capabilities: u32,
    /// Last fully completed guest-native state, distinct from observation.
    applied_guest_state: u32,
    /// Last fully completed persistent host state (maximized/fullscreen).
    /// Minimized is a one-shot compositor request and is never retained here.
    applied_host_state: u32,
    /// Latest host maximize-policy request, distinct from host application.
    requested_host_maximized: ?bool,
    /// Monotonic plan/completion ticket; never wraps or reuses.
    revision: u64,
    /// False after failed/partial adapter execution until fresh guest state.
    synced: bool,
    /// Diagnostic only; does not suppress later close requests.
    close_posted: bool,
    /// True only after a matching Hide or Destroy observation completes.
    retired: bool,

    /// Initialize a live machine from a validated first guest observation.
    /// The first GuestObservation must be successfully completed to reconcile
    /// host state before any other non-retirement input is accepted.
    /// window_id and generation must be nonzero; state may not include
    /// Activated or undefined bits. Returns InvalidIdentity or InvalidState.
    pub fn init(window_id: u64, generation: u32, guest_state: u32) error{ InvalidIdentity, InvalidState }!machine_t {
        if (window_id == 0 or generation == 0) return error.InvalidIdentity;
        try validate_guest_state(guest_state);
        const window_state = guest_state & WindowStateMask;
        return .{
            .reference = .{ .window_id = window_id, .generation = generation },
            .observed_guest_state = window_state,
            .guest_capabilities = guest_state & CapabilityMask,
            .applied_guest_state = window_state,
            .applied_host_state = 0,
            .requested_host_maximized = null,
            .revision = 1,
            .synced = false,
            .close_posted = false,
            .retired = false,
        };
    }

    /// Plan one event without mutating the machine. The caller must execute
    /// returned actions in order and call complete() exactly once. Errors:
    /// StaleWindow, Retired, Unsynchronized, InvalidState, InvalidGeometry,
    /// UnsupportedStateRequest, and RevisionExhausted.
    pub fn plan(self: *const machine_t, reference: window_ref_t, input: input_t) !plan_t {
        if (!same_reference(self.reference, reference)) return error.StaleWindow;
        if (self.retired) return error.Retired;
        if (self.revision == std.math.maxInt(u64)) return error.RevisionExhausted;
        const allows_unsynced = switch (input) {
            .GuestObservation, .HideObserved, .DestroyObserved => true,
            else => false,
        };
        if (!self.synced and !allows_unsynced) return error.Unsynchronized;

        var result = plan_t{ .reference = reference, .base_revision = self.revision, .actions = undefined, .action_count = 0, .pending = .CloseRequest };
        switch (input) {
            .GuestObservation => |observation| {
                try validate_guest_state(observation.state);
                const next_state = observation.state & WindowStateMask;
                const next_capabilities = observation.state & CapabilityMask;
                if (self.synced) {
                    plan_host_observation_actions(&result, self.observed_guest_state, next_state);
                } else {
                    plan_host_reconcile_actions(&result, next_state);
                }
                result.pending = .{ .GuestObservation = .{ .state = next_state, .capabilities = next_capabilities, .host_state = host_state_after_actions(self.applied_host_state, &result) } };
            },
            .HostConfigure => |configure| {
                try validate_configure(configure);
                plan_guest_configure_actions(&result, self.applied_guest_state, configure);
                var resulting_state = configure.state & WindowStateMask;
                if (configure.state & StateActivated != 0) resulting_state &= ~StateMinimized;
                result.pending = .{ .HostConfigure = resulting_state };
            },
            .HostStateRequest => |maximize| {
                if (maximize and self.guest_capabilities & StateMaximizable == 0) return error.UnsupportedStateRequest;
                const was_maximized = self.applied_host_state & StateMaximized != 0;
                if (maximize != was_maximized) result.append(if (maximize) .HostSetMaximized else .HostUnsetMaximized);
                result.pending = .{ .HostStateRequest = maximize };
            },
            .CloseRequest => {
                result.append(.GuestPostClose);
                result.pending = .CloseRequest;
            },
            .HideObserved, .DestroyObserved => result.pending = .Retire,
        }
        return result;
    }

    /// Commit one planned adapter result. completed_actions is the exact count
    /// of ordered prefix actions known to have completed. A failure marks the
    /// machine unsynchronized; only a fresh guest observation permits new
    /// adapter plans. Returns StaleWindow, Retired, StalePlan,
    /// InvalidCompletion, or RevisionExhausted without wrapping.
    pub fn complete(self: *machine_t, plan_value: plan_t, succeeded: bool, completed_actions: usize) !void {
        if (!same_reference(self.reference, plan_value.reference)) return error.StaleWindow;
        if (self.retired) return error.Retired;
        if (plan_value.base_revision != self.revision) return error.StalePlan;
        if (completed_actions > plan_value.action_count or (succeeded and completed_actions != plan_value.action_count)) return error.InvalidCompletion;
        if (self.revision == std.math.maxInt(u64)) return error.RevisionExhausted;
        if (switch (plan_value.pending) {
            .Retire => !succeeded or completed_actions != 0,
            else => false,
        }) return error.InvalidCompletion;

        self.revision += 1;
        switch (plan_value.pending) {
            .GuestObservation => |observation| {
                self.observed_guest_state = observation.state;
                self.guest_capabilities = observation.capabilities;
                // The observation is authoritative for the guest side even
                // when a host-side action in this plan failed.
                self.applied_guest_state = observation.state;
                if (succeeded) {
                    self.applied_host_state = observation.host_state;
                    self.synced = true;
                } else {
                    self.synced = false;
                }
            },
            .HostConfigure => |state| {
                if (succeeded) {
                    self.applied_guest_state = state;
                    self.synced = true;
                } else self.synced = false;
            },
            .HostStateRequest => |maximize| {
                self.requested_host_maximized = maximize;
                if (succeeded) {
                    self.applied_host_state = if (maximize) self.applied_host_state | StateMaximized else self.applied_host_state & ~StateMaximized;
                    self.synced = true;
                } else self.synced = false;
            },
            .CloseRequest => {
                if (succeeded) self.close_posted = true else self.synced = false;
            },
            .Retire => self.retired = true,
        }
    }
};

fn same_reference(left: window_ref_t, right: window_ref_t) bool {
    return left.window_id == right.window_id and left.generation == right.generation;
}

fn validate_guest_state(state: u32) error{InvalidState}!void {
    if (state & ~StateMask != 0 or state & StateActivated != 0) return error.InvalidState;
}

fn validate_configure(configure: configure_t) !void {
    const allowed = WindowStateMask | StateActivated;
    if (configure.state & ~allowed != 0) return error.InvalidState;
    const geometry = configure.geometry;
    if ((geometry.width == 0) != (geometry.height == 0) or geometry.width > MaxExtent or geometry.height > MaxExtent) return error.InvalidGeometry;
}

fn plan_host_observation_actions(result: *plan_t, previous: u32, next: u32) void {
    if (previous & StateMinimized == 0 and next & StateMinimized != 0) result.append(.HostSetMinimized);
    if ((previous ^ next) & StateMaximized != 0) result.append(if (next & StateMaximized != 0) .HostSetMaximized else .HostUnsetMaximized);
    if ((previous ^ next) & StateFullscreen != 0) result.append(if (next & StateFullscreen != 0) .HostSetFullscreen else .HostUnsetFullscreen);
}

fn plan_host_reconcile_actions(result: *plan_t, next: u32) void {
    if (next & StateMinimized != 0) result.append(.HostSetMinimized);
    result.append(if (next & StateMaximized != 0) .HostSetMaximized else .HostUnsetMaximized);
    result.append(if (next & StateFullscreen != 0) .HostSetFullscreen else .HostUnsetFullscreen);
}

fn host_state_after_actions(previous: u32, result: *const plan_t) u32 {
    var state = previous;
    for (result.actions[0..result.action_count]) |action| switch (action) {
        // xdg-shell has no persistent minimized state or unset-minimized
        // operation. This action is a one-shot request, not modeled state.
        .HostSetMinimized => {},
        .HostSetMaximized => state |= StateMaximized,
        .HostUnsetMaximized => state &= ~StateMaximized,
        .HostSetFullscreen => state |= StateFullscreen,
        .HostUnsetFullscreen => state &= ~StateFullscreen,
        else => {},
    };
    return state;
}

fn plan_guest_configure_actions(result: *plan_t, previous: u32, configure: configure_t) void {
    const next = configure.state & WindowStateMask;
    const activated = configure.state & StateActivated != 0;
    var restored = false;
    if (previous & StateMinimized != 0 and (next & StateMinimized == 0 or activated)) {
        result.append(.GuestRestore);
        restored = true;
    }
    if ((previous ^ next) & StateMaximized != 0) {
        if (next & StateMaximized != 0) {
            result.append(.GuestMaximize);
        } else if (!restored) {
            result.append(.GuestRestore);
            restored = true;
        }
    }
    if ((previous ^ next) & StateFullscreen != 0) {
        result.append(if (next & StateFullscreen != 0) .GuestEnterFullscreen else .GuestLeaveFullscreen);
    }
    if (activated) result.append(.GuestActivate);
    if (configure.geometry.width != 0) result.append(.{ .GuestSetGeometry = .{ .rectangle = configure.geometry, .maximized = next & StateMaximized != 0 } });
    // Activation while iconic is a restore-and-activate edge; a stale
    // minimized bit in the same configure must not immediately undo it.
    if (!activated and next & StateMinimized != 0 and (previous & StateMinimized == 0 or restored or (previous ^ next) & StateMaximized != 0)) result.append(.GuestMinimize);
}

// Test-only fixtures.
const testing = std.testing;

fn init_synced(window_id: u64, generation: u32, guest_state: u32) !machine_t {
    var machine = try machine_t.init(window_id, generation, guest_state);
    const initial_observation = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = guest_state } });
    try machine.complete(initial_observation, true, initial_observation.action_count);
    return machine;
}

fn expect_actions(plan_value: plan_t, expected: []const std.meta.Tag(action_t)) !void {
    try testing.expectEqual(expected.len, plan_value.action_count);
    for (expected, 0..) |tag, index| try testing.expectEqual(tag, std.meta.activeTag(try plan_value.action_at(index)));
    try testing.expectError(error.InvalidActionIndex, plan_value.action_at(plan_value.action_count));
}

test "guest observations generate minimized edges and independent state transitions" {
    var machine = try init_synced(4, 1, StateMaximizable | StateMinimizable);
    const first = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMinimized | StateMaximized | StateMaximizable | StateMinimizable } });
    try expect_actions(first, &.{ .HostSetMinimized, .HostSetMaximized });
    try machine.complete(first, true, first.action_count);
    const repeated = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMinimized | StateMaximized | StateMaximizable | StateMinimizable } });
    try expect_actions(repeated, &.{});
    try machine.complete(repeated, true, 0);
    const cleared = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMaximizable | StateMinimizable } });
    try expect_actions(cleared, &.{.HostUnsetMaximized});
    try machine.complete(cleared, true, 1);
}

test "minimize is a one-shot request and never persists in applied host state" {
    var machine = try init_synced(401, 1, 0);
    const minimize = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMinimized } });
    try expect_actions(minimize, &.{.HostSetMinimized});
    try machine.complete(minimize, true, minimize.action_count);
    try testing.expectEqual(@as(u32, 0), machine.applied_host_state & StateMinimized);

    const restore = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = 0 } });
    try expect_actions(restore, &.{});
    try machine.complete(restore, true, restore.action_count);
    try testing.expectEqual(@as(u32, 0), machine.observed_guest_state & StateMinimized);
    try testing.expectEqual(@as(u32, 0), machine.applied_host_state & StateMinimized);

    const repeated = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = 0 } });
    try expect_actions(repeated, &.{});
}

test "failed minimized reconciliation retries one-shot request without storing minimized host state" {
    var machine = try machine_t.init(402, 1, StateMinimized);
    const failed = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMinimized } });
    try expect_actions(failed, &.{ .HostSetMinimized, .HostUnsetMaximized, .HostUnsetFullscreen });
    try machine.complete(failed, false, 1);
    try testing.expect(!machine.synced);
    try testing.expectEqual(@as(u32, 0), machine.applied_host_state & StateMinimized);

    const retry = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMinimized } });
    try expect_actions(retry, &.{ .HostSetMinimized, .HostUnsetMaximized, .HostUnsetFullscreen });
    try machine.complete(retry, true, retry.action_count);
    try testing.expect(machine.synced);
    try testing.expectEqual(@as(u32, 0), machine.applied_host_state & StateMinimized);

    const stable = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMinimized } });
    try expect_actions(stable, &.{});
}

test "configure plan supports five ordered actions for iconic activation and state changes" {
    var machine = try init_synced(403, 1, StateMinimized);
    const configure = configure_t{
        .state = StateActivated | StateMaximized | StateFullscreen,
        .geometry = .{ .x = -5, .y = 9, .width = 800, .height = 600 },
    };
    const plan_value = try machine.plan(machine.reference, .{ .HostConfigure = configure });
    try expect_actions(plan_value, &.{ .GuestRestore, .GuestMaximize, .GuestEnterFullscreen, .GuestActivate, .GuestSetGeometry });
    try testing.expectEqual(@as(usize, 5), plan_value.action_count);
    try machine.complete(plan_value, true, plan_value.action_count);
    try testing.expectEqual(StateMaximized | StateFullscreen, machine.applied_guest_state);
}

test "initial guest observation reconciles host before other requests are accepted" {
    var machine = try machine_t.init(42, 1, StateMaximized | StateFullscreen);
    try testing.expect(!machine.synced);
    try testing.expectError(error.Unsynchronized, machine.plan(machine.reference, .CloseRequest));
    const initial = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMaximized | StateFullscreen } });
    try expect_actions(initial, &.{ .HostSetMaximized, .HostSetFullscreen });
    try machine.complete(initial, true, initial.action_count);
    try testing.expect(machine.synced);
    try testing.expectEqual(StateMaximized | StateFullscreen, machine.applied_host_state);
}

test "guest fullscreen edges are deduplicated and reconcile emits all required host state" {
    var machine = try init_synced(41, 4, 0);
    const enter = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateFullscreen } });
    try expect_actions(enter, &.{.HostSetFullscreen});
    try machine.complete(enter, true, 1);
    const same = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateFullscreen } });
    try expect_actions(same, &.{});
    try machine.complete(same, true, 0);
    const leave = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = 0 } });
    try expect_actions(leave, &.{.HostUnsetFullscreen});
    try machine.complete(leave, true, 1);

    machine.synced = false;
    const reconcile = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMinimized | StateFullscreen } });
    try expect_actions(reconcile, &.{ .HostSetMinimized, .HostUnsetMaximized, .HostSetFullscreen });
}

test "guest observations reject host-only and undefined state bits" {
    try testing.expectError(error.InvalidIdentity, machine_t.init(0, 1, 0));
    try testing.expectError(error.InvalidIdentity, machine_t.init(1, 0, 0));
    try testing.expectError(error.InvalidState, machine_t.init(1, 1, StateActivated));
    var machine = try init_synced(1, 1, 0);
    try testing.expectError(error.InvalidState, machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateActivated } }));
    try testing.expectError(error.InvalidState, machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMask + 1 } }));
}

test "host configure orders restore maximize fullscreen activation and geometry" {
    var machine = try init_synced(9, 3, StateMinimized | StateMaximized | StateMaximizable | StateMinimizable);
    const configure = configure_t{ .state = StateActivated | StateMaximized | StateFullscreen, .geometry = .{ .x = -12, .y = 34, .width = 1200, .height = 800 } };
    const plan_value = try machine.plan(machine.reference, .{ .HostConfigure = configure });
    try expect_actions(plan_value, &.{ .GuestRestore, .GuestEnterFullscreen, .GuestActivate, .GuestSetGeometry });
    const geometry_action = try plan_value.action_at(3);
    try testing.expectEqualDeep(geometry_action_t{ .rectangle = configure.geometry, .maximized = true }, geometry_action.GuestSetGeometry);
    try machine.complete(plan_value, true, plan_value.action_count);
    try testing.expectEqual(StateMaximized | StateFullscreen, machine.applied_guest_state);
}

test "host configure applies new maximize fullscreen activation and geometry in fixed order" {
    var machine = try init_synced(91, 2, 0);
    const configure = configure_t{ .state = StateActivated | StateMaximized | StateFullscreen, .geometry = .{ .x = std.math.minInt(i32), .y = std.math.maxInt(i32), .width = MaxExtent, .height = MaxExtent } };
    const plan_value = try machine.plan(machine.reference, .{ .HostConfigure = configure });
    try expect_actions(plan_value, &.{ .GuestMaximize, .GuestEnterFullscreen, .GuestActivate, .GuestSetGeometry });
    try testing.expectEqualDeep(configure.geometry, (try plan_value.action_at(3)).GuestSetGeometry.rectangle);
}

test "maximize clear restores before fullscreen leave and geometry" {
    var machine = try init_synced(10, 1, StateMaximized | StateFullscreen);
    const configure = configure_t{ .state = 0, .geometry = .{ .x = 2, .y = 3, .width = 40, .height = 50 } };
    const plan_value = try machine.plan(machine.reference, .{ .HostConfigure = configure });
    try expect_actions(plan_value, &.{ .GuestRestore, .GuestLeaveFullscreen, .GuestSetGeometry });
    try machine.complete(plan_value, true, plan_value.action_count);
}

test "maximized and minimized coexist and configure can re-minimize after transition" {
    var machine = try init_synced(11, 1, StateMinimized | StateMaximizable);
    const configure = configure_t{ .state = StateMinimized | StateMaximized, .geometry = .{ .x = 0, .y = 0, .width = 900, .height = 700 } };
    const plan_value = try machine.plan(machine.reference, .{ .HostConfigure = configure });
    try expect_actions(plan_value, &.{ .GuestMaximize, .GuestSetGeometry, .GuestMinimize });
    try machine.complete(plan_value, true, plan_value.action_count);
    try testing.expectEqual(StateMinimized | StateMaximized, machine.applied_guest_state);
}

test "activation of an iconic window restores and activates without re-minimizing" {
    var machine = try init_synced(111, 1, StateMinimized | StateMaximizable);
    const configure = configure_t{ .state = StateActivated | StateMinimized, .geometry = .{ .x = 0, .y = 0, .width = 0, .height = 0 } };
    const plan_value = try machine.plan(machine.reference, .{ .HostConfigure = configure });
    try expect_actions(plan_value, &.{ .GuestRestore, .GuestActivate });
    try machine.complete(plan_value, true, plan_value.action_count);
    try testing.expectEqual(@as(u32, 0), machine.applied_guest_state);
    const repeated = try machine.plan(machine.reference, .{ .HostConfigure = .{ .state = StateActivated | StateMinimized, .geometry = configure.geometry } });
    try expect_actions(repeated, &.{.GuestActivate});
}

test "clearing maximize while already restoring iconic state does not issue restore twice" {
    var machine = try init_synced(112, 1, StateMinimized | StateMaximized);
    const plan_value = try machine.plan(machine.reference, .{ .HostConfigure = .{ .state = 0, .geometry = .{ .x = 0, .y = 0, .width = 0, .height = 0 } } });
    try expect_actions(plan_value, &.{.GuestRestore});
}

test "zero geometry skips resize and invalid partial or oversized geometry fails" {
    var machine = try init_synced(12, 1, 0);
    const no_geometry = try machine.plan(machine.reference, .{ .HostConfigure = .{ .state = StateMaximized, .geometry = .{ .x = 40, .y = -20, .width = 0, .height = 0 } } });
    try expect_actions(no_geometry, &.{.GuestMaximize});
    try testing.expectError(error.InvalidGeometry, machine.plan(machine.reference, .{ .HostConfigure = .{ .state = 0, .geometry = .{ .x = 0, .y = 0, .width = 0, .height = 1 } } }));
    try testing.expectError(error.InvalidGeometry, machine.plan(machine.reference, .{ .HostConfigure = .{ .state = 0, .geometry = .{ .x = 0, .y = 0, .width = 1, .height = 16385 } } }));
    const maximum = try machine.plan(machine.reference, .{ .HostConfigure = .{ .state = 0, .geometry = .{ .x = 0, .y = 0, .width = MaxExtent, .height = MaxExtent } } });
    try expect_actions(maximum, &.{.GuestSetGeometry});
    try testing.expectError(error.InvalidState, machine.plan(machine.reference, .{ .HostConfigure = .{ .state = StateResizable, .geometry = .{ .x = 0, .y = 0, .width = 0, .height = 0 } } }));
}

test "host maximize request updates host policy only and requires capability" {
    var machine = try init_synced(13, 1, StateMaximizable);
    const request = try machine.plan(machine.reference, .{ .HostStateRequest = true });
    try expect_actions(request, &.{.HostSetMaximized});
    try machine.complete(request, true, 1);
    try testing.expectEqual(@as(?bool, true), machine.requested_host_maximized);
    try testing.expectEqual(@as(u32, 0), machine.applied_guest_state);
    const unchanged_observation = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMaximizable } });
    try expect_actions(unchanged_observation, &.{});
    try machine.complete(unchanged_observation, true, 0);
    try testing.expectEqual(StateMaximized, machine.applied_host_state & StateMaximized);
    const same = try machine.plan(machine.reference, .{ .HostStateRequest = true });
    try expect_actions(same, &.{});
    try machine.complete(same, true, 0);
    const guest_maximize = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMaximizable | StateMaximized } });
    try expect_actions(guest_maximize, &.{.HostSetMaximized});
    try machine.complete(guest_maximize, true, 1);
    const guest_unmaximize = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMaximizable } });
    try expect_actions(guest_unmaximize, &.{.HostUnsetMaximized});
    try machine.complete(guest_unmaximize, true, 1);
    const reissue = try machine.plan(machine.reference, .{ .HostStateRequest = true });
    try expect_actions(reissue, &.{.HostSetMaximized});
    var incapable = try init_synced(14, 1, 0);
    try testing.expectError(error.UnsupportedStateRequest, incapable.plan(incapable.reference, .{ .HostStateRequest = true }));
    const unmaximize = try incapable.plan(incapable.reference, .{ .HostStateRequest = false });
    try expect_actions(unmaximize, &.{});

    const host_maximize = try incapable.plan(incapable.reference, .{ .GuestObservation = .{ .state = StateMaximizable | StateMaximized } });
    try incapable.complete(host_maximize, true, host_maximize.action_count);
    const host_unmaximize = try incapable.plan(incapable.reference, .{ .HostStateRequest = false });
    try expect_actions(host_unmaximize, &.{.HostUnsetMaximized});
    try incapable.complete(host_unmaximize, true, 1);
    try testing.expectEqual(@as(?bool, false), incapable.requested_host_maximized);
}

test "close may be requested repeatedly but hide and destroy alone retire" {
    var machine = try init_synced(15, 1, 0);
    const close_first = try machine.plan(machine.reference, .CloseRequest);
    try expect_actions(close_first, &.{.GuestPostClose});
    try machine.complete(close_first, true, 1);
    const close_again = try machine.plan(machine.reference, .CloseRequest);
    try expect_actions(close_again, &.{.GuestPostClose});
    try machine.complete(close_again, true, 1);
    try testing.expect(!machine.retired);
    const hide = try machine.plan(machine.reference, .HideObserved);
    try expect_actions(hide, &.{});
    try machine.complete(hide, true, 0);
    try testing.expect(machine.retired);
    try testing.expectError(error.Retired, machine.plan(machine.reference, .CloseRequest));
}

test "generation checks stale plans and completion revisions" {
    var machine = try init_synced(16, 2, 0);
    try testing.expectError(error.StaleWindow, machine.plan(.{ .window_id = 16, .generation = 1 }, .CloseRequest));
    const old = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMinimized } });
    const other = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMaximized } });
    try machine.complete(other, true, other.action_count);
    try testing.expectError(error.StalePlan, machine.complete(old, true, old.action_count));
}

test "partial adapter execution blocks requests until fresh observation reconciles" {
    var machine = try init_synced(17, 1, 0);
    const configure = try machine.plan(machine.reference, .{ .HostConfigure = .{ .state = StateMaximized | StateFullscreen, .geometry = .{ .x = 0, .y = 0, .width = 10, .height = 20 } } });
    try testing.expectEqual(@as(usize, 3), configure.action_count);
    try machine.complete(configure, false, 1);
    try testing.expect(!machine.synced);
    try testing.expectError(error.Unsynchronized, machine.plan(machine.reference, .{ .HostConfigure = .{ .state = 0, .geometry = .{ .x = 0, .y = 0, .width = 0, .height = 0 } } }));
    const observation = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMaximized } });
    try expect_actions(observation, &.{ .HostSetMaximized, .HostUnsetFullscreen });
    try machine.complete(observation, true, observation.action_count);
    try testing.expect(machine.synced);
    try testing.expectEqual(StateMaximized, machine.applied_guest_state);
}

test "completion validates action prefix and refuses revision wrap" {
    var machine = try init_synced(18, 1, 0);
    const plan_value = try machine.plan(machine.reference, .CloseRequest);
    try testing.expectError(error.InvalidCompletion, machine.complete(plan_value, true, 0));
    try machine.complete(plan_value, false, 0);
    const after_failure = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = 0 } });
    try machine.complete(after_failure, true, after_failure.action_count);
    var exhausted = try init_synced(19, 1, 0);
    exhausted.revision = std.math.maxInt(u64);
    try testing.expectError(error.RevisionExhausted, exhausted.plan(exhausted.reference, .CloseRequest));
    const last_plan = plan_t{ .reference = exhausted.reference, .base_revision = exhausted.revision, .actions = undefined, .action_count = 0, .pending = .CloseRequest };
    try testing.expectError(error.RevisionExhausted, exhausted.complete(last_plan, true, 0));
    try testing.expectEqual(std.math.maxInt(u64), exhausted.revision);
}

test "completion refuses foreign identity, oversized prefix, failed retirement, and retired result" {
    var machine = try init_synced(43, 7, 0);
    const close = try machine.plan(machine.reference, .CloseRequest);
    try testing.expectError(error.StaleWindow, machine.complete(.{ .reference = .{ .window_id = 44, .generation = 7 }, .base_revision = close.base_revision, .actions = close.actions, .action_count = close.action_count, .pending = close.pending }, true, 1));
    try testing.expectError(error.InvalidCompletion, machine.complete(close, false, close.action_count + 1));
    const retire = try machine.plan(machine.reference, .HideObserved);
    try testing.expectError(error.InvalidCompletion, machine.complete(retire, false, 0));
    try machine.complete(retire, true, 0);
    try testing.expectError(error.Retired, machine.complete(close, true, 1));
}

test "failed observation, maximize request, and close each require a fresh observation" {
    var machine = try machine_t.init(44, 8, StateMaximizable);
    const initial = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMaximizable } });
    try machine.complete(initial, false, 0);
    try testing.expect(!machine.synced);
    const reconcile = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMaximizable } });
    try machine.complete(reconcile, true, reconcile.action_count);

    const request = try machine.plan(machine.reference, .{ .HostStateRequest = true });
    try machine.complete(request, false, 0);
    try testing.expect(!machine.synced);
    const after_request = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMaximizable } });
    try machine.complete(after_request, true, after_request.action_count);

    const close = try machine.plan(machine.reference, .CloseRequest);
    try machine.complete(close, false, 0);
    try testing.expect(!machine.synced);
    try testing.expectError(error.Unsynchronized, machine.plan(machine.reference, .CloseRequest));
    const after_close = try machine.plan(machine.reference, .{ .GuestObservation = .{ .state = StateMaximizable } });
    try machine.complete(after_close, true, after_close.action_count);
    try testing.expect(machine.synced);
}

test "host state tracking applies host actions and ignores guest-side actions" {
    var plan_value = plan_t{ .reference = .{ .window_id = 1, .generation = 1 }, .base_revision = 1, .actions = undefined, .action_count = 0, .pending = .CloseRequest };
    plan_value.append(.HostSetMaximized);
    plan_value.append(.GuestActivate);
    plan_value.append(.HostUnsetFullscreen);
    try testing.expectEqual(StateMaximized, host_state_after_actions(StateFullscreen, &plan_value));
}
