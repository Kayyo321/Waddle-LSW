//! C ABI boundary of the guest window graph (`include/waddle/interop_window_graph.h`).
//!
//! Every exported function is a thin wrapper around the matching function of `window_graph.zig`.
//! The wrappers add only what the engine cannot know about a C caller:
//!
//!  * Every C pointer is checked (non-null, natural alignment of the C type, address range that
//!    does not overflow `usize`, no overlap between any two of the call's regions and no overlap
//!    with the graph's own storage) BEFORE a Zig pointer or slice is formed from it. Pointer
//!    parameters are declared `align(1)` so that a misaligned argument is never a typed pointer
//!    until it has passed the check.
//!  * Outputs are staged in locals and copied to the caller only for the statuses that the header
//!    says write them (Ok everywhere; Ok, None and SinkFailed for the `delivered` count of drain).
//!    Array outputs (`windows`, `entries`) are written by the engine, but only after its last
//!    failure point, so they too are untouched on every non-Ok status.
//!    `create` clears a valid, disjoint output slot on failure; config/output overlap is rejected
//!    before either region is read or written.
//!  * The header's common call rules keep their order: a NULL (or misaligned) graph is
//!    InvalidArgument; then wrong thread, re-entrancy and a stale epoch are decided by the engine
//!    guards even when a pointer argument is also bad; only then does a bad pointer produce
//!    InvalidArgument. A bad pointer therefore never hides WrongThread, Reentrant or StaleEpoch.
//!  * The opaque handle is cast to the engine's graph type only after its alignment and extent were
//!    checked. `create` is the only function that allocates, through `abi_allocator`.
//!
//! No wrapper retains a pointer, allocates (except `create`), blocks, or touches the operating
//! system. Threading follows the header: a graph belongs to its creating thread and every call
//! except `create` and `classify` is rejected on any other thread by the engine guards.
const std_t = @import("std");
const graph_t = @import("window_graph.zig");

const c_t = @cImport({
    @cInclude("waddle/interop_window_graph.h");
});

/// Opaque graph handle (`interop_window_graph_t`); points at the engine's `graph_t`.
const graph_handle_t = opaque {};

const facts_t = c_t.interop_window_facts_t;
const verdict_t = c_t.interop_window_verdict_t;
const event_t = c_t.interop_window_event_t;
const observation_t = c_t.interop_window_observation_t;
const live_t = c_t.interop_window_live_t;
const retiring_entry_t = c_t.interop_window_retiring_entry_t;
const retire_proof_t = c_t.interop_window_retire_proof_t;
const diagnostic_t = c_t.interop_window_capacity_diagnostic_t;
const stats_t = c_t.interop_window_graph_stats_t;
const config_t = c_t.interop_window_graph_config_t;

const StatusOk: c_int = @intFromEnum(graph_t.status_t.Ok);
const StatusInvalidArgument: c_int = @intFromEnum(graph_t.status_t.InvalidArgument);
const StatusNoMemory: c_int = @intFromEnum(graph_t.status_t.NoMemory);

/// The only allocation seam of the ABI: `create` takes the graph's single allocation from it and
/// the graph remembers the allocator it came from, so `destroy` frees through the same one.
///
/// Defaults to the C allocator. It is mutable for tests only (failure injection and leak
/// checking); production code never writes it, and it is not synchronized, so a test must swap it
/// while no other thread calls `create`.
var abi_allocator: std_t.mem.Allocator = std_t.heap.c_allocator;

comptime {
    // The ABI file imports the header itself; it must be the very same types the engine uses.
    if (facts_t != graph_t.facts_t or event_t != graph_t.event_t or observation_t != graph_t.observation_t)
        @compileError("ABI and engine disagree on the imported header types");
    if (config_t != graph_t.config_t or stats_t != graph_t.stats_t or diagnostic_t != graph_t.diagnostic_t)
        @compileError("ABI and engine disagree on the imported header types");

    // Sizes asserted by the header's _Static_assert lines.
    const sizes = .{
        .{ facts_t, 1656 },
        .{ verdict_t, 24 },
        .{ event_t, 1328 },
        .{ observation_t, 56 },
        .{ live_t, 48 },
        .{ retiring_entry_t, 24 },
        .{ retire_proof_t, 8 },
        .{ diagnostic_t, 56 },
        .{ stats_t, 256 },
        .{ config_t, 48 },
    };
    for (sizes) |entry| {
        if (@sizeOf(entry[0]) != entry[1]) @compileError("ABI struct size drifted from the header");
    }
    // A cheap subset of the header's offset assertions: first and last field of every large struct.
    const offsets = .{
        .{ facts_t, "stamp", 40 },
        .{ facts_t, "title", 632 },
        .{ event_t, "reason", 88 },
        .{ event_t, "app_key", 1128 },
        .{ observation_t, "deferred_dropped", 48 },
        .{ live_t, "process_id", 40 },
        .{ retiring_entry_t, "role", 20 },
        .{ retire_proof_t, "pins_drained", 4 },
        .{ diagnostic_t, "events_pending", 52 },
        .{ stats_t, "suspended", 248 },
        .{ config_t, "now_ms_context", 40 },
    };
    for (offsets) |entry| {
        if (@offsetOf(entry[0], entry[1]) != entry[2]) @compileError("ABI field offset drifted from the header");
    }
    if (@alignOf(graph_t.graph_t) > 8) @compileError("the graph must be allocatable with 8-byte alignment");
}

const range_t = struct {
    start: usize,
    end: usize,
};

/// A zero-length region: stands in for an unused optional or ignored argument; overlaps nothing.
const EmptySpan: range_t = .{ .start = 0, .end = 0 };

fn status_code(status: graph_t.status_t) c_int {
    return @intFromEnum(status);
}

fn checked_range(address: usize, length: usize) ?range_t {
    if (address == 0 or length == 0) return null;
    const end = std_t.math.add(usize, address, length) catch return null;
    return .{ .start = address, .end = end };
}

/// Region of one `T` at `pointer`, or null for NULL, misalignment or an overflowing range.
fn span_of(comptime value_t: type, pointer: anytype) ?range_t {
    const address = @intFromPtr(pointer);
    if (address % @alignOf(value_t) != 0) return null;
    return checked_range(address, @sizeOf(value_t));
}

/// Region of `count` consecutive `T` at `pointer`, or null for NULL, misalignment or overflow.
fn array_span_of(comptime value_t: type, pointer: anytype, count: u32) ?range_t {
    const address = @intFromPtr(pointer);
    if (address % @alignOf(value_t) != 0) return null;
    const length = std_t.math.mul(usize, @sizeOf(value_t), count) catch return null;
    return checked_range(address, length);
}

/// True iff every region exists and no two regions overlap.
fn spans_valid(spans: []const ?range_t) bool {
    for (spans, 0..) |maybe_left, left_index| {
        const left = maybe_left orelse return false;
        for (spans[left_index + 1 ..]) |maybe_right| {
            const right = maybe_right orelse return false;
            if (left.start < right.end and right.start < left.end) return false;
        }
    }
    return true;
}

fn graph_span(pointer: *const graph_t.graph_t) ?range_t {
    return checked_range(@intFromPtr(pointer), @sizeOf(graph_t.graph_t));
}

/// The engine graph behind a handle, or null for NULL, misalignment or an overflowing extent.
fn graph_of(handle: ?*graph_handle_t) ?*graph_t.graph_t {
    const pointer = handle orelse return null;
    const address = @intFromPtr(pointer);
    if (address % @alignOf(graph_t.graph_t) != 0) return null;
    if (checked_range(address, @sizeOf(graph_t.graph_t)) == null) return null;
    const typed: *graph_t.graph_t = @ptrCast(@alignCast(pointer));
    return typed;
}

fn graph_of_const(handle: ?*const graph_handle_t) ?*const graph_t.graph_t {
    const pointer = handle orelse return null;
    const address = @intFromPtr(pointer);
    if (address % @alignOf(graph_t.graph_t) != 0) return null;
    if (checked_range(address, @sizeOf(graph_t.graph_t)) == null) return null;
    const typed: *const graph_t.graph_t = @ptrCast(@alignCast(pointer));
    return typed;
}

fn aligned_const(comptime value_t: type, pointer: ?*align(1) const value_t) *const value_t {
    return @alignCast(pointer.?);
}

fn aligned_mut(comptime value_t: type, pointer: ?*align(1) value_t) *value_t {
    return @alignCast(pointer.?);
}

/// Status of a call whose pointer arguments failed validation: the engine guards (wrong thread,
/// re-entrancy, stale epoch) still come first, then InvalidArgument.
fn fault_mutating(graph_pointer: *graph_t.graph_t, epoch: ?u64) c_int {
    if (graph_t.enter(graph_pointer, epoch)) |status| return status_code(status);
    graph_t.leave(graph_pointer);
    return StatusInvalidArgument;
}

/// Read-only variant of `fault_mutating` for calls without an epoch parameter that must not
/// change any state (not even the busy flag).
fn fault_reading(graph_pointer: *const graph_t.graph_t) c_int {
    if (graph_t.enter_read(graph_pointer)) |status| return status_code(status);
    return StatusInvalidArgument;
}

/// Create a graph bound to the calling thread (header: interop_window_graph_create).
///
/// @param[in] config Non-NULL, naturally aligned, borrowed; copied by value once, never
///                   retained. Must not overlap `out_graph`. Field rules are the engine's.
/// @param[out] out_graph Non-NULL, 8-byte aligned caller slot, disjoint from `config`. It is set
///                       to NULL before other config validation and receives the new handle on
///                       Ok. On other failures it remains NULL. A NULL/misaligned slot is never
///                       written; overlapping config/output ranges are rejected before either
///                       range is read or written, so both remain unchanged.
/// @return Ok; InvalidArgument for a bad slot or config pointer, an overlap, or a bad config
///         field; NoMemory when the single allocation fails (nothing leaks).
/// @note Ownership: on Ok the caller owns the handle until `interop_window_graph_destroy`. The
///       owner thread is the calling thread. Allocates exactly once through `abi_allocator`.
///       Thread-safe for distinct arguments.
export fn interop_window_graph_create(
    config: ?*align(1) const config_t,
    out_graph: ?*align(1) ?*graph_handle_t,
) c_int {
    const slot_span = span_of(?*graph_handle_t, out_graph) orelse return StatusInvalidArgument;
    // Establish non-overlap from the raw config extent before writing the output slot. This
    // deliberately does not require natural config alignment: even a misaligned aliased input
    // must remain untouched. An unrepresentable extent is rejected conservatively.
    if (config) |config_pointer| {
        const config_start = @intFromPtr(config_pointer);
        const config_end = std_t.math.add(usize, config_start, @sizeOf(config_t)) catch return StatusInvalidArgument;
        if (slot_span.start < config_end and config_start < slot_span.end) return StatusInvalidArgument;
    }
    const slot = aligned_mut(?*graph_handle_t, out_graph);
    slot.* = null;
    if (!spans_valid(&.{ slot_span, span_of(config_t, config) })) return StatusInvalidArgument;
    const local_config: config_t = aligned_const(config_t, config).*;
    const created = graph_t.create(abi_allocator, &local_config) catch |err| return switch (err) {
        error.OutOfMemory => StatusNoMemory,
        error.InvalidArgument => StatusInvalidArgument,
    };
    const handle: *graph_handle_t = @ptrCast(created);
    slot.* = handle;
    return StatusOk;
}

