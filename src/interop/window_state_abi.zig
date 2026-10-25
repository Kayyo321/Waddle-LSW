//! C ABI boundary of the portable window-state reducer (`src/interop/window_state.h`).
//!
//! Each exported function is a thin wrapper around one public operation of `window_state.zig`.
//! The wrapper adds only the work a C caller requires:
//!
//!  * Every caller pointer must be non-NULL where the header requires it and naturally aligned for
//!    its type. Inputs are copied by value. Every output is staged in a local and copied to the
//!    caller only on the statuses the header documents.
//!  * Enum-typed fields arrive as raw `u32`. Unknown values are rejected before a Zig enum or
//!    union is formed from them. Boolean fields accept exactly 0 and 1.
//!  * A plan is a fixed-size struct, not a Zig value with a private completion payload. Plan
//!    encodes the reducer's private `pending` payload into flat fields, and complete decodes it
//!    back. Every plan is sealed with a per-handle keyed hash over its meaningful fields, so
//!    a copy edited by the caller, or a plan issued by another handle, is `InvalidPlan` before
//!    the reducer runs. The seal detects accidental edits and cross-handle replay; it is not a
//!    cryptographic authenticator, and a caller that can read the handle's memory is outside
//!    the trust boundary.
//!  * The reducer's own checks (identity, retirement, revision, synchronization, completion
//!    prefix) remain authoritative and run unchanged after the ABI checks.
//!
//! Allocation: `create` is the only allocating function. It takes exactly one allocation from
//! `abi_allocator` and `destroy` frees it through the same allocator. The default is the C
//! allocator, so libc-based sanitizers see every allocation.
//!
//! Threading: a handle is owned by one caller at a time. Calls on distinct handles are
//! independent. `plan` and `snapshot` only read the handle; `complete` and `destroy` mutate or free
//! it, so the caller must serialize every call that touches one handle.
const std = @import("std");
const reducer = @import("window_state.zig");

const c = @cImport({
    @cInclude("window_state.h");
});

/// Protocol header, imported only to check that the header's state bits equal the frozen protocol values.
const proto = @cImport({
    @cInclude("waddle/interop_protocol.h");
});

/// Owner of one reducer machine and of the seed that keys its plan seals.
const handle_t = struct {
    /// The reducer state for one exact window generation.
    machine: reducer.machine_t,
    /// Per-handle key for `plan_seal`, distinct for every handle created in this process; never exposed to C.
    seal_seed: u64,
};

/// Process-wide count of handles created. Each create takes the next value, so no two handles
/// share a seal seed until the count wraps after 2^64 creations. An atomic add cannot fail,
/// and creation performs no OS call.
var seal_epoch = std.atomic.Value(u64).init(0);

/// Bijective 64-bit mixer (splitmix64 finalizer). Each step is invertible, so distinct epochs
/// give distinct seeds, and the seeds do not follow the creation order visibly.
fn mix64(value: u64) u64 {
    var z = value +% 0x9e3779b97f4a7c15;
    z = (z ^ (z >> 30)) *% 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) *% 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

/// Allocation seam of the ABI. Production never writes it. Tests swap it to observe and fail
/// allocations. It is not synchronized, so a test must swap it while no other thread calls `create`.
var abi_allocator: std.mem.Allocator = std.heap.c_allocator;

/// Status code type of every ABI function.
const status_t = c.waddle_window_state_status_t;

/// Raw input kind values from the header. Each member equals its header constant.
const input_kind_t = enum(u32) {
    GuestObservation = c.WaddleWindowStateInputGuestObservation,
    HostConfigure = c.WaddleWindowStateInputHostConfigure,
    HostStateRequest = c.WaddleWindowStateInputHostStateRequest,
    CloseRequest = c.WaddleWindowStateInputCloseRequest,
    HideObserved = c.WaddleWindowStateInputHideObserved,
    DestroyObserved = c.WaddleWindowStateInputDestroyObserved,
};

/// Raw action kind values from the header, in the reducer's `action_t` order.
const action_kind_t = enum(u32) {
    HostSetMinimized = c.WaddleWindowStateActionHostSetMinimized,
    HostSetMaximized = c.WaddleWindowStateActionHostSetMaximized,
    HostUnsetMaximized = c.WaddleWindowStateActionHostUnsetMaximized,
    HostSetFullscreen = c.WaddleWindowStateActionHostSetFullscreen,
    HostUnsetFullscreen = c.WaddleWindowStateActionHostUnsetFullscreen,
    GuestRestore = c.WaddleWindowStateActionGuestRestore,
    GuestMaximize = c.WaddleWindowStateActionGuestMaximize,
    GuestMinimize = c.WaddleWindowStateActionGuestMinimize,
    GuestEnterFullscreen = c.WaddleWindowStateActionGuestEnterFullscreen,
    GuestLeaveFullscreen = c.WaddleWindowStateActionGuestLeaveFullscreen,
    GuestActivate = c.WaddleWindowStateActionGuestActivate,
    GuestSetGeometry = c.WaddleWindowStateActionGuestSetGeometry,
    GuestPostClose = c.WaddleWindowStateActionGuestPostClose,
};

/// Raw completion payload kind values from the header.
const pending_kind_t = enum(u32) {
    GuestObservation = c.WaddleWindowStatePendingGuestObservation,
    HostConfigure = c.WaddleWindowStatePendingHostConfigure,
    HostStateRequest = c.WaddleWindowStatePendingHostStateRequest,
    CloseRequest = c.WaddleWindowStatePendingCloseRequest,
    Retire = c.WaddleWindowStatePendingRetire,
};

/// The reducer's private completion payload type, named without exporting it.
const pending_t = std.meta.FieldType(reducer.plan_t, .pending);

/// Errors that the input decoder can produce.
const input_error_t = error{InvalidArgument};
/// Errors that plan decoding can produce.
const plan_decode_error_t = error{InvalidPlan};
/// Errors that `machine_t.init` can produce.
const init_error_t = error{ InvalidIdentity, InvalidState };
/// Errors that `machine_t.plan` can produce.
const plan_error_t = error{ StaleWindow, Retired, RevisionExhausted, Unsynchronized, InvalidState, InvalidGeometry, UnsupportedStateRequest };
/// Errors that `machine_t.complete` can produce.
const complete_error_t = error{ StaleWindow, Retired, StalePlan, InvalidCompletion, RevisionExhausted };

/// Checks one struct's size, alignment and every named field offset against literal values.
/// Each table entry is `.{ field name, byte offset }`.
fn check_layout(comptime T: type, comptime size: usize, comptime alignment: usize, comptime table: anytype) void {
    if (@sizeOf(T) != size or @alignOf(T) != alignment) @compileError("window-state struct size or alignment differs from the contract");
    inline for (table) |entry| {
        if (@offsetOf(T, entry[0]) != entry[1]) @compileError("window-state field offset differs from the contract: " ++ entry[0]);
    }
}

/// Checks each named header constant against its literal value. Each table entry is `.{ name, value }`.
fn check_values(comptime table: anytype) void {
    inline for (table) |entry| {
        if (@field(c, entry[0]) != entry[1]) @compileError("window-state header value differs from the contract: " ++ entry[0]);
    }
}

comptime {
    if (c.WaddleWindowStateMinimized != proto.InteropStateMinimized or c.WaddleWindowStateMinimized != reducer.StateMinimized)
        @compileError("window-state Minimized bit differs from the protocol");
    if (c.WaddleWindowStateMaximized != proto.InteropStateMaximized or c.WaddleWindowStateMaximized != reducer.StateMaximized)
        @compileError("window-state Maximized bit differs from the protocol");
    if (c.WaddleWindowStateFullscreen != proto.InteropStateFullscreen or c.WaddleWindowStateFullscreen != reducer.StateFullscreen)
        @compileError("window-state Fullscreen bit differs from the protocol");
    if (c.WaddleWindowStateActivated != proto.InteropStateActivated or c.WaddleWindowStateActivated != reducer.StateActivated)
        @compileError("window-state Activated bit differs from the protocol");
    if (c.WaddleWindowStateResizable != proto.InteropStateResizable or c.WaddleWindowStateResizable != reducer.StateResizable)
        @compileError("window-state Resizable bit differs from the protocol");
    if (c.WaddleWindowStateMaximizable != proto.InteropStateMaximizable or c.WaddleWindowStateMaximizable != reducer.StateMaximizable)
        @compileError("window-state Maximizable bit differs from the protocol");
    if (c.WaddleWindowStateMinimizable != proto.InteropStateMinimizable or c.WaddleWindowStateMinimizable != reducer.StateMinimizable)
        @compileError("window-state Minimizable bit differs from the protocol");
    if (c.WaddleWindowStateMask != proto.InteropStateMask or c.WaddleWindowStateMask != reducer.StateMask)
        @compileError("window-state mask differs from the protocol");
    if (c.WaddleWindowStateWindowMask != reducer.WindowStateMask or c.WaddleWindowStateCapabilityMask != reducer.CapabilityMask)
        @compileError("window-state derived masks differ from the reducer");
}

