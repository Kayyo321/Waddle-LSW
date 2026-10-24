const std = @import("std");
const runtime = @import("interop_slot_runtime.zig");
const c = @cImport({
    @cInclude("interop_slot_runtime.h");
});

const handle_storage_t = struct {
    runtime: runtime.slot_runtime_t,
    host_drained: bool = false,
};

// C pointers enter as raw 64-bit words so alignment and wrap checks cannot be
// optimized away under Zig's typed-pointer validity assumptions. The public C
// header uses pointer types; the supported x86_64 C ABI passes both in one GP word.
comptime {
    if (@sizeOf(usize) != 8) @compileError("slot-runtime opaque pointer ABI requires supported 64-bit targets");
}

fn map_error(err: runtime.slot_runtime_error_t) c.enum_waddle_slot_status_t {
    return switch (err) {
        error.InvalidSessionEpoch => c.WaddleSlotInvalidSessionEpoch,
        error.WrongSessionEpoch => c.WaddleSlotWrongSessionEpoch,
        error.InvalidWindow => c.WaddleSlotInvalidWindow,
        error.InvalidGeneration => c.WaddleSlotInvalidGeneration,
        error.StaleGeneration => c.WaddleSlotStaleGeneration,
        error.DuplicateWindow => c.WaddleSlotDuplicateWindow,
        error.NoWindowCapacity => c.WaddleSlotNoWindowCapacity,
        error.UnknownWindow => c.WaddleSlotUnknownWindow,
        error.InvalidSlot => c.WaddleSlotInvalidSlot,
        error.InvalidGeometry => c.WaddleSlotInvalidGeometry,
        error.HeapFull => c.WaddleSlotHeapFull,
        error.TicketExhausted => c.WaddleSlotTicketExhausted,
        error.Busy => c.WaddleSlotBusy,
        error.SlotTooSmall => c.WaddleSlotSlotTooSmall,
        error.WriterAlreadyActive => c.WaddleSlotWriterAlreadyActive,
        error.NoWriter => c.WaddleSlotNoWriter,
        error.WriterStateMismatch => c.WaddleSlotWriterStateMismatch,
        error.InvalidSequence => c.WaddleSlotInvalidSequence,
        error.SequenceExhausted => c.WaddleSlotSequenceExhausted,
        error.WrongRelease => c.WaddleSlotWrongRelease,
        error.SlotPinned => c.WaddleSlotPinned,
        error.InvalidLifecycle => c.WaddleSlotInvalidLifecycle,
        error.MissingProof => c.WaddleSlotMissingProof,
        error.HeapStateCorrupt => c.WaddleSlotHeapStateCorrupt,
        error.FailedClosed => c.WaddleSlotFailedClosed,
    };
}

fn valid_range(address: usize, size: usize, alignment: usize) bool {
    return address != 0 and size != 0 and alignment != 0 and address % alignment == 0 and
        address <= std.math.maxInt(usize) - size;
}

fn ranges_overlap(left: usize, left_size: usize, right: usize, right_size: usize) bool {
    return left < right + right_size and right < left + left_size;
}

fn resolve_handle(address: usize) ?*handle_storage_t {
    if (!valid_range(address, @sizeOf(handle_storage_t), @alignOf(handle_storage_t))) return null;
    return @ptrFromInt(address);
}

fn valid_disjoint_output(handle: *handle_storage_t, address: usize, size: usize, alignment: usize) bool {
    if (!valid_range(address, size, alignment)) return false;
    return !ranges_overlap(address, size, @intFromPtr(handle), @sizeOf(handle_storage_t));
}

fn copy_view(view: runtime.slot_view_t) c.struct_waddle_slot_view_t {
    return .{
        .descriptor_index = view.descriptor_index,
        .window = view.window,
        .generation = view.generation,
        .width = view.frame_layout.width,
        .height = view.frame_layout.height,
        .stride = view.frame_layout.stride,
        .pixel_bytes = view.frame_layout.pixel_bytes,
        .data_offset = view.run.data_offset,
        .data_bytes = view.frame_layout.data_bytes,
    };
}

fn stale_epoch(handle: *const handle_storage_t, session_epoch: u64) bool {
    return handle.runtime.session_epoch != session_epoch;
}

/// Allocate a fresh opaque runtime using the C allocator.
/// @param[in] session_epoch Nonzero fresh session epoch; zero returns InvalidSessionEpoch.
/// @param[out] output_address Nonnull aligned address of a null handle slot; caller owns the slot.
/// @return Fixed C status: Ok, InvalidArgument, InvalidSessionEpoch, or OutOfMemory.
/// @ownership On success, the caller owns the opaque allocation until proof-gated destroy.
/// @thread_safety Each returned handle is externally serialized by one event-loop thread.
export fn waddle_slot_runtime_create(session_epoch: u64, output_address: usize) c.enum_waddle_slot_status_t {
    const address = output_address;
    if (!valid_range(address, @sizeOf(?*u8), @alignOf(?*u8))) return c.WaddleSlotInvalidArgument;
    const output: *?*u8 = @ptrFromInt(address);
    if (output.* != null) return c.WaddleSlotInvalidArgument;
    const slot_runtime = runtime.slot_runtime_t.init(session_epoch) catch |err| return map_error(err);
    const storage = std.heap.c_allocator.create(handle_storage_t) catch return c.WaddleSlotOutOfMemory;
    storage.* = .{ .runtime = slot_runtime };
    output.* = @ptrCast(storage);
    return c.WaddleSlotOk;
}