/// Free a graph and clear the caller's handle (header: interop_window_graph_destroy).
///
/// @param[in,out] graph_slot Non-NULL, 8-byte aligned caller slot that must not lie inside the
///                           graph it names. If `*graph_slot` is NULL the call is an Ok no-op.
///                           On Ok the graph is freed and `*graph_slot` is NULL; on any other
///                           status nothing is freed and the slot is unchanged.
/// @return Ok; InvalidArgument for a NULL or misaligned slot, a misaligned handle or a slot inside
///         the graph; WrongThread or Reentrant (graph not freed).
/// @note Ownership: consumes the handle in the slot, freeing it through the allocator that
///       created it. Owner thread only; not safe against concurrent use of the same graph.
export fn interop_window_graph_destroy(graph_slot: ?*align(1) ?*graph_handle_t) c_int {
    const slot_span = span_of(?*graph_handle_t, graph_slot) orelse return StatusInvalidArgument;
    const slot = aligned_mut(?*graph_handle_t, graph_slot);
    const handle = slot.* orelse return StatusOk;
    const graph_pointer = graph_of(handle) orelse return StatusInvalidArgument;
    if (!spans_valid(&.{ graph_span(graph_pointer), slot_span })) return fault_reading(graph_pointer);
    if (graph_t.enter_read(graph_pointer)) |status| return status_code(status);
    graph_t.destroy(graph_pointer);
    slot.* = null;
    return StatusOk;
}

/// Classify one fact snapshot without a graph (header: interop_window_graph_classify).
///
/// @param[in] facts Non-NULL, aligned, borrowed; read during the call only.
/// @param[in] uac_mode 0..2; anything above is InvalidArgument.
/// @param[out] verdict Non-NULL, aligned, disjoint from `facts`; written fully on Ok only.
/// @return Ok; InvalidArgument for a bad pointer, an overlap or `uac_mode` above 2;
///         InvalidFacts when the snapshot violates the validation list.
/// @note Pure: any thread, re-entrant, no allocation, no graph access.
export fn interop_window_graph_classify(
    facts: ?*align(1) const facts_t,
    uac_mode: u32,
    verdict: ?*align(1) verdict_t,
) c_int {
    if (!spans_valid(&.{ span_of(facts_t, facts), span_of(verdict_t, verdict) })) return StatusInvalidArgument;
    var staged: verdict_t = undefined;
    const status = graph_t.classify_public(aligned_const(facts_t, facts), uac_mode, &staged);
    if (status == .Ok) aligned_mut(verdict_t, verdict).* = staged;
    return status_code(status);
}

/// Offer one fact snapshot (header: interop_window_graph_observe).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @param[in] facts Non-NULL, aligned, borrowed; the engine copies it at entry.
/// @param[out] observation Non-NULL, aligned, disjoint from `facts` and the graph; written fully
///                         on Ok only.
/// @return The engine status (header lists them); InvalidArgument for a bad pointer or overlap
///         after the wrong-thread, re-entrancy and epoch rules.
/// @note Owner thread only. The app-key callback may run once. No allocation.
export fn interop_window_graph_observe(
    graph_handle: ?*graph_handle_t,
    session_epoch: u64,
    facts: ?*align(1) const facts_t,
    observation: ?*align(1) observation_t,
) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    if (!spans_valid(&.{ graph_span(graph_pointer), span_of(facts_t, facts), span_of(observation_t, observation) }))
        return fault_mutating(graph_pointer, session_epoch);
    var staged: observation_t = undefined;
    const status = graph_t.observe(graph_pointer, session_epoch, aligned_const(facts_t, facts), &staged);
    if (status == .Ok) aligned_mut(observation_t, observation).* = staged;
    return status_code(status);
}

/// Apply a window-destroyed hint (header: interop_window_graph_note_destroyed).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @param[in] window Nonzero HWND.
/// @param[in] hint_stamp Nonzero observer stamp.
/// @param[in] event_time_ms Raw 32-bit tick of the event.
/// @param[out] observation Non-NULL, aligned, disjoint from the graph; written fully on Ok only.
/// @return The engine status; InvalidArgument for a bad pointer, zero window or zero stamp.
/// @note Owner thread only; no callback; no allocation.
export fn interop_window_graph_note_destroyed(
    graph_handle: ?*graph_handle_t,
    session_epoch: u64,
    window: u64,
    hint_stamp: u64,
    event_time_ms: u32,
    observation: ?*align(1) observation_t,
) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    if (!spans_valid(&.{ graph_span(graph_pointer), span_of(observation_t, observation) }))
        return fault_mutating(graph_pointer, session_epoch);
    var staged: observation_t = undefined;
    const status = graph_t.note_destroyed(graph_pointer, session_epoch, window, hint_stamp, event_time_ms, &staged);
    if (status == .Ok) aligned_mut(observation_t, observation).* = staged;
    return status_code(status);
}

/// End the quarantine of one retired generation (header: interop_window_graph_complete_retire).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @param[in] window Nonzero HWND.
/// @param[in] generation Nonzero generation of a Retiring entry.
/// @param[in] proof Non-NULL, aligned, borrowed, disjoint from the graph; both flags must be 1.
/// @return The engine status (Ok, Stale, NotRetiring, ProofIncomplete, guards); InvalidArgument
///         for a bad proof pointer, zero window or zero generation.
/// @note Owner thread only; releases only the graph slot; no callback; no allocation.
export fn interop_window_graph_complete_retire(
    graph_handle: ?*graph_handle_t,
    session_epoch: u64,
    window: u64,
    generation: u64,
    proof: ?*align(1) const retire_proof_t,
) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    if (!spans_valid(&.{ graph_span(graph_pointer), span_of(retire_proof_t, proof) }))
        return fault_mutating(graph_pointer, session_epoch);
    const local_proof: retire_proof_t = aligned_const(retire_proof_t, proof).*;
    return status_code(graph_t.complete_retire(graph_pointer, session_epoch, window, generation, &local_proof));
}

/// Check that an action may be performed on (window, generation) (header:
/// interop_window_graph_check_action).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @param[in] window Nonzero HWND.
/// @param[in] generation Nonzero generation.
/// @return Ok only for an Active exact pair; Stale otherwise; InvalidArgument for zero values;
///         guard statuses.
/// @note Owner thread only; only counters can change; no allocation.
export fn interop_window_graph_check_action(
    graph_handle: ?*graph_handle_t,
    session_epoch: u64,
    window: u64,
    generation: u64,
) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    return status_code(graph_t.check_action(graph_pointer, session_epoch, window, generation));
}

/// Retire every Active entry and clear deferred records (header: interop_window_graph_invalidate_all).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @param[out] retired_out Nullable. When non-NULL it must be aligned and disjoint from the graph;
///                         receives the retired count on Ok only.
/// @return The engine status (Ok, QueueFull, guards); InvalidArgument for a non-NULL bad pointer.
/// @note Owner thread only; no callback; no allocation.
export fn interop_window_graph_invalidate_all(
    graph_handle: ?*graph_handle_t,
    session_epoch: u64,
    retired_out: ?*align(1) u32,
) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    const retired_span = if (retired_out == null) EmptySpan else span_of(u32, retired_out);
    if (!spans_valid(&.{ graph_span(graph_pointer), retired_span })) return fault_mutating(graph_pointer, session_epoch);
    var staged: u32 = 0;
    const status = graph_t.invalidate_all(graph_pointer, session_epoch, &staged);
    if (status == .Ok and retired_out != null) aligned_mut(u32, retired_out).* = staged;
    return status_code(status);
}

/// Open a refresh (header: interop_window_graph_refresh_begin).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @return Ok; RefreshActive; guard statuses.
/// @note Owner thread only; no callback; no allocation.
export fn interop_window_graph_refresh_begin(graph_handle: ?*graph_handle_t, session_epoch: u64) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    return status_code(graph_t.refresh_begin(graph_pointer, session_epoch));
}

/// Offer one snapshot inside an open refresh (header: interop_window_graph_refresh_offer).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @param[in] facts Non-NULL, aligned, borrowed; the engine copies it at entry.
/// @param[out] observation Non-NULL, aligned, disjoint from `facts` and the graph; written fully
///                         on Ok only.
/// @return As observe, plus NoRefresh; a bad pointer is InvalidArgument and marks nothing.
/// @note Owner thread only; the app-key callback may run once; no allocation.
export fn interop_window_graph_refresh_offer(
    graph_handle: ?*graph_handle_t,
    session_epoch: u64,
    facts: ?*align(1) const facts_t,
    observation: ?*align(1) observation_t,
) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    if (!spans_valid(&.{ graph_span(graph_pointer), span_of(facts_t, facts), span_of(observation_t, observation) }))
        return fault_mutating(graph_pointer, session_epoch);
    var staged: observation_t = undefined;
    const status = graph_t.refresh_offer(graph_pointer, session_epoch, aligned_const(facts_t, facts), &staged);
    if (status == .Ok) aligned_mut(observation_t, observation).* = staged;
    return status_code(status);
}

/// Mark a window seen without offering facts (header: interop_window_graph_refresh_keep).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @param[in] window Nonzero HWND.
/// @return Ok; None when neither an Active entry nor a deferred record exists; NoRefresh;
///         InvalidArgument for window 0; guard statuses.
/// @note Owner thread only; no callback; no allocation.
export fn interop_window_graph_refresh_keep(graph_handle: ?*graph_handle_t, session_epoch: u64, window: u64) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    return status_code(graph_t.refresh_keep(graph_pointer, session_epoch, window));
}

/// List the Active windows still unseen in the open refresh (header: interop_window_graph_refresh_unseen).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @param[out] windows Array of `capacity` aligned u64, disjoint from `count` and the graph; may be
///                     NULL only when `capacity` is 0 (the pointer is then ignored). Up to
///                     `capacity` entries are written on Ok; the rest is untouched.
/// @param[in] capacity Elements available in `windows`.
/// @param[out] count Non-NULL, aligned; receives the total unseen count on Ok only.
/// @return The engine status (Ok, NoRefresh, guards); InvalidArgument for a bad pointer or overlap.
/// @note Owner thread only; no callback; no allocation.
export fn interop_window_graph_refresh_unseen(
    graph_handle: ?*graph_handle_t,
    session_epoch: u64,
    windows: ?*align(1) u64,
    capacity: u32,
    count: ?*align(1) u32,
) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    const windows_span = if (capacity == 0) EmptySpan else array_span_of(u64, windows, capacity);
    if (!spans_valid(&.{ graph_span(graph_pointer), windows_span, span_of(u32, count) }))
        return fault_mutating(graph_pointer, session_epoch);
    var staged: u32 = 0;
    var many: ?[*]u64 = null;
    if (capacity != 0) {
        const first: *u64 = aligned_mut(u64, windows);
        many = @ptrCast(first);
    }
    const status = graph_t.refresh_unseen(graph_pointer, session_epoch, many, capacity, &staged);
    if (status == .Ok) aligned_mut(u32, count).* = staged;
    return status_code(status);
}