comptime {
    if (@sizeOf(usize) != 8) @compileError("window-state ABI requires a 64-bit target");
    if (reducer.MaxActions != c.WaddleWindowStateMaxActions) @compileError("window-state ABI action capacity differs from the reducer");
    check_layout(c.waddle_window_state_input_t, 28, 4, .{
        .{ "kind", 0 }, .{ "state", 4 }, .{ "x", 8 }, .{ "y", 12 },
        .{ "width", 16 }, .{ "height", 20 }, .{ "maximize", 24 },
    });
    check_layout(c.waddle_window_state_action_t, 24, 4, .{
        .{ "kind", 0 }, .{ "x", 4 }, .{ "y", 8 }, .{ "width", 12 },
        .{ "height", 16 }, .{ "maximized", 20 },
    });
    check_layout(c.waddle_window_state_plan_t, 248, 8, .{
        .{ "window_id", 0 }, .{ "base_revision", 8 }, .{ "action_count", 16 }, .{ "seal", 24 },
        .{ "generation", 32 }, .{ "pending_kind", 36 }, .{ "pending_state", 40 },
        .{ "pending_capabilities", 44 }, .{ "pending_host_state", 48 }, .{ "actions", 52 },
    });
    check_layout(c.waddle_window_state_snapshot_t, 56, 8, .{
        .{ "window_id", 0 }, .{ "revision", 8 }, .{ "generation", 16 },
        .{ "observed_guest_state", 20 }, .{ "applied_guest_state", 24 }, .{ "applied_host_state", 28 },
        .{ "guest_capabilities", 32 }, .{ "host_request", 36 }, .{ "synced", 40 },
        .{ "close_posted", 44 }, .{ "retired", 48 },
    });
    check_values(.{
        .{ "WaddleWindowStateOk", 0 },
        .{ "WaddleWindowStateInvalidArgument", 1 },
        .{ "WaddleWindowStateOutOfMemory", 2 },
        .{ "WaddleWindowStateInvalidIdentity", 3 },
        .{ "WaddleWindowStateInvalidState", 4 },
        .{ "WaddleWindowStateInvalidGeometry", 5 },
        .{ "WaddleWindowStateStaleWindow", 6 },
        .{ "WaddleWindowStateStalePlan", 7 },
        .{ "WaddleWindowStateRetired", 8 },
        .{ "WaddleWindowStateUnsynchronized", 9 },
        .{ "WaddleWindowStateUnsupportedStateRequest", 10 },
        .{ "WaddleWindowStateRevisionExhausted", 11 },
        .{ "WaddleWindowStateInvalidCompletion", 12 },
        .{ "WaddleWindowStateInvalidPlan", 13 },
        .{ "WaddleWindowStateInputGuestObservation", 1 },
        .{ "WaddleWindowStateInputHostConfigure", 2 },
        .{ "WaddleWindowStateInputHostStateRequest", 3 },
        .{ "WaddleWindowStateInputCloseRequest", 4 },
        .{ "WaddleWindowStateInputHideObserved", 5 },
        .{ "WaddleWindowStateInputDestroyObserved", 6 },
        .{ "WaddleWindowStateActionHostSetMinimized", 1 },
        .{ "WaddleWindowStateActionHostSetMaximized", 2 },
        .{ "WaddleWindowStateActionHostUnsetMaximized", 3 },
        .{ "WaddleWindowStateActionHostSetFullscreen", 4 },
        .{ "WaddleWindowStateActionHostUnsetFullscreen", 5 },
        .{ "WaddleWindowStateActionGuestRestore", 6 },
        .{ "WaddleWindowStateActionGuestMaximize", 7 },
        .{ "WaddleWindowStateActionGuestMinimize", 8 },
        .{ "WaddleWindowStateActionGuestEnterFullscreen", 9 },
        .{ "WaddleWindowStateActionGuestLeaveFullscreen", 10 },
        .{ "WaddleWindowStateActionGuestActivate", 11 },
        .{ "WaddleWindowStateActionGuestSetGeometry", 12 },
        .{ "WaddleWindowStateActionGuestPostClose", 13 },
        .{ "WaddleWindowStatePendingGuestObservation", 1 },
        .{ "WaddleWindowStatePendingHostConfigure", 2 },
        .{ "WaddleWindowStatePendingHostStateRequest", 3 },
        .{ "WaddleWindowStatePendingCloseRequest", 4 },
        .{ "WaddleWindowStatePendingRetire", 5 },
        .{ "WaddleWindowStateHostRequestNone", 0 },
        .{ "WaddleWindowStateHostRequestUnmaximize", 1 },
        .{ "WaddleWindowStateHostRequestMaximize", 2 },
    });
}

/// Maps an init error to its header status.
fn status_of_init_error(err: init_error_t) status_t {
    return switch (err) {
        error.InvalidIdentity => c.WaddleWindowStateInvalidIdentity,
        error.InvalidState => c.WaddleWindowStateInvalidState,
    };
}

/// Maps an input decoding error to its header status.
fn status_of_input_error(err: input_error_t) status_t {
    return switch (err) {
        error.InvalidArgument => c.WaddleWindowStateInvalidArgument,
    };
}

/// Maps a plan error to its header status.
fn status_of_plan_error(err: plan_error_t) status_t {
    return switch (err) {
        error.StaleWindow => c.WaddleWindowStateStaleWindow,
        error.Retired => c.WaddleWindowStateRetired,
        error.RevisionExhausted => c.WaddleWindowStateRevisionExhausted,
        error.Unsynchronized => c.WaddleWindowStateUnsynchronized,
        error.InvalidState => c.WaddleWindowStateInvalidState,
        error.InvalidGeometry => c.WaddleWindowStateInvalidGeometry,
        error.UnsupportedStateRequest => c.WaddleWindowStateUnsupportedStateRequest,
    };
}

/// Maps a completion error to its header status.
fn status_of_complete_error(err: complete_error_t) status_t {
    return switch (err) {
        error.StaleWindow => c.WaddleWindowStateStaleWindow,
        error.Retired => c.WaddleWindowStateRetired,
        error.StalePlan => c.WaddleWindowStateStalePlan,
        error.InvalidCompletion => c.WaddleWindowStateInvalidCompletion,
        error.RevisionExhausted => c.WaddleWindowStateRevisionExhausted,
    };
}

/// Maps a plan decoding error to its header status.
fn status_of_decode_error(err: plan_decode_error_t) status_t {
    return switch (err) {
        error.InvalidPlan => c.WaddleWindowStateInvalidPlan,
    };
}

/// Converts a header boolean word. Only 0 and 1 are valid.
fn bool_from_u32(value: u32) input_error_t!bool {
    return switch (value) {
        0 => false,
        1 => true,
        else => error.InvalidArgument,
    };
}

/// Decodes a raw input into the reducer's `input_t`. Unknown kinds and non-boolean maximize
/// words are InvalidArgument.
fn decode_input(raw: c.waddle_window_state_input_t) input_error_t!reducer.input_t {
    const kind = std.meta.intToEnum(input_kind_t, raw.kind) catch return error.InvalidArgument;
    return switch (kind) {
        .GuestObservation => .{ .GuestObservation = .{ .state = raw.state } },
        .HostConfigure => .{ .HostConfigure = .{
            .state = raw.state,
            .geometry = .{ .x = raw.x, .y = raw.y, .width = raw.width, .height = raw.height },
        } },
        .HostStateRequest => .{ .HostStateRequest = try bool_from_u32(raw.maximize) },
        .CloseRequest => .CloseRequest,
        .HideObserved => .HideObserved,
        .DestroyObserved => .DestroyObserved,
    };
}

/// Writes the reducer's private completion payload into the flat plan fields.
fn encode_pending(pending: pending_t, out: *c.waddle_window_state_plan_t) void {
    switch (pending) {
        .GuestObservation => |observation| {
            out.pending_kind = @intFromEnum(pending_kind_t.GuestObservation);
            out.pending_state = observation.state;
            out.pending_capabilities = observation.capabilities;
            out.pending_host_state = observation.host_state;
        },
        .HostConfigure => |state| {
            out.pending_kind = @intFromEnum(pending_kind_t.HostConfigure);
            out.pending_state = state;
        },
        .HostStateRequest => |maximize| {
            out.pending_kind = @intFromEnum(pending_kind_t.HostStateRequest);
            out.pending_state = @intFromBool(maximize);
        },
        .CloseRequest => out.pending_kind = @intFromEnum(pending_kind_t.CloseRequest),
        .Retire => out.pending_kind = @intFromEnum(pending_kind_t.Retire),
    }
}

