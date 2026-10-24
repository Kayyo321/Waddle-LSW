//! Opaque, copied-buffer C ABI for the serialized control peer.
const std_t = @import("std");
const c_t = @cImport({
    @cInclude("waddle/interop_peer.h");
    @cInclude("waddle/interop_protocol.h");
});
const peer_mod_t = @import("control_peer.zig");

comptime {
    if (@sizeOf(usize) != 8) @compileError("control-peer ABI requires supported 64-bit pointer and size_t ABI");
    if (@sizeOf(c_t.struct_interop_peer_config_t) != 48 or @alignOf(c_t.struct_interop_peer_config_t) != 8)
        @compileError("interop_peer_config_t ABI mismatch");
    if (@sizeOf(c_t.struct_interop_peer_event_t) != 56 or @alignOf(c_t.struct_interop_peer_event_t) != 8)
        @compileError("interop_peer_event_t ABI mismatch");
}

/// Private allocation containing the peer and the allocator used for every owned allocation.
const owner_t = struct {
    allocator: std_t.mem.Allocator,
    peer: peer_mod_t.peer_t,
};

/// Test and production constructor; ownership of the returned owner transfers to the caller.
fn create_owner(allocator: std_t.mem.Allocator, config: peer_mod_t.config_t) (peer_mod_t.control_peer_error_t || error{ InvalidVersion, UnknownCapabilities, OutOfMemory })!*owner_t {
    const peer = try peer_mod_t.peer_t.init(allocator, config);
    const owner = allocator.create(owner_t) catch {
        var cleanup = peer;
        cleanup.deinit();
        return error.OutOfMemory;
    };
    owner.* = .{ .allocator = allocator, .peer = peer };
    return owner;
}

/// Deinitialize peer-owned buffers and free the opaque allocation with its original allocator.
fn destroy_owner(owner: *owner_t) void {
    const allocator = owner.allocator;
    owner.peer.deinit();
    allocator.destroy(owner);
}

fn valid_range(address: usize, size: usize, alignment: usize) bool {
    return address != 0 and size != 0 and alignment != 0 and address % alignment == 0 and
        address <= std_t.math.maxInt(usize) - size;
}

fn valid_byte_range(address: usize, size: usize) bool {
    if (size == 0) return true;
    return valid_range(address, size, 1);
}

fn ranges_overlap(left: usize, left_size: usize, right: usize, right_size: usize) bool {
    if (left_size == 0 or right_size == 0) return false;
    // Compare ordered address differences: alias checks cannot overflow while
    // evaluating an interval endpoint. Public callers preflight non-wrapping ranges.
    if (left <= right) return right - left < left_size;
    return left - right < right_size;
}

fn resolve_owner(address: usize) ?*owner_t {
    if (!valid_range(address, @sizeOf(owner_t), @alignOf(owner_t))) return null;
    return @ptrFromInt(address);
}

fn owner_overlaps(owner: *const owner_t, address: usize, size: usize) bool {
    if (size == 0) return false;
    if (ranges_overlap(address, size, @intFromPtr(owner), @sizeOf(owner_t))) return true;
    const version = owner.peer.local_version;
    if (ranges_overlap(address, size, @intFromPtr(version.ptr), version.len)) return true;
    for (owner.peer.queue.frames) |maybe_record| {
        const record = maybe_record orelse continue;
        if (ranges_overlap(address, size, @intFromPtr(record.bytes.ptr), record.bytes.len)) return true;
    }
    return false;
}

fn valid_output(owner: *const owner_t, address: usize, size: usize, alignment: usize) bool {
    return valid_range(address, size, alignment) and !owner_overlaps(owner, address, size);
}

fn valid_disjoint_ranges(left: usize, left_size: usize, right: usize, right_size: usize) bool {
    return !ranges_overlap(left, left_size, right, right_size);
}

fn map_phase(phase: peer_mod_t.phase_t) u32 {
    return switch (phase) {
        .AwaitHello => @intCast(c_t.InteropPeerAwaitHello),
        .AwaitMapping => @intCast(c_t.InteropPeerAwaitMapping),
        .AwaitHelloAckSent => @intCast(c_t.InteropPeerAwaitHelloAckSent),
        .InitializingMapping => @intCast(c_t.InteropPeerInitializingMapping),
        .AwaitHelloSent => @intCast(c_t.InteropPeerAwaitHelloSent),
        .AwaitHelloAck => @intCast(c_t.InteropPeerAwaitHelloAck),
        .Ready => @intCast(c_t.InteropPeerReady),
        .Draining => @intCast(c_t.InteropPeerDraining),
        .Closed => @intCast(c_t.InteropPeerTransportClosed),
    };
}

fn map_event_kind(kind: peer_mod_t.event_kind_t) u32 {
    return switch (kind) {
        .None => @intCast(c_t.InteropPeerEventNone),
        .ValidateMapping => @intCast(c_t.InteropPeerEventValidateMapping),
        .Ready => @intCast(c_t.InteropPeerEventReady),
        .FeatureFrame => @intCast(c_t.InteropPeerEventFeatureFrame),
        .PeerError => @intCast(c_t.InteropPeerEventError),
        .Draining => @intCast(c_t.InteropPeerEventDraining),
    };
}

fn copy_event(owner: *const owner_t, event: peer_mod_t.event_t) c_t.struct_interop_peer_event_t {
    var flags: u32 = 0;
    if (owner.peer.must_abort_transport()) flags |= c_t.InteropPeerAbortTransport;
    if (owner.peer.terminal_send_unavailable()) flags |= c_t.InteropPeerTerminalSendUnavailable;
    return .{
        .kind = map_event_kind(event.kind),
        .phase = map_phase(owner.peer.phase()),
        .flags = flags,
        .error_code = if (event.error_code > 0) @intCast(event.error_code) else 0,
        .session_epoch = event.epoch,
        .capabilities = event.capabilities,
        .layout_version = event.layout_version,
        .message_type = event.message_type,
        .request_id = event.request_id,
        .reserved_zero = 0,
        .copied_bytes = event.copied_bytes,
    };
}

fn preflight_live(owner: *const owner_t) ?c_int {
    if (!owner.peer.live) return c_t.InteropPeerClosed;
    return null;
}

fn preflight_token(owner: *const owner_t, token: u64) ?c_int {
    if (preflight_live(owner)) |result| return result;
    if (token == 0) return c_t.InteropPeerInvalidArgument;
    if (token != owner.peer.connection_token) return c_t.InteropPeerWrongToken;
    return null;
}

fn preflight_time(owner: *const owner_t, now_ms: u64) ?c_int {
    if (now_ms < owner.peer.last_now_ms) return c_t.InteropPeerTimeWentBackwards;
    return null;
}

fn map_create_error(err: anyerror) c_int {
    return switch (err) {
        error.InvalidConnectionToken, error.InvalidVersion, error.UnknownCapabilities => c_t.InteropPeerInvalidArgument,
        error.OutOfMemory => c_t.InteropPeerOutOfMemory,
        else => c_t.InteropPeerInternalInvariant,
    };
}