/// Close the refresh, retiring every unseen entry (header: interop_window_graph_refresh_commit).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @param[out] retired_out Nullable. When non-NULL it must be aligned and disjoint from the graph;
///                         receives the retired count on Ok only.
/// @return The engine status (Ok, NoRefresh, QueueFull, guards); InvalidArgument for a bad pointer.
/// @note Owner thread only; no callback; no allocation.
export fn interop_window_graph_refresh_commit(
    graph_handle: ?*graph_handle_t,
    session_epoch: u64,
    retired_out: ?*align(1) u32,
) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    const retired_span = if (retired_out == null) EmptySpan else span_of(u32, retired_out);
    if (!spans_valid(&.{ graph_span(graph_pointer), retired_span })) return fault_mutating(graph_pointer, session_epoch);
    var staged: u32 = 0;
    const status = graph_t.refresh_commit(graph_pointer, session_epoch, &staged);
    if (status == .Ok and retired_out != null) aligned_mut(u32, retired_out).* = staged;
    return status_code(status);
}

/// Close the refresh without retiring anything (header: interop_window_graph_refresh_abort).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @return Ok; NoRefresh; guard statuses.
/// @note Owner thread only; no callback; no allocation.
export fn interop_window_graph_refresh_abort(graph_handle: ?*graph_handle_t, session_epoch: u64) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    return status_code(graph_t.refresh_abort(graph_pointer, session_epoch));
}

/// Pop the oldest queued event (header: interop_window_graph_next_event).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @param[out] event Non-NULL, aligned, disjoint from the graph; a value copy on Ok only.
/// @return Ok; None for an empty queue; guard statuses; InvalidArgument for a bad pointer.
/// @note Owner thread only; no callback; no allocation.
export fn interop_window_graph_next_event(
    graph_handle: ?*graph_handle_t,
    session_epoch: u64,
    event: ?*align(1) event_t,
) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    if (!spans_valid(&.{ graph_span(graph_pointer), span_of(event_t, event) }))
        return fault_mutating(graph_pointer, session_epoch);
    var staged: event_t = undefined;
    const status = graph_t.next_event(graph_pointer, session_epoch, &staged);
    if (status == .Ok) aligned_mut(event_t, event).* = staged;
    return status_code(status);
}

/// Deliver up to `max_events` queued events to a sink (header: interop_window_graph_drain).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[in] session_epoch Captured epoch; must equal the graph's.
/// @param[in] sink Non-NULL C-ABI callback (NULL is InvalidArgument after the guards); passed
///                 through unchanged; it must not call into the graph (Reentrant).
/// @param[in,out] sink_context Opaque pointer handed to the sink unchanged; may be NULL; never
///                             dereferenced here.
/// @param[in] max_events At least 1.
/// @param[out] delivered Non-NULL, aligned, disjoint from the graph; written on Ok, None and
///                       SinkFailed only.
/// @return Ok; None; SinkFailed (the refused event stays queued); guard statuses; InvalidArgument.
/// @note Owner thread only; the sink runs on the owner thread; no allocation.
export fn interop_window_graph_drain(
    graph_handle: ?*graph_handle_t,
    session_epoch: u64,
    sink: graph_t.event_sink_t,
    sink_context: ?*anyopaque,
    max_events: u32,
    delivered: ?*align(1) u32,
) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    if (!spans_valid(&.{ graph_span(graph_pointer), span_of(u32, delivered) }))
        return fault_mutating(graph_pointer, session_epoch);
    var staged: u32 = 0;
    const status = graph_t.drain(graph_pointer, session_epoch, sink, sink_context, max_events, &staged);
    switch (status) {
        .Ok, .None, .SinkFailed => aligned_mut(u32, delivered).* = staged,
        else => {},
    }
    return status_code(status);
}

/// Take the rate-limited capacity diagnostic (header: interop_window_graph_take_capacity_diagnostic).
///
/// @param[in,out] graph_handle Graph handle; NULL or misaligned is InvalidArgument.
/// @param[out] diagnostic Non-NULL, aligned, disjoint from the graph; written fully on Ok only.
/// @return Ok; None when nothing is due; guard statuses (no epoch check); InvalidArgument.
/// @note Owner thread only; calls the configured clock; no allocation.
export fn interop_window_graph_take_capacity_diagnostic(
    graph_handle: ?*graph_handle_t,
    diagnostic: ?*align(1) diagnostic_t,
) c_int {
    const graph_pointer = graph_of(graph_handle) orelse return StatusInvalidArgument;
    if (!spans_valid(&.{ graph_span(graph_pointer), span_of(diagnostic_t, diagnostic) }))
        return fault_mutating(graph_pointer, null);
    var staged: diagnostic_t = undefined;
    const status = graph_t.take_capacity_diagnostic(graph_pointer, &staged);
    if (status == .Ok) aligned_mut(diagnostic_t, diagnostic).* = staged;
    return status_code(status);
}

/// Copy the cumulative counters and gauges (header: interop_window_graph_get_stats).
///
/// @param[in] graph_handle Graph handle; NULL or misaligned is InvalidArgument. Never written.
/// @param[out] stats Non-NULL, aligned, disjoint from the graph; written fully on Ok only.
/// @return Ok; WrongThread; Reentrant; InvalidArgument.
/// @note Owner thread only; changes no state or counter; no allocation.
export fn interop_window_graph_get_stats(graph_handle: ?*const graph_handle_t, stats: ?*align(1) stats_t) c_int {
    const graph_pointer = graph_of_const(graph_handle) orelse return StatusInvalidArgument;
    if (!spans_valid(&.{ graph_span(graph_pointer), span_of(stats_t, stats) })) return fault_reading(graph_pointer);
    var staged: stats_t = undefined;
    const status = graph_t.get_stats(graph_pointer, &staged);
    if (status == .Ok) aligned_mut(stats_t, stats).* = staged;
    return status_code(status);
}

/// Read the largest accepted stamp (header: interop_window_graph_stamp_high_water).
///
/// @param[in] graph_handle Graph handle; NULL or misaligned is InvalidArgument. Never written.
/// @param[out] stamp Non-NULL, 8-byte aligned, disjoint from the graph; written on Ok only.
/// @return Ok; WrongThread; Reentrant; InvalidArgument.
/// @note Owner thread only; changes no state or counter; no allocation.
export fn interop_window_graph_stamp_high_water(graph_handle: ?*const graph_handle_t, stamp: ?*align(1) u64) c_int {
    const graph_pointer = graph_of_const(graph_handle) orelse return StatusInvalidArgument;
    if (!spans_valid(&.{ graph_span(graph_pointer), span_of(u64, stamp) })) return fault_reading(graph_pointer);
    var staged: u64 = 0;
    const status = graph_t.stamp_high_water(graph_pointer, &staged);
    if (status == .Ok) aligned_mut(u64, stamp).* = staged;
    return status_code(status);
}

/// Look up the Active entry for a window (header: interop_window_graph_find_live).
///
/// @param[in] graph_handle Graph handle; NULL or misaligned is InvalidArgument. Never written.
/// @param[in] window Nonzero HWND.
/// @param[out] live Non-NULL, aligned, disjoint from the graph; written fully on Ok only.
/// @return Ok; None when no Active entry exists; InvalidArgument; WrongThread; Reentrant.
/// @note Owner thread only; changes no state or counter; no allocation.
export fn interop_window_graph_find_live(
    graph_handle: ?*const graph_handle_t,
    window: u64,
    live: ?*align(1) live_t,
) c_int {
    const graph_pointer = graph_of_const(graph_handle) orelse return StatusInvalidArgument;
    if (!spans_valid(&.{ graph_span(graph_pointer), span_of(live_t, live) })) return fault_reading(graph_pointer);
    var staged: live_t = undefined;
    const status = graph_t.find_live(graph_pointer, window, &staged);
    if (status == .Ok) aligned_mut(live_t, live).* = staged;
    return status_code(status);
}

/// List the quarantined entries (header: interop_window_graph_list_retiring).
///
/// @param[in] graph_handle Graph handle; NULL or misaligned is InvalidArgument. Never written.
/// @param[out] entries Array of `capacity` aligned entries, disjoint from `count` and the graph;
///                     may be NULL only when `capacity` is 0 (the pointer is then ignored). Up to
///                     `capacity` entries are written on Ok; the rest is untouched.
/// @param[in] capacity Elements available in `entries`.
/// @param[out] count Non-NULL, aligned; receives the total number of Retiring entries on Ok only.
/// @return Ok; InvalidArgument for a bad pointer or overlap; WrongThread; Reentrant.
/// @note Owner thread only; changes no state or counter; no allocation.
export fn interop_window_graph_list_retiring(
    graph_handle: ?*const graph_handle_t,
    entries: ?*align(1) retiring_entry_t,
    capacity: u32,
    count: ?*align(1) u32,
) c_int {
    const graph_pointer = graph_of_const(graph_handle) orelse return StatusInvalidArgument;
    const entries_span = if (capacity == 0) EmptySpan else array_span_of(retiring_entry_t, entries, capacity);
    if (!spans_valid(&.{ graph_span(graph_pointer), entries_span, span_of(u32, count) }))
        return fault_reading(graph_pointer);
    var staged: u32 = 0;
    var many: ?[*]retiring_entry_t = null;
    if (capacity != 0) {
        const first: *retiring_entry_t = aligned_mut(retiring_entry_t, entries);
        many = @ptrCast(first);
    }
    const status = graph_t.list_retiring(graph_pointer, many, capacity, &staged);
    if (status == .Ok) aligned_mut(u32, count).* = staged;
    return status_code(status);
}

/// Copy the unique Active/Retiring HWND set and exact Free-slot count (header: interop_window_graph_list_tracked_windows).
///
/// @param[in] graph_handle Graph handle; NULL or misaligned is InvalidArgument. Never written.
/// @param[in] epoch Exact graph epoch; checked after WrongThread/Reentrant and before argument errors.
/// @param[out] windows Non-NULL aligned array of `capacity` u64 values; only `count` values are
///                     copied on Ok. The complete claimed capacity is validated for overlap and
///                     address wrap; the unused tail stays unchanged.
/// @param[in] capacity Must be at least `InteropMaxWindows`.
/// @param[out] count Non-NULL aligned; receives the unique nonzero Active/Retiring HWND count on Ok.
/// @param[out] free_entry_count Non-NULL aligned; receives the exact Free entry-slot count on Ok.
/// @return Ok; InvalidArgument; WrongThread; Reentrant; StaleEpoch. StaleEpoch changes no counter.
///         Every non-Ok status leaves graph state/counters and all caller bytes unchanged.
/// @note Owner thread only; no callback, clock, allocation, lock, wait or graph mutation.
export fn interop_window_graph_list_tracked_windows(
    graph_handle: ?*const graph_handle_t,
    epoch: u64,
    windows: ?*align(1) u64,
    capacity: u32,
    count: ?*align(1) u32,
    free_entry_count: ?*align(1) u32,
) c_int {
    const graph_pointer = graph_of_const(graph_handle) orelse return StatusInvalidArgument;
    const windows_span = if (capacity == 0) EmptySpan else array_span_of(u64, windows, capacity);
    const count_span = span_of(u32, count);
    const free_span = span_of(u32, free_entry_count);
    const pointers_valid = capacity >= graph_t.MaxEntries and
        spans_valid(&.{ graph_span(graph_pointer), windows_span, count_span, free_span });

    var staged_windows: [graph_t.MaxEntries]u64 = undefined;
    var staged_count: u32 = 0;
    var staged_free_count: u32 = 0;
    const engine_windows: ?[*]u64 = if (pointers_valid) @ptrCast(&staged_windows) else null;
    const engine_count: ?*u32 = if (pointers_valid) &staged_count else null;
    const engine_free_count: ?*u32 = if (pointers_valid) &staged_free_count else null;
    const status = graph_t.list_tracked_windows(
        graph_pointer,
        epoch,
        engine_windows,
        capacity,
        engine_count,
        engine_free_count,
    );
    if (status == .Ok) {
        const output: [*]u64 = @ptrCast(aligned_mut(u64, windows));
        @memcpy(output[0..staged_count], staged_windows[0..staged_count]);
        aligned_mut(u32, count).* = staged_count;
        aligned_mut(u32, free_entry_count).* = staged_free_count;
    }
    return status_code(status);
}