/// Reads the flat plan fields back into the reducer's private completion payload.
fn decode_pending(raw: *const c.waddle_window_state_plan_t) plan_decode_error_t!pending_t {
    const kind = std.meta.intToEnum(pending_kind_t, raw.pending_kind) catch return error.InvalidPlan;
    return switch (kind) {
        .GuestObservation => .{ .GuestObservation = .{
            .state = raw.pending_state,
            .capabilities = raw.pending_capabilities,
            .host_state = raw.pending_host_state,
        } },
        .HostConfigure => .{ .HostConfigure = raw.pending_state },
        .HostStateRequest => .{ .HostStateRequest = bool_from_u32(raw.pending_state) catch return error.InvalidPlan },
        .CloseRequest => .CloseRequest,
        .Retire => .Retire,
    };
}

/// Writes one reducer action into its fixed-size form. Fields other than the geometry
/// payload are zero.
fn encode_action(action: reducer.action_t) c.waddle_window_state_action_t {
    var out = std.mem.zeroes(c.waddle_window_state_action_t);
    switch (action) {
        .HostSetMinimized => out.kind = @intFromEnum(action_kind_t.HostSetMinimized),
        .HostSetMaximized => out.kind = @intFromEnum(action_kind_t.HostSetMaximized),
        .HostUnsetMaximized => out.kind = @intFromEnum(action_kind_t.HostUnsetMaximized),
        .HostSetFullscreen => out.kind = @intFromEnum(action_kind_t.HostSetFullscreen),
        .HostUnsetFullscreen => out.kind = @intFromEnum(action_kind_t.HostUnsetFullscreen),
        .GuestRestore => out.kind = @intFromEnum(action_kind_t.GuestRestore),
        .GuestMaximize => out.kind = @intFromEnum(action_kind_t.GuestMaximize),
        .GuestMinimize => out.kind = @intFromEnum(action_kind_t.GuestMinimize),
        .GuestEnterFullscreen => out.kind = @intFromEnum(action_kind_t.GuestEnterFullscreen),
        .GuestLeaveFullscreen => out.kind = @intFromEnum(action_kind_t.GuestLeaveFullscreen),
        .GuestActivate => out.kind = @intFromEnum(action_kind_t.GuestActivate),
        .GuestSetGeometry => |payload| {
            out.kind = @intFromEnum(action_kind_t.GuestSetGeometry);
            out.x = payload.rectangle.x;
            out.y = payload.rectangle.y;
            out.width = payload.rectangle.width;
            out.height = payload.rectangle.height;
            out.maximized = @intFromBool(payload.maximized);
        },
        .GuestPostClose => out.kind = @intFromEnum(action_kind_t.GuestPostClose),
    }
    return out;
}

/// Decodes one raw action. An unknown kind or a non-boolean maximized word is InvalidPlan.
fn decode_action(raw: c.waddle_window_state_action_t) plan_decode_error_t!reducer.action_t {
    const kind = std.meta.intToEnum(action_kind_t, raw.kind) catch return error.InvalidPlan;
    return switch (kind) {
        .HostSetMinimized => .HostSetMinimized,
        .HostSetMaximized => .HostSetMaximized,
        .HostUnsetMaximized => .HostUnsetMaximized,
        .HostSetFullscreen => .HostSetFullscreen,
        .HostUnsetFullscreen => .HostUnsetFullscreen,
        .GuestRestore => .GuestRestore,
        .GuestMaximize => .GuestMaximize,
        .GuestMinimize => .GuestMinimize,
        .GuestEnterFullscreen => .GuestEnterFullscreen,
        .GuestLeaveFullscreen => .GuestLeaveFullscreen,
        .GuestActivate => .GuestActivate,
        .GuestSetGeometry => .{ .GuestSetGeometry = .{
            .rectangle = .{ .x = raw.x, .y = raw.y, .width = raw.width, .height = raw.height },
            .maximized = bool_from_u32(raw.maximized) catch return error.InvalidPlan,
        } },
        .GuestPostClose => .GuestPostClose,
    };
}

/// Hashes one fixed-size field into the seal.
fn hash_field(hasher: *std.hash.Wyhash, value: anytype) void {
    hasher.update(std.mem.asBytes(&value));
}

/// Computes the seal over every meaningful field of a plan, excluding `seal` itself.
/// The caller guarantees `action_count` is at most `MaxActions`.
fn plan_seal(seed: u64, plan: *const c.waddle_window_state_plan_t) u64 {
    var hasher = std.hash.Wyhash.init(seed);
    hash_field(&hasher, plan.window_id);
    hash_field(&hasher, plan.base_revision);
    hash_field(&hasher, plan.action_count);
    hash_field(&hasher, plan.generation);
    hash_field(&hasher, plan.pending_kind);
    hash_field(&hasher, plan.pending_state);
    hash_field(&hasher, plan.pending_capabilities);
    hash_field(&hasher, plan.pending_host_state);
    const count: usize = @intCast(plan.action_count);
    for (plan.actions[0..count]) |action| {
        hash_field(&hasher, action.kind);
        hash_field(&hasher, action.x);
        hash_field(&hasher, action.y);
        hash_field(&hasher, action.width);
        hash_field(&hasher, action.height);
        hash_field(&hasher, action.maximized);
    }
    return hasher.final();
}

/// Encodes a reducer plan into `out` and seals it. `out` must hold no meaningful prior content.
fn encode_plan(seed: u64, plan: reducer.plan_t, out: *c.waddle_window_state_plan_t) void {
    out.* = std.mem.zeroes(c.waddle_window_state_plan_t);
    out.window_id = plan.reference.window_id;
    out.generation = plan.reference.generation;
    out.base_revision = plan.base_revision;
    out.action_count = @intCast(plan.action_count);
    encode_pending(plan.pending, out);
    for (plan.actions[0..plan.action_count], 0..) |action, index| {
        out.actions[index] = encode_action(action);
    }
    out.seal = plan_seal(seed, out);
}

/// Validates and decodes a caller-held plan. Structure is checked before the seal, so a plan
/// with an impossible count or kind is InvalidPlan regardless of its seal.
fn decode_plan(seed: u64, raw: *const c.waddle_window_state_plan_t) plan_decode_error_t!reducer.plan_t {
    if (raw.action_count > reducer.MaxActions) return error.InvalidPlan;
    const count: usize = @intCast(raw.action_count);
    const pending = try decode_pending(raw);
    var result = reducer.plan_t{
        .reference = .{ .window_id = raw.window_id, .generation = raw.generation },
        .base_revision = raw.base_revision,
        .actions = undefined,
        .action_count = count,
        .pending = pending,
    };
    for (raw.actions[0..count], 0..) |action, index| {
        result.actions[index] = try decode_action(action);
    }
    if (plan_seal(seed, raw) != raw.seal) return error.InvalidPlan;
    return result;
}

/// Maps the optional maximize policy to its snapshot word.
fn host_request_word(requested: ?bool) u32 {
    const maximize = requested orelse return c.WaddleWindowStateHostRequestNone;
    return if (maximize) c.WaddleWindowStateHostRequestMaximize else c.WaddleWindowStateHostRequestUnmaximize;
}

/// Creates a machine bound to one window generation, initialized from its first guest observation.
///
/// @param window_id   in: nonzero protocol window identity.
/// @param generation  in: nonzero window incarnation.
/// @param guest_state in: guest state and capability bits; no Activated or undefined bits.
/// @param out_handle  out: non-NULL slot, set to NULL first and to the new handle on Ok.
/// @return Ok, InvalidArgument for a NULL slot, InvalidIdentity, InvalidState, or OutOfMemory.
///         Every non-Ok status leaves *out_handle NULL.
/// @note Ownership: the caller owns the returned handle until `waddle_window_state_destroy`.
/// @note Thread safety: independent calls on distinct handles are safe from any thread.
export fn waddle_window_state_create(
    window_id: u64,
    generation: u32,
    guest_state: u32,
    out_handle: ?*?*handle_t,
) status_t {
    const slot = out_handle orelse return c.WaddleWindowStateInvalidArgument;
    slot.* = null;
    const machine = reducer.machine_t.init(window_id, generation, guest_state) catch |err| return status_of_init_error(err);
    const storage = abi_allocator.create(handle_t) catch return c.WaddleWindowStateOutOfMemory;
    const epoch = seal_epoch.fetchAdd(1, .monotonic);
    storage.* = .{ .machine = machine, .seal_seed = mix64(epoch) };
    slot.* = storage;
    return c.WaddleWindowStateOk;
}