fn read_config(address: usize) ?peer_mod_t.config_t {
    if (!valid_range(address, @sizeOf(c_t.struct_interop_peer_config_t), @alignOf(c_t.struct_interop_peer_config_t))) return null;
    const source: *const c_t.struct_interop_peer_config_t = @ptrFromInt(address);
    if (source.reserved_zero != 0 or source.connection_token == 0) return null;
    const role: peer_mod_t.role_t = switch (source.role) {
        c_t.InteropPeerHost => .Host,
        c_t.InteropPeerGuest => .Guest,
        else => return null,
    };
    const version_address = @intFromPtr(source.agent_version);
    if (source.agent_version_bytes < peer_mod_t.MinVersionBytes or source.agent_version_bytes > peer_mod_t.MaxVersionBytes or
        !valid_byte_range(version_address, source.agent_version_bytes)) return null;
    if (ranges_overlap(address, @sizeOf(c_t.struct_interop_peer_config_t), version_address, source.agent_version_bytes)) return null;
    const version_ptr: [*]const u8 = @ptrFromInt(version_address);
    return .{
        .role = role,
        .connection_token = source.connection_token,
        .accepted_at_ms = source.accepted_at_ms,
        .local_version = version_ptr[0..source.agent_version_bytes],
        .local_capabilities = source.local_capabilities,
    };
}

fn byte_slice(address: usize, size: usize) []u8 {
    if (size == 0) return &.{};
    const pointer: [*]u8 = @ptrFromInt(address);
    return pointer[0..size];
}

fn const_byte_slice(address: usize, size: usize) []const u8 {
    if (size == 0) return &.{};
    const pointer: [*]const u8 = @ptrFromInt(address);
    return pointer[0..size];
}

/// Create one opaque peer from a borrowed, validated C configuration.
/// @param[in] config_address Raw C config pointer word; validated before dereference.
/// @param[out] out_peer_address Raw address of aligned NULL owner slot, unchanged on error.
/// @return Fixed `interop_peer_result_t`; successful ownership transfers to caller.
/// @ownership Copies version; owner and internal queue use C allocator until destroy.
/// @thread_safety Safe for distinct owners; caller serializes each created peer.
export fn interop_peer_create(config_address: usize, out_peer_address: usize) c_int {
    if (!valid_range(config_address, @sizeOf(c_t.struct_interop_peer_config_t), @alignOf(c_t.struct_interop_peer_config_t)) or
        !valid_range(out_peer_address, @sizeOf(?*anyopaque), @alignOf(?*anyopaque))) return c_t.InteropPeerInvalidArgument;
    if (!valid_disjoint_ranges(config_address, @sizeOf(c_t.struct_interop_peer_config_t), out_peer_address, @sizeOf(?*anyopaque))) return c_t.InteropPeerInvalidArgument;
    const config = read_config(config_address) orelse return c_t.InteropPeerInvalidArgument;
    const source: *const c_t.struct_interop_peer_config_t = @ptrFromInt(config_address);
    const version_address = @intFromPtr(source.agent_version);
    if (!valid_disjoint_ranges(version_address, source.agent_version_bytes, out_peer_address, @sizeOf(?*anyopaque))) return c_t.InteropPeerInvalidArgument;
    const output: *?*anyopaque = @ptrFromInt(out_peer_address);
    if (output.* != null) return c_t.InteropPeerInvalidArgument;
    const owner = create_owner(std_t.heap.c_allocator, config) catch |err| return map_create_error(err);
    output.* = @ptrCast(owner);
    return c_t.InteropPeerSuccess;
}

/// Destroy one opaque peer after its callbacks have quiesced; NULL slot is idempotent.
/// @param[in,out] owner_slot_address Raw aligned address of the caller-owned owner slot.
/// @return Success, InvalidArgument, or InternalInvariant; success writes NULL.
/// @ownership Frees peer queue/version and owner using its original allocator.
/// @thread_safety Caller serializes with every peer operation and borrowed buffer.
export fn interop_peer_destroy(owner_slot_address: usize) c_int {
    if (!valid_range(owner_slot_address, @sizeOf(?*anyopaque), @alignOf(?*anyopaque))) return c_t.InteropPeerInvalidArgument;
    const slot: *?*anyopaque = @ptrFromInt(owner_slot_address);
    const opaque_value = slot.* orelse return c_t.InteropPeerSuccess;
    const owner = resolve_owner(@intFromPtr(opaque_value)) orelse return c_t.InteropPeerInvalidArgument;
    if (owner_overlaps(owner, owner_slot_address, @sizeOf(?*anyopaque))) return c_t.InteropPeerInvalidArgument;
    destroy_owner(owner);
    slot.* = null;
    return c_t.InteropPeerSuccess;
}

/// Snapshot phase and persistent flags after checking the captured connection token.
/// @param[in] owner_address Raw opaque owner pointer word.
/// @param[in] token Captured nonzero connection token.
/// @param[out] event_address Aligned caller-owned event, unchanged on error.
/// @return Fixed C result; succeeds after transport Closed while owner remains live.
/// @ownership Copies scalar state only; no pointer escapes.
/// @thread_safety Caller serializes observation with peer mutation.
export fn interop_peer_get_state(owner_address: usize, token: u64, event_address: usize) c_int {
    const owner = resolve_owner(owner_address) orelse return c_t.InteropPeerInvalidArgument;
    if (!valid_output(owner, event_address, @sizeOf(c_t.struct_interop_peer_event_t), @alignOf(c_t.struct_interop_peer_event_t))) return c_t.InteropPeerInvalidArgument;
    if (preflight_token(owner, token)) |result| return result;
    const output: *c_t.struct_interop_peer_event_t = @ptrFromInt(event_address);
    output.* = copy_event(owner, .{ .kind = .None, .phase = owner.peer.phase() });
    return c_t.InteropPeerSuccess;
}