// Test-only fixtures.
const testing_t = std_t.testing;

const TestEpoch: u64 = 0x5EED;
const FixtureStyle: u32 = 0x00CF0000;

fn expect_status(expected: graph_t.status_t, actual: c_int) !void {
    try testing_t.expectEqual(@as(c_int, @intFromEnum(expected)), actual);
}

fn make_facts(window: u64, stamp: u64) facts_t {
    var facts = std_t.mem.zeroes(facts_t);
    facts.window = window;
    facts.root = window;
    facts.stamp = stamp;
    facts.style = FixtureStyle;
    facts.flags = graph_t.FactVisible | graph_t.FactEnabled | graph_t.FactOnInputDesktop;
    facts.process_id = 4242;
    facts.thread_id = 4243;
    facts.process_start_time = 1;
    facts.captured_ms = 1000;
    facts.width = 640;
    facts.height = 480;
    facts.dpi = 96;
    facts.class_length = 7;
    @memcpy(facts.class_name[0..7], "Notepad");
    facts.image_length = 11;
    @memcpy(facts.image_name[0..11], "notepad.exe");
    facts.title_length = 3;
    @memcpy(facts.title[0..3], "abc");
    return facts;
}

fn make_dialog_facts(window: u64, owner: u64, stamp: u64) facts_t {
    var facts = make_facts(window, stamp);
    facts.owner = owner;
    facts.owner_root = owner;
    return facts;
}

fn sentinel(comptime value_t: type) value_t {
    var value: value_t = undefined;
    @memset(std_t.mem.asBytes(&value), 0xA5);
    return value;
}

fn is_sentinel(value: anytype) bool {
    for (std_t.mem.asBytes(value)) |byte| {
        if (byte != 0xA5) return false;
    }
    return true;
}

fn fake_now(context: ?*anyopaque) callconv(.C) u64 {
    const clock: *u64 = @ptrCast(@alignCast(context.?));
    return clock.*;
}

const BatteryCount = 20;

/// Statuses of one call to every graph-taking export (destroy last) plus classify.
const battery_t = struct {
    status: [BatteryCount]c_int = [_]c_int{-1} ** BatteryCount,
    classify_status: c_int = -1,
    slot_kept: bool = false,
};

const lookup_probe_t = struct {
    handle: ?*graph_handle_t = null,
    epoch: u64 = TestEpoch,
    calls: u32 = 0,
    fail: bool = false,
    run_battery: bool = false,
    battery: battery_t = .{},
};

const sink_probe_t = struct {
    handle: ?*graph_handle_t = null,
    epoch: u64 = TestEpoch,
    calls: u32 = 0,
    fail_on_call: u32 = 0,
    run_battery: bool = false,
    windows: [8]u64 = [_]u64{0} ** 8,
    kinds: [8]u32 = [_]u32{0} ** 8,
    battery: battery_t = .{},
};

fn probe_lookup(
    context: ?*anyopaque,
    facts: [*c]const facts_t,
    key: [*c]u8,
    capacity: u32,
    length: [*c]u32,
) callconv(.C) c_int {
    _ = facts;
    const probe: *lookup_probe_t = @ptrCast(@alignCast(context.?));
    probe.calls += 1;
    if (probe.run_battery) run_battery(probe.handle, probe.epoch, &probe.battery);
    if (probe.fail) return 1;
    const text = "a-test";
    if (capacity < text.len) return 1;
    @memcpy(key[0..text.len], text);
    length.* = text.len;
    return 0;
}

fn probe_sink(context: ?*anyopaque, event: [*c]const event_t) callconv(.C) c_int {
    const probe: *sink_probe_t = @ptrCast(@alignCast(context.?));
    if (probe.run_battery) run_battery(probe.handle, probe.epoch, &probe.battery);
    if (probe.calls < probe.windows.len) {
        probe.windows[probe.calls] = event.*.window;
        probe.kinds[probe.calls] = event.*.kind;
    }
    probe.calls += 1;
    if (probe.fail_on_call != 0 and probe.calls == probe.fail_on_call) return 1;
    return 0;
}

// ---- never-inlined call wrappers: every export is reached through its C symbol only ----

fn call_create(config: ?*align(1) const config_t, out_graph: ?*align(1) ?*graph_handle_t) c_int {
    return @call(.never_inline, interop_window_graph_create, .{ config, out_graph });
}

fn call_destroy(slot: ?*align(1) ?*graph_handle_t) c_int {
    return @call(.never_inline, interop_window_graph_destroy, .{slot});
}

fn call_classify(facts: ?*align(1) const facts_t, uac_mode: u32, verdict: ?*align(1) verdict_t) c_int {
    return @call(.never_inline, interop_window_graph_classify, .{ facts, uac_mode, verdict });
}

fn call_observe(handle: ?*graph_handle_t, epoch: u64, facts: ?*align(1) const facts_t, out: ?*align(1) observation_t) c_int {
    return @call(.never_inline, interop_window_graph_observe, .{ handle, epoch, facts, out });
}

fn call_note_destroyed(
    handle: ?*graph_handle_t,
    epoch: u64,
    window: u64,
    stamp: u64,
    tick: u32,
    out: ?*align(1) observation_t,
) c_int {
    return @call(.never_inline, interop_window_graph_note_destroyed, .{ handle, epoch, window, stamp, tick, out });
}

fn call_complete_retire(
    handle: ?*graph_handle_t,
    epoch: u64,
    window: u64,
    generation: u64,
    proof: ?*align(1) const retire_proof_t,
) c_int {
    return @call(.never_inline, interop_window_graph_complete_retire, .{ handle, epoch, window, generation, proof });
}

fn call_check_action(handle: ?*graph_handle_t, epoch: u64, window: u64, generation: u64) c_int {
    return @call(.never_inline, interop_window_graph_check_action, .{ handle, epoch, window, generation });
}

fn call_invalidate_all(handle: ?*graph_handle_t, epoch: u64, retired: ?*align(1) u32) c_int {
    return @call(.never_inline, interop_window_graph_invalidate_all, .{ handle, epoch, retired });
}

fn call_refresh_begin(handle: ?*graph_handle_t, epoch: u64) c_int {
    return @call(.never_inline, interop_window_graph_refresh_begin, .{ handle, epoch });
}

fn call_refresh_offer(handle: ?*graph_handle_t, epoch: u64, facts: ?*align(1) const facts_t, out: ?*align(1) observation_t) c_int {
    return @call(.never_inline, interop_window_graph_refresh_offer, .{ handle, epoch, facts, out });
}

fn call_refresh_keep(handle: ?*graph_handle_t, epoch: u64, window: u64) c_int {
    return @call(.never_inline, interop_window_graph_refresh_keep, .{ handle, epoch, window });
}

fn call_refresh_unseen(
    handle: ?*graph_handle_t,
    epoch: u64,
    windows: ?*align(1) u64,
    capacity: u32,
    count: ?*align(1) u32,
) c_int {
    return @call(.never_inline, interop_window_graph_refresh_unseen, .{ handle, epoch, windows, capacity, count });
}

fn call_refresh_commit(handle: ?*graph_handle_t, epoch: u64, retired: ?*align(1) u32) c_int {
    return @call(.never_inline, interop_window_graph_refresh_commit, .{ handle, epoch, retired });
}

fn call_refresh_abort(handle: ?*graph_handle_t, epoch: u64) c_int {
    return @call(.never_inline, interop_window_graph_refresh_abort, .{ handle, epoch });
}

fn call_next_event(handle: ?*graph_handle_t, epoch: u64, event: ?*align(1) event_t) c_int {
    return @call(.never_inline, interop_window_graph_next_event, .{ handle, epoch, event });
}

fn call_drain(
    handle: ?*graph_handle_t,
    epoch: u64,
    sink: graph_t.event_sink_t,
    context: ?*anyopaque,
    max_events: u32,
    delivered: ?*align(1) u32,
) c_int {
    return @call(.never_inline, interop_window_graph_drain, .{ handle, epoch, sink, context, max_events, delivered });
}

fn call_take_diagnostic(handle: ?*graph_handle_t, diagnostic: ?*align(1) diagnostic_t) c_int {
    return @call(.never_inline, interop_window_graph_take_capacity_diagnostic, .{ handle, diagnostic });
}

fn call_get_stats(handle: ?*const graph_handle_t, stats: ?*align(1) stats_t) c_int {
    return @call(.never_inline, interop_window_graph_get_stats, .{ handle, stats });
}

fn call_stamp_high_water(handle: ?*const graph_handle_t, stamp: ?*align(1) u64) c_int {
    return @call(.never_inline, interop_window_graph_stamp_high_water, .{ handle, stamp });
}

fn call_find_live(handle: ?*const graph_handle_t, window: u64, live: ?*align(1) live_t) c_int {
    return @call(.never_inline, interop_window_graph_find_live, .{ handle, window, live });
}

fn call_list_retiring(
    handle: ?*const graph_handle_t,
    entries: ?*align(1) retiring_entry_t,
    capacity: u32,
    count: ?*align(1) u32,
) c_int {
    return @call(.never_inline, interop_window_graph_list_retiring, .{ handle, entries, capacity, count });
}

fn call_list_tracked_windows(
    handle: ?*const graph_handle_t,
    epoch: u64,
    windows: ?*align(1) u64,
    capacity: u32,
    count: ?*align(1) u32,
    free_entry_count: ?*align(1) u32,
) c_int {
    return @call(.never_inline, interop_window_graph_list_tracked_windows, .{ handle, epoch, windows, capacity, count, free_entry_count });
}

fn accept_sink(context: ?*anyopaque, event: [*c]const event_t) callconv(.C) c_int {
    _ = context;
    _ = event;
    return 0;
}

/// Call every graph-taking export once with otherwise valid arguments; destroy is last.
fn run_battery(handle: ?*graph_handle_t, epoch: u64, result: *battery_t) void {
    const facts = make_facts(0x9990, 1);
    var verdict: verdict_t = undefined;
    var observation: observation_t = undefined;
    var event: event_t = undefined;
    const proof: retire_proof_t = .{ .capture_stopped = 1, .pins_drained = 1 };
    var diagnostic: diagnostic_t = undefined;
    var stats: stats_t = undefined;
    var live: live_t = undefined;
    var entries: [4]retiring_entry_t = undefined;
    var windows: [64]u64 = undefined;
    var free_count: u32 = 0;
    var stamp: u64 = 0;
    var count: u32 = 0;
    var delivered: u32 = 0;
    var slot = handle;
    result.status = .{
        call_observe(handle, epoch, &facts, &observation),
        call_note_destroyed(handle, epoch, 0x9990, 2, 1000, &observation),
        call_complete_retire(handle, epoch, 0x9990, 1, &proof),
        call_check_action(handle, epoch, 0x9990, 1),
        call_invalidate_all(handle, epoch, null),
        call_refresh_begin(handle, epoch),
        call_refresh_offer(handle, epoch, &facts, &observation),
        call_refresh_keep(handle, epoch, 0x9990),
        call_refresh_unseen(handle, epoch, &windows[0], 4, &count),
        call_refresh_commit(handle, epoch, null),
        call_refresh_abort(handle, epoch),
        call_next_event(handle, epoch, &event),
        call_drain(handle, epoch, accept_sink, null, 1, &delivered),
        call_take_diagnostic(handle, &diagnostic),
        call_get_stats(handle, &stats),
        call_stamp_high_water(handle, &stamp),
        call_find_live(handle, 0x9990, &live),
        call_list_retiring(handle, &entries[0], 4, &count),
        call_list_tracked_windows(handle, epoch, &windows[0], 64, &count, &free_count),
        call_destroy(&slot),
    };
    result.slot_kept = slot == handle;
    result.classify_status = call_classify(&facts, 0, &verdict);
}