/// Frees a handle and sets the caller's slot to NULL.
///
/// @param handle_slot in/out: non-NULL slot. A NULL *handle_slot is a successful no-op.
/// @return Ok, or InvalidArgument when handle_slot is NULL.
/// @note Ownership: releases the handle allocated by `create`. The slot is NULL after Ok.
/// @note Thread safety: must not run concurrently with any other call on the same handle.
export fn waddle_window_state_destroy(handle_slot: ?*?*handle_t) status_t {
    const slot = handle_slot orelse return c.WaddleWindowStateInvalidArgument;
    const storage = slot.* orelse return c.WaddleWindowStateOk;
    abi_allocator.destroy(storage);
    slot.* = null;
    return c.WaddleWindowStateOk;
}

/// Plans one input for the bound window generation without changing the machine.
///
/// *out_plan is zeroed before any other check and is written with the plan only on Ok.
///
/// @param handle     in: non-NULL live handle.
/// @param window_id  in: must equal the bound window identity.
/// @param generation in: must equal the bound generation.
/// @param input      in: non-NULL input, read by value during the call.
/// @param out_plan   out: non-NULL plan slot.
/// @return Ok, InvalidArgument, StaleWindow, Retired, RevisionExhausted, Unsynchronized,
///         InvalidState, InvalidGeometry, or UnsupportedStateRequest.
/// @note Thread safety: may run concurrently with other plans on one handle only when no complete
///       or destroy runs on that handle at the same time.
export fn waddle_window_state_plan(
    handle: ?*const handle_t,
    window_id: u64,
    generation: u32,
    input: ?*const c.waddle_window_state_input_t,
    out_plan: ?*c.waddle_window_state_plan_t,
) status_t {
    const out = out_plan orelse return c.WaddleWindowStateInvalidArgument;
    out.* = std.mem.zeroes(c.waddle_window_state_plan_t);
    const storage = handle orelse return c.WaddleWindowStateInvalidArgument;
    const input_ptr = input orelse return c.WaddleWindowStateInvalidArgument;
    const raw_input = input_ptr.*;
    const decoded = decode_input(raw_input) catch |err| return status_of_input_error(err);
    const plan = storage.machine.plan(.{ .window_id = window_id, .generation = generation }, decoded) catch |err| return status_of_plan_error(err);
    var staged: c.waddle_window_state_plan_t = undefined;
    encode_plan(storage.seal_seed, plan, &staged);
    out.* = staged;
    return c.WaddleWindowStateOk;
}

/// Commits the outcome of a plan's ordered prefix.
///
/// @param handle            in/out: non-NULL live handle. Changed only on Ok.
/// @param plan              in: non-NULL plan exactly as returned by `waddle_window_state_plan`.
/// @param succeeded         in: 1 when every action succeeded, 0 otherwise; any other value is InvalidArgument.
/// @param completed_actions in: count of leading actions known to have completed.
/// @return Ok, InvalidArgument, InvalidPlan, StaleWindow, Retired, StalePlan, InvalidCompletion, or
///         RevisionExhausted. Every non-Ok status leaves the machine unchanged.
/// @note Thread safety: calls on one handle must be serialized by the caller.
export fn waddle_window_state_complete(
    handle: ?*handle_t,
    plan: ?*const c.waddle_window_state_plan_t,
    succeeded: u32,
    completed_actions: u64,
) status_t {
    const storage = handle orelse return c.WaddleWindowStateInvalidArgument;
    const plan_ptr = plan orelse return c.WaddleWindowStateInvalidArgument;
    const ok = bool_from_u32(succeeded) catch return c.WaddleWindowStateInvalidArgument;
    const raw = plan_ptr.*;
    const decoded = decode_plan(storage.seal_seed, &raw) catch |err| return status_of_decode_error(err);
    const count: usize = @intCast(completed_actions);
    storage.machine.complete(decoded, ok, count) catch |err| return status_of_complete_error(err);
    return c.WaddleWindowStateOk;
}

/// Copies the machine fields into caller storage.
///
/// @param handle       in: non-NULL live handle.
/// @param out_snapshot out: non-NULL. Written only on Ok.
/// @return Ok or InvalidArgument.
/// @note Thread safety: must not run concurrently with complete or destroy on the same handle.
export fn waddle_window_state_snapshot(
    handle: ?*const handle_t,
    out_snapshot: ?*c.waddle_window_state_snapshot_t,
) status_t {
    const out = out_snapshot orelse return c.WaddleWindowStateInvalidArgument;
    const storage = handle orelse return c.WaddleWindowStateInvalidArgument;
    const machine = &storage.machine;
    var staged = std.mem.zeroes(c.waddle_window_state_snapshot_t);
    staged.window_id = machine.reference.window_id;
    staged.revision = machine.revision;
    staged.generation = machine.reference.generation;
    staged.observed_guest_state = machine.observed_guest_state;
    staged.applied_guest_state = machine.applied_guest_state;
    staged.applied_host_state = machine.applied_host_state;
    staged.guest_capabilities = machine.guest_capabilities;
    staged.host_request = host_request_word(machine.requested_host_maximized);
    staged.synced = @intFromBool(machine.synced);
    staged.close_posted = @intFromBool(machine.close_posted);
    staged.retired = @intFromBool(machine.retired);
    out.* = staged;
    return c.WaddleWindowStateOk;
}

// Test-only fixtures.

const testing = std.testing;

// Every test call to an export goes through a `never_inline` wrapper. Without it the optimizer
// inlines an export into the test with constant arguments and folds its checks away, so the coverage
// recorder would measure copies that the tests never execute. The wrappers keep the real bodies live.
fn call_create(window_id: u64, generation: u32, guest_state: u32, out_handle: ?*?*handle_t) status_t {
    return @call(.never_inline, waddle_window_state_create, .{ window_id, generation, guest_state, out_handle });
}

fn call_destroy(handle_slot: ?*?*handle_t) status_t {
    return @call(.never_inline, waddle_window_state_destroy, .{handle_slot});
}

fn call_plan(handle: ?*const handle_t, window_id: u64, generation: u32, input: ?*const c.waddle_window_state_input_t, out_plan: ?*c.waddle_window_state_plan_t) status_t {
    return @call(.never_inline, waddle_window_state_plan, .{ handle, window_id, generation, input, out_plan });
}

fn call_complete(handle: ?*handle_t, plan: ?*const c.waddle_window_state_plan_t, succeeded: u32, completed_actions: u64) status_t {
    return @call(.never_inline, waddle_window_state_complete, .{ handle, plan, succeeded, completed_actions });
}

fn call_snapshot(handle: ?*const handle_t, out_snapshot: ?*c.waddle_window_state_snapshot_t) status_t {
    return @call(.never_inline, waddle_window_state_snapshot, .{ handle, out_snapshot });
}

/// Asserts that an ABI status equals an expected status.
fn expect_status(expected: status_t, actual: status_t) !void {
    try testing.expectEqual(expected, actual);
}

/// Asserts that a raw u32 field equals an expected header constant.
fn expect_u32(expected: c_int, actual: u32) !void {
    try testing.expectEqual(@as(u32, @intCast(expected)), actual);
}

/// Asserts that a plan buffer is entirely zero.
fn expect_zero_plan(plan: *const c.waddle_window_state_plan_t) !void {
    const zero = std.mem.zeroes(c.waddle_window_state_plan_t);
    try testing.expectEqualSlices(u8, std.mem.asBytes(&zero), std.mem.asBytes(plan));
}

/// Creates a handle with the testing allocator installed. The caller destroys it.
fn create_for_test(window_id: u64, generation: u32, guest_state: u32) !*handle_t {
    abi_allocator = testing.allocator;
    var slot: ?*handle_t = null;
    try expect_status(c.WaddleWindowStateOk, call_create(window_id, generation, guest_state, &slot));
    return slot orelse error.TestUnexpectedResult;
}

/// Destroys a handle through the ABI and checks that the slot is cleared.
fn destroy_for_test(handle: *handle_t) !void {
    var slot: ?*handle_t = handle;
    try expect_status(c.WaddleWindowStateOk, call_destroy(&slot));
    try testing.expect(slot == null);
}