/// Destroy only after successful host-drain confirmation has been recorded.
/// @param[in,out] output_address Nonnull aligned address of the caller-owned handle slot.
/// @return Fixed C status: Ok, InvalidArgument, or InvalidLifecycle; null handle is Ok.
/// @ownership Frees and NULLs the handle only after confirmed host drain.
/// @thread_safety Caller serializes destruction with every operation on the handle.
export fn waddle_slot_runtime_destroy(output_address: usize) c.enum_waddle_slot_status_t {
    if (!valid_range(output_address, @sizeOf(?*u8), @alignOf(?*u8)))
        return c.WaddleSlotInvalidArgument;
    const output: *?*u8 = @ptrFromInt(output_address);
    const opaque_ptr = output.* orelse return c.WaddleSlotOk;
    const storage_address = @intFromPtr(opaque_ptr);
    const storage = resolve_handle(storage_address) orelse return c.WaddleSlotInvalidArgument;
    if (ranges_overlap(output_address, @sizeOf(?*u8), @intFromPtr(storage), @sizeOf(handle_storage_t)))
        return c.WaddleSlotInvalidArgument;
    if (!storage.host_drained) return c.WaddleSlotInvalidLifecycle;
    std.heap.c_allocator.destroy(storage);
    output.* = null;
    return c.WaddleSlotOk;
}

/// Admit a window and copy three scalar views after the pure operation succeeds.
/// @param[in] handle_address Nonnull aligned live opaque handle; not owned by this call.
/// @param[in] session_epoch Exact captured epoch; mismatch returns WrongSessionEpoch.
/// @param[in] window Nonzero window identity; @param[in] generation increasing nonzero incarnation.
/// @param[in] width Requested width; @param[in] height Requested height, clamped by frozen layout policy.
/// @param[out] out_views_address Nonnull aligned disjoint array of three caller-owned snapshots.
/// @return Fixed C status for argument, epoch, identity, capacity, geometry, or heap outcomes.
/// @ownership Runtime retains heap reservations; snapshots contain no pointer or release token.
/// @thread_safety One externally serialized event-loop thread per handle.
export fn waddle_slot_runtime_admit_window(
    handle_address: usize,
    session_epoch: u64,
    window: u64,
    generation: u64,
    width: u32,
    height: u32,
    out_views_address: usize,
) c.enum_waddle_slot_status_t {
    const storage = resolve_handle(handle_address) orelse return c.WaddleSlotInvalidArgument;
    const address = out_views_address;
    const size = 3 * @sizeOf(c.struct_waddle_slot_view_t);
    if (!valid_disjoint_output(storage, address, size, @alignOf(c.struct_waddle_slot_view_t))) return c.WaddleSlotInvalidArgument;
    if (stale_epoch(storage, session_epoch)) return c.WaddleSlotWrongSessionEpoch;
    const output: [*]c.struct_waddle_slot_view_t = @ptrFromInt(address);
    var views: [runtime.SlotsPerWindow]runtime.slot_view_t = undefined;
    storage.runtime.admit_window(window, generation, width, height, &views) catch |err| return map_error(err);
    for (views, 0..) |view, index| output[index] = copy_view(view);
    return c.WaddleSlotOk;
}

/// Request future geometry or transactionally stage a replacement triple.
/// @param[in] handle_address Nonnull aligned live opaque handle; not owned by this call.
/// @param[in] session_epoch Exact captured epoch; mismatch returns WrongSessionEpoch.
/// @param[in] window Nonzero admitted window; @param[in] generation exact incarnation.
/// @param[in] width Requested width; @param[in] height Requested height.
/// @return Fixed C status for argument, epoch, identity, geometry, growth, and lifecycle outcomes.
/// @ownership Runtime retains active and pending reservations; no pointer is returned.
/// @thread_safety One externally serialized event-loop thread per handle.
export fn waddle_slot_runtime_request_geometry(
    handle_address: usize,
    session_epoch: u64,
    window: u64,
    generation: u64,
    width: u32,
    height: u32,
) c.enum_waddle_slot_status_t {
    const storage = resolve_handle(handle_address) orelse return c.WaddleSlotInvalidArgument;
    if (stale_epoch(storage, session_epoch)) return c.WaddleSlotWrongSessionEpoch;
    storage.runtime.request_geometry(window, generation, width, height) catch |err| return map_error(err);
    return c.WaddleSlotOk;
}