const harness_t = struct {
    probe: lookup_probe_t = .{},
    clock: u64 = 0,
    handle: ?*graph_handle_t = null,
    saved_allocator: std_t.mem.Allocator = undefined,
    stamp: u64 = 0,

    fn open(self: *harness_t) !void {
        self.saved_allocator = abi_allocator;
        abi_allocator = testing_t.allocator;
        var config = std_t.mem.zeroes(config_t);
        config.session_epoch = TestEpoch;
        config.app_key_lookup = probe_lookup;
        config.app_key_context = &self.probe;
        config.now_ms = fake_now;
        config.now_ms_context = &self.clock;
        try expect_status(.Ok, call_create(&config, &self.handle));
        try testing_t.expect(self.handle != null);
        self.probe.handle = self.handle;
    }

    fn close(self: *harness_t) void {
        if (self.handle != null) {
            testing_t.expectEqual(StatusOk, call_destroy(&self.handle)) catch @panic("destroy failed");
        }
        abi_allocator = self.saved_allocator;
    }

    fn engine(self: *harness_t) *graph_t.graph_t {
        return graph_of(self.handle).?;
    }

    fn next_stamp(self: *harness_t) u64 {
        self.stamp += 1;
        return self.stamp;
    }

    fn admit(self: *harness_t, window: u64) !observation_t {
        const facts = make_facts(window, self.next_stamp());
        var observation: observation_t = undefined;
        try expect_status(.Ok, call_observe(self.handle, TestEpoch, &facts, &observation));
        return observation;
    }

    fn snapshot(self: *harness_t) ![]u8 {
        return testing_t.allocator.dupe(u8, std_t.mem.asBytes(self.engine()));
    }

    fn expect_unchanged(self: *harness_t, before: []const u8) !void {
        try testing_t.expect(std_t.mem.eql(u8, before, std_t.mem.asBytes(self.engine())));
    }
};

test "create rejects bad slot and config pointers and nulls every valid slot" {
    const saved = abi_allocator;
    abi_allocator = testing_t.allocator;
    defer abi_allocator = saved;
    var config = std_t.mem.zeroes(config_t);
    config.session_epoch = TestEpoch;
    config.app_key_lookup = probe_lookup;
    const stale: ?*graph_handle_t = @ptrFromInt(0x1000);

    try expect_status(.InvalidArgument, call_create(&config, null));
    var slot: ?*graph_handle_t = stale;
    try expect_status(.InvalidArgument, call_create(null, &slot));
    try testing_t.expect(slot == null);

    var raw: [128]u8 align(8) = [_]u8{0xA5} ** 128;
    const misaligned_slot: ?*align(1) ?*graph_handle_t = @ptrCast(&raw[1]);
    try expect_status(.InvalidArgument, call_create(&config, misaligned_slot));
    for (raw) |byte| try testing_t.expectEqual(@as(u8, 0xA5), byte);

    slot = stale;
    const misaligned_config: ?*align(1) const config_t = @ptrCast(&raw[1]);
    try expect_status(.InvalidArgument, call_create(misaligned_config, &slot));
    try testing_t.expect(slot == null);

    // An aliased output is rejected before either caller region is changed.
    var shared: [64]u8 align(8) = [_]u8{0} ** 64;
    const shared_config: *align(8) config_t = @ptrCast(&shared);
    shared_config.* = config;
    const shared_slot: *align(8) ?*graph_handle_t = @ptrCast(&shared[8]);
    shared_slot.* = stale;
    const shared_before = shared;
    try expect_status(.InvalidArgument, call_create(shared_config, shared_slot));
    try testing_t.expectEqualSlices(u8, &shared_before, &shared);

    // Raw extent overlap is detected before config alignment is required.
    var misaligned_shared: [128]u8 align(8) = [_]u8{0x5A} ** 128;
    const misaligned_shared_config: ?*align(1) const config_t = @ptrCast(&misaligned_shared[1]);
    const misaligned_shared_slot: *align(8) ?*graph_handle_t = @ptrCast(&misaligned_shared[16]);
    const misaligned_shared_before = misaligned_shared;
    try expect_status(.InvalidArgument, call_create(misaligned_shared_config, misaligned_shared_slot));
    try testing_t.expectEqualSlices(u8, &misaligned_shared_before, &misaligned_shared);

    var bad = config;
    bad.session_epoch = 0;
    slot = stale;
    try expect_status(.InvalidArgument, call_create(&bad, &slot));
    try testing_t.expect(slot == null);
    bad = config;
    bad.reserved0 = 1;
    try expect_status(.InvalidArgument, call_create(&bad, &slot));
    bad = config;
    bad.uac_mode = 3;
    try expect_status(.InvalidArgument, call_create(&bad, &slot));
    bad = config;
    bad.app_key_lookup = null;
    slot = stale;
    try expect_status(.InvalidArgument, call_create(&bad, &slot));
    try testing_t.expect(slot == null);

    try expect_status(.Ok, call_create(&config, &slot));
    try testing_t.expect(slot != null);
    // Mutating the caller's config after create changes nothing (the config was copied).
    config.session_epoch = 0;
    try expect_status(.StaleEpoch, call_refresh_begin(slot, TestEpoch + 1));
    try expect_status(.Ok, call_refresh_begin(slot, TestEpoch));
    try expect_status(.Ok, call_destroy(&slot));
    try testing_t.expect(slot == null);
}

test "create maps allocation failure to NoMemory and frees everything it took" {
    const saved = abi_allocator;
    defer abi_allocator = saved;
    var config = std_t.mem.zeroes(config_t);
    config.session_epoch = TestEpoch;
    config.app_key_lookup = probe_lookup;
    var fail_index: usize = 0;
    while (fail_index < 3) : (fail_index += 1) {
        var failing = testing_t.FailingAllocator.init(testing_t.allocator, .{ .fail_index = fail_index });
        abi_allocator = failing.allocator();
        var slot: ?*graph_handle_t = @ptrFromInt(0x1000);
        const status = call_create(&config, &slot);
        if (fail_index == 0) {
            try expect_status(.NoMemory, status);
            try testing_t.expect(slot == null);
        } else {
            try expect_status(.Ok, status);
            try testing_t.expect(slot != null);
            try expect_status(.Ok, call_destroy(&slot));
            try testing_t.expect(slot == null);
        }
        abi_allocator = saved;
        try testing_t.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
}

test "destroy handles null, empty, repeated, misaligned and self-containing slots" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    try expect_status(.InvalidArgument, call_destroy(null));
    var raw: [32]u8 align(8) = [_]u8{0} ** 32;
    const misaligned: ?*align(1) ?*graph_handle_t = @ptrCast(&raw[1]);
    try expect_status(.InvalidArgument, call_destroy(misaligned));

    var empty: ?*graph_handle_t = null;
    try expect_status(.Ok, call_destroy(&empty));
    try testing_t.expect(empty == null);

    // A misaligned handle value is rejected and the slot keeps it.
    const bogus: ?*graph_handle_t = @ptrFromInt(@intFromPtr(h.handle.?) + 1);
    var bogus_slot = bogus;
    try expect_status(.InvalidArgument, call_destroy(&bogus_slot));
    try testing_t.expect(bogus_slot == bogus);

    // A slot that lives inside the graph it names must not free the graph.
    const before = try h.snapshot();
    defer testing_t.allocator.free(before);
    const inner: *?*graph_handle_t = @ptrFromInt(@intFromPtr(h.engine()) + @offsetOf(graph_t.graph_t, "ring"));
    inner.* = h.handle;
    try expect_status(.InvalidArgument, call_destroy(inner));
    try testing_t.expect(inner.* == h.handle);
    inner.* = null;
    try h.expect_unchanged(before);

    var again = h.handle;
    try expect_status(.Ok, call_destroy(&again));
    try testing_t.expect(again == null);
    try expect_status(.Ok, call_destroy(&again));
    h.handle = null;
}

test "null and misaligned graph handles are InvalidArgument for every export" {
    var result: battery_t = .{};
    run_battery(null, TestEpoch, &result);
    for (result.status[0 .. BatteryCount - 1]) |status| try expect_status(.InvalidArgument, status);
    try expect_status(.Ok, result.status[BatteryCount - 1]);
    try expect_status(.Ok, result.classify_status);

    var h: harness_t = .{};
    try h.open();
    defer h.close();
    const before = try h.snapshot();
    defer testing_t.allocator.free(before);
    const bogus: ?*graph_handle_t = @ptrFromInt(@intFromPtr(h.handle.?) + 1);
    result = .{};
    run_battery(bogus, TestEpoch, &result);
    for (result.status) |status| try expect_status(.InvalidArgument, status);
    try testing_t.expect(result.slot_kept);
    try expect_status(.Ok, result.classify_status);
    try h.expect_unchanged(before);
}

const thread_job_t = struct {
    handle: ?*graph_handle_t,
    result: battery_t = .{},
    create_status: c_int = -1,
    created: ?*graph_handle_t = null,
    classify_status: c_int = -1,
};

fn thread_main(job: *thread_job_t) void {
    run_battery(job.handle, TestEpoch, &job.result);
    var config = std_t.mem.zeroes(config_t);
    config.session_epoch = TestEpoch;
    config.app_key_lookup = probe_lookup;
    job.create_status = call_create(&config, &job.created);
    if (job.created != null) _ = call_destroy(&job.created);
}

test "a foreign thread gets WrongThread from every graph export and touches nothing" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    _ = try h.admit(0x100);
    const before = try h.snapshot();
    defer testing_t.allocator.free(before);
    var job: thread_job_t = .{ .handle = h.handle };
    const thread = try std_t.Thread.spawn(.{}, thread_main, .{&job});
    thread.join();
    for (job.result.status) |status| try expect_status(.WrongThread, status);
    try testing_t.expect(job.result.slot_kept);
    // create and classify have no thread binding: they work on any thread.
    try expect_status(.Ok, job.result.classify_status);
    try expect_status(.Ok, job.create_status);
    try testing_t.expect(job.created == null);
    try h.expect_unchanged(before);
    var live: live_t = undefined;
    try expect_status(.Ok, call_find_live(h.handle, 0x100, &live));
}