/// Builds an input of the given kind with every other field zero.
fn input_of(kind: input_kind_t, state: u32) c.waddle_window_state_input_t {
    return .{
        .kind = @intFromEnum(kind),
        .state = state,
        .x = 0,
        .y = 0,
        .width = 0,
        .height = 0,
        .maximize = 0,
    };
}

/// Builds a host configure input.
fn configure_of(state: u32, x: i32, y: i32, width: u32, height: u32) c.waddle_window_state_input_t {
    var raw = input_of(.HostConfigure, state);
    raw.x = x;
    raw.y = y;
    raw.width = width;
    raw.height = height;
    return raw;
}

/// Plans through the ABI and requires Ok.
fn plan_ok(handle: *const handle_t, window_id: u64, generation: u32, raw: c.waddle_window_state_input_t) !c.waddle_window_state_plan_t {
    var plan = std.mem.zeroes(c.waddle_window_state_plan_t);
    var input = raw;
    try expect_status(c.WaddleWindowStateOk, call_plan(handle, window_id, generation, &input, &plan));
    return plan;
}

/// Completes through the ABI and returns the raw status.
fn complete_abi(handle: *handle_t, plan: *const c.waddle_window_state_plan_t, succeeded: u32, completed: u64) status_t {
    return call_complete(handle, plan, succeeded, completed);
}

/// Reads the ABI snapshot and requires Ok.
fn snapshot_of(handle: *const handle_t) !c.waddle_window_state_snapshot_t {
    var out = std.mem.zeroes(c.waddle_window_state_snapshot_t);
    try expect_status(c.WaddleWindowStateOk, call_snapshot(handle, &out));
    return out;
}

/// Plans and completes a guest observation so that the handle is synchronized.
fn synchronize(handle: *handle_t, guest_state: u32) !void {
    const plan = try plan_ok(handle, handle.machine.reference.window_id, handle.machine.reference.generation, input_of(.GuestObservation, guest_state));
    try expect_status(c.WaddleWindowStateOk, complete_abi(handle, &plan, 1, plan.action_count));
}

/// Recomputes the seal after a deliberate edit, so that only the structural checks can reject it.
fn reseal(handle: *const handle_t, plan: *c.waddle_window_state_plan_t) void {
    plan.seal = plan_seal(handle.seal_seed, plan);
}

/// Requires two snapshots to match field for field.
fn expect_same_snapshot(expected: c.waddle_window_state_snapshot_t, actual: c.waddle_window_state_snapshot_t) !void {
    try testing.expect(std.meta.eql(expected, actual));
}

test "create rejects a NULL slot, zero identity, and forbidden guest state without writing" {
    abi_allocator = testing.allocator;
    try expect_status(c.WaddleWindowStateInvalidArgument, call_create(1, 1, 0, null));
    var slot: ?*handle_t = null;
    try expect_status(c.WaddleWindowStateInvalidIdentity, call_create(0, 1, 0, &slot));
    try testing.expect(slot == null);
    try expect_status(c.WaddleWindowStateInvalidIdentity, call_create(1, 0, 0, &slot));
    try testing.expect(slot == null);
    try expect_status(c.WaddleWindowStateInvalidState, call_create(1, 1, reducer.StateActivated, &slot));
    try testing.expect(slot == null);
    try expect_status(c.WaddleWindowStateInvalidState, call_create(1, 1, 0x80, &slot));
    try testing.expect(slot == null);
}

test "create and destroy release the single allocation and destroy is idempotent" {
    const handle = try create_for_test(7, 3, reducer.StateResizable);
    try destroy_for_test(handle);

    var empty: ?*handle_t = null;
    try expect_status(c.WaddleWindowStateOk, call_destroy(&empty));
    try expect_status(c.WaddleWindowStateInvalidArgument, call_destroy(null));
}

test "create maps an injected allocation failure to OutOfMemory with a NULL output" {
    var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = 0 });
    abi_allocator = failing.allocator();
    defer abi_allocator = testing.allocator;
    var slot: ?*handle_t = null;
    try expect_status(c.WaddleWindowStateOutOfMemory, call_create(9, 2, 0, &slot));
    try testing.expect(slot == null);
    try testing.expect(failing.has_induced_failure);
}

test "create succeeds when the injected failure index is never reached" {
    var failing = testing.FailingAllocator.init(testing.allocator, .{ .fail_index = 1 });
    abi_allocator = failing.allocator();
    defer abi_allocator = testing.allocator;
    var slot: ?*handle_t = null;
    try expect_status(c.WaddleWindowStateOk, call_create(9, 2, 0, &slot));
    try testing.expect(!failing.has_induced_failure);
    try expect_status(c.WaddleWindowStateOk, call_destroy(&slot));
    try testing.expect(slot == null);
}

test "reconciliation and restore with geometry reproduce the reducer action order" {
    const handle = try create_for_test(11, 4, reducer.StateMinimized | reducer.StateMaximizable);
    defer destroy_for_test(handle) catch {};

    // Reconciliation of a minimized guest: one-shot minimize, then unmaximize and unfullscreen.
    const reconcile = try plan_ok(handle, 11, 4, input_of(.GuestObservation, reducer.StateMinimized | reducer.StateMaximizable));
    try testing.expectEqual(@as(u64, 3), reconcile.action_count);
    try expect_u32(c.WaddleWindowStateActionHostSetMinimized, reconcile.actions[0].kind);
    try expect_u32(c.WaddleWindowStateActionHostUnsetMaximized, reconcile.actions[1].kind);
    try expect_u32(c.WaddleWindowStateActionHostUnsetFullscreen, reconcile.actions[2].kind);
    try expect_status(c.WaddleWindowStateOk, complete_abi(handle, &reconcile, 1, 3));

    const after = try snapshot_of(handle);
    try testing.expectEqual(@as(u64, 2), after.revision);
    try testing.expectEqual(@as(u32, 1), after.synced);
    try testing.expectEqual(reducer.StateMinimized, after.observed_guest_state);
    try testing.expectEqual(reducer.StateMaximizable, after.guest_capabilities);
    try testing.expectEqual(@as(u32, 0), after.applied_host_state);
    try expect_u32(c.WaddleWindowStateHostRequestNone, after.host_request);

    // Activation of an iconic window: restore, activate, then geometry. Minimize is never re-issued.
    const restore = try plan_ok(handle, 11, 4, configure_of(reducer.StateActivated, -5, 6, 800, 600));
    try testing.expectEqual(@as(u64, 3), restore.action_count);
    try expect_u32(c.WaddleWindowStateActionGuestRestore, restore.actions[0].kind);
    try expect_u32(c.WaddleWindowStateActionGuestActivate, restore.actions[1].kind);
    try expect_u32(c.WaddleWindowStateActionGuestSetGeometry, restore.actions[2].kind);
    try testing.expectEqual(@as(i32, -5), restore.actions[2].x);
    try testing.expectEqual(@as(i32, 6), restore.actions[2].y);
    try testing.expectEqual(@as(u32, 800), restore.actions[2].width);
    try testing.expectEqual(@as(u32, 600), restore.actions[2].height);
    try testing.expectEqual(@as(u32, 0), restore.actions[2].maximized);
    try expect_status(c.WaddleWindowStateOk, complete_abi(handle, &restore, 1, 3));
    try testing.expectEqual(@as(u32, 0), (try snapshot_of(handle)).applied_guest_state & reducer.StateMinimized);
}

test "plan zeroes the output slot on every failing status" {
    const handle = try create_for_test(12, 1, reducer.StateMaximizable);
    defer destroy_for_test(handle) catch {};
    var out = std.mem.zeroes(c.waddle_window_state_plan_t);
    var input = input_of(.GuestObservation, 0);

    @memset(std.mem.asBytes(&out), 0xaa);
    try expect_status(c.WaddleWindowStateInvalidArgument, call_plan(null, 12, 1, &input, &out));
    try expect_zero_plan(&out);

    @memset(std.mem.asBytes(&out), 0xaa);
    try expect_status(c.WaddleWindowStateStaleWindow, call_plan(handle, 13, 1, &input, &out));
    try expect_zero_plan(&out);

    var configure = configure_of(0, 0, 0, 0, 0);
    @memset(std.mem.asBytes(&out), 0xaa);
    try expect_status(c.WaddleWindowStateUnsynchronized, call_plan(handle, 12, 1, &configure, &out));
    try expect_zero_plan(&out);
}