/// Reserve a provisional writer claim and copy its scalar snapshot on success.
/// @param[in] handle_address Nonnull aligned live opaque handle; not owned by this call.
/// @param[in] session_epoch Exact captured epoch; mismatch returns WrongSessionEpoch.
/// @param[in] window Nonzero admitted window; @param[in] generation exact incarnation.
/// @param[in] slot Slot index 0..2; @param[in] frame_sequence increasing nonzero sequence.
/// @param[out] out_view_address Nonnull aligned disjoint caller-owned scalar view.
/// @return Fixed C status for argument, epoch, identity, slot, sequence, capacity, and lifecycle.
/// @ownership Runtime retains the provisional claim; the output contains no token.
/// @thread_safety One externally serialized event-loop thread per handle.
export fn waddle_slot_runtime_prepare_publish(
    handle_address: usize,
    session_epoch: u64,
    window: u64,
    generation: u64,
    slot: u8,
    frame_sequence: u64,
    out_view_address: usize,
) c.enum_waddle_slot_status_t {
    const storage = resolve_handle(handle_address) orelse return c.WaddleSlotInvalidArgument;
    const address = out_view_address;
    if (!valid_disjoint_output(storage, address, @sizeOf(c.struct_waddle_slot_view_t), @alignOf(c.struct_waddle_slot_view_t)))
        return c.WaddleSlotInvalidArgument;
    if (stale_epoch(storage, session_epoch)) return c.WaddleSlotWrongSessionEpoch;
    const output: *c.struct_waddle_slot_view_t = @ptrFromInt(address);
    var view: runtime.slot_view_t = undefined;
    storage.runtime.prepare_publish(session_epoch, window, generation, slot, frame_sequence, &view) catch |err| return map_error(err);
    output.* = copy_view(view);
    return c.WaddleSlotOk;
}

/// Record caller-confirmed Free-to-Writing success.
/// @param[in] handle_address Nonnull aligned live opaque handle; not owned by this call.
/// @param[in] session_epoch Exact captured epoch; mismatch returns WrongSessionEpoch.
/// @param[in] window Window identity; @param[in] generation incarnation; @param[in] slot index 0..2.
/// @param[in] frame_sequence Exact provisional writer sequence.
/// @return Fixed C status for argument, epoch, identity, writer-state, or lifecycle outcomes.
/// @ownership No transfer occurs; this call performs no shared atomic operation.
/// @thread_safety One externally serialized event-loop thread per handle.
export fn waddle_slot_runtime_mark_writer_entered(
    handle_address: usize,
    session_epoch: u64,
    window: u64,
    generation: u64,
    slot: u8,
    frame_sequence: u64,
) c.enum_waddle_slot_status_t {
    const storage = resolve_handle(handle_address) orelse return c.WaddleSlotInvalidArgument;
    if (stale_epoch(storage, session_epoch)) return c.WaddleSlotWrongSessionEpoch;
    storage.runtime.mark_writer_entered(session_epoch, window, generation, slot, frame_sequence) catch |err| return map_error(err);
    return c.WaddleSlotOk;
}

/// Commit the published pin after caller-confirmed Writing-to-Ready.
/// @param[in] handle_address Nonnull aligned live opaque handle; not owned by this call.
/// @param[in] session_epoch Exact captured epoch; mismatch returns WrongSessionEpoch before mutation.
/// @param[in] window Window identity; @param[in] generation incarnation; @param[in] slot index 0..2.
/// @param[in] frame_sequence Exact provisional writer sequence.
/// @return Fixed C status for argument, epoch, identity, writer-state, or lifecycle outcomes.
/// @ownership Runtime retains the published pin until matching release or proven drain.
/// @thread_safety One externally serialized event-loop thread per handle.
export fn waddle_slot_runtime_commit_publish(
    handle_address: usize,
    session_epoch: u64,
    window: u64,
    generation: u64,
    slot: u8,
    frame_sequence: u64,
) c.enum_waddle_slot_status_t {
    const storage = resolve_handle(handle_address) orelse return c.WaddleSlotInvalidArgument;
    if (stale_epoch(storage, session_epoch)) return c.WaddleSlotWrongSessionEpoch;
    storage.runtime.commit_publish(session_epoch, window, generation, slot, frame_sequence) catch |err| return map_error(err);
    return c.WaddleSlotOk;
}

/// Cancel only the provisional claim whose write CAS did not enter Writing.
/// @param[in] handle_address Nonnull aligned live opaque handle; not owned by this call.
/// @param[in] session_epoch Exact captured epoch; mismatch returns WrongSessionEpoch.
/// @param[in] window Window identity; @param[in] generation incarnation; @param[in] slot index 0..2.
/// @param[in] frame_sequence Exact unentered provisional sequence.
/// @return Fixed C status for argument, epoch, identity, writer-state, or lifecycle outcomes.
/// @ownership Runtime consumes but does not reuse the sequence; reservations remain owned.
/// @thread_safety One externally serialized event-loop thread per handle.
export fn waddle_slot_runtime_cancel_unentered(
    handle_address: usize,
    session_epoch: u64,
    window: u64,
    generation: u64,
    slot: u8,
    frame_sequence: u64,
) c.enum_waddle_slot_status_t {
    const storage = resolve_handle(handle_address) orelse return c.WaddleSlotInvalidArgument;
    if (stale_epoch(storage, session_epoch)) return c.WaddleSlotWrongSessionEpoch;
    storage.runtime.cancel_unentered(session_epoch, window, generation, slot, frame_sequence) catch |err| return map_error(err);
    return c.WaddleSlotOk;
}