test "calls from inside the lookup and sink callbacks return Reentrant and destroy keeps the graph" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    h.probe.run_battery = true;
    const first = try h.admit(0x200);
    try testing_t.expectEqual(@as(u32, 1), first.entries_admitted);
    try testing_t.expectEqual(@as(u32, 1), h.probe.calls);
    for (h.probe.battery.status) |status| try expect_status(.Reentrant, status);
    try testing_t.expect(h.probe.battery.slot_kept);
    try expect_status(.Ok, h.probe.battery.classify_status);
    var live: live_t = undefined;
    try expect_status(.Ok, call_find_live(h.handle, 0x200, &live));

    var sink: sink_probe_t = .{ .handle = h.handle, .run_battery = true };
    var delivered: u32 = 0;
    try expect_status(.Ok, call_drain(h.handle, TestEpoch, probe_sink, &sink, 4, &delivered));
    try testing_t.expectEqual(@as(u32, 1), delivered);
    for (sink.battery.status) |status| try expect_status(.Reentrant, status);
    try testing_t.expect(sink.battery.slot_kept);
}

test "a stale epoch is decided before argument errors and changes only its counter" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    _ = try h.admit(0x100);
    var stats: stats_t = undefined;
    try expect_status(.Ok, call_get_stats(h.handle, &stats));
    const stale = TestEpoch + 1;
    const count: u32 = 0;
    var windows: [2]u64 = undefined;
    const facts = make_facts(0x100, 50);
    var observation = sentinel(observation_t);
    var event = sentinel(event_t);
    var delivered: u32 = 0;
    try expect_status(.StaleEpoch, call_observe(h.handle, stale, null, null));
    try expect_status(.StaleEpoch, call_observe(h.handle, stale, &facts, &observation));
    try expect_status(.StaleEpoch, call_note_destroyed(h.handle, stale, 0x100, 9, 1000, null));
    try expect_status(.StaleEpoch, call_complete_retire(h.handle, stale, 0, 0, null));
    try expect_status(.StaleEpoch, call_check_action(h.handle, stale, 0x100, 1));
    try expect_status(.StaleEpoch, call_invalidate_all(h.handle, stale, @ptrFromInt(3)));
    try expect_status(.StaleEpoch, call_refresh_begin(h.handle, stale));
    try expect_status(.StaleEpoch, call_refresh_offer(h.handle, stale, null, &observation));
    try expect_status(.StaleEpoch, call_refresh_keep(h.handle, stale, 0));
    try expect_status(.StaleEpoch, call_refresh_unseen(h.handle, stale, &windows[0], 2, null));
    try expect_status(.StaleEpoch, call_refresh_commit(h.handle, stale, @ptrFromInt(3)));
    try expect_status(.StaleEpoch, call_refresh_abort(h.handle, stale));
    try expect_status(.StaleEpoch, call_next_event(h.handle, stale, &event));
    try expect_status(.StaleEpoch, call_drain(h.handle, stale, null, null, 0, &delivered));
    try testing_t.expect(is_sentinel(&observation));
    try testing_t.expect(is_sentinel(&event));
    try testing_t.expectEqual(@as(u32, 0), count);
    var after: stats_t = undefined;
    try expect_status(.Ok, call_get_stats(h.handle, &after));
    try testing_t.expectEqual(stats.rejected_stale_epoch + 14, after.rejected_stale_epoch);
    after.rejected_stale_epoch = stats.rejected_stale_epoch;
    try testing_t.expect(std_t.mem.eql(u8, std_t.mem.asBytes(&stats), std_t.mem.asBytes(&after)));
}

const bad_kind_t = enum { NullPointer, Misaligned, InsideGraph, AliasInput, PartialAlias };

var scratch_bytes: [4096]u8 align(16) = undefined;

/// A pointer of type `T` that every export must reject: NULL, misaligned, inside the graph, or
/// overlapping `alias` (fully or from byte 8 on).
fn bad_pointer(comptime value_t: type, kind: bad_kind_t, graph_pointer: *graph_t.graph_t, alias: *anyopaque) ?*align(1) value_t {
    switch (kind) {
        .NullPointer => return null,
        .Misaligned => {
            @memset(&scratch_bytes, 0xA5);
            return @ptrCast(&scratch_bytes[1]);
        },
        .InsideGraph => return @ptrFromInt(@intFromPtr(graph_pointer) + @offsetOf(graph_t.graph_t, "ring")),
        .AliasInput => return @ptrCast(alias),
        .PartialAlias => return @ptrFromInt(@intFromPtr(alias) + 8),
    }
}

fn scratch_untouched(kind: bad_kind_t) bool {
    if (kind != .Misaligned) return true;
    for (scratch_bytes) |byte| {
        if (byte != 0xA5) return false;
    }
    return true;
}

const AllKinds = [_]bad_kind_t{ .NullPointer, .Misaligned, .InsideGraph, .AliasInput, .PartialAlias };
const PlainKinds = [_]bad_kind_t{ .NullPointer, .Misaligned, .InsideGraph };
const OptionalKinds = [_]bad_kind_t{ .Misaligned, .InsideGraph };

test "pointer faults are rejected before the engine for every export with pointer arguments" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    _ = try h.admit(0x100);
    _ = try h.admit(0x101);
    try expect_status(.Ok, call_refresh_begin(h.handle, TestEpoch));
    const gp = h.engine();
    const before = try h.snapshot();
    defer testing_t.allocator.free(before);
    var facts = make_facts(0x100, 90);
    const facts_before = facts;
    var combined: [4]u64 align(8) = [_]u64{ 0xA5A5A5A5A5A5A5A5, 0xA5A5A5A5A5A5A5A5, 0xA5A5A5A5A5A5A5A5, 0xA5A5A5A5A5A5A5A5 };

    for (AllKinds) |kind| {
        const observation = bad_pointer(observation_t, kind, gp, &facts);
        try expect_status(.InvalidArgument, call_observe(h.handle, TestEpoch, &facts, observation));
        try expect_status(.InvalidArgument, call_refresh_offer(h.handle, TestEpoch, &facts, observation));
        try testing_t.expect(scratch_untouched(kind));
        try testing_t.expect(std_t.mem.eql(u8, std_t.mem.asBytes(&facts), std_t.mem.asBytes(&facts_before)));
        // classify has no graph, so a verdict buffer that happens to sit in graph memory is legal.
        if (kind == .InsideGraph) continue;
        const verdict_target = bad_pointer(verdict_t, kind, gp, &facts);
        try expect_status(.InvalidArgument, call_classify(&facts, 0, verdict_target));
    }
    for (PlainKinds) |kind| {
        const input = bad_pointer(facts_t, kind, gp, &facts);
        var observation = sentinel(observation_t);
        try expect_status(.InvalidArgument, call_observe(h.handle, TestEpoch, input, &observation));
        try expect_status(.InvalidArgument, call_refresh_offer(h.handle, TestEpoch, input, &observation));
        try testing_t.expect(is_sentinel(&observation));
        var verdict = sentinel(verdict_t);
        if (kind != .InsideGraph) try expect_status(.InvalidArgument, call_classify(input, 0, &verdict));
        try testing_t.expect(is_sentinel(&verdict));
        try expect_status(.InvalidArgument, call_note_destroyed(h.handle, TestEpoch, 0x100, 99, 1000, bad_pointer(observation_t, kind, gp, &facts)));
        try expect_status(.InvalidArgument, call_complete_retire(h.handle, TestEpoch, 0x100, 1, bad_pointer(retire_proof_t, kind, gp, &facts)));
        try expect_status(.InvalidArgument, call_next_event(h.handle, TestEpoch, bad_pointer(event_t, kind, gp, &facts)));
        try expect_status(.InvalidArgument, call_drain(h.handle, TestEpoch, accept_sink, null, 1, bad_pointer(u32, kind, gp, &facts)));
        try expect_status(.InvalidArgument, call_take_diagnostic(h.handle, bad_pointer(diagnostic_t, kind, gp, &facts)));
        try expect_status(.InvalidArgument, call_get_stats(h.handle, bad_pointer(stats_t, kind, gp, &facts)));
        try expect_status(.InvalidArgument, call_stamp_high_water(h.handle, bad_pointer(u64, kind, gp, &facts)));
        try expect_status(.InvalidArgument, call_find_live(h.handle, 0x100, bad_pointer(live_t, kind, gp, &facts)));
        try testing_t.expect(scratch_untouched(kind));
    }
    for (OptionalKinds) |kind| {
        try expect_status(.InvalidArgument, call_invalidate_all(h.handle, TestEpoch, bad_pointer(u32, kind, gp, &facts)));
        try expect_status(.InvalidArgument, call_refresh_commit(h.handle, TestEpoch, bad_pointer(u32, kind, gp, &facts)));
        try testing_t.expect(scratch_untouched(kind));
    }
    // refresh_unseen and list_retiring: the count and the array may not be NULL, misaligned, in the
    // graph or overlap each other.
    for (PlainKinds) |kind| {
        try expect_status(.InvalidArgument, call_refresh_unseen(h.handle, TestEpoch, &combined[2], 1, bad_pointer(u32, kind, gp, &facts)));
        try expect_status(.InvalidArgument, call_refresh_unseen(h.handle, TestEpoch, bad_pointer(u64, kind, gp, &facts), 2, @ptrCast(&combined[3])));
        try expect_status(.InvalidArgument, call_list_retiring(h.handle, @ptrCast(&combined[0]), 1, bad_pointer(u32, kind, gp, &facts)));
        try expect_status(.InvalidArgument, call_list_retiring(h.handle, bad_pointer(retiring_entry_t, kind, gp, &facts), 1, @ptrCast(&combined[3])));
        try testing_t.expect(scratch_untouched(kind));
    }
    try expect_status(.InvalidArgument, call_refresh_unseen(h.handle, TestEpoch, &combined[0], 2, @ptrCast(&combined[1])));
    try expect_status(.InvalidArgument, call_list_retiring(h.handle, @ptrCast(&combined[0]), 1, @ptrCast(&combined[0])));
    // An array whose range wraps the address space is rejected even though its start is valid.
    const high: ?*align(1) u64 = @ptrFromInt(std_t.math.maxInt(usize) - 7);
    var count: u32 = 0;
    try expect_status(.InvalidArgument, call_refresh_unseen(h.handle, TestEpoch, high, 4, &count));
    const high_entries: ?*align(1) retiring_entry_t = @ptrFromInt(std_t.math.maxInt(usize) - 7);
    try expect_status(.InvalidArgument, call_list_retiring(h.handle, high_entries, 4, &count));
    try testing_t.expectEqual(@as(u32, 0), count);
    for (combined) |word| try testing_t.expectEqual(@as(u64, 0xA5A5A5A5A5A5A5A5), word);
    try h.expect_unchanged(before);
}

test "a pointer fault on a foreign thread is WrongThread and a stale epoch is StaleEpoch" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    const before = try h.snapshot();
    defer testing_t.allocator.free(before);
    const job_t = struct {
        handle: ?*graph_handle_t,
        status: c_int = -1,
        snapshot_status: c_int = -1,
        fn run(job: *@This()) void {
            job.status = call_observe(job.handle, TestEpoch, null, null);
            job.snapshot_status = call_list_tracked_windows(job.handle, 0, null, 0, null, null);
        }
    };
    var job: job_t = .{ .handle = h.handle };
    const thread = try std_t.Thread.spawn(.{}, job_t.run, .{&job});
    thread.join();
    try expect_status(.WrongThread, job.status);
    try expect_status(.WrongThread, job.snapshot_status);
    try h.expect_unchanged(before);
    try expect_status(.StaleEpoch, call_list_tracked_windows(h.handle, 0, null, 0, null, null));
    try expect_status(.StaleEpoch, call_observe(h.handle, 0, null, null));
    try expect_status(.InvalidArgument, call_observe(h.handle, TestEpoch, null, null));
}