/// Feed one borrowed byte prefix and return the consumed count plus terminal flags.
/// @param[in,out] owner_address Raw owner pointer word; operation is serialized.
/// @param[in] token Captured token; @param[in] bytes_address/length borrowed input range.
/// @param[out] consumed_address and event_address disjoint caller-owned outputs.
/// @return Success for framing violation (reported in event); local errors preserve outputs.
/// @ownership Copies bytes into the fixed reader; no input pointer is retained.
/// @thread_safety Caller serializes calls on this peer.
export fn interop_peer_feed(owner_address: usize, token: u64, bytes_address: usize, length: usize, consumed_address: usize, event_address: usize) c_int {
    const owner = resolve_owner(owner_address) orelse return c_t.InteropPeerInvalidArgument;
    if (!valid_byte_range(bytes_address, length) or
        !valid_output(owner, consumed_address, @sizeOf(usize), @alignOf(usize)) or
        !valid_output(owner, event_address, @sizeOf(c_t.struct_interop_peer_event_t), @alignOf(c_t.struct_interop_peer_event_t)) or
        !valid_disjoint_ranges(consumed_address, @sizeOf(usize), event_address, @sizeOf(c_t.struct_interop_peer_event_t)) or
        owner_overlaps(owner, bytes_address, length) or
        !valid_disjoint_ranges(bytes_address, length, consumed_address, @sizeOf(usize)) or
        !valid_disjoint_ranges(bytes_address, length, event_address, @sizeOf(c_t.struct_interop_peer_event_t))) return c_t.InteropPeerInvalidArgument;
    if (preflight_token(owner, token)) |result| return result;
    if (owner.peer.phase() == .Draining or owner.peer.phase() == .Closed) return c_t.InteropPeerWrongPhase;
    const result = owner.peer.feed(token, const_byte_slice(bytes_address, length)) catch return c_t.InteropPeerInternalInvariant;
    const consumed_output: *usize = @ptrFromInt(consumed_address);
    const event_output: *c_t.struct_interop_peer_event_t = @ptrFromInt(event_address);
    const event = peer_mod_t.event_t{ .kind = if (result.phase == .Draining) .Draining else .None, .phase = result.phase };
    consumed_output.* = result.consumed;
    var copied_event = copy_event(owner, event);
    if (result.terminal_error) |code| copied_event.error_code = @intCast(code);
    if (result.terminal_send_failed) copied_event.flags |= c_t.InteropPeerTerminalSendUnavailable;
    event_output.* = copied_event;
    return c_t.InteropPeerSuccess;
}

/// Report the complete pending receive-frame length without consuming the frame.
/// @param[in] owner_address Raw opaque owner pointer word; @param[in] token captured token.
/// @param[out] length_address Aligned caller-owned `size_t`, unchanged on error.
/// @return Fixed C result; zero indicates no complete frame.
/// @ownership Scalar copy only.
/// @thread_safety Serialize with feed/next_event.
export fn interop_peer_pending_bytes(owner_address: usize, token: u64, length_address: usize) c_int {
    const owner = resolve_owner(owner_address) orelse return c_t.InteropPeerInvalidArgument;
    if (!valid_output(owner, length_address, @sizeOf(usize), @alignOf(usize))) return c_t.InteropPeerInvalidArgument;
    if (preflight_token(owner, token)) |result| return result;
    const output: *usize = @ptrFromInt(length_address);
    output.* = owner.peer.pending_bytes();
    return c_t.InteropPeerSuccess;
}

/// Dispatch one pending complete frame, copying only frames surfaced to the caller.
/// @param[in,out] owner_address Raw serialized owner; @param[in] token/time captured inputs.
/// @param[out] bytes_address/capacity Caller-owned output; NULL allowed only at zero capacity.
/// @param[out] event_address Caller-owned disjoint event, unchanged on local error.
/// @return Fixed C result; short output leaves reader, clock, bytes and event unchanged.
/// @ownership Copies full selected frame; no borrowed peer storage escapes.
/// @thread_safety Caller serializes dispatch with all peer operations.
export fn interop_peer_next_event(owner_address: usize, token: u64, now_ms: u64, bytes_address: usize, capacity: usize, event_address: usize) c_int {
    const owner = resolve_owner(owner_address) orelse return c_t.InteropPeerInvalidArgument;
    if (!valid_byte_range(bytes_address, capacity) or
        !valid_output(owner, event_address, @sizeOf(c_t.struct_interop_peer_event_t), @alignOf(c_t.struct_interop_peer_event_t)) or
        owner_overlaps(owner, bytes_address, capacity) or
        !valid_disjoint_ranges(bytes_address, capacity, event_address, @sizeOf(c_t.struct_interop_peer_event_t))) return c_t.InteropPeerInvalidArgument;
    if (preflight_token(owner, token)) |result| return result;
    if (preflight_time(owner, now_ms)) |result| return result;
    if (owner.peer.phase() == .Closed) return c_t.InteropPeerWrongPhase;
    const result = owner.peer.next_event(token, now_ms, byte_slice(bytes_address, capacity)) catch |err| switch (err) {
        error.OutputTooSmall => return c_t.InteropPeerOutputTooSmall,
        else => return c_t.InteropPeerInternalInvariant,
    };
    const event_output: *c_t.struct_interop_peer_event_t = @ptrFromInt(event_address);
    event_output.* = copy_event(owner, result.event);
    return c_t.InteropPeerSuccess;
}

/// Apply the configured role's external mapping initialization or validation result.
/// @param[in,out] owner_address Raw owner; @param[in] token/time/epoch/version/success scalars.
/// @param[out] event_address Aligned caller-owned event, unchanged on API error.
/// @return Fixed result; host failed proof drains, guest false result maps MappingFailed.
/// @ownership No mapping pointer is accepted or retained.
/// @thread_safety Caller serializes external mapping callback with peer operations.
export fn interop_peer_mapping_result(owner_address: usize, token: u64, now_ms: u64, epoch: u64, layout_version: u32, succeeded: u32, event_address: usize) c_int {
    const owner = resolve_owner(owner_address) orelse return c_t.InteropPeerInvalidArgument;
    if (!valid_output(owner, event_address, @sizeOf(c_t.struct_interop_peer_event_t), @alignOf(c_t.struct_interop_peer_event_t)) or succeeded > 1) return c_t.InteropPeerInvalidArgument;
    if (owner.peer.role == .Guest and layout_version != c_t.InteropLayoutVersion) return c_t.InteropPeerInvalidArgument;
    if (preflight_token(owner, token)) |result| return result;
    if (preflight_time(owner, now_ms)) |result| return result;
    if (owner.peer.role == .Host) {
        if (owner.peer.phase() != .AwaitMapping) return c_t.InteropPeerWrongPhase;
    } else {
        if (owner.peer.phase() != .InitializingMapping) return c_t.InteropPeerWrongPhase;
        if (succeeded == 0) return c_t.InteropPeerMappingFailed;
        if (epoch == 0) return c_t.InteropPeerInvalidArgument;
    }
    const result = if (owner.peer.role == .Host)
        owner.peer.host_mapping_validated(token, now_ms, epoch, layout_version, succeeded == 1)
    else
        owner.peer.guest_mapping_initialized(token, now_ms, epoch, succeeded == 1);
    const event = result catch return c_t.InteropPeerInternalInvariant;
    const output: *c_t.struct_interop_peer_event_t = @ptrFromInt(event_address);
    output.* = copy_event(owner, event);
    return c_t.InteropPeerSuccess;
}