/// Cancel an entered writer only after caller-confirmed Writing-to-Free.
/// @param[in] handle_address Nonnull aligned live opaque handle; not owned by this call.
/// @param[in] session_epoch Exact captured epoch; mismatch returns WrongSessionEpoch.
/// @param[in] window Window identity; @param[in] generation incarnation; @param[in] slot index 0..2.
/// @param[in] frame_sequence Exact entered provisional sequence.
/// @return Fixed C status for argument, epoch, identity, writer-state, or lifecycle outcomes.
/// @ownership Runtime retains its reservation; this call performs no shared atomic operation.
/// @thread_safety One externally serialized event-loop thread per handle.
export fn waddle_slot_runtime_cancel_entered(
    handle_address: usize,
    session_epoch: u64,
    window: u64,
    generation: u64,
    slot: u8,
    frame_sequence: u64,
) c.enum_waddle_slot_status_t {
    const storage = resolve_handle(handle_address) orelse return c.WaddleSlotInvalidArgument;
    if (stale_epoch(storage, session_epoch)) return c.WaddleSlotWrongSessionEpoch;
    storage.runtime.cancel_entered(session_epoch, window, generation, slot, frame_sequence) catch |err| return map_error(err);
    return c.WaddleSlotOk;
}

/// Clear one exact published pin after validated FrameRelease.
/// @param[in] handle_address Nonnull aligned live opaque handle; not owned by this call.
/// @param[in] session_epoch Exact captured epoch; mismatch returns WrongSessionEpoch.
/// @param[in] window Window identity; @param[in] generation incarnation; @param[in] slot index 0..2.
/// @param[in] frame_sequence Exact published sequence from the validated release.
/// @return Fixed C status for argument, epoch, identity, sequence, pin, or lifecycle outcomes.
/// @ownership Only the matching pin is cleared; shared Free alone does not release it.
/// @thread_safety One externally serialized event-loop thread per handle.
export fn waddle_slot_runtime_release_frame(
    handle_address: usize,
    session_epoch: u64,
    window: u64,
    generation: u64,
    slot: u8,
    frame_sequence: u64,
) c.enum_waddle_slot_status_t {
    const storage = resolve_handle(handle_address) orelse return c.WaddleSlotInvalidArgument;
    if (stale_epoch(storage, session_epoch)) return c.WaddleSlotWrongSessionEpoch;
    storage.runtime.release_frame(session_epoch, window, generation, slot, frame_sequence) catch |err| return map_error(err);
    return c.WaddleSlotOk;
}

/// Retire one window after matching session and incarnation checks.
/// @param[in] handle_address Nonnull aligned live opaque handle; not owned by this call.
/// @param[in] session_epoch Exact captured epoch; mismatch returns WrongSessionEpoch.
/// @param[in] window Exact admitted identity; @param[in] generation exact incarnation.
/// @return Fixed C status for argument, epoch, identity, pin, or lifecycle outcomes.
/// @ownership Runtime quarantines descriptors until all claims and pins end.
/// @thread_safety One externally serialized event-loop thread per handle.
export fn waddle_slot_runtime_retire_window(
    handle_address: usize,
    session_epoch: u64,
    window: u64,
    generation: u64,
) c.enum_waddle_slot_status_t {
    const storage = resolve_handle(handle_address) orelse return c.WaddleSlotInvalidArgument;
    if (stale_epoch(storage, session_epoch)) return c.WaddleSlotWrongSessionEpoch;
    storage.runtime.retire_window(window, generation) catch |err| return map_error(err);
    return c.WaddleSlotOk;
}

/// Begin producer drain; proof is a fixed uint32 value 0 or 1.
/// @param[in] handle_address Nonnull aligned live opaque handle; not owned by this call.
/// @param[in] session_epoch Exact captured epoch; mismatch returns WrongSessionEpoch.
/// @param[in] producers_stopped Caller proof, exactly 0 or 1; other values are InvalidArgument.
/// @return Fixed C status for argument, epoch, missing proof, or lifecycle outcomes.
/// @ownership Does not free pins or reservations; caller owns truth of the proof.
/// @thread_safety One externally serialized event-loop thread per handle.
export fn waddle_slot_runtime_begin_drain(
    handle_address: usize,
    session_epoch: u64,
    producers_stopped: u32,
) c.enum_waddle_slot_status_t {
    const storage = resolve_handle(handle_address) orelse return c.WaddleSlotInvalidArgument;
    if (stale_epoch(storage, session_epoch)) return c.WaddleSlotWrongSessionEpoch;
    if (producers_stopped > 1) return c.WaddleSlotInvalidArgument;
    storage.runtime.begin_drain(producers_stopped == 1) catch |err| return map_error(err);
    return c.WaddleSlotOk;
}