test "classify validates its arguments and facts" {
    var facts = make_facts(0x300, 1);
    var verdict = sentinel(verdict_t);
    try expect_status(.Ok, call_classify(&facts, 0, &verdict));
    try testing_t.expectEqual(@as(u32, 1), verdict.kind);
    try testing_t.expectEqual(graph_t.RoleToplevel, verdict.role);
    try expect_status(.InvalidArgument, call_classify(&facts, 3, &verdict));
    facts.stamp = 0;
    verdict = sentinel(verdict_t);
    try expect_status(.InvalidFacts, call_classify(&facts, 0, &verdict));
    try testing_t.expect(is_sentinel(&verdict));
    facts = make_facts(0x300, 1);
    facts.flags &= ~graph_t.FactOnInputDesktop;
    try expect_status(.Ok, call_classify(&facts, 0, &verdict));
    try testing_t.expectEqual(@as(u32, 3), verdict.kind);
    facts = make_facts(0x300, 1);
    facts.root = 0x301;
    try expect_status(.Ok, call_classify(&facts, 0, &verdict));
    try testing_t.expectEqual(@as(u32, 2), verdict.kind);
    try testing_t.expectEqual(graph_t.ReasonChild, verdict.reason);
}

test "zero-capacity arrays ignore their pointer and truncation reports the full count" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    _ = try h.admit(0x100);
    _ = try h.admit(0x101);
    _ = try h.admit(0x102);
    var count: u32 = 0xFFFF;
    try expect_status(.NoRefresh, call_refresh_unseen(h.handle, TestEpoch, null, 0, &count));
    try expect_status(.Ok, call_refresh_begin(h.handle, TestEpoch));
    try expect_status(.RefreshActive, call_refresh_begin(h.handle, TestEpoch));
    try expect_status(.Ok, call_refresh_unseen(h.handle, TestEpoch, null, 0, &count));
    try testing_t.expectEqual(@as(u32, 3), count);
    // A garbage pointer is ignored when capacity is zero.
    const garbage: ?*align(1) u64 = @ptrFromInt(1);
    try expect_status(.Ok, call_refresh_unseen(h.handle, TestEpoch, garbage, 0, &count));
    var windows = [_]u64{ 7, 7, 7 };
    count = 0;
    try expect_status(.Ok, call_refresh_unseen(h.handle, TestEpoch, &windows[0], 2, &count));
    try testing_t.expectEqual(@as(u32, 3), count);
    try testing_t.expectEqual(@as(u64, 0x100), windows[0]);
    try testing_t.expectEqual(@as(u64, 0x101), windows[1]);
    try testing_t.expectEqual(@as(u64, 7), windows[2]);
    try expect_status(.Ok, call_refresh_keep(h.handle, TestEpoch, 0x100));
    try expect_status(.None, call_refresh_keep(h.handle, TestEpoch, 0xBEEF));
    try expect_status(.InvalidArgument, call_refresh_keep(h.handle, TestEpoch, 0));
    var retired: u32 = 0xFFFF;
    try expect_status(.Ok, call_refresh_commit(h.handle, TestEpoch, &retired));
    try testing_t.expectEqual(@as(u32, 2), retired);
    retired = 0xFFFF;
    try expect_status(.NoRefresh, call_refresh_commit(h.handle, TestEpoch, &retired));
    try testing_t.expectEqual(@as(u32, 0xFFFF), retired);

    var entries = [_]retiring_entry_t{std_t.mem.zeroes(retiring_entry_t)} ** 3;
    try expect_status(.Ok, call_list_retiring(h.handle, null, 0, &count));
    try testing_t.expectEqual(@as(u32, 2), count);
    try expect_status(.Ok, call_list_retiring(h.handle, garbage_entries(), 0, &count));
    count = 0;
    try expect_status(.Ok, call_list_retiring(h.handle, &entries[0], 1, &count));
    try testing_t.expectEqual(@as(u32, 2), count);
    try testing_t.expectEqual(@as(u64, 0x101), entries[0].window);
    try testing_t.expectEqual(@as(u64, 0), entries[1].window);
    try expect_status(.InvalidArgument, call_list_retiring(h.handle, null, 1, &count));
}

test "list_tracked_windows ABI stages sorted unique handles and exact Free slots" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    var windows = [_]u64{0xCAFE} ** 66;
    var count: u32 = 0x1234;
    var free_count: u32 = 0x5678;
    try expect_status(.Ok, call_list_tracked_windows(h.handle, TestEpoch, &windows[0], 64, &count, &free_count));
    try testing_t.expectEqual(@as(u32, 0), count);
    try testing_t.expectEqual(@as(u32, 64), free_count);
    for (windows) |window| try testing_t.expectEqual(@as(u64, 0xCAFE), window);

    const engine = h.engine();
    engine.entries[12] = std_t.mem.zeroes(graph_t.entry_t);
    engine.entries[12].state = .Active;
    engine.entries[12].record.window = 0x500;
    engine.entries[12].record.generation = 30;
    engine.entries[4] = std_t.mem.zeroes(graph_t.entry_t);
    engine.entries[4].state = .Retiring;
    engine.entries[4].record.window = 0x300;
    engine.entries[4].record.generation = 10;
    engine.entries[6] = std_t.mem.zeroes(graph_t.entry_t);
    engine.entries[6].state = .Retiring;
    engine.entries[6].record.window = 0x500;
    engine.entries[6].record.generation = 20;
    engine.deferred[3] = std_t.mem.zeroes(graph_t.deferred_t);
    engine.deferred[3].used = true;
    engine.deferred[3].record.window = 0x50;
    @memset(&windows, 0xA5A5A5A5A5A5A5A5);
    count = 0x1111;
    free_count = 0x2222;
    try expect_status(.Ok, call_list_tracked_windows(h.handle, TestEpoch, &windows[0], 64, &count, &free_count));
    try testing_t.expectEqual(@as(u32, 2), count);
    try testing_t.expectEqual(@as(u32, 61), free_count);
    try testing_t.expectEqualSlices(u64, &[_]u64{ 0x300, 0x500 }, windows[0..2]);
    try testing_t.expectEqual(@as(u64, 0xA5A5A5A5A5A5A5A5), windows[2]);
    try testing_t.expectEqual(@as(u64, 0xA5A5A5A5A5A5A5A5), windows[65]);

    const proof: retire_proof_t = .{ .capture_stopped = 1, .pins_drained = 1 };
    try expect_status(.Ok, call_complete_retire(h.handle, TestEpoch, 0x300, 10, &proof));
    try expect_status(.Ok, call_list_tracked_windows(h.handle, TestEpoch, &windows[0], 64, &count, &free_count));
    try testing_t.expectEqual(@as(u32, 1), count);
    try testing_t.expectEqual(@as(u32, 62), free_count);
    try testing_t.expectEqual(@as(u64, 0x500), windows[0]);

    // Fill every remaining Free entry. The Active+Retiring generations for 0x500 consume two
    // slots but contribute one HWND, so the exact free count and unique count differ.
    for (&engine.entries, 0..) |*entry, index| {
        if (entry.state != .Free) continue;
        entry.* = std_t.mem.zeroes(graph_t.entry_t);
        entry.state = if (index % 2 == 0) .Active else .Retiring;
        entry.record.window = 0x1000 + index;
        entry.record.generation = 100 + index;
    }
    try expect_status(.Ok, call_list_tracked_windows(h.handle, TestEpoch, &windows[0], 64, &count, &free_count));
    try testing_t.expectEqual(@as(u32, 63), count);
    try testing_t.expectEqual(@as(u32, 0), free_count);
    try testing_t.expectEqual(@as(u64, 0x500), windows[0]);
    for (windows[1..count], 1..) |window, index| try testing_t.expect(window > windows[index - 1]);
    try testing_t.expectEqual(@as(u64, 0xA5A5A5A5A5A5A5A5), windows[63]);
}

test "list_tracked_windows ABI rejects malformed spans before reading or writing caller memory" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    _ = try h.admit(0x100);
    const before = try h.snapshot();
    defer testing_t.allocator.free(before);
    const engine = h.engine();
    var windows = [_]u64{0xABCD} ** 64;
    var count: u32 = 0x1234;
    var free_count: u32 = 0x5678;

    try expect_status(.InvalidArgument, call_list_tracked_windows(h.handle, TestEpoch, null, 64, &count, &free_count));
    try expect_status(.InvalidArgument, call_list_tracked_windows(h.handle, TestEpoch, &windows[0], 63, &count, &free_count));
    try expect_status(.InvalidArgument, call_list_tracked_windows(h.handle, TestEpoch, &windows[0], 64, null, &free_count));
    try expect_status(.InvalidArgument, call_list_tracked_windows(h.handle, TestEpoch, &windows[0], 64, &count, null));

    var bytes: [64 * @sizeOf(u64) + 32]u8 align(8) = [_]u8{0x5A} ** (64 * @sizeOf(u64) + 32);
    const overlap_windows: ?*align(1) u64 = @ptrCast(&bytes[0]);
    const overlap_count: ?*align(1) u32 = @ptrCast(&bytes[8]);
    const overlap_free: ?*align(1) u32 = @ptrCast(&bytes[16]);
    const bytes_before = bytes;
    try expect_status(.InvalidArgument, call_list_tracked_windows(h.handle, TestEpoch, overlap_windows, 64, overlap_count, overlap_free));
    try testing_t.expectEqualSlices(u8, &bytes_before, &bytes);

    var scalar_pair: [8]u8 align(4) = [_]u8{0x6B} ** 8;
    const same_count: ?*align(1) u32 = @ptrCast(&scalar_pair[0]);
    const same_free: ?*align(1) u32 = @ptrCast(&scalar_pair[0]);
    const scalar_before = scalar_pair;
    try expect_status(.InvalidArgument, call_list_tracked_windows(h.handle, TestEpoch, &windows[0], 64, same_count, same_free));
    try testing_t.expectEqualSlices(u8, &scalar_before, &scalar_pair);

    const graph_pointer = engine;
    const inside_graph: ?*align(1) u64 = @ptrFromInt(@intFromPtr(graph_pointer) + @offsetOf(graph_t.graph_t, "ring"));
    try expect_status(.InvalidArgument, call_list_tracked_windows(h.handle, TestEpoch, inside_graph, 64, &count, &free_count));
    const misaligned: ?*align(1) u64 = @ptrFromInt(@intFromPtr(&windows[0]) + 1);
    try expect_status(.InvalidArgument, call_list_tracked_windows(h.handle, TestEpoch, misaligned, 64, &count, &free_count));
    const wrapped: ?*align(1) u64 = @ptrFromInt(std_t.math.maxInt(usize) - 7);
    try expect_status(.InvalidArgument, call_list_tracked_windows(h.handle, TestEpoch, wrapped, 64, &count, &free_count));

    try expect_status(.StaleEpoch, call_list_tracked_windows(h.handle, TestEpoch + 1, null, 0, null, null));
    var after_stats: stats_t = undefined;
    try expect_status(.Ok, call_get_stats(h.handle, &after_stats));
    try testing_t.expectEqual(@as(u64, 0), after_stats.rejected_stale_epoch);
    try testing_t.expectEqual(@as(u32, 0x1234), count);
    try testing_t.expectEqual(@as(u32, 0x5678), free_count);
    try testing_t.expectEqualSlices(u64, &([_]u64{0xABCD} ** 64), &windows);
    try h.expect_unchanged(before);
}