test "plan rejects NULL arguments and bad input kinds or boolean words with InvalidArgument" {
    const handle = try create_for_test(13, 1, 0);
    defer destroy_for_test(handle) catch {};
    var out = std.mem.zeroes(c.waddle_window_state_plan_t);
    var input = input_of(.GuestObservation, 0);

    try expect_status(c.WaddleWindowStateInvalidArgument, call_plan(handle, 13, 1, null, &out));
    try expect_status(c.WaddleWindowStateInvalidArgument, call_plan(handle, 13, 1, &input, null));

    var bad_kind = input;
    bad_kind.kind = 0;
    try expect_status(c.WaddleWindowStateInvalidArgument, call_plan(handle, 13, 1, &bad_kind, &out));
    try expect_zero_plan(&out);
    bad_kind.kind = 7;
    try expect_status(c.WaddleWindowStateInvalidArgument, call_plan(handle, 13, 1, &bad_kind, &out));

    var bad_maximize = input_of(.HostStateRequest, 0);
    bad_maximize.maximize = 2;
    try expect_status(c.WaddleWindowStateInvalidArgument, call_plan(handle, 13, 1, &bad_maximize, &out));
    try expect_zero_plan(&out);
}

test "every reducer action kind survives a plan encode and decode round trip" {
    const actions = [_]reducer.action_t{
        .HostSetMinimized,
        .HostSetMaximized,
        .HostUnsetMaximized,
        .HostSetFullscreen,
        .HostUnsetFullscreen,
        .GuestRestore,
        .GuestMaximize,
        .GuestMinimize,
        .GuestEnterFullscreen,
        .GuestLeaveFullscreen,
        .GuestActivate,
        .{ .GuestSetGeometry = .{ .rectangle = .{ .x = -7, .y = 9, .width = 640, .height = 480 }, .maximized = true } },
        .GuestPostClose,
    };
    for (actions) |action| {
        const raw = encode_action(action);
        const decoded = try decode_action(raw);
        try testing.expect(std.meta.eql(action, decoded));
    }
    try testing.expectError(error.InvalidPlan, decode_action(.{ .kind = 0, .x = 0, .y = 0, .width = 0, .height = 0, .maximized = 0 }));
    try testing.expectError(error.InvalidPlan, decode_action(.{ .kind = 14, .x = 0, .y = 0, .width = 0, .height = 0, .maximized = 0 }));
}

/// Documented status rows. The values are the header numbers from contract section 4.5, written as
/// literals so the table does not reuse the mapping under test.
const plan_status_row_t = struct { err: plan_error_t, value: u32 };
const plan_status_rows = [_]plan_status_row_t{
    .{ .err = error.StaleWindow, .value = 6 },
    .{ .err = error.Retired, .value = 8 },
    .{ .err = error.RevisionExhausted, .value = 11 },
    .{ .err = error.Unsynchronized, .value = 9 },
    .{ .err = error.InvalidState, .value = 4 },
    .{ .err = error.InvalidGeometry, .value = 5 },
    .{ .err = error.UnsupportedStateRequest, .value = 10 },
};
const complete_status_row_t = struct { err: complete_error_t, value: u32 };
const complete_status_rows = [_]complete_status_row_t{
    .{ .err = error.StaleWindow, .value = 6 },
    .{ .err = error.Retired, .value = 8 },
    .{ .err = error.StalePlan, .value = 7 },
    .{ .err = error.InvalidCompletion, .value = 12 },
    .{ .err = error.RevisionExhausted, .value = 11 },
};
const init_status_row_t = struct { err: init_error_t, value: u32 };
const init_status_rows = [_]init_status_row_t{
    .{ .err = error.InvalidIdentity, .value = 3 },
    .{ .err = error.InvalidState, .value = 4 },
};

test "status mapping follows the documented header values row by row" {
    // Each error set is covered in full: the table length equals the number of errors in the set.
    try testing.expectEqual(@typeInfo(plan_error_t).ErrorSet.?.len, plan_status_rows.len);
    try testing.expectEqual(@typeInfo(complete_error_t).ErrorSet.?.len, complete_status_rows.len);
    try testing.expectEqual(@typeInfo(init_error_t).ErrorSet.?.len, init_status_rows.len);
    try testing.expectEqual(@typeInfo(input_error_t).ErrorSet.?.len, 1);
    try testing.expectEqual(@typeInfo(plan_decode_error_t).ErrorSet.?.len, 1);

    for (plan_status_rows) |row| try testing.expectEqual(row.value, @as(u32, @intCast(status_of_plan_error(row.err))));
    for (complete_status_rows) |row| try testing.expectEqual(row.value, @as(u32, @intCast(status_of_complete_error(row.err))));
    for (init_status_rows) |row| try testing.expectEqual(row.value, @as(u32, @intCast(status_of_init_error(row.err))));
    try testing.expectEqual(@as(u32, 1), @as(u32, @intCast(status_of_input_error(error.InvalidArgument))));
    try testing.expectEqual(@as(u32, 13), @as(u32, @intCast(status_of_decode_error(error.InvalidPlan))));
}

test "plan maps identity, synchronization, state, geometry, capability, and revision errors" {
    const handle = try create_for_test(15, 2, reducer.StateMinimizable);
    defer destroy_for_test(handle) catch {};
    var out = std.mem.zeroes(c.waddle_window_state_plan_t);

    var configure = configure_of(0, 0, 0, 0, 0);
    try expect_status(c.WaddleWindowStateStaleWindow, call_plan(handle, 16, 2, &configure, &out));
    try expect_status(c.WaddleWindowStateStaleWindow, call_plan(handle, 15, 3, &configure, &out));
    try expect_status(c.WaddleWindowStateUnsynchronized, call_plan(handle, 15, 2, &configure, &out));

    // Guest observations are accepted while unsynchronized, but their own bits are still checked.
    var observation = input_of(.GuestObservation, reducer.StateActivated);
    try expect_status(c.WaddleWindowStateInvalidState, call_plan(handle, 15, 2, &observation, &out));
    try expect_zero_plan(&out);
    observation.state = 0x80;
    try expect_status(c.WaddleWindowStateInvalidState, call_plan(handle, 15, 2, &observation, &out));

    try synchronize(handle, reducer.StateMinimizable);

    var zero_height = configure_of(0, 0, 0, 0, 5);
    try expect_status(c.WaddleWindowStateInvalidGeometry, call_plan(handle, 15, 2, &zero_height, &out));
    var too_wide = configure_of(0, 0, 0, 16385, 1);
    try expect_status(c.WaddleWindowStateInvalidGeometry, call_plan(handle, 15, 2, &too_wide, &out));
    var capability_bit = configure_of(reducer.StateMinimizable, 0, 0, 0, 0);
    try expect_status(c.WaddleWindowStateInvalidState, call_plan(handle, 15, 2, &capability_bit, &out));
    try expect_zero_plan(&out);

    // Only Minimizable is held, so a maximize policy request is refused.
    var maximize = input_of(.HostStateRequest, 0);
    maximize.maximize = 1;
    try expect_status(c.WaddleWindowStateUnsupportedStateRequest, call_plan(handle, 15, 2, &maximize, &out));

    handle.machine.revision = std.math.maxInt(u64);
    var close = input_of(.CloseRequest, 0);
    try expect_status(c.WaddleWindowStateRevisionExhausted, call_plan(handle, 15, 2, &close, &out));
    try expect_zero_plan(&out);
}

test "complete rejects stale and malformed completions without changing state, then resynchronizes" {
    const handle = try create_for_test(17, 5, reducer.StateMaximizable);
    defer destroy_for_test(handle) catch {};
    try synchronize(handle, reducer.StateMaximizable);
    const before = try snapshot_of(handle);

    const close_a = try plan_ok(handle, 17, 5, input_of(.CloseRequest, 0));
    const close_b = try plan_ok(handle, 17, 5, input_of(.CloseRequest, 0));
    try testing.expectEqual(@as(u64, 1), close_a.action_count);
    try expect_u32(c.WaddleWindowStateActionGuestPostClose, close_a.actions[0].kind);

    try expect_status(c.WaddleWindowStateInvalidCompletion, complete_abi(handle, &close_a, 1, 0));
    try expect_status(c.WaddleWindowStateInvalidCompletion, complete_abi(handle, &close_a, 0, 2));
    try expect_same_snapshot(before, try snapshot_of(handle));

    // A failed close: the revision advances, the machine is unsynchronized, and close is not posted.
    try expect_status(c.WaddleWindowStateOk, complete_abi(handle, &close_a, 0, 0));
    const failed = try snapshot_of(handle);
    try testing.expectEqual(before.revision + 1, failed.revision);
    try testing.expectEqual(@as(u32, 0), failed.synced);
    try testing.expectEqual(@as(u32, 0), failed.close_posted);

    // The other plan from the same revision is now stale, and new close requests are refused.
    try expect_status(c.WaddleWindowStateStalePlan, complete_abi(handle, &close_b, 1, 1));
    var close_input = input_of(.CloseRequest, 0);
    var out = std.mem.zeroes(c.waddle_window_state_plan_t);
    try expect_status(c.WaddleWindowStateUnsynchronized, call_plan(handle, 17, 5, &close_input, &out));

    // A fresh observation resynchronizes, and a successful close then posts.
    try synchronize(handle, reducer.StateMaximizable);
    const close_c = try plan_ok(handle, 17, 5, input_of(.CloseRequest, 0));
    try expect_status(c.WaddleWindowStateOk, complete_abi(handle, &close_c, 1, 1));
    try testing.expectEqual(@as(u32, 1), (try snapshot_of(handle)).close_posted);
}