/// Confirm destroyed host buffers; successful proof enables destroy.
/// @param[in] handle_address Nonnull aligned live opaque handle; not owned by this call.
/// @param[in] session_epoch Exact captured epoch; mismatch returns WrongSessionEpoch.
/// @param[in] host_buffers_destroyed Caller proof, exactly 0 or 1; other values are InvalidArgument.
/// @return Fixed C status for argument, epoch, missing proof, or lifecycle outcomes.
/// @ownership On success, wrapper records proof; caller then owns the duty to destroy the handle.
/// @thread_safety One externally serialized event-loop thread per handle.
export fn waddle_slot_runtime_confirm_host_drained(
    handle_address: usize,
    session_epoch: u64,
    host_buffers_destroyed: u32,
) c.enum_waddle_slot_status_t {
    const storage = resolve_handle(handle_address) orelse return c.WaddleSlotInvalidArgument;
    if (stale_epoch(storage, session_epoch)) return c.WaddleSlotWrongSessionEpoch;
    if (host_buffers_destroyed > 1) return c.WaddleSlotInvalidArgument;
    storage.runtime.confirm_host_drained(host_buffers_destroyed == 1) catch |err| return map_error(err);
    storage.host_drained = true;
    return c.WaddleSlotOk;
}

// Test-only fixtures.
fn call_map_error(err: runtime.slot_runtime_error_t) c.enum_waddle_slot_status_t {
    return @call(.never_inline, map_error, .{err});
}

fn expect_status(expected: c_uint, actual: c.enum_waddle_slot_status_t) !void {
    try std.testing.expectEqual(expected, actual);
}

fn call_create(epoch: u64, output_address: usize) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_create, .{ epoch, output_address });
}

fn call_destroy(output_address: usize) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_destroy, .{output_address});
}

fn call_admit(handle_address: usize, epoch: u64, window: u64, generation: u64, width: u32, height: u32, views_address: usize) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_admit_window, .{ handle_address, epoch, window, generation, width, height, views_address });
}

fn call_geometry(handle_address: usize, epoch: u64, window: u64, generation: u64, width: u32, height: u32) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_request_geometry, .{ handle_address, epoch, window, generation, width, height });
}

fn call_prepare(handle_address: usize, epoch: u64, window: u64, generation: u64, slot: u8, sequence: u64, view_address: usize) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_prepare_publish, .{ handle_address, epoch, window, generation, slot, sequence, view_address });
}

fn call_writer_entered(handle_address: usize, epoch: u64, window: u64, generation: u64, slot: u8, sequence: u64) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_mark_writer_entered, .{ handle_address, epoch, window, generation, slot, sequence });
}

fn call_commit(handle_address: usize, epoch: u64, window: u64, generation: u64, slot: u8, sequence: u64) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_commit_publish, .{ handle_address, epoch, window, generation, slot, sequence });
}

fn call_cancel_unentered(handle_address: usize, epoch: u64, window: u64, generation: u64, slot: u8, sequence: u64) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_cancel_unentered, .{ handle_address, epoch, window, generation, slot, sequence });
}

fn call_cancel_entered(handle_address: usize, epoch: u64, window: u64, generation: u64, slot: u8, sequence: u64) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_cancel_entered, .{ handle_address, epoch, window, generation, slot, sequence });
}

fn call_release(handle_address: usize, epoch: u64, window: u64, generation: u64, slot: u8, sequence: u64) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_release_frame, .{ handle_address, epoch, window, generation, slot, sequence });
}

fn call_retire(handle_address: usize, epoch: u64, window: u64, generation: u64) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_retire_window, .{ handle_address, epoch, window, generation });
}

fn call_begin_drain(handle_address: usize, epoch: u64, producers_stopped: u32) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_begin_drain, .{ handle_address, epoch, producers_stopped });
}

fn call_confirm_drained(handle_address: usize, epoch: u64, host_buffers_destroyed: u32) c.enum_waddle_slot_status_t {
    return @call(.never_inline, waddle_slot_runtime_confirm_host_drained, .{ handle_address, epoch, host_buffers_destroyed });
}

fn all_runtime_errors() [25]runtime.slot_runtime_error_t {
    return .{
        error.InvalidSessionEpoch,
        error.WrongSessionEpoch,
        error.InvalidWindow,
        error.InvalidGeneration,
        error.StaleGeneration,
        error.DuplicateWindow,
        error.NoWindowCapacity,
        error.UnknownWindow,
        error.InvalidSlot,
        error.InvalidGeometry,
        error.HeapFull,
        error.TicketExhausted,
        error.Busy,
        error.SlotTooSmall,
        error.WriterAlreadyActive,
        error.NoWriter,
        error.WriterStateMismatch,
        error.InvalidSequence,
        error.SequenceExhausted,
        error.WrongRelease,
        error.SlotPinned,
        error.InvalidLifecycle,
        error.MissingProof,
        error.HeapStateCorrupt,
        error.FailedClosed,
    };
}

test "runtime error status mapping is exhaustive and has frozen values" {
    for (all_runtime_errors(), 0..) |err, index| {
        var runtime_error = err;
        const volatile_error: *volatile runtime.slot_runtime_error_t = &runtime_error;
        try std.testing.expectEqual(@as(u32, @intCast(index + 3)), call_map_error(volatile_error.*));
    }
    try std.testing.expectEqual(@as(u32, 0), c.WaddleSlotOk);
    try std.testing.expectEqual(@as(u32, 1), c.WaddleSlotInvalidArgument);
    try std.testing.expectEqual(@as(u32, 2), c.WaddleSlotOutOfMemory);
}