/// Query the number of currently offered TX bytes without exposing a borrowed pointer.
/// @param[in] owner_address Raw opaque owner; @param[in] token captured connection token.
/// @param[out] length_address Aligned caller-owned length, unchanged on error.
/// @return Fixed C result; length is zero for empty or abort-required TX.
/// @ownership Scalar copy only.
/// @thread_safety Serialize with copy_tx and acknowledgements.
export fn interop_peer_tx_bytes(owner_address: usize, token: u64, length_address: usize) c_int {
    const owner = resolve_owner(owner_address) orelse return c_t.InteropPeerInvalidArgument;
    if (!valid_output(owner, length_address, @sizeOf(usize), @alignOf(usize))) return c_t.InteropPeerInvalidArgument;
    if (preflight_token(owner, token)) |result| return result;
    if (owner.peer.phase() == .Closed) return c_t.InteropPeerWrongPhase;
    const bytes = owner.peer.peek_tx(token) catch return c_t.InteropPeerInternalInvariant;
    const output: *usize = @ptrFromInt(length_address);
    output.* = if (bytes) |value| value.len else 0;
    return c_t.InteropPeerSuccess;
}

/// Copy the current front TX remainder; this operation never acknowledges a send.
/// @param[in] owner_address Raw opaque owner; @param[in] token captured connection token.
/// @param[out] bytes_address/capacity Caller-owned disjoint range; NULL only at zero capacity.
/// @param[out] length_address Aligned disjoint length, unchanged on error.
/// @return Fixed result; a short output writes neither bytes nor length.
/// @ownership Returned copy belongs wholly to the caller.
/// @thread_safety Serialize with send-copy and ACK operations.
export fn interop_peer_copy_tx(owner_address: usize, token: u64, bytes_address: usize, capacity: usize, length_address: usize) c_int {
    const owner = resolve_owner(owner_address) orelse return c_t.InteropPeerInvalidArgument;
    if (!valid_byte_range(bytes_address, capacity) or
        !valid_output(owner, length_address, @sizeOf(usize), @alignOf(usize)) or
        owner_overlaps(owner, bytes_address, capacity) or
        !valid_disjoint_ranges(bytes_address, capacity, length_address, @sizeOf(usize))) return c_t.InteropPeerInvalidArgument;
    if (preflight_token(owner, token)) |result| return result;
    if (owner.peer.phase() == .Closed) return c_t.InteropPeerWrongPhase;
    const bytes = owner.peer.peek_tx(token) catch return c_t.InteropPeerInternalInvariant;
    if (bytes) |value| {
        if (capacity < value.len) return c_t.InteropPeerOutputTooSmall;
        if (value.len != 0) @memcpy(byte_slice(bytes_address, value.len), value);
        const output: *usize = @ptrFromInt(length_address);
        output.* = value.len;
    } else {
        const output: *usize = @ptrFromInt(length_address);
        output.* = 0;
    }
    return c_t.InteropPeerSuccess;
}

/// Acknowledge exactly the successful OS send count and copy the resulting event.
/// @param[in,out] owner_address Raw owner; @param[in] token/time/count actual callback values.
/// @param[out] event_address Aligned event unchanged on API error.
/// @return Fixed result; OS-accepted bytes remain accounted at deadline/drain.
/// @ownership Queue retains or frees its own frame allocation as ACK advances.
/// @thread_safety Caller serializes with TX copies and all peer calls.
export fn interop_peer_ack_tx(owner_address: usize, token: u64, now_ms: u64, written: usize, event_address: usize) c_int {
    const owner = resolve_owner(owner_address) orelse return c_t.InteropPeerInvalidArgument;
    if (!valid_output(owner, event_address, @sizeOf(c_t.struct_interop_peer_event_t), @alignOf(c_t.struct_interop_peer_event_t))) return c_t.InteropPeerInvalidArgument;
    if (preflight_token(owner, token)) |result| return result;
    if (preflight_time(owner, now_ms)) |result| return result;
    if (owner.peer.phase() == .Closed) return c_t.InteropPeerWrongPhase;
    const front = owner.peer.queue.peek() catch |err| switch (err) {
        error.Empty => return c_t.InteropPeerNoQueuedFrame,
        error.QueueClosed => return c_t.InteropPeerInternalInvariant,
    };
    if (written > front.len) return c_t.InteropPeerInvalidWriteCount;
    const event = owner.peer.ack_tx(token, now_ms, written) catch return c_t.InteropPeerInternalInvariant;
    const output: *c_t.struct_interop_peer_event_t = @ptrFromInt(event_address);
    output.* = copy_event(owner, event);
    return c_t.InteropPeerSuccess;
}

/// Advance handshake/liveness timers and copy phase/flags event.
/// @param[in,out] owner_address Raw peer; @param[in] token/time timer callback values.
/// @param[out] event_address Aligned event unchanged on API error.
/// @return Fixed result; timeout and mandatory-send failure are successful Draining events.
/// @ownership No timer or native resource is owned.
/// @thread_safety Caller serializes timer with peer events.
export fn interop_peer_tick(owner_address: usize, token: u64, now_ms: u64, event_address: usize) c_int {
    const owner = resolve_owner(owner_address) orelse return c_t.InteropPeerInvalidArgument;
    if (!valid_output(owner, event_address, @sizeOf(c_t.struct_interop_peer_event_t), @alignOf(c_t.struct_interop_peer_event_t))) return c_t.InteropPeerInvalidArgument;
    if (preflight_token(owner, token)) |result| return result;
    if (preflight_time(owner, now_ms)) |result| return result;
    const event = owner.peer.tick(token, now_ms) catch return c_t.InteropPeerInternalInvariant;
    const output: *c_t.struct_interop_peer_event_t = @ptrFromInt(event_address);
    output.* = copy_event(owner, event);
    return c_t.InteropPeerSuccess;
}

/// Begin local protocol drain and report persistent terminal flags.
/// @param[in,out] owner_address Raw peer; @param[in] token/time local stop callback.
/// @param[out] event_address Aligned event unchanged on API error.
/// @return Fixed C result; no mapping/detach proof is implied.
/// @ownership Peer retains any queued terminal TX until ACK or destroy.
/// @thread_safety Caller serializes with all peer operations.
export fn interop_peer_begin_drain(owner_address: usize, token: u64, now_ms: u64, event_address: usize) c_int {
    const owner = resolve_owner(owner_address) orelse return c_t.InteropPeerInvalidArgument;
    if (!valid_output(owner, event_address, @sizeOf(c_t.struct_interop_peer_event_t), @alignOf(c_t.struct_interop_peer_event_t))) return c_t.InteropPeerInvalidArgument;
    if (preflight_token(owner, token)) |result| return result;
    if (preflight_time(owner, now_ms)) |result| return result;
    const event = owner.peer.begin_drain(token, now_ms) catch return c_t.InteropPeerInternalInvariant;
    const output: *c_t.struct_interop_peer_event_t = @ptrFromInt(event_address);
    output.* = copy_event(owner, event);
    return c_t.InteropPeerSuccess;
}