test "retirement completes only as a zero-action success and then rejects all planning" {
    const handle = try create_for_test(18, 6, 0);
    defer destroy_for_test(handle) catch {};
    try synchronize(handle, 0);

    const hide = try plan_ok(handle, 18, 6, input_of(.HideObserved, 0));
    try testing.expectEqual(@as(u64, 0), hide.action_count);
    try expect_status(c.WaddleWindowStateInvalidCompletion, complete_abi(handle, &hide, 0, 0));
    try expect_status(c.WaddleWindowStateInvalidCompletion, complete_abi(handle, &hide, 1, 1));
    try expect_status(c.WaddleWindowStateOk, complete_abi(handle, &hide, 1, 0));
    try expect_u32(1, (try snapshot_of(handle)).retired);

    var out = std.mem.zeroes(c.waddle_window_state_plan_t);
    var close = input_of(.CloseRequest, 0);
    try expect_status(c.WaddleWindowStateRetired, call_plan(handle, 18, 6, &close, &out));
    var destroy_input = input_of(.DestroyObserved, 0);
    try expect_status(c.WaddleWindowStateRetired, call_plan(handle, 18, 6, &destroy_input, &out));
    try expect_status(c.WaddleWindowStateRetired, complete_abi(handle, &hide, 1, 0));
}

test "destroy observation retires a synchronized window with no native action" {
    const handle = try create_for_test(23, 10, reducer.StateMaximizable);
    defer destroy_for_test(handle) catch {};
    try synchronize(handle, reducer.StateMaximizable);

    const destroyed = try plan_ok(handle, 23, 10, input_of(.DestroyObserved, 0));
    try testing.expectEqual(@as(u64, 0), destroyed.action_count);
    try expect_status(c.WaddleWindowStateOk, call_complete(handle, &destroyed, 1, 0));

    const snap = try snapshot_of(handle);
    try expect_u32(1, snap.retired);
    try expect_u32(0, snap.close_posted);
    try testing.expectEqual(@as(u32, 0), snap.applied_host_state);
    try expect_u32(0, snap.applied_guest_state);
}

test "destroy observation retires an unsynchronized window with no native action" {
    const handle = try create_for_test(24, 11, reducer.StateMinimized);
    defer destroy_for_test(handle) catch {};

    const destroyed = try plan_ok(handle, 24, 11, input_of(.DestroyObserved, 0));
    try testing.expectEqual(@as(u64, 0), destroyed.action_count);
    try expect_status(c.WaddleWindowStateOk, call_complete(handle, &destroyed, 1, 0));

    const snap = try snapshot_of(handle);
    try expect_u32(1, snap.retired);
    try expect_u32(0, snap.close_posted);
    var out = std.mem.zeroes(c.waddle_window_state_plan_t);
    var close = input_of(.CloseRequest, 0);
    try expect_status(c.WaddleWindowStateRetired, call_plan(handle, 24, 11, &close, &out));
}

test "a plan from a destroyed handle is rejected by a recreated handle with the same identity" {
    const first = try create_for_test(25, 12, 0);
    try synchronize(first, 0);
    const stale = try plan_ok(first, 25, 12, input_of(.CloseRequest, 0));
    try destroy_for_test(first);

    const second = try create_for_test(25, 12, 0);
    defer destroy_for_test(second) catch {};
    try synchronize(second, 0);
    // Same identity and same revision: only the per-handle seal separates the two.
    try testing.expectEqual(second.machine.revision, stale.base_revision);
    const before = try snapshot_of(second);
    try expect_status(c.WaddleWindowStateInvalidPlan, call_complete(second, &stale, 1, 1));
    try expect_same_snapshot(before, try snapshot_of(second));
}

test "sealed plans reject edits, resealed invalid structure, foreign handles, and NULL arguments" {
    const owner = try create_for_test(19, 7, 0);
    defer destroy_for_test(owner) catch {};
    const other = try create_for_test(19, 7, 0);
    defer destroy_for_test(other) catch {};
    try synchronize(owner, 0);
    try synchronize(other, 0);

    // Maximize with geometry: GuestMaximize, then SetGeometry carrying maximized = 1.
    const configured = try plan_ok(owner, 19, 7, configure_of(reducer.StateMaximized, 1, 2, 300, 200));
    try testing.expectEqual(@as(u64, 2), configured.action_count);
    try expect_u32(c.WaddleWindowStateActionGuestMaximize, configured.actions[0].kind);
    try expect_u32(c.WaddleWindowStateActionGuestSetGeometry, configured.actions[1].kind);
    const before = try snapshot_of(owner);

    var edited_window = configured;
    edited_window.window_id = 20;
    try expect_status(c.WaddleWindowStateInvalidPlan, complete_abi(owner, &edited_window, 1, 2));

    var edited_pending = configured;
    edited_pending.pending_state = 0;
    try expect_status(c.WaddleWindowStateInvalidPlan, complete_abi(owner, &edited_pending, 1, 2));

    var edited_action = configured;
    edited_action.actions[1].width = 301;
    try expect_status(c.WaddleWindowStateInvalidPlan, complete_abi(owner, &edited_action, 1, 2));

    try expect_status(c.WaddleWindowStateInvalidPlan, complete_abi(other, &configured, 1, 2));

    const zero = std.mem.zeroes(c.waddle_window_state_plan_t);
    try expect_status(c.WaddleWindowStateInvalidPlan, complete_abi(owner, &zero, 1, 0));

    var too_many = configured;
    too_many.action_count = c.WaddleWindowStateMaxActions + 1;
    try expect_status(c.WaddleWindowStateInvalidPlan, complete_abi(owner, &too_many, 1, 2));

    var bad_pending_kind = configured;
    bad_pending_kind.pending_kind = 99;
    reseal(owner, &bad_pending_kind);
    try expect_status(c.WaddleWindowStateInvalidPlan, complete_abi(owner, &bad_pending_kind, 1, 2));

    var bad_action_kind = configured;
    bad_action_kind.actions[0].kind = 99;
    reseal(owner, &bad_action_kind);
    try expect_status(c.WaddleWindowStateInvalidPlan, complete_abi(owner, &bad_action_kind, 1, 2));

    var bad_maximized = configured;
    bad_maximized.actions[1].maximized = 2;
    reseal(owner, &bad_maximized);
    try expect_status(c.WaddleWindowStateInvalidPlan, complete_abi(owner, &bad_maximized, 1, 2));

    const unmaximize = try plan_ok(owner, 19, 7, input_of(.HostStateRequest, 0));
    var bad_request = unmaximize;
    bad_request.pending_state = 2;
    reseal(owner, &bad_request);
    try expect_status(c.WaddleWindowStateInvalidPlan, complete_abi(owner, &bad_request, 0, 0));

    try expect_same_snapshot(before, try snapshot_of(owner));
    try expect_status(c.WaddleWindowStateInvalidArgument, call_complete(null, &configured, 1, 2));
    try expect_status(c.WaddleWindowStateInvalidArgument, call_complete(owner, null, 1, 2));
    try expect_status(c.WaddleWindowStateInvalidArgument, call_complete(owner, &configured, 2, 2));

    // The untouched plan still completes exactly once.
    try expect_status(c.WaddleWindowStateOk, complete_abi(owner, &configured, 1, 2));
    try expect_status(c.WaddleWindowStateStalePlan, complete_abi(owner, &configured, 1, 2));
}

test "complete reports revision exhaustion for a resealed plan at the maximum revision" {
    const handle = try create_for_test(21, 8, 0);
    defer destroy_for_test(handle) catch {};
    try synchronize(handle, 0);

    var close_plan = try plan_ok(handle, 21, 8, input_of(.CloseRequest, 0));
    handle.machine.revision = std.math.maxInt(u64);
    close_plan.base_revision = std.math.maxInt(u64);
    reseal(handle, &close_plan);
    const before = try snapshot_of(handle);
    try expect_status(c.WaddleWindowStateRevisionExhausted, complete_abi(handle, &close_plan, 1, 1));
    try expect_same_snapshot(before, try snapshot_of(handle));
}