test "C ABI address checks preserve outputs and session epochs gate every operation" {
    var raw_handle: ?*anyopaque = null;
    const output_address = @intFromPtr(&raw_handle);
    const sentinel_handle: ?*anyopaque = @ptrFromInt(0x1234);
    raw_handle = sentinel_handle;
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_create(7, 0));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_create(7, output_address + 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_create(7, std.math.maxInt(usize) - 3));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_create(7, output_address));
    try std.testing.expectEqual(sentinel_handle, raw_handle);
    raw_handle = null;
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidSessionEpoch)), call_create(0, output_address));
    try std.testing.expect(raw_handle == null);
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_create(71, output_address));
    const handle_address = @intFromPtr(raw_handle.?);

    var views: [runtime.SlotsPerWindow]c.struct_waddle_slot_view_t = undefined;
    @memset(std.mem.asBytes(&views), 0xa5);
    const views_before = views;
    const views_address = @intFromPtr(&views);
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongSessionEpoch)), call_admit(handle_address, 72, 100, 1, 64, 64, views_address));
    try std.testing.expectEqualDeep(views_before, views);
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_admit(handle_address, 71, 100, 1, 64, 64, 0));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_admit(handle_address, 71, 100, 1, 64, 64, views_address + 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_admit(handle_address, 71, 100, 1, 64, 64, handle_address));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_admit(handle_address, 71, 100, 1, 64, 64, std.math.maxInt(usize) - 7));
    try std.testing.expectEqualDeep(views_before, views);
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_admit(handle_address, 71, 100, 1, 64, 64, views_address));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotDuplicateWindow)), call_admit(handle_address, 71, 100, 2, 64, 64, views_address));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotStaleGeneration)), call_admit(handle_address, 71, 100, 1, 1, 64, views_address));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidWindow)), call_admit(handle_address, 71, 0, 2, 64, 64, views_address));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidGeneration)), call_admit(handle_address, 71, 101, 0, 64, 64, views_address));

    var output = c.struct_waddle_slot_view_t{
        .descriptor_index = 77,
        .window = 78,
        .generation = 79,
        .width = 80,
        .height = 81,
        .stride = 82,
        .pixel_bytes = 83,
        .data_offset = 84,
        .data_bytes = 85,
    };
    const output_address_view = @intFromPtr(&output);
    const output_before = output;
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongSessionEpoch)), call_prepare(handle_address, 72, 100, 1, 0, 1, output_address_view));
    try std.testing.expectEqualDeep(output_before, output);
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_prepare(handle_address, 71, 100, 1, 0, 1, 0));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_prepare(handle_address, 71, 100, 1, 0, 1, output_address_view + 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_prepare(handle_address, 71, 100, 1, 0, 1, handle_address));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_prepare(handle_address, 71, 100, 1, 0, 1, std.math.maxInt(usize) - 7));
    try std.testing.expectEqualDeep(output_before, output);
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidSequence)), call_prepare(handle_address, 71, 100, 1, 0, 0, output_address_view));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidSlot)), call_prepare(handle_address, 71, 100, 1, 3, 1, output_address_view));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_prepare(handle_address, 71, 100, 1, 0, 1, output_address_view));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongSessionEpoch)), call_writer_entered(handle_address, 72, 100, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongSessionEpoch)), call_commit(handle_address, 72, 100, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongSessionEpoch)), call_cancel_unentered(handle_address, 72, 100, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongSessionEpoch)), call_cancel_entered(handle_address, 72, 100, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongSessionEpoch)), call_release(handle_address, 72, 100, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongSessionEpoch)), call_retire(handle_address, 72, 100, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongSessionEpoch)), call_geometry(handle_address, 72, 100, 1, 128, 96));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongSessionEpoch)), call_begin_drain(handle_address, 72, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongSessionEpoch)), call_confirm_drained(handle_address, 72, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotWrongRelease)), call_release(handle_address, 71, 100, 1, 0, 99));

    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_writer_entered(handle_address, 71, 100, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_commit(handle_address, 71, 100, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_geometry(handle_address, 71, 100, 1, 2000, 1200));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_release(handle_address, 71, 100, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_prepare(handle_address, 71, 100, 1, 1, 2, output_address_view));
    try std.testing.expect(output.data_bytes >= views[1].data_bytes);
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_cancel_unentered(handle_address, 71, 100, 1, 1, 2));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_prepare(handle_address, 71, 100, 1, 2, 3, output_address_view));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_writer_entered(handle_address, 71, 100, 1, 2, 3));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_cancel_entered(handle_address, 71, 100, 1, 2, 3));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_retire(handle_address, 71, 100, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotUnknownWindow)), call_prepare(handle_address, 71, 100, 1, 0, 4, output_address_view));

    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_begin_drain(handle_address, 71, 2));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotMissingProof)), call_begin_drain(handle_address, 71, 0));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_begin_drain(handle_address, 71, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_confirm_drained(handle_address, 71, 2));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotMissingProof)), call_confirm_drained(handle_address, 71, 0));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidLifecycle)), call_destroy(output_address));
    try std.testing.expect(raw_handle != null);
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_confirm_drained(handle_address, 71, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_destroy(output_address));
    try std.testing.expect(raw_handle == null);
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotOk)), call_destroy(output_address));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_destroy(0));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_destroy(output_address + 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_destroy(std.math.maxInt(usize) - 3));
}