/// Mark transport Closed after caller-confirmed callback quiescence.
/// @param[in,out] owner_address Raw peer; @param[in] token captured connection token.
/// @param[out] event_address Aligned event unchanged on API error.
/// @return Fixed result; peer-owned allocations remain until destroy.
/// @ownership Does not close native handles, unmap memory, or establish proof.
/// @thread_safety Caller guarantees native I/O callbacks are quiesced.
export fn interop_peer_transport_lost(owner_address: usize, token: u64, event_address: usize) c_int {
    const owner = resolve_owner(owner_address) orelse return c_t.InteropPeerInvalidArgument;
    if (!valid_output(owner, event_address, @sizeOf(c_t.struct_interop_peer_event_t), @alignOf(c_t.struct_interop_peer_event_t))) return c_t.InteropPeerInvalidArgument;
    if (preflight_token(owner, token)) |result| return result;
    const event = owner.peer.transport_lost(token) catch return c_t.InteropPeerInternalInvariant;
    const output: *c_t.struct_interop_peer_event_t = @ptrFromInt(event_address);
    output.* = copy_event(owner, event);
    return c_t.InteropPeerSuccess;
}

// Test-only fixtures.
test "raw pointer guards reject null alignment wrap and output alias before dereference" {
    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, interop_peer_create(0, 8));
    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, interop_peer_create(8, 0));
    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, interop_peer_create(std_t.math.maxInt(usize) - 7, 16));
    try std_t.testing.expect(!valid_range(3, @sizeOf(usize), @alignOf(usize)));
    try std_t.testing.expect(!valid_range(std_t.math.maxInt(usize) - 1, 8, 1));
    try std_t.testing.expect(ranges_overlap(100, 8, 107, 2));
    try std_t.testing.expect(!ranges_overlap(100, 8, 108, 2));
    try std_t.testing.expect(!ranges_overlap(110, 2, 100, 8));
}

test "ABI rejects overflowing input and misaligned event output after prior guards pass" {
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 0x7711,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    const before = event;
    const owner_address = @intFromPtr(owner);
    var overflow_value: usize = std_t.math.maxInt(usize) - 3;
    var misaligned_value: usize = @intFromPtr(&event) + 1;
    const overflow_address: *volatile usize = &overflow_value;
    const misaligned_event_address: *volatile usize = &misaligned_value;

    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, @call(.never_inline, interop_peer_next_event, .{
        owner_address, 0x7711, 0, overflow_address.*, 8, @intFromPtr(&event),
    }));
    try std_t.testing.expectEqual(before, event);
    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, @call(.never_inline, interop_peer_tick, .{
        owner_address, 0x7711, 0, misaligned_event_address.*,
    }));
    try std_t.testing.expectEqual(before, event);
}

test "ABI operations preserve outputs for wrong tokens after pointer validation" {
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 0x7722,
        .accepted_at_ms = 10,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    const event_before = event;
    var length: usize = 0xA5A5;
    var token_value: u64 = 0x7723;
    const token_pointer: *volatile u64 = &token_value;

    try std_t.testing.expectEqual(c_t.InteropPeerWrongToken, @call(.never_inline, interop_peer_next_event, .{
        @intFromPtr(owner), token_pointer.*, 10, 0, 0, @intFromPtr(&event),
    }));
    try std_t.testing.expectEqual(event_before, event);
    try std_t.testing.expectEqual(c_t.InteropPeerWrongToken, @call(.never_inline, interop_peer_tick, .{
        @intFromPtr(owner), token_pointer.*, 10, @intFromPtr(&event),
    }));
    try std_t.testing.expectEqual(event_before, event);
    try std_t.testing.expectEqual(c_t.InteropPeerWrongToken, @call(.never_inline, interop_peer_tx_bytes, .{
        @intFromPtr(owner), token_pointer.*, @intFromPtr(&length),
    }));
    try std_t.testing.expectEqual(@as(usize, 0xA5A5), length);
}

test "fixed ABI enum mappings and creation-only error classification" {
    const phases = [_]peer_mod_t.phase_t{
        .AwaitHello,     .AwaitMapping,  .AwaitHelloAckSent, .InitializingMapping,
        .AwaitHelloSent, .AwaitHelloAck, .Ready,             .Draining,
        .Closed,
    };
    const phase_values = [_]u32{
        c_t.InteropPeerAwaitHello,          c_t.InteropPeerAwaitMapping,   c_t.InteropPeerAwaitHelloAckSent,
        c_t.InteropPeerInitializingMapping, c_t.InteropPeerAwaitHelloSent, c_t.InteropPeerAwaitHelloAck,
        c_t.InteropPeerReady,               c_t.InteropPeerDraining,       c_t.InteropPeerTransportClosed,
    };
    for (phases, phase_values) |phase, value| try std_t.testing.expectEqual(value, map_phase(phase));

    const kinds = [_]peer_mod_t.event_kind_t{ .None, .ValidateMapping, .Ready, .FeatureFrame, .PeerError, .Draining };
    const kind_values = [_]u32{
        c_t.InteropPeerEventNone,         c_t.InteropPeerEventValidateMapping, c_t.InteropPeerEventReady,
        c_t.InteropPeerEventFeatureFrame, c_t.InteropPeerEventError,           c_t.InteropPeerEventDraining,
    };
    for (kinds, kind_values) |kind, value| try std_t.testing.expectEqual(value, @call(.never_inline, map_event_kind, .{kind}));

    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, map_create_error(error.InvalidVersion));
    try std_t.testing.expectEqual(c_t.InteropPeerOutOfMemory, map_create_error(error.OutOfMemory));
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, map_create_error(error.FutureConstructorFailure));
}

test "range and owner overlap helpers cover empty and nonempty owned storage" {
    try std_t.testing.expect(!valid_range(0, 8, 8));
    try std_t.testing.expect(!valid_range(8, 0, 8));
    try std_t.testing.expect(!valid_range(8, 8, 0));
    try std_t.testing.expect(valid_byte_range(0, 0));
    try std_t.testing.expect(!valid_byte_range(0, 1));
    try std_t.testing.expect(valid_byte_range(8, 1));
    try std_t.testing.expect(valid_disjoint_ranges(8, 4, 12, 4));
    try std_t.testing.expect(!valid_disjoint_ranges(8, 4, 11, 1));

    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 12,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    try std_t.testing.expect(!owner_overlaps(owner, 1, 0));
    try std_t.testing.expect(!owner_overlaps(owner, 1, 1));
    try std_t.testing.expect(!valid_output(owner, 0, 8, 8));
    try std_t.testing.expect(valid_output(owner, 8, 8, 8));
    try std_t.testing.expect(!valid_output(owner, @intFromPtr(owner.peer.local_version.ptr), 1, 1));
}