fn garbage_entries() ?*align(1) retiring_entry_t {
    return @ptrFromInt(1);
}

test "drain reports SinkFailed with the delivered count and keeps the refused event" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    _ = try h.admit(0x100);
    _ = try h.admit(0x101);
    _ = try h.admit(0x102);
    var delivered: u32 = 0xFFFF;
    try expect_status(.InvalidArgument, call_drain(h.handle, TestEpoch, null, null, 1, &delivered));
    try expect_status(.InvalidArgument, call_drain(h.handle, TestEpoch, accept_sink, null, 0, &delivered));
    try testing_t.expectEqual(@as(u32, 0xFFFF), delivered);
    var sink: sink_probe_t = .{ .handle = h.handle, .fail_on_call = 2 };
    try expect_status(.SinkFailed, call_drain(h.handle, TestEpoch, probe_sink, &sink, 8, &delivered));
    try testing_t.expectEqual(@as(u32, 1), delivered);
    try testing_t.expectEqual(@as(u32, 2), sink.calls);
    sink.fail_on_call = 0;
    try expect_status(.Ok, call_drain(h.handle, TestEpoch, probe_sink, &sink, 8, &delivered));
    try testing_t.expectEqual(@as(u32, 2), delivered);
    try testing_t.expectEqual(@as(u64, 0x101), sink.windows[1]);
    try testing_t.expectEqual(@as(u64, 0x101), sink.windows[2]);
    try testing_t.expectEqual(@as(u64, 0x102), sink.windows[3]);
    delivered = 0xFFFF;
    try expect_status(.None, call_drain(h.handle, TestEpoch, probe_sink, &sink, 8, &delivered));
    try testing_t.expectEqual(@as(u32, 0), delivered);
}

test "capacity diagnostic, invalidate_all and lookup failures through the exports" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    var diagnostic = sentinel(diagnostic_t);
    try expect_status(.None, call_take_diagnostic(h.handle, &diagnostic));
    try testing_t.expect(is_sentinel(&diagnostic));
    h.probe.fail = true;
    const refused = make_facts(0x3000, h.next_stamp());
    var observation = sentinel(observation_t);
    try expect_status(.LookupFailed, call_observe(h.handle, TestEpoch, &refused, &observation));
    try testing_t.expect(is_sentinel(&observation));
    h.probe.fail = false;
    var window: u64 = 0x1000;
    while (window < 0x1000 + 64) : (window += 1) _ = try h.admit(window);
    const extra = make_facts(0x2000, h.next_stamp());
    try expect_status(.AtCapacity, call_observe(h.handle, TestEpoch, &extra, &observation));
    try testing_t.expect(is_sentinel(&observation));
    h.clock = 5;
    try expect_status(.Ok, call_take_diagnostic(h.handle, &diagnostic));
    try testing_t.expectEqual(@as(u64, 1), diagnostic.at_capacity);
    try testing_t.expectEqual(@as(u32, 64), diagnostic.active_count);
    try expect_status(.None, call_take_diagnostic(h.handle, &diagnostic));

    var retired: u32 = 0;
    try expect_status(.Ok, call_invalidate_all(h.handle, TestEpoch, &retired));
    try testing_t.expectEqual(@as(u32, 64), retired);
    try expect_status(.Ok, call_invalidate_all(h.handle, TestEpoch, null));
    var count: u32 = 0;
    var entries: [64]retiring_entry_t = undefined;
    try expect_status(.Ok, call_list_retiring(h.handle, &entries[0], 64, &count));
    try testing_t.expectEqual(@as(u32, 64), count);
}

test "lifecycle through the exports: admit, events, retire, complete, refresh, drain" {
    var h: harness_t = .{};
    try h.open();
    defer h.close();
    var stamp_value: u64 = 99;
    try expect_status(.Ok, call_stamp_high_water(h.handle, &stamp_value));
    try testing_t.expectEqual(@as(u64, 0), stamp_value);

    const a_facts = make_facts(0xA00, h.next_stamp());
    var verdict: verdict_t = undefined;
    try expect_status(.Ok, call_classify(&a_facts, 0, &verdict));
    try testing_t.expectEqual(@as(u32, 1), verdict.kind);
    var observation: observation_t = undefined;
    try expect_status(.Ok, call_observe(h.handle, TestEpoch, &a_facts, &observation));
    try testing_t.expectEqual(graph_t.DispositionAdmitted, observation.disposition);
    try testing_t.expectEqual(@as(u64, 1), observation.generation);
    try testing_t.expectEqual(@as(u32, 1), observation.events_queued);

    var event: event_t = undefined;
    try expect_status(.Ok, call_next_event(h.handle, TestEpoch, &event));
    try testing_t.expectEqual(graph_t.EventAnnounce, event.kind);
    try testing_t.expectEqual(@as(u64, 0xA00), event.window);
    try testing_t.expectEqual(@as(u32, 6), event.app_key_length);
    try testing_t.expect(std_t.mem.eql(u8, event.app_key[0..6], "a-test"));
    try expect_status(.None, call_next_event(h.handle, TestEpoch, &event));

    var live: live_t = undefined;
    try expect_status(.Ok, call_find_live(h.handle, 0xA00, &live));
    try testing_t.expectEqual(@as(u64, 1), live.generation);
    live = sentinel(live_t);
    try expect_status(.None, call_find_live(h.handle, 0xDEAD, &live));
    try testing_t.expect(is_sentinel(&live));
    try expect_status(.Ok, call_stamp_high_water(h.handle, &stamp_value));
    try testing_t.expectEqual(@as(u64, 1), stamp_value);
    try expect_status(.Ok, call_check_action(h.handle, TestEpoch, 0xA00, 1));
    try expect_status(.Stale, call_check_action(h.handle, TestEpoch, 0xA00, 2));
    try expect_status(.InvalidArgument, call_check_action(h.handle, TestEpoch, 0, 1));

    // A dialog owned by A, then a refresh that only offers A: the dialog is retired as unseen.
    const b_facts = make_dialog_facts(0xB00, 0xA00, h.next_stamp());
    try expect_status(.Ok, call_observe(h.handle, TestEpoch, &b_facts, &observation));
    try testing_t.expectEqual(graph_t.DispositionAdmitted, observation.disposition);
    try expect_status(.Ok, call_refresh_begin(h.handle, TestEpoch));
    try expect_status(.RefreshActive, call_refresh_begin(h.handle, TestEpoch));
    const a_again = make_facts(0xA00, h.next_stamp());
    try expect_status(.Ok, call_refresh_offer(h.handle, TestEpoch, &a_again, &observation));
    try testing_t.expectEqual(graph_t.DispositionUnchanged, observation.disposition);
    var windows: [4]u64 = undefined;
    var count: u32 = 0;
    try expect_status(.Ok, call_refresh_unseen(h.handle, TestEpoch, &windows[0], 4, &count));
    try testing_t.expectEqual(@as(u32, 1), count);
    try testing_t.expectEqual(@as(u64, 0xB00), windows[0]);
    var retired: u32 = 0;
    try expect_status(.Ok, call_refresh_commit(h.handle, TestEpoch, &retired));
    try testing_t.expectEqual(@as(u32, 1), retired);
    try expect_status(.NoRefresh, call_refresh_abort(h.handle, TestEpoch));
    try expect_status(.Ok, call_refresh_begin(h.handle, TestEpoch));
    try expect_status(.Ok, call_refresh_abort(h.handle, TestEpoch));

    // Drain with a C-ABI sink: Announce(B) then RetireIntent(B).
    var sink: sink_probe_t = .{ .handle = h.handle };
    var delivered: u32 = 0;
    try expect_status(.Ok, call_drain(h.handle, TestEpoch, probe_sink, &sink, 8, &delivered));
    try testing_t.expectEqual(@as(u32, 2), delivered);
    try testing_t.expectEqual(graph_t.EventAnnounce, sink.kinds[0]);
    try testing_t.expectEqual(graph_t.EventRetireIntent, sink.kinds[1]);
    try testing_t.expectEqual(@as(u64, 0xB00), sink.windows[1]);

    // B is quarantined: listed, not live, proof checked in the documented order.
    var entries: [4]retiring_entry_t = undefined;
    try expect_status(.Ok, call_list_retiring(h.handle, &entries[0], 4, &count));
    try testing_t.expectEqual(@as(u32, 1), count);
    try testing_t.expectEqual(@as(u64, 0xB00), entries[0].window);
    try testing_t.expectEqual(@as(u64, 2), entries[0].generation);
    try expect_status(.None, call_find_live(h.handle, 0xB00, &live));
    var proof: retire_proof_t = .{ .capture_stopped = 1, .pins_drained = 0 };
    try expect_status(.ProofIncomplete, call_complete_retire(h.handle, TestEpoch, 0xB00, 2, &proof));
    try expect_status(.NotRetiring, call_complete_retire(h.handle, TestEpoch, 0xA00, 1, &proof));
    proof.pins_drained = 1;
    try expect_status(.Ok, call_complete_retire(h.handle, TestEpoch, 0xB00, 2, &proof));
    try expect_status(.Stale, call_complete_retire(h.handle, TestEpoch, 0xB00, 2, &proof));
    try expect_status(.InvalidArgument, call_complete_retire(h.handle, TestEpoch, 0, 2, &proof));

    // A destroy hint retires A.
    const hint_stamp = h.next_stamp();
    try expect_status(.Ok, call_note_destroyed(h.handle, TestEpoch, 0xA00, hint_stamp, 2000, &observation));
    try testing_t.expectEqual(graph_t.DispositionRetired, observation.disposition);
    try testing_t.expectEqual(@as(u64, 1), observation.retired_generation);
    try expect_status(.InvalidArgument, call_note_destroyed(h.handle, TestEpoch, 0, hint_stamp, 2000, &observation));
    try expect_status(.Stale, call_check_action(h.handle, TestEpoch, 0xA00, 1));
    sink = .{ .handle = h.handle };
    try expect_status(.Ok, call_drain(h.handle, TestEpoch, probe_sink, &sink, 8, &delivered));
    try testing_t.expectEqual(@as(u32, 1), delivered);
    try expect_status(.Ok, call_complete_retire(h.handle, TestEpoch, 0xA00, 1, &proof));
    try expect_status(.Ok, call_list_retiring(h.handle, null, 0, &count));
    try testing_t.expectEqual(@as(u32, 0), count);

    var stats: stats_t = undefined;
    try expect_status(.Ok, call_get_stats(h.handle, &stats));
    try testing_t.expectEqual(TestEpoch, stats.session_epoch);
    try testing_t.expectEqual(@as(u64, 2), stats.admitted);
    try testing_t.expectEqual(@as(u64, 2), stats.retire_completed);
    try testing_t.expectEqual(@as(u32, 0), stats.active_count);

    // A new generation after the teardown; destroy with a pending event is fine.
    _ = try h.admit(0xC00);
    var handle = h.handle;
    try expect_status(.Ok, call_destroy(&handle));
    try testing_t.expect(handle == null);
    h.handle = null;
}