test "every exported operation rejects a null runtime before reading caller outputs" {
    var output: ?*anyopaque = null;
    const address = @intFromPtr(&output);
    var view: c.struct_waddle_slot_view_t = undefined;
    const view_address = @intFromPtr(&view);
    var views: [runtime.SlotsPerWindow]c.struct_waddle_slot_view_t = undefined;
    const views_address = @intFromPtr(&views);
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_destroy(0));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_admit(0, 1, 1, 1, 1, 1, views_address));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_geometry(0, 1, 1, 1, 1, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_prepare(0, 1, 1, 1, 0, 1, view_address));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_writer_entered(0, 1, 1, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_commit(0, 1, 1, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_cancel_unentered(0, 1, 1, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_cancel_entered(0, 1, 1, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_release(0, 1, 1, 1, 0, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_retire(0, 1, 1, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_begin_drain(0, 1, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_confirm_drained(0, 1, 1));
    try std.testing.expectEqual(@as(u32, @intCast(c.WaddleSlotInvalidArgument)), call_create(1, address + 1));
}

test "exported writer and release errors preserve scalar output snapshots" {
    var raw_handle: ?*anyopaque = null;
    const handle_slot = @intFromPtr(&raw_handle);
    try expect_status(c.WaddleSlotOk, call_create(401, handle_slot));
    const handle_address = @intFromPtr(raw_handle.?);
    var views: [runtime.SlotsPerWindow]c.struct_waddle_slot_view_t = undefined;
    try expect_status(c.WaddleSlotOk, call_admit(handle_address, 401, 401, 1, 64, 64, @intFromPtr(&views)));

    var view = c.struct_waddle_slot_view_t{
        .descriptor_index = 91,
        .window = 92,
        .generation = 93,
        .width = 94,
        .height = 95,
        .stride = 96,
        .pixel_bytes = 97,
        .data_offset = 98,
        .data_bytes = 99,
    };
    const view_address = @intFromPtr(&view);
    try expect_status(c.WaddleSlotNoWriter, call_cancel_unentered(handle_address, 401, 401, 1, 0, 1));
    try expect_status(c.WaddleSlotNoWriter, call_cancel_entered(handle_address, 401, 401, 1, 0, 1));
    try expect_status(c.WaddleSlotOk, call_prepare(handle_address, 401, 401, 1, 0, 1, view_address));
    const claimed_view = view;
    try expect_status(c.WaddleSlotWriterAlreadyActive, call_prepare(handle_address, 401, 401, 1, 1, 2, view_address));
    try std.testing.expectEqualDeep(claimed_view, view);
    try expect_status(c.WaddleSlotWriterStateMismatch, call_cancel_entered(handle_address, 401, 401, 1, 0, 1));
    try expect_status(c.WaddleSlotOk, call_writer_entered(handle_address, 401, 401, 1, 0, 1));
    try expect_status(c.WaddleSlotWriterStateMismatch, call_cancel_unentered(handle_address, 401, 401, 1, 0, 1));
    try expect_status(c.WaddleSlotPinned, call_begin_drain(handle_address, 401, 1));
    try expect_status(c.WaddleSlotOk, call_cancel_entered(handle_address, 401, 401, 1, 0, 1));

    try expect_status(c.WaddleSlotOk, call_prepare(handle_address, 401, 401, 1, 0, 2, view_address));
    try expect_status(c.WaddleSlotOk, call_writer_entered(handle_address, 401, 401, 1, 0, 2));
    try expect_status(c.WaddleSlotOk, call_commit(handle_address, 401, 401, 1, 0, 2));
    try expect_status(c.WaddleSlotWrongRelease, call_release(handle_address, 401, 401, 1, 0, 3));
    try expect_status(c.WaddleSlotOk, call_release(handle_address, 401, 401, 1, 0, 2));

    try expect_status(c.WaddleSlotOk, call_retire(handle_address, 401, 401, 1));
    try expect_status(c.WaddleSlotOk, call_begin_drain(handle_address, 401, 1));
    try expect_status(c.WaddleSlotOk, call_confirm_drained(handle_address, 401, 1));
    try expect_status(c.WaddleSlotOk, call_destroy(handle_slot));
}

test "exported geometry growth capacity and lifecycle errors are observable" {
    var raw_handle: ?*anyopaque = null;
    const handle_slot = @intFromPtr(&raw_handle);
    try expect_status(c.WaddleSlotOk, call_create(402, handle_slot));
    const handle_address = @intFromPtr(raw_handle.?);
    var views: [runtime.SlotsPerWindow]c.struct_waddle_slot_view_t = undefined;
    @memset(std.mem.asBytes(&views), 0x6d);
    const views_before = views;
    const views_address = @intFromPtr(&views);
    try expect_status(c.WaddleSlotInvalidGeometry, call_admit(handle_address, 402, 1, 1, 0, 64, views_address));
    try std.testing.expectEqualDeep(views_before, views);
    try expect_status(c.WaddleSlotOk, call_admit(handle_address, 402, 10, 1, 64, 64, views_address));
    try expect_status(c.WaddleSlotOk, call_geometry(handle_address, 402, 10, 1, 2000, 1200));
    try expect_status(c.WaddleSlotBusy, call_geometry(handle_address, 402, 10, 1, 3000, 1600));

    const storage: *handle_storage_t = @ptrFromInt(handle_address);
    const prior_layout = storage.runtime.windows[0].desired_layout;
    storage.runtime.windows[0].desired_layout.data_bytes = std.math.maxInt(u32);
    var view = c.struct_waddle_slot_view_t{
        .descriptor_index = 101,
        .window = 102,
        .generation = 103,
        .width = 104,
        .height = 105,
        .stride = 106,
        .pixel_bytes = 107,
        .data_offset = 108,
        .data_bytes = 109,
    };
    const view_before = view;
    const view_address = @intFromPtr(&view);
    try expect_status(c.WaddleSlotSlotTooSmall, call_prepare(handle_address, 402, 10, 1, 0, 1, view_address));
    try std.testing.expectEqualDeep(view_before, view);
    storage.runtime.windows[0].desired_layout = prior_layout;

    try expect_status(c.WaddleSlotOk, call_retire(handle_address, 402, 10, 1));
    try expect_status(c.WaddleSlotOk, call_begin_drain(handle_address, 402, 1));
    @memset(std.mem.asBytes(&views), 0xa5);
    const drained_views = views;
    try expect_status(c.WaddleSlotInvalidLifecycle, call_admit(handle_address, 402, 11, 2, 64, 64, views_address));
    try std.testing.expectEqualDeep(drained_views, views);
    try expect_status(c.WaddleSlotMissingProof, call_confirm_drained(handle_address, 402, 0));
    try expect_status(c.WaddleSlotOk, call_confirm_drained(handle_address, 402, 1));
    try expect_status(c.WaddleSlotOk, call_destroy(handle_slot));
}

test "exported heap exhaustion and window-table capacity leave outputs unchanged" {
    var heap_handle: ?*anyopaque = null;
    const heap_slot = @intFromPtr(&heap_handle);
    try expect_status(c.WaddleSlotOk, call_create(403, heap_slot));
    const heap_address = @intFromPtr(heap_handle.?);
    var views: [runtime.SlotsPerWindow]c.struct_waddle_slot_view_t = undefined;
    @memset(std.mem.asBytes(&views), 0xd3);
    var admitted_large: u32 = 0;
    for (0..8) |index| {
        const views_before_attempt = views;
        const status = call_admit(heap_address, 403, @intCast(index + 1), @intCast(index + 1), 7680, 4320, @intFromPtr(&views));
        if (status == c.WaddleSlotOk) {
            admitted_large += 1;
        } else {
            try expect_status(c.WaddleSlotHeapFull, status);
            try std.testing.expectEqualDeep(views_before_attempt, views);
            break;
        }
    }
    try std.testing.expect(admitted_large > 0 and admitted_large < 8);
    try expect_status(c.WaddleSlotOk, call_begin_drain(heap_address, 403, 1));
    try expect_status(c.WaddleSlotOk, call_confirm_drained(heap_address, 403, 1));
    try expect_status(c.WaddleSlotOk, call_destroy(heap_slot));

    var table_handle: ?*anyopaque = null;
    const table_slot = @intFromPtr(&table_handle);
    try expect_status(c.WaddleSlotOk, call_create(404, table_slot));
    const table_address = @intFromPtr(table_handle.?);
    @memset(std.mem.asBytes(&views), 0x3c);
    for (0..runtime.MaxWindows) |index| {
        try expect_status(c.WaddleSlotOk, call_admit(table_address, 404, @intCast(index + 1), @intCast(index + 1), 1, 1, @intFromPtr(&views)));
    }
    @memset(std.mem.asBytes(&views), 0x7e);
    const views_before = views;
    try expect_status(c.WaddleSlotNoWindowCapacity, call_admit(table_address, 404, 1000, runtime.MaxWindows + 1, 1, 1, @intFromPtr(&views)));
    try std.testing.expectEqualDeep(views_before, views);
    try expect_status(c.WaddleSlotOk, call_begin_drain(table_address, 404, 1));
    try expect_status(c.WaddleSlotOk, call_confirm_drained(table_address, 404, 1));
    try expect_status(c.WaddleSlotOk, call_destroy(table_slot));
}

test "exported current-session corruption fails closed and remains drainable" {
    var raw_handle: ?*anyopaque = null;
    const handle_slot = @intFromPtr(&raw_handle);
    try expect_status(c.WaddleSlotOk, call_create(405, handle_slot));
    const handle_address = @intFromPtr(raw_handle.?);
    var views: [runtime.SlotsPerWindow]c.struct_waddle_slot_view_t = undefined;
    try expect_status(c.WaddleSlotOk, call_admit(handle_address, 405, 50, 1, 64, 64, @intFromPtr(&views)));
    try expect_status(c.WaddleSlotFailedClosed, call_commit(handle_address, 405, 50, 1, 0, 1));
    try expect_status(c.WaddleSlotFailedClosed, call_geometry(handle_address, 405, 50, 1, 100, 100));
    try expect_status(c.WaddleSlotOk, call_begin_drain(handle_address, 405, 1));
    try expect_status(c.WaddleSlotOk, call_confirm_drained(handle_address, 405, 1));
    try expect_status(c.WaddleSlotOk, call_destroy(handle_slot));
}