test "snapshot reports every host request word and rejects NULL arguments" {
    var out = std.mem.zeroes(c.waddle_window_state_snapshot_t);
    try expect_status(c.WaddleWindowStateInvalidArgument, call_snapshot(null, &out));

    const handle = try create_for_test(22, 9, reducer.StateMaximizable);
    defer destroy_for_test(handle) catch {};
    try expect_status(c.WaddleWindowStateInvalidArgument, call_snapshot(handle, null));
    try expect_u32(c.WaddleWindowStateHostRequestNone, (try snapshot_of(handle)).host_request);

    try synchronize(handle, reducer.StateMaximizable);

    // Unmaximize needs no capability and no action while the host is not maximized.
    const unmaximize = try plan_ok(handle, 22, 9, input_of(.HostStateRequest, 0));
    try testing.expectEqual(@as(u64, 0), unmaximize.action_count);
    try expect_status(c.WaddleWindowStateOk, complete_abi(handle, &unmaximize, 1, 0));
    try expect_u32(c.WaddleWindowStateHostRequestUnmaximize, (try snapshot_of(handle)).host_request);

    var maximize_input = input_of(.HostStateRequest, 0);
    maximize_input.maximize = 1;
    const maximize = try plan_ok(handle, 22, 9, maximize_input);
    try testing.expectEqual(@as(u64, 1), maximize.action_count);
    try expect_u32(c.WaddleWindowStateActionHostSetMaximized, maximize.actions[0].kind);
    try expect_status(c.WaddleWindowStateOk, complete_abi(handle, &maximize, 1, 1));
    const snap = try snapshot_of(handle);
    try expect_u32(c.WaddleWindowStateHostRequestMaximize, snap.host_request);
    try testing.expectEqual(reducer.StateMaximized, snap.applied_host_state);
}

/// One generated input, carried in both the ABI form and the reducer form.
const step_t = struct {
    kind: input_kind_t,
    state: u32,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    maximize: bool,
};

fn raw_of(step: step_t) c.waddle_window_state_input_t {
    return .{
        .kind = @intFromEnum(step.kind),
        .state = step.state,
        .x = step.x,
        .y = step.y,
        .width = step.width,
        .height = step.height,
        .maximize = @intFromBool(step.maximize),
    };
}

fn reducer_of(step: step_t) reducer.input_t {
    return switch (step.kind) {
        .GuestObservation => .{ .GuestObservation = .{ .state = step.state } },
        .HostConfigure => .{ .HostConfigure = .{
            .state = step.state,
            .geometry = .{ .x = step.x, .y = step.y, .width = step.width, .height = step.height },
        } },
        .HostStateRequest => .{ .HostStateRequest = step.maximize },
        .CloseRequest => .CloseRequest,
        .HideObserved => .HideObserved,
        .DestroyObserved => .DestroyObserved,
    };
}

/// Draws one input from a distribution that mixes valid transitions with forbidden bits,
/// oversized or half-zero geometry, and rare retirement.
fn random_step(random: std.rand.Random) step_t {
    var step = step_t{ .kind = .CloseRequest, .state = 0, .x = 0, .y = 0, .width = 0, .height = 0, .maximize = false };
    const roll = random.uintLessThan(u32, 100);
    if (roll < 35) {
        step.kind = .GuestObservation;
        step.state = random.int(u32) & 0x77;
        if (random.uintLessThan(u32, 20) == 0) step.state |= if (random.boolean()) reducer.StateActivated else 0x80;
    } else if (roll < 60) {
        step.kind = .HostConfigure;
        step.state = random.int(u32) & 0x0f;
        if (random.uintLessThan(u32, 20) == 0) step.state |= 0x40;
        step.x = random.int(i32);
        step.y = random.int(i32);
        const sizes = [_][2]u32{ .{ 0, 0 }, .{ 1, 1 }, .{ 800, 600 }, .{ 16384, 16384 }, .{ 0, 5 }, .{ 16385, 1 } };
        const pick = sizes[random.uintLessThan(usize, sizes.len)];
        step.width = pick[0];
        step.height = pick[1];
    } else if (roll < 75) {
        step.kind = .HostStateRequest;
        step.maximize = random.boolean();
    } else if (roll < 98) {
        step.kind = .CloseRequest;
    } else if (roll == 98) {
        step.kind = .HideObserved;
    } else {
        step.kind = .DestroyObserved;
    }
    return step;
}

/// Completes the oracle and returns the status that the ABI must report.
fn oracle_complete_status(oracle: *reducer.machine_t, plan: reducer.plan_t, succeeded: bool, completed: usize) status_t {
    oracle.complete(plan, succeeded, completed) catch |err| return status_of_complete_error(err);
    return c.WaddleWindowStateOk;
}

test "random sequences through the ABI match direct reducer use step for step" {
    abi_allocator = testing.allocator;
    var prng = std.rand.DefaultPrng.init(0x5eed_0001_2026);
    const random = prng.random();
    var successful_plans: usize = 0;
    var successful_completions: usize = 0;
    var rejected_completions: usize = 0;

    var round: u32 = 0;
    while (round < 12) : (round += 1) {
        const window_id: u64 = 1000 + round;
        const generation: u32 = 1 + round;
        const initial = random.int(u32) & 0x77;
        var oracle = try reducer.machine_t.init(window_id, generation, initial);
        const handle = try create_for_test(window_id, generation, initial);
        defer destroy_for_test(handle) catch {};

        var previous_abi: ?c.waddle_window_state_plan_t = null;
        var previous_oracle: ?reducer.plan_t = null;
        var index: u32 = 0;
        while (index < 300 and !oracle.retired) : (index += 1) {
            // Replay the previous plan: it must be accepted or refused identically on both sides.
            if (previous_abi) |old_abi| {
                if (random.uintLessThan(u32, 8) == 0) {
                    const old_oracle = previous_oracle orelse return error.TestUnexpectedResult;
                    const succeeded = random.boolean();
                    const completed: usize = random.uintLessThan(usize, old_oracle.action_count + 2);
                    const expected = oracle_complete_status(&oracle, old_oracle, succeeded, completed);
                    const actual = complete_abi(handle, &old_abi, @intFromBool(succeeded), completed);
                    try expect_status(expected, actual);
                    if (actual == c.WaddleWindowStateStalePlan) continue;
                }
            }

            const step = random_step(random);
            const generation_used: u32 = if (random.uintLessThan(u32, 40) == 0) generation + 1 else generation;
            const raw = raw_of(step);
            var out = std.mem.zeroes(c.waddle_window_state_plan_t);
            const oracle_plan = oracle.plan(.{ .window_id = window_id, .generation = generation_used }, reducer_of(step));
            if (oracle_plan) |plan_value| {
                try expect_status(c.WaddleWindowStateOk, call_plan(handle, window_id, generation_used, &raw, &out));
                successful_plans += 1;
                try testing.expectEqual(@as(u64, plan_value.action_count), out.action_count);
                try testing.expectEqual(plan_value.base_revision, out.base_revision);
                var position: usize = 0;
                while (position < plan_value.action_count) : (position += 1) {
                    try testing.expect(std.meta.eql(encode_action(plan_value.actions[position]), out.actions[position]));
                }

                const choice = random.uintLessThan(u32, 10);
                if (choice == 9) {
                    // Leave the plan pending. A later step will replay it.
                    previous_abi = out;
                    previous_oracle = plan_value;
                } else {
                    const succeeded = choice < 7;
                    const completed: usize = if (succeeded)
                        plan_value.action_count
                    else
                        random.uintLessThan(usize, plan_value.action_count + 2);
                    const expected = oracle_complete_status(&oracle, plan_value, succeeded, completed);
                    const actual = complete_abi(handle, &out, @intFromBool(succeeded), completed);
                    try expect_status(expected, actual);
                    if (actual == c.WaddleWindowStateOk) {
                        successful_completions += 1;
                    } else {
                        rejected_completions += 1;
                    }
                    previous_abi = out;
                    previous_oracle = plan_value;
                }
            } else |err| {
                try expect_status(status_of_plan_error(err), call_plan(handle, window_id, generation_used, &raw, &out));
                try expect_zero_plan(&out);
            }
            try testing.expect(std.meta.eql(handle.machine, oracle));
        }
    }

    // The fixed seed must actually exercise the accepted paths, not only the refusals.
    try testing.expect(successful_plans > 200);
    try testing.expect(successful_completions > 100);
    try testing.expect(rejected_completions > 0);
}