test "ABI localizes scalar internal faults without corrupting owned buffers" {
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 13,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    const owner_address = @intFromPtr(owner);
    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    var before = event;
    var length: usize = 0xA5A5;
    var byte: u8 = 0;
    var consumed: usize = 0x5A5A;

    // Force only the reader's sticky scalar failure state; its backing bytes
    // and lengths remain valid and are still released by normal deinit.
    owner.peer.reader.failed = true;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_feed(
        owner_address,
        13,
        @intFromPtr(&byte),
        1,
        @intFromPtr(&consumed),
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(@as(usize, 0x5A5A), consumed);
    try std_t.testing.expectEqual(before, event);
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, @call(.never_inline, interop_peer_next_event, .{
        owner_address, 13, 0, 0, 0, @intFromPtr(&event),
    }));
    try std_t.testing.expectEqual(before, event);

    // A closed queue is a scalar API fault with live allocations. Peek/copy
    // must report the normalized peer-closed result without changing outputs.
    owner.peer.reader.failed = false;
    try owner.peer.reader.finish_input();
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_feed(
        owner_address,
        13,
        @intFromPtr(&byte),
        1,
        @intFromPtr(&consumed),
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(@as(usize, 0x5A5A), consumed);
    try std_t.testing.expectEqual(before, event);
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_next_event(
        owner_address,
        13,
        0,
        0,
        0,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
    owner.peer.reader.input_closed = false;
    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, interop_peer_feed(
        owner_address,
        13,
        @intFromPtr(owner.peer.reader.buffer[0..].ptr),
        1,
        @intFromPtr(&consumed),
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(@as(usize, 0x5A5A), consumed);
    try std_t.testing.expectEqual(before, event);

    owner.peer.queue.live = false;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, @call(.never_inline, interop_peer_tx_bytes, .{
        owner_address, 13, @intFromPtr(&length),
    }));
    try std_t.testing.expectEqual(@as(usize, 0xA5A5), length);
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, @call(.never_inline, interop_peer_copy_tx, .{
        owner_address, 13, @intFromPtr(&byte), 1, @intFromPtr(&length),
    }));
    try std_t.testing.expectEqual(@as(usize, 0xA5A5), length);
    owner.peer.queue.live = true;

    // A missing Ready liveness value is an owned-state invariant failure;
    // timer output remains untouched and allocations retain their owner.
    try std_t.testing.expectEqual(c_t.InteropPeerSuccess, interop_peer_mapping_result(
        owner_address,
        13,
        1,
        1,
        c_t.InteropLayoutVersion,
        1,
        @intFromPtr(&event),
    ));
    owner.peer.connection_phase = .Ready;
    owner.peer.liveness = null;
    before = event;
    const outstanding = owner.peer.queue.peek() catch unreachable;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_ack_tx(
        owner_address,
        13,
        1,
        outstanding.len,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_tick(
        owner_address,
        13,
        1,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
}

test "ABI handshake callback guards normalize impossible scalar phase faults" {
    var guest = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 14,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(guest);
    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    var event_before = event;
    try std_t.testing.expectEqual(c_t.InteropPeerSuccess, interop_peer_mapping_result(
        @intFromPtr(guest),
        14,
        1,
        3,
        c_t.InteropLayoutVersion,
        1,
        @intFromPtr(&event),
    ));
    event_before = event;
    const hello = guest.peer.queue.peek() catch unreachable;
    guest.peer.handshake.phase = .Ready;
    const ack_result = interop_peer_ack_tx(
        @intFromPtr(guest),
        14,
        1,
        hello.len,
        @intFromPtr(&event),
    );
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, ack_result);
    try std_t.testing.expectEqual(event_before, event);

    var host = try create_owner(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 15,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(host);
    const guest_sender = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 16,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(guest_sender);
    try std_t.testing.expectEqual(c_t.InteropPeerSuccess, interop_peer_mapping_result(
        @intFromPtr(guest_sender),
        16,
        1,
        4,
        c_t.InteropLayoutVersion,
        1,
        @intFromPtr(&event),
    ));
    var hello_bytes: [256]u8 = undefined;
    var hello_length: usize = 0;
    try std_t.testing.expectEqual(c_t.InteropPeerSuccess, interop_peer_copy_tx(
        @intFromPtr(guest_sender),
        16,
        @intFromPtr(&hello_bytes),
        hello_bytes.len,
        @intFromPtr(&hello_length),
    ));
    var consumed: usize = 0;
    try std_t.testing.expectEqual(c_t.InteropPeerSuccess, interop_peer_feed(
        @intFromPtr(host),
        15,
        @intFromPtr(&hello_bytes),
        hello_length,
        @intFromPtr(&consumed),
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(hello_length, consumed);
    host.peer.handshake.phase = .Closed;
    event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    event_before = event;
    const next_result = interop_peer_next_event(
        @intFromPtr(host),
        15,
        1,
        0,
        0,
        @intFromPtr(&event),
    );
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, next_result);
    try std_t.testing.expectEqual(event_before, event);

    var callback_guest = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 17,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(callback_guest);
    callback_guest.peer.connection_phase = .InitializingMapping;
    callback_guest.peer.handshake.phase = .Ready;
    event_before = event;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_mapping_result(
        @intFromPtr(callback_guest),
        17,
        1,
        5,
        c_t.InteropLayoutVersion,
        1,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(event_before, event);
}

test "ABI Ready dispatch normalizes liveness callback faults" {
    const liveness_mod_t = @import("session_liveness.zig");
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 18,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    owner.peer.connection_phase = .Ready;
    owner.peer.handshake.phase = .Ready;
    owner.peer.liveness = try liveness_mod_t.session_liveness_t.init(.Guest, 18, 0);

    var frame: [c_t.InteropHeaderBytes]u8 = undefined;
    var writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&writer, &frame, frame.len);
    var frame_length: usize = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_writer_finish(
        &writer,
        c_t.InteropMsgPing,
        1,
        c_t.InteropFromHost,
        &frame_length,
    ));
    _ = try owner.peer.reader.feed(frame[0..frame_length]);

    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    const before = event;
    owner.peer.liveness.?.connection_token = 99;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_next_event(
        @intFromPtr(owner),
        18,
        1,
        0,
        0,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);

    owner.peer.liveness.?.connection_token = 18;
    owner.peer.liveness.?.last_event_ms = 2;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_next_event(
        @intFromPtr(owner),
        18,
        1,
        0,
        0,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
}

test "owner creation failure sweep releases every staged allocation" {
    for (0..3) |fail_index| {
        var failing = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = fail_index });
        const made = create_owner(failing.allocator(), .{
            .role = .Guest,
            .connection_token = 9,
            .accepted_at_ms = 0,
            .local_version = "1.2.3",
            .local_capabilities = 0,
        });
        if (fail_index < 2) {
            try std_t.testing.expectError(error.OutOfMemory, made);
        } else {
            const owner = try made;
            destroy_owner(owner);
        }
        try std_t.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
}

test "owned-range validation includes peer value version and full queued frame" {
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 10,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    try std_t.testing.expect(owner_overlaps(owner, @intFromPtr(owner), 1));
    try std_t.testing.expect(owner_overlaps(owner, @intFromPtr(owner.peer.local_version.ptr), owner.peer.local_version.len));
    _ = try owner.peer.guest_mapping_initialized(10, 1, 5, true);
    const record = owner.peer.queue.frames[owner.peer.queue.head].?;
    try owner.peer.queue.consume_written(1);
    try std_t.testing.expect(owner_overlaps(owner, @intFromPtr(record.bytes.ptr), record.bytes.len));
}

test "destroy rejects an owner slot inside peer-owned version storage" {
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 11,
        .accepted_at_ms = 0,
        .local_version = "1.2.3+abcdefghijklmnop",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    const version_address = @intFromPtr(owner.peer.local_version.ptr);
    const slot_address = (version_address + @alignOf(?*anyopaque) - 1) & ~(@as(usize, @alignOf(?*anyopaque)) - 1);
    const slot_offset = slot_address - version_address;
    try std_t.testing.expect(slot_offset + @sizeOf(?*anyopaque) <= owner.peer.local_version.len);

    var original_version: [22]u8 = undefined;
    @memcpy(&original_version, owner.peer.local_version);
    const overlapping_slot: *?*anyopaque = @ptrFromInt(slot_address);
    overlapping_slot.* = @ptrCast(owner);

    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, interop_peer_destroy(slot_address));

    @memcpy(owner.peer.local_version, &original_version);
}

test "ABI config and output range overlaps are rejected before access" {
    var config = c_t.struct_interop_peer_config_t{
        .role = c_t.InteropPeerGuest,
        .reserved_zero = 0,
        .connection_token = 41,
        .accepted_at_ms = 0,
        .agent_version = undefined,
        .agent_version_bytes = 5,
        .local_capabilities = 0,
    };
    var owner_slot: ?*anyopaque = null;

    // The length is valid, but the borrowed version bytes begin inside config.
    // read_config must detect this overlap before reading the version contents.
    config.agent_version = @ptrCast(&config);
    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, interop_peer_create(
        @intFromPtr(&config),
        @intFromPtr(&owner_slot),
    ));
    try std_t.testing.expect(owner_slot == null);

    // A separate aligned version range may not overlap the output owner slot.
    var overlapping_storage: [32]u8 align(8) = [_]u8{0xA7} ** 32;
    config.agent_version = &overlapping_storage;
    config.agent_version_bytes = overlapping_storage.len;
    const original_storage = overlapping_storage;
    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, interop_peer_create(
        @intFromPtr(&config),
        @intFromPtr(&overlapping_storage),
    ));
    try std_t.testing.expectEqual(original_storage, overlapping_storage);
}

test "ABI rejects input and output ranges inside full peer-owned buffers" {
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 42,
        .accepted_at_ms = 0,
        .local_version = "1.2.3+abcdefghijklmnopq",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);

    const owner_address = @intFromPtr(owner);
    var consumed: usize = 0x1234;
    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    const before = event;
    const version_address = @intFromPtr(owner.peer.local_version.ptr);
    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, interop_peer_feed(
        owner_address,
        42,
        version_address,
        1,
        @intFromPtr(&consumed),
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(@as(usize, 0x1234), consumed);
    try std_t.testing.expectEqual(before, event);

    // The rounded address is suitably aligned and the requested output range is
    // valid; overlap with the owned version is the rejection cause.
    const aligned_version = (version_address + @alignOf(c_t.struct_interop_peer_event_t) - 1) &
        ~(@as(usize, @alignOf(c_t.struct_interop_peer_event_t)) - 1);
    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, interop_peer_get_state(
        owner_address,
        42,
        aligned_version,
    ));

    _ = try owner.peer.guest_mapping_initialized(42, 1, 2, true);
    const queued_record = owner.peer.queue.frames[owner.peer.queue.head].?;
    const queued_address = @intFromPtr(queued_record.bytes.ptr);
    try std_t.testing.expectEqual(c_t.InteropPeerInvalidArgument, interop_peer_feed(
        owner_address,
        42,
        queued_address,
        1,
        @intFromPtr(&consumed),
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(@as(usize, 0x1234), consumed);
    try std_t.testing.expectEqual(before, event);
}

test "ABI timer and dispatch contain liveness and retirement invariant faults" {
    const liveness_mod_t = @import("session_liveness.zig");
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 43,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    owner.peer.connection_phase = .Ready;
    owner.peer.handshake.phase = .Ready;
    owner.peer.liveness = try liveness_mod_t.session_liveness_t.init(.Host, 43, 0);

    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    const before = event;
    owner.peer.liveness.?.closed = true;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_tick(
        @intFromPtr(owner),
        43,
        1,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);

    // A well-formed inbound Ping reaches the helper role guard. The mismatch is
    // injected in a scalar test state only; the opaque ABI normalizes it.
    owner.peer.liveness = try liveness_mod_t.session_liveness_t.init(.Host, 43, 0);
    var frame: [c_t.InteropHeaderBytes]u8 = undefined;
    var writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&writer, &frame, frame.len);
    var frame_length: usize = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_writer_finish(
        &writer,
        c_t.InteropMsgPing,
        7,
        c_t.InteropFromHost,
        &frame_length,
    ));
    _ = try owner.peer.reader.feed(frame[0..frame_length]);
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_next_event(
        @intFromPtr(owner),
        43,
        1,
        0,
        0,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);

    // A syntactically valid Pong can match a live helper record whose ID is
    // absent from the peer's shared allocator. The release inconsistency is
    // reported as InternalInvariant without writing the caller's event.
    try owner.peer.reader.consume();
    const reader_mod_t = @import("stream_reader.zig");
    owner.peer.role = .Host;
    owner.peer.reader = try reader_mod_t.reader_t.init(c_t.InteropFromGuest);
    owner.peer.liveness = try liveness_mod_t.session_liveness_t.init(.Host, 43, 0);
    owner.peer.liveness.?.pings[0] = .{ .request_id = 19, .issued_at_ms = 0 };
    owner.peer.request_ids.retire_all();
    c_t.interop_writer_init(&writer, &frame, frame.len);
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_writer_finish(
        &writer,
        c_t.InteropMsgPong,
        19,
        c_t.InteropFromGuest,
        &frame_length,
    ));
    _ = try owner.peer.reader.feed(frame[0..frame_length]);
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_next_event(
        @intFromPtr(owner),
        43,
        1,
        0,
        0,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
}

test "ABI drain callback contains stale helper token without output mutation" {
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 44,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    const before = event;
    owner.peer.handshake.connection_token = 99;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_begin_drain(
        @intFromPtr(owner),
        44,
        1,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
}

test "ABI acknowledges OS bytes before containing a late liveness callback fault" {
    const liveness_mod_t = @import("session_liveness.zig");
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 45,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    owner.peer.connection_phase = .Ready;
    owner.peer.handshake.phase = .Ready;
    owner.peer.liveness = try liveness_mod_t.session_liveness_t.init(.Host, 45, 0);
    _ = try owner.peer.tick(45, liveness_mod_t.PingIntervalMs);
    const queued = try owner.peer.queue.peek();
    const written = queued.len;

    // ACK bytes represent a completed OS write. A subsequently failing scalar
    // liveness check must be normalized without exposing a partially written
    // C event, while deinit still owns all remaining allocations.
    owner.peer.liveness.?.closed = true;
    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    const before = event;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_ack_tx(
        @intFromPtr(owner),
        45,
        liveness_mod_t.PingIntervalMs,
        written,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
    try std_t.testing.expectError(error.Empty, owner.peer.queue.peek());
}

test "ABI transport loss reports a closed owner without touching output" {
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 46,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    owner.peer.live = false;
    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    const before = event;
    try std_t.testing.expectEqual(c_t.InteropPeerClosed, interop_peer_transport_lost(
        @intFromPtr(owner),
        46,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
    owner.peer.live = true;
}

test "ABI catches queue and helper lifecycle failures at method boundaries" {
    const liveness_mod_t = @import("session_liveness.zig");
    const guest = try create_owner(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 47,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(guest);
    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    const before = event;

    guest.peer.live = false;
    try std_t.testing.expectEqual(c_t.InteropPeerClosed, interop_peer_tick(
        @intFromPtr(guest),
        47,
        1,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
    guest.peer.live = true;

    guest.peer.handshake.connection_token = 99;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_tick(
        @intFromPtr(guest),
        47,
        1,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
    guest.peer.handshake.connection_token = 47;

    _ = try guest.peer.guest_mapping_initialized(47, 1, 2, true);
    const record_index = guest.peer.send_head;
    const original_record = guest.peer.send_records[record_index];
    guest.peer.send_records[record_index].kind = .None;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_ack_tx(
        @intFromPtr(guest),
        47,
        2,
        0,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
    guest.peer.send_records[record_index] = original_record;

    guest.peer.queue.live = false;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_ack_tx(
        @intFromPtr(guest),
        47,
        2,
        0,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
    guest.peer.queue.live = true;

    const host = try create_owner(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 48,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(host);
    host.peer.connection_phase = .Ready;
    host.peer.handshake.phase = .Ready;
    host.peer.liveness = try liveness_mod_t.session_liveness_t.init(.Host, 48, 0);
    host.peer.liveness.?.last_valid_rx_ms = liveness_mod_t.LivenessTimeoutMs + 1;
    host.peer.liveness.?.pings[0] = .{ .request_id = 29, .issued_at_ms = 0 };
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_tick(
        @intFromPtr(host),
        48,
        liveness_mod_t.LivenessTimeoutMs + 1,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
}

test "ABI returns a valid scoped PeerError as copied event data" {
    const liveness_mod_t = @import("session_liveness.zig");
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 49,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    owner.peer.connection_phase = .Ready;
    owner.peer.handshake.phase = .Ready;
    owner.peer.liveness = try liveness_mod_t.session_liveness_t.init(.Host, 49, 0);

    var frame: [128]u8 = undefined;
    var writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&writer, &frame, frame.len);
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_put_uint(
        &writer,
        c_t.InteropTagErrorCode,
        c_t.InteropErrorProtocolViolation,
    ));
    var frame_length: usize = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_writer_finish(
        &writer,
        c_t.InteropMsgError,
        71,
        c_t.InteropFromGuest,
        &frame_length,
    ));

    var consumed: usize = 0;
    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    try std_t.testing.expectEqual(c_t.InteropPeerSuccess, interop_peer_feed(
        @intFromPtr(owner),
        49,
        @intFromPtr(&frame),
        frame_length,
        @intFromPtr(&consumed),
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(frame_length, consumed);
    var output: [128]u8 = undefined;
    try std_t.testing.expectEqual(c_t.InteropPeerSuccess, interop_peer_next_event(
        @intFromPtr(owner),
        49,
        1,
        @intFromPtr(&output),
        output.len,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(@as(u32, @intCast(c_t.InteropPeerEventError)), event.kind);
    try std_t.testing.expectEqual(@as(u32, @intCast(c_t.InteropPeerReady)), event.phase);
    try std_t.testing.expectEqual(@as(u16, @intCast(c_t.InteropMsgError)), event.message_type);
    try std_t.testing.expectEqual(@as(u32, 71), event.request_id);
    try std_t.testing.expectEqual(@as(u32, @intCast(c_t.InteropErrorProtocolViolation)), event.error_code);
    try std_t.testing.expectEqual(frame_length, event.copied_bytes);
    try std_t.testing.expectEqualSlices(u8, frame[0..frame_length], output[0..frame_length]);
}

test "ABI Ready dispatch contains a closed liveness observer" {
    const liveness_mod_t = @import("session_liveness.zig");
    const owner = try create_owner(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 50,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer destroy_owner(owner);
    owner.peer.connection_phase = .Ready;
    owner.peer.handshake.phase = .Ready;
    owner.peer.liveness = try liveness_mod_t.session_liveness_t.init(.Host, 50, 0);
    owner.peer.liveness.?.closed = true;

    var frame: [c_t.InteropHeaderBytes]u8 = undefined;
    var writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&writer, &frame, frame.len);
    var frame_length: usize = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_writer_finish(
        &writer,
        c_t.InteropMsgCatalogBegin,
        0,
        c_t.InteropFromGuest,
        &frame_length,
    ));
    _ = try owner.peer.reader.feed(frame[0..frame_length]);

    var event = std_t.mem.zeroes(c_t.struct_interop_peer_event_t);
    const before = event;
    var output: [c_t.InteropHeaderBytes]u8 = [_]u8{0xA5} ** c_t.InteropHeaderBytes;
    try std_t.testing.expectEqual(c_t.InteropPeerInternalInvariant, interop_peer_next_event(
        @intFromPtr(owner),
        50,
        1,
        @intFromPtr(&output),
        output.len,
        @intFromPtr(&event),
    ));
    try std_t.testing.expectEqual(before, event);
    try std_t.testing.expectEqual([_]u8{0xA5} ** c_t.InteropHeaderBytes, output);
}

// The C fixture is compiled with main renamed only for test/coverage linkage.
extern fn interop_peer_c_fixture_main() c_int;
test "independent C client crosses the opaque pointer ABI" {
    try std_t.testing.expectEqual(@as(c_int, 0), interop_peer_c_fixture_main());
}

test "runtime half open address intervals distinguish adjacency from intersection" {
    // Scalar interval arithmetic only; these values are never dereferenced.
    var values = [_]usize{ 108, 4, 100, 12 };
    const runtime_values: *volatile [4]usize = &values;
    const intersecting = runtime_values.*;
    try std_t.testing.expect(@call(.never_inline, ranges_overlap, .{
        intersecting[0], intersecting[1], intersecting[2], intersecting[3],
    }));
    values[0] = 112;
    const adjacent = runtime_values.*;
    try std_t.testing.expect(!@call(.never_inline, ranges_overlap, .{
        adjacent[0], adjacent[1], adjacent[2], adjacent[3],
    }));
}
