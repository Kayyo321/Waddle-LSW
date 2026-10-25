//! Serialized protocol-v2 connected-peer composition for control and frame dispatch.
const std_t = @import("std");
const c_t = @cImport({
    @cInclude("waddle/interop_protocol.h");
});
const reader_mod_t = @import("stream_reader.zig");
const queue_mod_t = @import("stream_send_queue.zig");
const handshake_mod_t = @import("session_handshake.zig");
const liveness_mod_t = @import("session_liveness.zig");
const request_ids_mod_t = @import("request_ids.zig");
const agent_version_t = @import("agent_version.zig");

/// Maximum original frame bytes stored in internal completion metadata.
pub const QueueCapacity: usize = queue_mod_t.QueueCapacity;
/// Maximum configured full agent-version bytes copied by init.
pub const MaxVersionBytes: usize = 32;
/// Public endpoint role alias from the frozen handshake helper.
pub const role_t = handshake_mod_t.role_t;
/// Public lifecycle phase alias from the frozen handshake helper.
pub const phase_t = handshake_mod_t.phase_t;
/// Minimum configured full agent-version bytes accepted by init.
pub const MinVersionBytes: usize = 5;

/// Scalar event kinds emitted to the serialized host/guest event loop.
pub const event_kind_t = enum {
    /// No complete incoming frame or timer action needs caller handling.
    None,
    /// Host must validate the external mapping before the peer may send Ack.
    ValidateMapping,
    /// Handshake reached Ready; this is connection negotiation only.
    Ready,
    /// A codec-valid feature frame was copied for external authorization/dispatch.
    FeatureFrame,
    /// A valid remote Error frame was copied as a scoped diagnostic.
    PeerError,
    /// Negotiation or transport entered terminal local Draining state.
    Draining,
};

/// Copied event data; `copied_bytes` refers only to the caller's output buffer.
/// @ownership Value-only result. No member borrows peer or reader storage.
/// @thread_safety Returned to the same serialized owner that called the peer.
pub const event_t = struct {
    /// Discriminator identifying which scalar fields are meaningful.
    kind: event_kind_t = .None,
    /// Current protocol-handshake phase after the operation.
    phase: handshake_mod_t.phase_t,
    /// Validated/staged mapping epoch for ValidateMapping or Ready.
    epoch: u64 = 0,
    /// Negotiated capability intersection for Ready.
    capabilities: u64 = 0,
    /// Layout version from a valid guest Hello awaiting mapping validation.
    layout_version: u32 = 0,
    /// Frozen message type of a copied feature/error/goodbye frame.
    message_type: u16 = 0,
    /// Exact header request ID of a copied feature/error/goodbye frame.
    request_id: u32 = 0,
    /// Frozen ErrorCode for a PeerError or fatal protocol result; zero otherwise.
    error_code: c_int = c_t.InteropErrorNone,
    /// Number of complete encoded frame bytes copied to caller output.
    copied_bytes: usize = 0,
    /// First recorded drain reason when kind is Draining.
    reason: ?handshake_mod_t.drain_reason_t = null,
    /// Caller must abort the transport instead of sending an unfinished Hello/Ack.
    abort_transport: bool = false,
};

/// Result from a feed attempt; framing failures are terminal, not local API errors.
/// @ownership Copied scalar fields only; input bytes are never retained.
/// @thread_safety Result is returned to the serialized peer owner.
pub const feed_result_t = struct {
    /// Exact prefix copied into the fixed-capacity reader.
    consumed: usize,
    /// Current handshake/connection phase after successful call.
    phase: handshake_mod_t.phase_t,
    /// Nonzero codec error from a terminal invalid header; null otherwise.
    terminal_error: ?c_int = null,
    /// True when best-effort terminal Error could not be queued.
    terminal_send_failed: bool = false,
    /// True when an unfinished Hello/Ack requires the caller to abort transport.
    abort_transport: bool = false,
};

/// Result from processing one pending complete frame.
/// @ownership Event contains scalar copies; application bytes are copied to caller output.
/// @thread_safety Returned to the serialized peer owner.
pub const event_result_t = struct {
    /// One copied event, or None if there is no complete pending frame.
    event: event_t,
    /// Full frame bytes required for copy-out; zero for internally handled control.
    required_bytes: usize = 0,
};

/// Initialization inputs for one connected control peer.
/// @ownership Version slice is borrowed only during init and copied before return.
/// @thread_safety Caller must not concurrently mutate the supplied version bytes.
pub const config_t = struct {
    /// Host or Guest endpoint role from the handshake contract.
    role: handshake_mod_t.role_t,
    /// Nonzero token binding all delayed work to this exact connection.
    connection_token: u64,
    /// Monotonic timestamp at transport acceptance.
    accepted_at_ms: u64,
    /// Full bounded SemVer2 string advertised on Hello/HelloAck.
    local_version: []const u8,
    /// Locally implemented known capability offer.
    local_capabilities: u64,
};

/// Errors returned by local peer operations. Caller-preflight and output-validation
/// errors preserve peer state; internal/helper failures may follow protocol or
/// confirmed-write accounting mutation and require the owner to abort/quiesce
/// before reuse. Copy-out buffers are written only on successful copy-out.
/// @ownership Compile-time error set; no runtime storage.
pub const control_peer_error_t = error{
    /// Init or method was given a zero connection token.
    InvalidConnectionToken,
    /// Method token does not identify this connection instance.
    WrongConnectionToken,
    /// Timestamp precedes the last committed event timestamp.
    TimeWentBackwards,
    /// Method is invalid for the current role or lifecycle phase.
    WrongPhase,
    /// Method was called after idempotent deinit.
    PeerClosed,
    /// Input is unreadable through the declared slice, aliases peer state, or violates API bounds.
    InvalidInput,
    /// Full output frame does not fit; state/output are unchanged.
    OutputTooSmall,
    /// Output overlaps peer-owned memory or pending queue storage.
    OutputAliasesPeer,
    /// No complete frame is currently pending when a required frame is queried.
    NoPendingFrame,
    /// No outbound frame exists when a send view is requested.
    NoQueuedFrame,
    /// Confirmed write count exceeds the front frame remainder.
    InvalidWriteCount,
    /// Fixed request-ID namespace has no available nonzero ID.
    RequestIdsFull,
    /// Codec writer failed to construct a frozen internal control frame.
    CodecFailure,
    /// Unexpected helper state indicates an internal composition inconsistency.
    InternalState,
};

/// Errors returned by outbound feature admission. Each error leaves peer, queue and caller
/// state unchanged. Ready-deadline expiry is not an error: it is reported as a Draining event.
/// @ownership Compile-time error set; no runtime storage.
/// @thread_safety Serialized with every other peer method by the connection owner.
pub const feature_send_error_t = error{
    /// Frame is shorter than a header, longer than MaxFrameBytes, has an invalid header,
    /// a header length that disagrees with the frame size, or a malformed TLV payload.
    InvalidFrame,
    /// Frame type is control (0x0001-0x0006), local-only (0x7Fxx), absent from the message
    /// table, unknown, or not legal in this peer's producer direction.
    UnsupportedFrame,
    /// The frame's facility is not in the negotiated Ready capability intersection.
    FeatureNotNegotiated,
    /// Data-class admission has no slot: all 512 ring entries are used, or only the
    /// ReservedControlSlots remain. Retry after acknowledgements drain the queue.
    QueueFull,
    /// The owned copy of the frame could not be allocated.
    OutOfMemory,
};

const send_kind_t = enum { None, Hello, HelloAck, Ping, Pong, Error, Goodbye, Feature };
const send_record_t = struct {
    kind: send_kind_t = .None,
    request_id: u32 = 0,
    total_bytes: usize = 0,
    remaining_bytes: usize = 0,
};

/// Allocation-owning pure composition of protocol reader, queue and control machines.
/// @ownership Caller owns this value and borrows its allocator through deinit. It owns
/// a copied version string and queued frame allocations; it owns no socket, mapping,
/// compositor object, timer, or native teardown proof.
/// @thread_safety All methods must be serialized by one connection event-loop thread.
pub const peer_t = struct {
    /// Allocator borrowed for peer-owned allocations until `deinit`.
    allocator: std_t.mem.Allocator,
    /// Immutable token identifying this transport incarnation.
    connection_token: u64,
    /// Endpoint role controlling protocol directions and handshake behavior.
    role: handshake_mod_t.role_t,
    /// Owned copy of the bounded advertised SemVer string.
    local_version: []u8,
    /// Parsed major version retained for the frozen handshake helper.
    local_version_major: u32,
    /// Locally implemented known capability offer.
    local_capabilities: u64,
    /// Helper negotiation state; terminal phase is authoritative in `connection_phase`.
    handshake: handshake_mod_t.session_handshake_t,
    /// Fixed-capacity incoming frame reader owned by this peer.
    reader: reader_mod_t.reader_t,
    /// Allocator-backed outgoing FIFO owned by this peer.
    queue: queue_mod_t.queue_t,
    /// Fixed local request-ID namespace, retired after callback quiescence.
    request_ids: request_ids_mod_t.request_ids_t,
    /// Ready-session liveness state, absent until successful Ready transition.
    liveness: ?liveness_mod_t.session_liveness_t = null,
    /// Fixed ring metadata synchronized with queued control-frame boundaries.
    send_records: [QueueCapacity]send_record_t = [_]send_record_t{.{}} ** QueueCapacity,
    /// Oldest live completion-metadata index.
    send_head: usize = 0,
    /// Number of live completion-metadata records.
    send_count: usize = 0,
    /// Guest-owned Hello ID or Host peer-Hello ID retained for control correlation.
    pending_hello_id: u32 = 0,
    /// Last committed monotonic callback timestamp.
    last_now_ms: u64,
    /// Authoritative phase latch; terminal values never reopen.
    connection_phase: phase_t,
    /// True when a best-effort terminal control frame could not be queued.
    terminal_send_failed: bool = false,
    /// Requires transport abort because an unfinished Hello/Ack remained queued at drain.
    abort_transport: bool = false,
    /// False after idempotent deinitialization.
    live: bool = true,

    /// Initialize one fresh peer and copy its bounded full SemVer string.
    /// @param[in] allocator Allocator borrowed until idempotent deinit.
    /// @param[in] config Role/token/time/version/capabilities for one accepted connection.
    /// @return Initialized caller-owned peer or a local configuration/allocation error.
    /// @errors InvalidConnectionToken, InvalidVersion, UnknownCapabilities, or OutOfMemory;
    /// failure transfers no allocation to caller.
    /// @ownership Copies version bytes and owns queue frames until deinit; allocator must outlive it.
    /// @thread_safety Subsequent calls are serialized on one event-loop thread.
    pub fn init(allocator: std_t.mem.Allocator, config: config_t) (control_peer_error_t || error{ InvalidVersion, UnknownCapabilities, OutOfMemory })!peer_t {
        if (config.connection_token == 0) return error.InvalidConnectionToken;
        if (config.local_version.len < MinVersionBytes or config.local_version.len > MaxVersionBytes) return error.InvalidVersion;
        const major = agent_version_t.parse_major(config.local_version) catch return error.InvalidVersion;
        const handshake = handshake_mod_t.session_handshake_t.init(config.role, config.connection_token, major, config.local_capabilities, config.accepted_at_ms) catch |err| switch (err) {
            error.InvalidConnectionToken => return error.InvalidConnectionToken,
            error.UnknownCapabilities => return error.UnknownCapabilities,
            else => return error.InternalState,
        };
        const incoming_direction = if (config.role == .Host) c_t.InteropFromGuest else c_t.InteropFromHost;
        const output_direction = if (config.role == .Host) c_t.InteropFromHost else c_t.InteropFromGuest;
        const reader = reader_mod_t.reader_t.init(@intCast(incoming_direction)) catch return error.InternalState;
        var queue = queue_mod_t.queue_t.init(allocator, @intCast(output_direction)) catch return error.InternalState;
        errdefer queue.deinit();
        const version_copy = allocator.alloc(u8, config.local_version.len) catch return error.OutOfMemory;
        errdefer allocator.free(version_copy);
        @memcpy(version_copy, config.local_version);
        return .{
            .allocator = allocator,
            .connection_token = config.connection_token,
            .role = config.role,
            .local_version = version_copy,
            .local_version_major = major,
            .local_capabilities = config.local_capabilities,
            .handshake = handshake,
            .reader = reader,
            .queue = queue,
            .request_ids = request_ids_mod_t.request_ids_t.init(),
            .last_now_ms = config.accepted_at_ms,
            .connection_phase = handshake.phase,
        };
    }

    /// Release queued frame copies and version storage; repeated calls are harmless.
    /// @param[in,out] self Peer whose owned queue/version allocations are freed.
    /// @return No value; subsequent stateful calls return PeerClosed.
    /// @ownership Frees only allocations owned by this peer; no socket/mapping is touched.
    /// @thread_safety Caller first stops all peer callbacks and borrowed TX views.
    pub fn deinit(self: *peer_t) void {
        if (!self.live) return;
        self.queue.deinit();
        self.request_ids.retire_all();
        self.allocator.free(self.local_version);
        self.local_version = &.{};
        self.send_records = [_]send_record_t{.{}} ** QueueCapacity;
        self.send_count = 0;
        self.send_head = 0;
        self.connection_phase = .Closed;
        self.live = false;
    }

    /// Return current negotiation/lifecycle phase without mutation.
    /// @param[in] self Peer to inspect.
    /// @return Handshake phase, or Closed after deinit.
    /// @ownership Value-only snapshot.
    /// @thread_safety Serialize with mutations.
    pub fn phase(self: *const peer_t) handshake_mod_t.phase_t {
        return self.connection_phase;
    }

    /// Return the size of the complete pending frame, or zero when none is ready.
    /// @param[in] self Peer whose fixed reader is inspected.
    /// @return Required copy-out bytes for its one complete pending frame.
    /// @ownership Scalar only; no reader slice escapes.
    /// @thread_safety Serialize with feed and next_event.
    pub fn pending_bytes(self: *const peer_t) usize {
        if (!self.live or self.reader.frame_length == 0 or self.reader.received != self.reader.frame_length) return 0;
        return self.reader.frame_length;
    }

    /// Feed a borrowed prefix into the fixed reader without dispatching it.
    /// @param[in,out] self Live peer whose reader accepts at most one frame.
    /// @param[in] connection_token Captured token for this transport callback.
    /// @param[in] bytes Borrowed input; copied immediately and never retained.
    /// @return Exact consumed count, current phase and optional terminal header error.
    /// @errors WrongConnectionToken, PeerClosed, WrongPhase or InvalidInput without mutation.
    /// @ownership Caller retains input. Reader owns its bounded copy.
    /// @thread_safety Caller serializes feed with next_event and transport callbacks.
    pub fn feed(self: *peer_t, connection_token: u64, bytes: []const u8) (control_peer_error_t || error{ InputAliasesReader, ReaderFailed, InputClosed })!feed_result_t {
        try self.check_live_token(connection_token);
        if (self.connection_phase == .Draining or self.connection_phase == .Closed) return error.WrongPhase;
        const fed = self.reader.feed(bytes) catch |err| switch (err) {
            error.InputAliasesReader => return error.InputAliasesReader,
            error.ReaderFailed => return error.ReaderFailed,
            error.InputClosed => return error.InputClosed,
        };
        var terminal_error: ?c_int = null;
        var terminal_send_failed = false;
        if (fed.header_error != c_t.InteropErrorNone) {
            terminal_error = fed.header_error;
            self.enter_draining_at(self.last_now_ms, .WireError);
            terminal_send_failed = !self.enqueue_error(c_t.InteropErrorProtocolViolation, 0, null);
            self.terminal_send_failed = self.terminal_send_failed or terminal_send_failed;
        }
        return .{
            .consumed = fed.consumed,
            .phase = self.connection_phase,
            .terminal_error = terminal_error,
            .terminal_send_failed = terminal_send_failed,
            .abort_transport = self.abort_transport,
        };
    }

    /// Process exactly one complete pending frame and copy outward frames to output.
    /// @param[in,out] self Live peer with zero or one pending reader frame.
    /// @param[in] connection_token Captured token; stale calls do not mutate state.
    /// @param[in] now_ms Monotonic processing time; committed only after local checks pass.
    /// @param[out] output Caller-owned nonoverlapping buffer for feature/Error/Goodbye frame copy.
    /// @return Copied event and required byte count; no pending frame yields None.
    /// @errors WrongConnectionToken, TimeWentBackwards, PeerClosed, WrongPhase,
    /// OutputTooSmall, OutputAliasesPeer, or reader errors with state/output unchanged.
    /// @ownership Copies complete frames; no reader slice escapes.
    /// @thread_safety Caller serializes frame dispatch with feed, timers and send callbacks.
    pub fn next_event(self: *peer_t, connection_token: u64, now_ms: u64, output: []u8) (control_peer_error_t || handshake_mod_t.handshake_error_t || liveness_mod_t.liveness_error_t || request_ids_mod_t.request_ids_error_t || error{ ReaderFailed, InputClosed, FrameNotReady })!event_result_t {
        try self.check_live_token(connection_token);
        try self.check_time(now_ms);
        if (self.connection_phase == .Closed) return error.WrongPhase;
        const view = self.reader.peek() catch |err| switch (err) {
            error.ReaderFailed => return error.ReaderFailed,
            error.InputClosed => return error.InputClosed,
            error.FrameNotReady => return .{ .event = self.event(.None), .required_bytes = 0 },
        };
        const frame_type = view.header.type;
        const validation = view.payload_validation;
        const is_error = frame_type == c_t.InteropMsgError;
        const is_goodbye = frame_type == c_t.InteropMsgGoodbye and view.header.request_id == 0;
        const should_copy = validation == c_t.InteropErrorNone and
            (self.connection_phase == .Ready and (is_error or is_goodbye or !is_control_type(frame_type)));
        if (should_copy and output.len < view.frame.len) return error.OutputTooSmall;
        if (should_copy and self.output_aliases_peer(output.ptr, output.len)) return error.OutputAliasesPeer;
        if (self.connection_phase == .Draining or self.connection_phase == .Closed)
            return .{ .event = self.event(.Draining), .required_bytes = if (should_copy) view.frame.len else 0 };
        if (self.connection_phase == .Ready and self.deadline_expired(now_ms)) {
            self.enter_draining_at(now_ms, .TransportLost);
            return .{ .event = self.event(.Draining), .required_bytes = 0 };
        }
        // This serialized preflight is the sole Ready timeout decision for this event.
        // Dispatch below uses the same timestamp and unchanged last-valid-RX state.
        if (self.connection_phase != .Ready) {
            if (now_ms >= self.handshake.accepted_at_ms and now_ms - self.handshake.accepted_at_ms >= handshake_mod_t.HelloTimeoutMs) {
                self.enter_draining_at(now_ms, .HelloTimeout);
                return .{ .event = self.event(.Draining), .required_bytes = 0 };
            }
            return self.dispatch_handshake(view, connection_token, now_ms, output);
        }
        return self.dispatch_ready(view, connection_token, now_ms, output);
    }

    /// Stage host mapping validation and enqueue the resulting HelloAck or Error.
    /// @param[in,out] self Host peer awaiting external mapping validation.
    /// @param[in] connection_token Captured validation callback token.
    /// @param[in] now_ms Monotonic callback time.
    /// @param[in] epoch Exact staged guest epoch checked by the external owner.
    /// @param[in] layout_version Actual validated layout version.
    /// @param[in] succeeded True only for full external validation success.
    /// @return None, Ready, or Draining event after staged helper action.
    /// @errors WrongConnectionToken, TimeWentBackwards, WrongPhase unchanged.
    /// @ownership Mapping remains externally owned; peer retains only copied scalars.
    /// @thread_safety Caller serializes mapping callbacks with peer I/O.
    pub fn host_mapping_validated(self: *peer_t, connection_token: u64, now_ms: u64, epoch: u64, layout_version: u32, succeeded: bool) (control_peer_error_t || handshake_mod_t.handshake_error_t)!event_t {
        try self.check_live_token(connection_token);
        try self.check_time(now_ms);
        if (self.role != .Host or self.connection_phase != .AwaitMapping) return error.WrongPhase;
        if (now_ms >= self.handshake.accepted_at_ms and now_ms - self.handshake.accepted_at_ms >= handshake_mod_t.HelloTimeoutMs) {
            self.enter_draining_at(now_ms, .HelloTimeout);
            return self.event(.Draining);
        }
        var next = self.handshake;
        const action = try next.mapping_validated(connection_token, now_ms, epoch, layout_version, succeeded);
        self.handshake = next;
        self.connection_phase = next.phase;
        self.last_now_ms = now_ms;
        if (next.phase == .Draining) self.enter_draining_at(now_ms, .WireError);
        switch (action) {
            .SendHelloAck => |ack| {
                if (!self.enqueue_hello_ack(ack)) {
                    self.enter_draining_at(now_ms, .WireError);
                    self.terminal_send_failed = true;
                    return self.event(.Draining);
                }
                return self.event(.None);
            },
            .ErrorAndDrain => |wire_error| {
                const code: c_int = @intCast(@intFromEnum(wire_error));
                if (!self.abort_transport and !self.enqueue_error(code, self.pending_hello_id, c_t.InteropMsgHello)) self.terminal_send_failed = true;
                return self.event(.Draining);
            },
            .Drain => return self.event(.Draining),
            .None => return self.event(.None),
            else => return error.InternalState,
        }
    }

    /// Initialize Guest handshake state and queue Hello with one shared request ID.
    /// @param[in,out] self Guest peer awaiting external mapping initialization.
    /// @param[in] connection_token Captured initializer callback token.
    /// @param[in] now_ms Monotonic callback timestamp.
    /// @param[in] epoch Fresh externally initialized mapping epoch.
    /// @param[in] succeeded True only after complete external layout initialization.
    /// @return None after Hello queue success, or Draining on mandatory queue failure.
    /// @errors WrongConnectionToken, TimeWentBackwards, WrongPhase, invalid epoch,
    /// failed mapping initialization, or exhausted request IDs; local errors preserve state.
    /// @ownership Peer reserves and owns Hello ID until Ready or transport quiescence.
    /// @thread_safety Caller serializes mapping callbacks with peer I/O.
    pub fn guest_mapping_initialized(self: *peer_t, connection_token: u64, now_ms: u64, epoch: u64, succeeded: bool) (control_peer_error_t || handshake_mod_t.handshake_error_t || request_ids_mod_t.request_ids_error_t)!event_t {
        try self.check_live_token(connection_token);
        try self.check_time(now_ms);
        if (self.role != .Guest or self.connection_phase != .InitializingMapping) return error.WrongPhase;
        if (!succeeded) return error.MappingInitializationFailed;
        if (epoch == 0) return error.InvalidSessionEpoch;
        if (now_ms >= self.handshake.accepted_at_ms and now_ms - self.handshake.accepted_at_ms >= handshake_mod_t.HelloTimeoutMs) {
            self.enter_draining_at(now_ms, .HelloTimeout);
            return self.event(.Draining);
        }
        var next_ids = self.request_ids;
        const request_id = next_ids.reserve() catch return error.RequestIdsFull;
        var next_handshake = self.handshake;
        const action = try next_handshake.mapping_initialized(connection_token, now_ms, succeeded, epoch, request_id);
        const hello = switch (action) {
            .SendHello => |value| value,
            .Drain => {
                self.handshake = next_handshake;
                self.connection_phase = next_handshake.phase;
                self.enter_draining_at(now_ms, .WireError);
                self.request_ids = next_ids;
                self.last_now_ms = now_ms;
                return self.event(.Draining);
            },
            else => return error.InternalState,
        };
        self.handshake = next_handshake;
        self.connection_phase = next_handshake.phase;
        self.request_ids = next_ids;
        self.pending_hello_id = request_id;
        self.last_now_ms = now_ms;
        if (!self.enqueue_hello(hello)) {
            self.enter_draining_at(now_ms, .WireError);
            self.terminal_send_failed = true;
            return self.event(.Draining);
        }
        return self.event(.None);
    }

    /// Borrow the remaining front frame for an OS send attempt.
    /// @param[in] self Live peer whose outgoing queue owns the frame.
    /// @param[in] connection_token Captured transport callback token.
    /// @return Borrowed remaining bytes, or null if no frame is queued.
    /// @errors WrongConnectionToken or PeerClosed without mutation.
    /// @ownership Slice expires at any mutating peer call, acknowledgement or deinit.
    /// @thread_safety Caller serializes the send attempt with every peer operation.
    pub fn peek_tx(self: *const peer_t, connection_token: u64) control_peer_error_t!?[]const u8 {
        try self.check_live_token_const(connection_token);
        if (self.connection_phase == .Closed) return error.WrongPhase;
        if (self.abort_transport) return null;
        return self.queue.peek() catch |err| switch (err) {
            error.Empty => null,
            error.QueueClosed => return error.PeerClosed,
        };
    }

    /// Acknowledge exact successful OS write bytes and fire complete-frame control callbacks.
    /// @param[in,out] self Peer and owned FIFO whose confirmed front bytes advance.
    /// @param[in] connection_token Captured send callback token.
    /// @param[in] now_ms Monotonic completion timestamp.
    /// @param[in] written Bytes the OS confirmed written; zero means no progress.
    /// @return Ready exactly when the complete HelloAck commits host readiness; otherwise None/Draining.
    /// @errors WrongConnectionToken, TimeWentBackwards, InvalidWriteCount, or PeerClosed
    /// with state unchanged.
    /// @ownership Queue frees frame storage only after full acknowledgement.
    /// @thread_safety Caller serializes send acknowledgements with peeks and other methods.
    pub fn ack_tx(self: *peer_t, connection_token: u64, now_ms: u64, written: usize) (control_peer_error_t || handshake_mod_t.handshake_error_t || liveness_mod_t.liveness_error_t || request_ids_mod_t.request_ids_error_t)!event_t {
        try self.check_live_token(connection_token);
        try self.check_time(now_ms);
        if (self.connection_phase == .Closed) return error.WrongPhase;
        const front = self.queue.peek() catch |err| switch (err) {
            error.Empty => return error.NoQueuedFrame,
            error.QueueClosed => return error.PeerClosed,
        };
        if (written > front.len) return error.InvalidWriteCount;
        const expires_now = self.deadline_expired(now_ms);
        const meta_index = self.send_head;
        var meta = self.send_records[meta_index];
        if (meta.kind == .None or written > meta.remaining_bytes) return error.InternalState;
        const completes = written == meta.remaining_bytes;

        // The caller reports bytes already accepted by the OS. Account them even
        // when the timestamp expires the session or the peer is already terminal.
        if (written != 0) self.queue.consume_written(written) catch return error.InternalState;
        self.last_now_ms = now_ms;
        if (written == 0) {
            if (self.connection_phase == .Draining) return self.event(.Draining);
            if (expires_now) {
                self.enter_draining_at(now_ms, if (self.connection_phase == .Ready) .TransportLost else .HelloTimeout);
                return self.event(.Draining);
            }
            return self.event(.None);
        } else if (!completes) {
            meta.remaining_bytes -= written;
            self.send_records[meta_index] = meta;
        } else {
            self.send_records[meta_index] = .{};
            self.send_head = (self.send_head + 1) % QueueCapacity;
            self.send_count -= 1;
        }

        if (self.connection_phase == .Draining or self.connection_phase == .Closed) return self.event(.Draining);
        if (expires_now) {
            self.enter_draining_at(now_ms, if (self.connection_phase == .Ready) .TransportLost else .HelloTimeout);
            return self.event(.Draining);
        }
        if (self.connection_phase == .Ready) {
            var live = self.liveness orelse return error.InternalState;
            const action = try live.tick(connection_token, now_ms);
            self.apply_released_ids(action.released_ids[0..action.released_count]) catch return error.InternalState;
            self.liveness = live;
        }
        if (!completes) return self.event(.None);

        const prior_phase = self.connection_phase;
        var next_handshake = self.handshake;
        if (prior_phase != .Ready and meta.kind == .Hello) {
            _ = try next_handshake.hello_sent(connection_token, now_ms, meta.request_id);
        } else if (prior_phase != .Ready and meta.kind == .HelloAck) {
            _ = try next_handshake.ack_sent(connection_token, now_ms, meta.request_id);
        }
        self.handshake = next_handshake;
        self.connection_phase = next_handshake.phase;
        if (prior_phase != .Ready and self.connection_phase == .Ready) {
            self.initialize_liveness(now_ms) catch return self.fail_internal(now_ms);
            return self.ready_event();
        }
        return self.event(.None);
    }

    /// Advance handshake timeout or Ready liveness and queue a due host Ping.
    /// @param[in,out] self Peer whose serialized timers and owned queue advance.
    /// @param[in] connection_token Captured timer callback token.
    /// @param[in] now_ms Monotonic timer value.
    /// @return None, Draining, or Ready-phase Ping cadence result.
    /// @errors WrongConnectionToken, TimeWentBackwards, PeerClosed or request ID exhaustion;
    /// local errors leave state unchanged except mandatory send failure, which drains.
    /// @ownership No timer thread or mapping is owned; Ping IDs share request_ids.
    /// @thread_safety Caller serializes with I/O and mapping callbacks.
    pub fn tick(self: *peer_t, connection_token: u64, now_ms: u64) (control_peer_error_t || handshake_mod_t.handshake_error_t || liveness_mod_t.liveness_error_t || request_ids_mod_t.request_ids_error_t)!event_t {
        try self.check_live_token(connection_token);
        try self.check_time(now_ms);
        if (self.connection_phase == .Draining or self.connection_phase == .Closed) return self.event(.Draining);
        if (self.connection_phase != .Ready) {
            const action = try self.handshake.tick(connection_token, now_ms);
            self.last_now_ms = now_ms;
            self.connection_phase = self.handshake.phase;
            if (action == .Drain) {
                self.enter_draining_at(now_ms, self.handshake.drain_reason orelse .HelloTimeout);
                return self.event(.Draining);
            }
            return self.event(.None);
        }
        var live = self.liveness orelse return error.InternalState;
        const action = try live.tick(connection_token, now_ms);
        self.apply_released_ids(action.released_ids[0..action.released_count]) catch return error.InternalState;
        self.liveness = live;
        self.last_now_ms = now_ms;
        if (action.disconnect) {
            self.enter_draining_at(now_ms, .TransportLost);
            return self.event(.Draining);
        }
        if (!action.ping_due) return self.event(.None);
        const request_id = self.request_ids.reserve() catch {
            self.enter_draining_at(now_ms, .WireError);
            return self.event(.Draining);
        };
        live = self.liveness.?;
        const registered = live.register_ping(connection_token, now_ms, request_id) catch {
            _ = self.request_ids.release(request_id) catch {};
            self.enter_draining_at(now_ms, .WireError);
            return self.event(.Draining);
        };
        self.apply_released_ids(registered.released_ids[0..registered.released_count]) catch return error.InternalState;
        self.liveness = live;
        if (!self.enqueue_empty_control(c_t.InteropMsgPing, request_id, .Ping)) {
            self.enter_draining_at(now_ms, .WireError);
            self.terminal_send_failed = true;
            return self.event(.Draining);
        }
        return self.event(.None);
    }

    /// Admit one complete caller-built feature frame to the outbound FIFO of a Ready connection.
    /// Validation order is pure and runs before any state change: (1) token, then monotonic
    /// time, then phase must be Ready; (2) frame length in [16, MaxFrameBytes] and a valid v2
    /// header whose length field matches the frame size; (3) type is not control or local-only
    /// and is present in the facility table; (4) the codec payload validator accepts the type
    /// and payload for this role's producer direction; (5) the type's facility is negotiated.
    /// Only then is the Ready deadline checked: if the 6 s liveness window has expired at
    /// now_ms, the peer enters Draining (reason TransportLost) and the frame is not queued.
    /// Otherwise the frame is copied into the Data admission class, which keeps the
    /// ReservedControlSlots free for Ping, Pong, Error and Goodbye.
    /// @param[in,out] self Live Ready peer; changed only on successful admission or deadline drain.
    /// @param[in] connection_token Captured token identifying this connection instance.
    /// @param[in] now_ms Monotonic admission time; committed as last_now_ms only on success.
    /// @param[in] frame Borrowed complete header plus payload bytes, copied before return.
    /// @return None after the frame is queued; Draining when the Ready deadline expired at
    /// now_ms (frame not queued). event.phase is the phase after the call.
    /// @errors WrongConnectionToken, InvalidConnectionToken, PeerClosed, TimeWentBackwards and
    /// WrongPhase (every phase other than Ready, including Draining and Closed), plus
    /// feature_send_error_t. Every error leaves peer, queue, ID and clock state unchanged.
    /// @ownership The caller keeps `frame`. The queue owns one copy until its full
    /// acknowledgement or deinit. No request ID is reserved or released here: feature request
    /// correlation is owned by the external dispatcher (IMPL_DESC 8.20).
    /// @thread_safety Caller serializes with feed, next_event, tick, peek_tx, ack_tx and lifecycle calls.
    pub fn enqueue_feature(self: *peer_t, connection_token: u64, now_ms: u64, frame: []const u8) (control_peer_error_t || feature_send_error_t)!event_t {
        try self.check_live_token(connection_token);
        try self.check_time(now_ms);
        if (self.connection_phase != .Ready) return error.WrongPhase;
        const header = try self.validate_feature_frame(frame);
        if (self.deadline_expired(now_ms)) {
            self.enter_draining_at(now_ms, .TransportLost);
            return self.event(.Draining);
        }
        self.queue.enqueue(frame, .Data) catch |err| return map_admission_error(err);
        const index = (self.send_head + self.send_count) % QueueCapacity;
        self.send_records[index] = .{ .kind = .Feature, .request_id = header.request_id, .total_bytes = frame.len, .remaining_bytes = frame.len };
        self.send_count += 1;
        self.last_now_ms = now_ms;
        return self.event(.None);
    }

    /// Begin local orderly protocol drain and best-effort queue Goodbye(status Cancelled, ID zero).
    /// @param[in,out] self Live peer whose negotiation and future admissions stop.
    /// @param[in] connection_token Captured local stop token.
    /// @param[in] now_ms Monotonic stop timestamp.
    /// @return Draining; event reason records the first helper drain cause.
    /// @errors WrongConnectionToken, TimeWentBackwards or PeerClosed without local-error mutation.
    /// @ownership Does not unmap, close a socket, or establish resource-release proof.
    /// @thread_safety Caller serializes with all peer operations.
    pub fn begin_drain(self: *peer_t, connection_token: u64, now_ms: u64) (control_peer_error_t || handshake_mod_t.handshake_error_t)!event_t {
        try self.check_live_token(connection_token);
        try self.check_time(now_ms);
        if (self.connection_phase == .Closed) return self.event(.Draining);
        if (self.connection_phase == .Draining) return self.event(.Draining);
        if (self.deadline_expired(now_ms)) {
            self.enter_draining_at(now_ms, if (self.connection_phase == .Ready) .TransportLost else .HelloTimeout);
            return self.event(.Draining);
        }
        const action = try self.handshake.begin_drain(connection_token, now_ms);
        self.last_now_ms = now_ms;
        self.connection_phase = self.handshake.phase;
        if (action == .Drain) {
            self.enter_draining_at(now_ms, .LocalRequest);
            if (!self.abort_transport and !self.enqueue_goodbye()) self.terminal_send_failed = true;
        }
        return self.event(.Draining);
    }

    /// Record transport loss after native callbacks are quiesced and retire local request IDs.
    /// @param[in,out] self Peer whose protocol instance becomes terminal.
    /// @param[in] connection_token Captured transport token.
    /// @return Closed phase; repeated terminal callback is idempotent.
    /// @errors WrongConnectionToken or PeerClosed without mutation.
    /// @ownership Retires only local request IDs and retains queue allocations until deinit.
    /// It proves no peer/native mapping release.
    /// @thread_safety Caller has quiesced I/O callbacks and serializes this final operation.
    pub fn transport_lost(self: *peer_t, connection_token: u64) control_peer_error_t!event_t {
        try self.check_live_token(connection_token);
        if (self.connection_phase == .Closed) return self.event(.Draining);
        self.request_ids.retire_all();
        self.connection_phase = .Closed;
        return self.event(.Draining);
    }

    /// Current peer-local flag for an unavailable best-effort terminal send.
    /// @param[in] self Peer to inspect.
    /// @return True if an Error/Goodbye could not be queued after entering Draining.
    /// @ownership Scalar only; not a retry or delivery claim.
    /// @thread_safety Serialize with queue mutations.
    pub fn terminal_send_unavailable(self: *const peer_t) bool {
        return self.terminal_send_failed;
    }

    /// Report whether the platform must abort instead of completing partial Hello/Ack.
    /// @param[in] self Peer to inspect.
    /// @return True if Draining began while a readiness-causing frame remained queued.
    /// @ownership Value-only observation; this function does not abort or close transport.
    /// @thread_safety Serialize with peer mutations.
    pub fn must_abort_transport(self: *const peer_t) bool {
        return self.abort_transport;
    }

    fn check_live_token(self: *const peer_t, token: u64) control_peer_error_t!void {
        if (!self.live) return error.PeerClosed;
        if (token == 0) return error.InvalidConnectionToken;
        if (token != self.connection_token) return error.WrongConnectionToken;
    }

    fn check_live_token_const(self: *const peer_t, token: u64) control_peer_error_t!void {
        try self.check_live_token(token);
    }

    fn check_time(self: *const peer_t, now_ms: u64) control_peer_error_t!void {
        if (now_ms < self.last_now_ms) return error.TimeWentBackwards;
    }

    fn event(self: *const peer_t, kind: event_kind_t) event_t {
        return .{ .kind = kind, .phase = self.connection_phase, .reason = self.handshake.drain_reason, .abort_transport = self.abort_transport };
    }

    fn ready_event(self: *const peer_t) event_t {
        return .{ .kind = .Ready, .phase = self.connection_phase, .epoch = self.handshake.session_epoch, .capabilities = self.handshake.negotiated_capabilities, .abort_transport = self.abort_transport };
    }

    fn enter_draining_at(self: *peer_t, now_ms: u64, reason: handshake_mod_t.drain_reason_t) void {
        if (self.connection_phase == .Closed) return;
        if (self.connection_phase != .Draining) {
            self.connection_phase = .Draining;
        }
        if (self.handshake.drain_reason == null) self.handshake.drain_reason = reason;
        for (0..self.send_count) |offset| {
            const index = (self.send_head + offset) % QueueCapacity;
            const record = self.send_records[index];
            if (record.kind == .Hello or record.kind == .HelloAck) {
                self.abort_transport = true;
                break;
            }
        }
        self.last_now_ms = @max(self.last_now_ms, now_ms);
    }

    fn deadline_expired(self: *const peer_t, now_ms: u64) bool {
        if (self.connection_phase == .Ready) {
            if (self.liveness) |live| return now_ms >= live.last_valid_rx_ms and now_ms - live.last_valid_rx_ms >= liveness_mod_t.LivenessTimeoutMs;
            return false;
        }
        if (self.connection_phase == .Draining or self.connection_phase == .Closed) return false;
        return now_ms >= self.handshake.accepted_at_ms and now_ms - self.handshake.accepted_at_ms >= handshake_mod_t.HelloTimeoutMs;
    }

    fn initialize_liveness(self: *peer_t, ready_at_ms: u64) liveness_mod_t.liveness_error_t!void {
        self.liveness = try liveness_mod_t.session_liveness_t.init(
            if (self.role == .Host) .Host else .Guest,
            self.connection_token,
            ready_at_ms,
        );
    }

    fn enqueue_hello(self: *peer_t, hello: handshake_mod_t.hello_out_t) bool {
        var frame: [queue_mod_t.MaxFrameBytes]u8 = undefined;
        const length = encode_hello(&frame, hello, self.local_version) catch return false;
        return self.enqueue_encoded(frame[0..length], .Hello, hello.request_id);
    }

    fn enqueue_hello_ack(self: *peer_t, ack: handshake_mod_t.hello_ack_out_t) bool {
        var frame: [queue_mod_t.MaxFrameBytes]u8 = undefined;
        const length = encode_hello_ack(&frame, ack, self.local_version) catch return false;
        return self.enqueue_encoded(frame[0..length], .HelloAck, ack.request_id);
    }

    fn enqueue_empty_control(self: *peer_t, message_type: u16, request_id: u32, kind: send_kind_t) bool {
        var frame: [c_t.InteropHeaderBytes]u8 = undefined;
        var writer: c_t.interop_writer_t = undefined;
        c_t.interop_writer_init(&writer, &frame, frame.len);
        var length: usize = 0;
        const result = c_t.interop_writer_finish(&writer, message_type, request_id, self.outgoing_direction(), &length);
        if (result != c_t.InteropErrorNone) return false;
        return self.enqueue_encoded(frame[0..length], kind, request_id);
    }

    fn enqueue_error(self: *peer_t, code: c_int, request_id: u32, refused_type: ?u16) bool {
        if (self.abort_transport or self.connection_phase == .Closed) return false;
        var frame: [2048]u8 = undefined;
        var writer: c_t.interop_writer_t = undefined;
        c_t.interop_writer_init(&writer, &frame, frame.len);
        if (c_t.interop_put_uint(&writer, c_t.InteropTagErrorCode, @intCast(code)) != c_t.InteropErrorNone) return false;
        if (refused_type) |value| if (c_t.interop_put_uint(&writer, c_t.InteropTagRefusedType, value) != c_t.InteropErrorNone) return false;
        var length: usize = 0;
        if (c_t.interop_writer_finish(&writer, c_t.InteropMsgError, request_id, self.outgoing_direction(), &length) != c_t.InteropErrorNone) return false;
        return self.enqueue_encoded(frame[0..length], .Error, request_id);
    }

    fn enqueue_goodbye(self: *peer_t) bool {
        var frame: [c_t.InteropHeaderBytes + 8]u8 = undefined;
        var writer: c_t.interop_writer_t = undefined;
        c_t.interop_writer_init(&writer, &frame, frame.len);
        if (c_t.interop_put_uint(&writer, c_t.InteropTagStatus, c_t.InteropCancelled) != c_t.InteropErrorNone) return false;
        var length: usize = 0;
        if (c_t.interop_writer_finish(&writer, c_t.InteropMsgGoodbye, 0, self.outgoing_direction(), &length) != c_t.InteropErrorNone) return false;
        return self.enqueue_encoded(frame[0..length], .Goodbye, 0);
    }

    fn enqueue_encoded(self: *peer_t, bytes: []const u8, kind: send_kind_t, request_id: u32) bool {
        if (self.send_count == QueueCapacity) return false;
        self.queue.enqueue(bytes, .Control) catch return false;
        const index = (self.send_head + self.send_count) % QueueCapacity;
        self.send_records[index] = .{ .kind = kind, .request_id = request_id, .total_bytes = bytes.len, .remaining_bytes = bytes.len };
        self.send_count += 1;
        return true;
    }

    fn dispatch_handshake(self: *peer_t, view: reader_mod_t.frame_view_t, token: u64, now_ms: u64, output: []u8) (control_peer_error_t || handshake_mod_t.handshake_error_t || liveness_mod_t.liveness_error_t || request_ids_mod_t.request_ids_error_t || error{ ReaderFailed, InputClosed, FrameNotReady })!event_result_t {
        if (view.payload_validation != c_t.InteropErrorNone) {
            const code = view.payload_validation;
            const is_remote_error = view.header.type == c_t.InteropMsgError;
            self.enter_draining_at(now_ms, .WireError);
            if (!is_remote_error and !self.enqueue_error(code, view.header.request_id, view.header.type)) self.terminal_send_failed = true;
            try self.reader.consume();
            self.last_now_ms = now_ms;
            return .{ .event = self.event(.Draining) };
        }
        if (view.header.type == c_t.InteropMsgError) {
            self.enter_draining_at(now_ms, .WireError);
            try self.reader.consume();
            self.last_now_ms = now_ms;
            return .{ .event = self.event(.Draining) };
        }
        if (self.role == .Host and view.header.type == c_t.InteropMsgHello) {
            const hello = parse_hello(view) catch {
                self.enter_draining_at(now_ms, .WireError);
                if (!self.enqueue_error(c_t.InteropErrorMalformedFrame, view.header.request_id, view.header.type)) self.terminal_send_failed = true;
                try self.reader.consume();
                self.last_now_ms = now_ms;
                return .{ .event = self.event(.Draining) };
            };
            var next = self.handshake;
            const action = try next.receive_hello(token, now_ms, hello);
            self.handshake = next;
            self.connection_phase = next.phase;
            if (next.phase == .Draining) self.enter_draining_at(now_ms, .WireError);
            self.last_now_ms = now_ms;
            try self.reader.consume();
            switch (action) {
                .None => {
                    self.pending_hello_id = hello.request_id;
                    return .{ .event = .{ .kind = .ValidateMapping, .phase = self.connection_phase, .epoch = hello.session_epoch, .layout_version = hello.layout_version } };
                },
                .ErrorAndDrain => |wire_error| {
                    const code: c_int = @intCast(@intFromEnum(wire_error));
                    if (!self.abort_transport and !self.enqueue_error(code, hello.request_id, c_t.InteropMsgHello)) self.terminal_send_failed = true;
                    return .{ .event = self.event(.Draining) };
                },
                .Drain => return .{ .event = self.event(.Draining) },
                else => return error.InternalState,
            }
        }
        if (self.role == .Guest and view.header.type == c_t.InteropMsgHelloAck) {
            const ack = parse_hello_ack(view) catch {
                self.enter_draining_at(now_ms, .WireError);
                if (!self.enqueue_error(c_t.InteropErrorMalformedFrame, view.header.request_id, view.header.type)) self.terminal_send_failed = true;
                try self.reader.consume();
                self.last_now_ms = now_ms;
                return .{ .event = self.event(.Draining) };
            };
            var next = self.handshake;
            const action = try next.receive_ack(token, now_ms, ack);
            self.handshake = next;
            self.connection_phase = next.phase;
            if (next.phase == .Draining) self.enter_draining_at(now_ms, .WireError);
            self.last_now_ms = now_ms;
            try self.reader.consume();
            switch (action) {
                .None => {
                    self.request_ids.release(self.pending_hello_id) catch return error.InternalState;
                    self.pending_hello_id = 0;
                    self.connection_phase = self.handshake.phase;
                    try self.initialize_liveness(now_ms);
                    return .{ .event = self.ready_event() };
                },
                .ErrorAndDrain => |wire_error| {
                    const code: c_int = @intCast(@intFromEnum(wire_error));
                    if (!self.abort_transport and !self.enqueue_error(code, ack.request_id, c_t.InteropMsgHelloAck)) self.terminal_send_failed = true;
                    return .{ .event = self.event(.Draining) };
                },
                .Drain => return .{ .event = self.event(.Draining) },
                else => return error.InternalState,
            }
        }
        self.enter_draining_at(now_ms, .WireError);
        if (!self.enqueue_error(c_t.InteropErrorProtocolViolation, view.header.request_id, view.header.type)) self.terminal_send_failed = true;
        try self.reader.consume();
        self.last_now_ms = now_ms;
        _ = output;
        return .{ .event = self.event(.Draining) };
    }

    fn dispatch_ready(self: *peer_t, view: reader_mod_t.frame_view_t, token: u64, now_ms: u64, output: []u8) (control_peer_error_t || liveness_mod_t.liveness_error_t || request_ids_mod_t.request_ids_error_t || error{ ReaderFailed, InputClosed, FrameNotReady })!event_result_t {
        if (view.payload_validation != c_t.InteropErrorNone) {
            const remote_error = view.header.type == c_t.InteropMsgError;
            self.enter_draining_at(now_ms, .WireError);
            if (!remote_error and !self.enqueue_error(view.payload_validation, view.header.request_id, view.header.type)) self.terminal_send_failed = true;
            try self.reader.consume();
            self.last_now_ms = now_ms;
            return .{ .event = self.event(.Draining) };
        }
        if (view.header.type == c_t.InteropMsgPing or view.header.type == c_t.InteropMsgPong) {
            var live = self.liveness orelse return error.InternalState;
            if (view.header.type == c_t.InteropMsgPing) {
                const action = live.receive_ping(token, now_ms, view.header.request_id) catch |err| switch (err) {
                    error.InvalidRequestId => {
                        self.enter_draining_at(now_ms, .WireError);
                        if (!self.enqueue_error(c_t.InteropErrorProtocolViolation, view.header.request_id, c_t.InteropMsgPing)) self.terminal_send_failed = true;
                        try self.reader.consume();
                        return .{ .event = self.event(.Draining) };
                    },
                    else => return err,
                };
                self.apply_released_ids(action.released_ids[0..action.released_count]) catch return error.InternalState;
                self.liveness = live;
                if (action.send_pong) |id| {
                    if (!self.enqueue_empty_control(c_t.InteropMsgPong, id, .Pong)) {
                        self.enter_draining_at(now_ms, .WireError);
                        self.terminal_send_failed = true;
                    }
                }
            } else {
                const action = live.receive_pong(token, now_ms, view.header.request_id) catch |err| switch (err) {
                    error.InvalidRequestId => {
                        self.enter_draining_at(now_ms, .WireError);
                        if (!self.enqueue_error(c_t.InteropErrorProtocolViolation, view.header.request_id, c_t.InteropMsgPong)) self.terminal_send_failed = true;
                        try self.reader.consume();
                        return .{ .event = self.event(.Draining) };
                    },
                    else => return err,
                };
                self.apply_released_ids(action.released_ids[0..action.released_count]) catch return error.InternalState;
                self.liveness = live;
            }
            try self.reader.consume();
            self.last_now_ms = now_ms;
            return .{ .event = self.event(if (self.connection_phase == .Draining) .Draining else .None) };
        }
        var live = self.liveness orelse return error.InternalState;
        const observed = try live.observe_valid_frame(token, now_ms);
        self.apply_released_ids(observed.released_ids[0..observed.released_count]) catch return error.InternalState;
        self.liveness = live;
        if (view.header.type == c_t.InteropMsgHello or view.header.type == c_t.InteropMsgHelloAck) {
            self.enter_draining_at(now_ms, .WireError);
            if (!self.enqueue_error(c_t.InteropErrorProtocolViolation, view.header.request_id, view.header.type)) self.terminal_send_failed = true;
            try self.reader.consume();
            self.last_now_ms = now_ms;
            return .{ .event = self.event(.Draining) };
        }
        if (view.header.type == c_t.InteropMsgError) {
            const error_code = field_uint(view.payload, c_t.InteropTagErrorCode, 4) catch c_t.InteropErrorMalformedFrame;
            @memcpy(output[0..view.frame.len], view.frame);
            try self.reader.consume();
            self.last_now_ms = now_ms;
            var result = self.event(.PeerError);
            result.message_type = view.header.type;
            result.request_id = view.header.request_id;
            result.error_code = @intCast(error_code);
            result.copied_bytes = view.frame.len;
            return .{ .event = result, .required_bytes = view.frame.len };
        }
        if (view.header.type == c_t.InteropMsgGoodbye) {
            if (view.header.request_id != 0) {
                self.enter_draining_at(now_ms, .WireError);
                if (!self.enqueue_error(c_t.InteropErrorProtocolViolation, view.header.request_id, c_t.InteropMsgGoodbye)) self.terminal_send_failed = true;
                try self.reader.consume();
                self.last_now_ms = now_ms;
                return .{ .event = self.event(.Draining) };
            }
            @memcpy(output[0..view.frame.len], view.frame);
            self.enter_draining_at(now_ms, .LocalRequest);
            try self.reader.consume();
            self.last_now_ms = now_ms;
            var result = self.event(.Draining);
            result.message_type = view.header.type;
            result.request_id = 0;
            result.copied_bytes = view.frame.len;
            return .{ .event = result, .required_bytes = view.frame.len };
        }
        @memcpy(output[0..view.frame.len], view.frame);
        try self.reader.consume();
        self.last_now_ms = now_ms;
        var result = self.event(.FeatureFrame);
        result.message_type = view.header.type;
        result.request_id = view.header.request_id;
        result.copied_bytes = view.frame.len;
        return .{ .event = result, .required_bytes = view.frame.len };
    }

    fn apply_released_ids(self: *peer_t, released: []const u32) request_ids_mod_t.request_ids_error_t!void {
        for (released) |request_id| try self.request_ids.release(request_id);
    }

    /// Run the pure admission checks for one outbound feature frame; mutates nothing.
    /// @param[in] self Peer whose role supplies the producer direction and negotiated facilities.
    /// @param[in] frame Borrowed candidate frame bytes.
    /// @return Decoded header of an admissible frame.
    /// @errors InvalidFrame, UnsupportedFrame or FeatureNotNegotiated in the order documented on enqueue_feature.
    /// @ownership No allocation; the returned header is a scalar copy.
    /// @thread_safety Serialized with every other peer method.
    fn validate_feature_frame(self: *const peer_t, frame: []const u8) feature_send_error_t!c_t.interop_header_t {
        if (frame.len < c_t.InteropHeaderBytes or frame.len > queue_mod_t.MaxFrameBytes) return error.InvalidFrame;
        var header: c_t.interop_header_t = undefined;
        if (c_t.interop_header_decode(frame.ptr, c_t.InteropHeaderBytes, &header) != c_t.InteropErrorNone) return error.InvalidFrame;
        if (@as(usize, header.length) + c_t.InteropHeaderBytes != frame.len) return error.InvalidFrame;
        if (is_control_type(header.type) or is_local_type(header.type)) return error.UnsupportedFrame;
        const requirement = feature_requirement(header.type) orelse return error.UnsupportedFrame;
        const payload = frame[c_t.InteropHeaderBytes..];
        const payload_pointer: [*c]const u8 = if (payload.len == 0) null else payload.ptr;
        switch (c_t.interop_payload_validate(header.type, self.outgoing_direction(), payload_pointer, payload.len)) {
            c_t.InteropErrorNone => {},
            c_t.InteropErrorUnsupportedType, c_t.InteropErrorUnsupportedField => return error.UnsupportedFrame,
            else => return error.InvalidFrame,
        }
        if (!requirement.satisfied_by(self.handshake.negotiated_capabilities)) return error.FeatureNotNegotiated;
        return header;
    }

    fn outgoing_direction(self: *const peer_t) c_t.interop_direction_t {
        return if (self.role == .Host) c_t.InteropFromHost else c_t.InteropFromGuest;
    }

    fn output_aliases_peer(self: *const peer_t, pointer: [*]u8, length: usize) bool {
        if (length == 0) return false;
        const start = @intFromPtr(pointer);
        const end = std_t.math.add(usize, start, length) catch return true;
        const peer_start = @intFromPtr(self);
        const peer_end = std_t.math.add(usize, peer_start, @sizeOf(peer_t)) catch return true;
        if (ranges_overlap(start, end, peer_start, peer_end)) return true;
        const version_start = @intFromPtr(self.local_version.ptr);
        const version_end = std_t.math.add(usize, version_start, self.local_version.len) catch return true;
        if (ranges_overlap(start, end, version_start, version_end)) return true;
        for (self.queue.frames) |maybe_record| {
            const record = maybe_record orelse continue;
            const bytes_start = @intFromPtr(record.bytes.ptr);
            const bytes_end = std_t.math.add(usize, bytes_start, record.bytes.len) catch return true;
            if (ranges_overlap(start, end, bytes_start, bytes_end)) return true;
        }
        return false;
    }

    fn fail_internal(self: *peer_t, now_ms: u64) event_t {
        self.enter_draining_at(now_ms, .WireError);
        return self.event(.Draining);
    }
};

fn ranges_overlap(left_start: usize, left_end: usize, right_start: usize, right_end: usize) bool {
    return left_start < right_end and right_start < left_end;
}

fn is_control_type(message_type: u16) bool {
    return message_type >= c_t.InteropMsgHello and message_type <= c_t.InteropMsgError;
}

/// Local-only session.sock types occupy the 0x7Fxx range and are never sent over VSOCK.
fn is_local_type(message_type: u16) bool {
    return (message_type & 0xFF00) == 0x7F00;
}

/// Negotiated facility mask bits, mirrored from interop_capability_t for table entries.
const CapWindows: u64 = @intCast(c_t.InteropCapWindows);
const CapInput: u64 = @intCast(c_t.InteropCapInput);
const CapCursor: u64 = @intCast(c_t.InteropCapCursor);
const CapRelativePointer: u64 = @intCast(c_t.InteropCapRelativePointer);
const CapTouch: u64 = @intCast(c_t.InteropCapTouch);
const CapTablet: u64 = @intCast(c_t.InteropCapTablet);
const CapGestures: u64 = @intCast(c_t.InteropCapGestures);
const CapTextInput: u64 = @intCast(c_t.InteropCapTextInput);
const CapClipboard: u64 = @intCast(c_t.InteropCapClipboard);
const CapDragDrop: u64 = @intCast(c_t.InteropCapDragDrop);
const CapCatalog: u64 = @intCast(c_t.InteropCapCatalog);
const CapLaunch: u64 = @intCast(c_t.InteropCapLaunch);
const CapAudio: u64 = @intCast(c_t.InteropCapAudio);
const CapPrinters: u64 = @intCast(c_t.InteropCapPrinters);
const CapUacNative: u64 = @intCast(c_t.InteropCapUacNative);
const CapDisplay: u64 = @intCast(c_t.InteropCapDisplay);
const CapKeyboardLayout: u64 = @intCast(c_t.InteropCapKeyboardLayout);

/// Facility requirement of one feature message type, checked against the negotiated set.
/// A type is admissible only when every `all` bit is negotiated and, if `any` is nonzero,
/// at least one `any` bit is negotiated.
const feature_requirement_t = struct {
    /// Every bit in this mask must be negotiated; zero requires nothing.
    all: u64 = 0,
    /// When nonzero, at least one bit in this mask must be negotiated.
    any: u64 = 0,

    /// Decide whether a negotiated intersection satisfies this requirement.
    /// @param[in] self Requirement from feature_requirement.
    /// @param[in] negotiated Ready-session capability intersection.
    /// @return True when the frame's facility may be used on this connection.
    fn satisfied_by(self: feature_requirement_t, negotiated: u64) bool {
        if ((negotiated & self.all) != self.all) return false;
        return self.any == 0 or (negotiated & self.any) != 0;
    }
};

/// Facility table for every feature message in IMPL_DESC 3.2.5. Window messages require
/// Windows; transfers are gated by the facility whose request created them (3.2.6), so any
/// one of those facilities satisfies them. Control and local-only types return null.
/// @param[in] message_type Frozen type from a validated header.
/// @return Requirement for an admissible feature type, or null for any other type.
fn feature_requirement(message_type: u16) ?feature_requirement_t {
    return switch (message_type) {
        c_t.InteropMsgWindowAnnounce, c_t.InteropMsgWindowUpdate, c_t.InteropMsgWindowRetire, c_t.InteropMsgWindowConfigure, c_t.InteropMsgWindowClose, c_t.InteropMsgWindowMoveRequest, c_t.InteropMsgWindowResizeRequest, c_t.InteropMsgWindowStateRequest, c_t.InteropMsgFrameReady, c_t.InteropMsgFrameRelease, c_t.InteropMsgWindowIconChanged, c_t.InteropMsgPopupRepositioned, c_t.InteropMsgPopupDismissed => .{ .all = CapWindows },
        c_t.InteropMsgInputFocus, c_t.InteropMsgInputKey, c_t.InteropMsgInputPointerMotion, c_t.InteropMsgInputPointerButton, c_t.InteropMsgInputPointerAxis, c_t.InteropMsgInputRelease, c_t.InteropMsgInputLockState, c_t.InteropMsgInputRepeatInfo, c_t.InteropMsgLeaseRevoked, c_t.InteropMsgLeaseAck, c_t.InteropMsgLeaseReady => .{ .all = CapInput },
        c_t.InteropMsgInputPointerRelative => .{ .all = CapInput | CapRelativePointer },
        c_t.InteropMsgInputTouch, c_t.InteropMsgInputTouchFrame, c_t.InteropMsgInputTouchCancel => .{ .all = CapInput | CapTouch },
        c_t.InteropMsgInputTablet, c_t.InteropMsgInputTabletProximity => .{ .all = CapInput | CapTablet },
        c_t.InteropMsgInputGesturePinch => .{ .all = CapInput | CapGestures },
        c_t.InteropMsgInputTextCommit, c_t.InteropMsgInputTextPreedit => .{ .all = CapInput | CapTextInput },
        c_t.InteropMsgCaretRect => .{ .all = CapTextInput },
        c_t.InteropMsgKeyboardLayout => .{ .all = CapKeyboardLayout },
        c_t.InteropMsgCursorShape => .{ .all = CapCursor },
        c_t.InteropMsgCursorConstraint => .{ .all = CapCursor | CapRelativePointer },
        c_t.InteropMsgClipboardOffer, c_t.InteropMsgClipboardRequest, c_t.InteropMsgClipboardClear => .{ .all = CapClipboard },
        c_t.InteropMsgDndEnter, c_t.InteropMsgDndMotion, c_t.InteropMsgDndLeave, c_t.InteropMsgDndDrop, c_t.InteropMsgDndResult, c_t.InteropMsgDndSourceStart, c_t.InteropMsgDndSourceRequest, c_t.InteropMsgDndSourceFinished => .{ .all = CapDragDrop },
        c_t.InteropMsgCatalogBegin, c_t.InteropMsgCatalogAdd, c_t.InteropMsgCatalogUpdate, c_t.InteropMsgCatalogRemove, c_t.InteropMsgCatalogEnd, c_t.InteropMsgIconRequest => .{ .all = CapCatalog },
        c_t.InteropMsgLaunchRequest, c_t.InteropMsgLaunchResult => .{ .all = CapLaunch },
        c_t.InteropMsgElevationNotice, c_t.InteropMsgUacPrompt, c_t.InteropMsgUacDecision, c_t.InteropMsgDesktopSwitch => .{ .all = CapUacNative },
        c_t.InteropMsgAudioStreamOpen, c_t.InteropMsgAudioStreamClose => .{ .all = CapAudio },
        c_t.InteropMsgPrinterSync, c_t.InteropMsgPrinterResult => .{ .all = CapPrinters },
        c_t.InteropMsgDisplayConfig, c_t.InteropMsgDisplayApplied => .{ .all = CapDisplay },
        c_t.InteropMsgTransferBegin, c_t.InteropMsgTransferChunk, c_t.InteropMsgTransferEnd, c_t.InteropMsgTransferCancel => .{ .any = CapClipboard | CapDragDrop | CapCatalog | CapCursor | CapUacNative },
        else => null,
    };
}

/// Map a queue admission error to the outbound feature error set. Queue validation is
/// already performed by validate_feature_frame, so its invalid-frame arms are defensive.
/// @param[in] err Error returned by stream_send_queue enqueue.
/// @return Feature admission error with the same meaning; OutOfMemory stays OutOfMemory.
/// @ownership Scalar mapping only.
/// @thread_safety Pure function.
fn map_admission_error(err: anyerror) (feature_send_error_t || error{PeerClosed}) {
    return switch (err) {
        error.DataCapacityReserved, error.QueueFull => error.QueueFull,
        error.OutOfMemory => error.OutOfMemory,
        error.QueueClosed => error.PeerClosed,
        error.UnsupportedFrame => error.UnsupportedFrame,
        else => error.InvalidFrame,
    };
}

fn field_uint(payload: []const u8, tag: u16, width: u32) error{MalformedField}!u64 {
    var value: u64 = 0;
    const pointer: [*c]const u8 = if (payload.len == 0) null else payload.ptr;
    if (c_t.interop_field_uint(pointer, payload.len, tag, width, &value) != 0) return error.MalformedField;
    return value;
}

fn field_bytes(payload: []const u8, tag: u16) error{MalformedField}![]const u8 {
    var pointer: [*c]const u8 = null;
    var length: u16 = 0;
    const payload_pointer: [*c]const u8 = if (payload.len == 0) null else payload.ptr;
    if (c_t.interop_field_get(payload_pointer, payload.len, tag, 0, &pointer, &length) != 0 or pointer == null) return error.MalformedField;
    return pointer[0..length];
}

fn parse_hello(view: reader_mod_t.frame_view_t) error{MalformedField}!handshake_mod_t.hello_facts_t {
    const version = field_bytes(view.payload, c_t.InteropTagAgentVersion) catch return error.MalformedField;
    const major = agent_version_t.parse_major(version) catch null;
    return .{
        .protocol_version = view.header.version,
        .version_major = major,
        .layout_version = @intCast(try field_uint(view.payload, c_t.InteropTagLayoutVersion, 4)),
        .capabilities = try field_uint(view.payload, c_t.InteropTagCapabilities, 8),
        .session_epoch = try field_uint(view.payload, c_t.InteropTagSessionEpoch, 8),
        .request_id = view.header.request_id,
    };
}

fn parse_hello_ack(view: reader_mod_t.frame_view_t) error{MalformedField}!handshake_mod_t.hello_ack_facts_t {
    const version = field_bytes(view.payload, c_t.InteropTagAgentVersion) catch return error.MalformedField;
    const major = agent_version_t.parse_major(version) catch null;
    return .{
        .protocol_version = view.header.version,
        .version_major = major,
        .layout_version = @intCast(try field_uint(view.payload, c_t.InteropTagLayoutVersion, 4)),
        .capabilities = try field_uint(view.payload, c_t.InteropTagCapabilities, 8),
        .request_id = view.header.request_id,
    };
}

fn encode_hello(frame: []u8, hello: handshake_mod_t.hello_out_t, version: []const u8) error{CodecFailure}!usize {
    var writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&writer, frame.ptr, frame.len);
    if (c_t.interop_put_uint(&writer, c_t.InteropTagCapabilities, hello.capabilities) != c_t.InteropErrorNone or
        c_t.interop_put_bytes(&writer, c_t.InteropTagAgentVersion, version.ptr, @intCast(version.len)) != c_t.InteropErrorNone or
        c_t.interop_put_uint(&writer, c_t.InteropTagLayoutVersion, hello.layout_version) != c_t.InteropErrorNone or
        c_t.interop_put_uint(&writer, c_t.InteropTagSessionEpoch, hello.session_epoch) != c_t.InteropErrorNone)
        return error.CodecFailure;
    var length: usize = 0;
    if (c_t.interop_writer_finish(&writer, c_t.InteropMsgHello, hello.request_id, c_t.InteropFromGuest, &length) != c_t.InteropErrorNone) return error.CodecFailure;
    return length;
}

fn encode_hello_ack(frame: []u8, ack: handshake_mod_t.hello_ack_out_t, version: []const u8) error{CodecFailure}!usize {
    var writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&writer, frame.ptr, frame.len);
    if (c_t.interop_put_uint(&writer, c_t.InteropTagCapabilities, ack.capabilities) != c_t.InteropErrorNone or
        c_t.interop_put_bytes(&writer, c_t.InteropTagAgentVersion, version.ptr, @intCast(version.len)) != c_t.InteropErrorNone or
        c_t.interop_put_uint(&writer, c_t.InteropTagLayoutVersion, ack.layout_version) != c_t.InteropErrorNone)
        return error.CodecFailure;
    var length: usize = 0;
    if (c_t.interop_writer_finish(&writer, c_t.InteropMsgHelloAck, ack.request_id, c_t.InteropFromHost, &length) != c_t.InteropErrorNone) return error.CodecFailure;
    return length;
}

// Test-only fixtures.
fn test_initialization_allocation_failure() !void {
    var failing = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = 0 });
    try std_t.testing.expectError(error.OutOfMemory, peer_t.init(failing.allocator(), .{
        .role = .Guest,
        .connection_token = 1,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    }));
    try std_t.testing.expectEqual(@as(usize, 0), failing.allocated_bytes);
    try std_t.testing.expectEqual(@as(usize, 0), failing.freed_bytes);
}

fn test_initialized_peer_release() !void {
    var peer = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 2,
        .accepted_at_ms = 10,
        .local_version = "1.2.3+build",
        .local_capabilities = 0,
    });
    try std_t.testing.expectEqual(phase_t.AwaitHello, peer.phase());
    try std_t.testing.expectEqualStrings("1.2.3+build", peer.local_version);
    peer.deinit();
    peer.deinit();
    try std_t.testing.expectEqual(phase_t.Closed, peer.phase());
}

fn test_zero_progress_deadline_abort() !void {
    var peer = try peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 3,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer peer.deinit();
    _ = try peer.guest_mapping_initialized(3, 1, 9, true);
    const event = try peer.ack_tx(3, handshake_mod_t.HelloTimeoutMs, 0);
    try std_t.testing.expectEqual(event_kind_t.Draining, event.kind);
    try std_t.testing.expect(event.abort_transport);
    try std_t.testing.expect(peer.must_abort_transport());
    try std_t.testing.expect((try peer.peek_tx(3)) == null);
    try std_t.testing.expectEqual(phase_t.Draining, peer.phase());
}

fn test_allocator_failure_sweep() !void {
    for (0..3) |fail_index| {
        var failing = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = fail_index });
        const maybe_peer = peer_t.init(failing.allocator(), .{
            .role = .Guest,
            .connection_token = 7,
            .accepted_at_ms = 0,
            .local_version = "1.2.3",
            .local_capabilities = 0,
        });
        if (fail_index == 0) {
            try std_t.testing.expectError(error.OutOfMemory, maybe_peer);
        } else {
            var peer = try maybe_peer;
            const init_event = try peer.guest_mapping_initialized(7, 1, 5, true);
            if (fail_index == 1) {
                try std_t.testing.expectEqual(event_kind_t.Draining, init_event.kind);
                try std_t.testing.expect(peer.terminal_send_unavailable());
            } else {
                try std_t.testing.expectEqual(event_kind_t.None, init_event.kind);
                try std_t.testing.expect((try peer.peek_tx(7)) != null);
            }
            peer.deinit();
        }
        try std_t.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
    }
}

fn test_output_alias_detection() !void {
    var peer = try peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 4,
        .accepted_at_ms = 0,
        .local_version = "1.2.3+build",
        .local_capabilities = 0,
    });
    defer peer.deinit();
    try std_t.testing.expect(peer.output_aliases_peer(peer.local_version.ptr, peer.local_version.len));
    try std_t.testing.expect(peer.output_aliases_peer(@ptrCast(&peer), @sizeOf(peer_t)));
    var unrelated: [8]u8 = undefined;
    try std_t.testing.expect(!peer.output_aliases_peer(&unrelated, unrelated.len));
    try std_t.testing.expect(peer.enqueue_goodbye());
    const stored = peer.queue.frames[peer.queue.head].?.bytes;
    try std_t.testing.expect(stored.len > 1);
    _ = try peer.ack_tx(4, 1, 1);
    try std_t.testing.expect(peer.output_aliases_peer(stored.ptr, stored.len));
}

fn test_codec_helpers() !void {
    var frame: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const hello = handshake_mod_t.hello_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 3,
        .session_epoch = 11,
        .request_id = 7,
    };
    const length = try encode_hello(&frame, hello, "1.2.3");
    var header: c_t.interop_header_t = undefined;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(&frame, c_t.InteropHeaderBytes, &header));
    const view = reader_mod_t.frame_view_t{
        .header = header,
        .frame = frame[0..length],
        .payload = frame[c_t.InteropHeaderBytes..length],
        .payload_validation = c_t.InteropErrorNone,
    };
    const parsed_hello = try parse_hello(view);
    try std_t.testing.expectEqual(@as(?u32, 1), parsed_hello.version_major);
    try std_t.testing.expectEqual(@as(u64, 11), parsed_hello.session_epoch);
    try std_t.testing.expectEqual(@as(u64, 3), try field_uint(view.payload, c_t.InteropTagCapabilities, 8));
    try std_t.testing.expectEqualStrings("1.2.3", try field_bytes(view.payload, c_t.InteropTagAgentVersion));
    try std_t.testing.expectError(error.MalformedField, field_uint(view.payload, 0xffff, 4));
    try std_t.testing.expectError(error.MalformedField, field_bytes(view.payload, 0xffff));
    try std_t.testing.expectError(error.MalformedField, field_uint(view.payload, c_t.InteropTagAgentVersion, 4));
    try std_t.testing.expectError(error.MalformedField, parse_hello(.{
        .header = header,
        .frame = frame[0..length],
        .payload = &.{},
        .payload_validation = c_t.InteropErrorNone,
    }));

    const version_field = try field_bytes(view.payload, c_t.InteropTagAgentVersion);
    frame[@intFromPtr(version_field.ptr) - @intFromPtr(&frame)] = 'x';
    const invalid_version_length = length;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(&frame, c_t.InteropHeaderBytes, &header));
    const invalid_version_view = reader_mod_t.frame_view_t{
        .header = header,
        .frame = frame[0..invalid_version_length],
        .payload = frame[c_t.InteropHeaderBytes..invalid_version_length],
        .payload_validation = c_t.InteropErrorNone,
    };
    try std_t.testing.expectEqual(@as(?u32, null), (try parse_hello(invalid_version_view)).version_major);

    const ack = handshake_mod_t.hello_ack_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 1,
        .request_id = 7,
    };
    const ack_length = try encode_hello_ack(&frame, ack, "1.0.0");
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(&frame, c_t.InteropHeaderBytes, &header));
    const ack_view = reader_mod_t.frame_view_t{
        .header = header,
        .frame = frame[0..ack_length],
        .payload = frame[c_t.InteropHeaderBytes..ack_length],
        .payload_validation = c_t.InteropErrorNone,
    };
    const parsed_ack = try parse_hello_ack(ack_view);
    try std_t.testing.expectEqual(@as(u32, 7), parsed_ack.request_id);
    try std_t.testing.expectEqual(@as(u64, 1), parsed_ack.capabilities);

    var capabilities_ptr: [*c]const u8 = null;
    var capabilities_length: u16 = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_field_get(ack_view.payload.ptr, ack_view.payload.len, c_t.InteropTagCapabilities, 0, &capabilities_ptr, &capabilities_length));
    const capability_tag_offset = @intFromPtr(capabilities_ptr) - @intFromPtr(&frame) - 4;
    const original_capability_tag = std_t.mem.readInt(u16, @ptrCast(frame[capability_tag_offset..].ptr), .little);
    std_t.mem.writeInt(u16, @ptrCast(frame[capability_tag_offset..].ptr), 0x7ffe, .little);
    const missing_capabilities_view = reader_mod_t.frame_view_t{
        .header = ack_view.header,
        .frame = frame[0..ack_length],
        .payload = frame[c_t.InteropHeaderBytes..ack_length],
        .payload_validation = c_t.InteropErrorNone,
    };
    try std_t.testing.expectError(error.MalformedField, parse_hello_ack(missing_capabilities_view));
    std_t.mem.writeInt(u16, @ptrCast(frame[capability_tag_offset..].ptr), original_capability_tag, .little);

    try std_t.testing.expectError(error.MalformedField, parse_hello_ack(.{
        .header = header,
        .frame = frame[0..ack_length],
        .payload = &.{},
        .payload_validation = c_t.InteropErrorNone,
    }));

    var short_frame: [c_t.InteropHeaderBytes]u8 = undefined;
    try std_t.testing.expectError(error.CodecFailure, encode_hello(&short_frame, hello, "1.2.3"));
    try std_t.testing.expectError(error.CodecFailure, encode_hello_ack(&short_frame, ack, "1.2.3"));
    const hello_failure_capacities = [_]usize{
        c_t.InteropHeaderBytes + 12,
        c_t.InteropHeaderBytes + 21,
        c_t.InteropHeaderBytes + 29,
    };
    for (hello_failure_capacities) |capacity| {
        const result = encode_hello(frame[0..capacity], hello, "1.2.3");
        try std_t.testing.expectError(error.CodecFailure, result);
    }
    const ack_failure_capacities = [_]usize{
        c_t.InteropHeaderBytes + 12,
        c_t.InteropHeaderBytes + 21,
    };
    for (ack_failure_capacities) |capacity| {
        const result = encode_hello_ack(frame[0..capacity], ack, "1.2.3");
        try std_t.testing.expectError(error.CodecFailure, result);
    }
    try std_t.testing.expectError(error.MalformedField, parse_hello(.{
        .header = header,
        .frame = &.{},
        .payload = &.{},
        .payload_validation = c_t.InteropErrorNone,
    }));
}

fn test_control_and_range_boundaries() !void {
    try std_t.testing.expect(is_control_type(c_t.InteropMsgHello));
    try std_t.testing.expect(is_control_type(c_t.InteropMsgError));
    try std_t.testing.expect(!is_control_type(c_t.InteropMsgCatalogBegin));
    try std_t.testing.expect(ranges_overlap(10, 20, 19, 21));
    try std_t.testing.expect(!ranges_overlap(10, 20, 20, 30));

    var peer = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 15,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer peer.deinit();
    try std_t.testing.expect(peer.output_aliases_peer(@ptrFromInt(std_t.math.maxInt(usize) - 2), 8));
    try std_t.testing.expect(!peer.output_aliases_peer(@ptrFromInt(std_t.math.maxInt(usize) - 2), 0));
}

fn test_private_handshake_dispatch_deadline() !void {
    var host = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 21,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer host.deinit();
    var frame: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const hello = handshake_mod_t.hello_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .session_epoch = 7,
        .request_id = 1,
    };
    const frame_length = try encode_hello(&frame, hello, "1.2.3");
    const feed_result = try host.feed(21, frame[0..frame_length]);
    try std_t.testing.expectEqual(frame_length, feed_result.consumed);
    const view = try host.reader.peek();
    var output: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const result = try host.dispatch_handshake(view, 21, handshake_mod_t.HelloTimeoutMs, &output);
    try std_t.testing.expectEqual(event_kind_t.Draining, result.event.kind);
    try std_t.testing.expectEqual(phase_t.Draining, host.phase());
    try std_t.testing.expectEqual(@as(usize, 0), host.pending_bytes());
}

fn test_private_handshake_error_and_malformed_payload() !void {
    var host = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 22,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer host.deinit();
    var frame: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    var writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&writer, &frame, frame.len);
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_put_uint(&writer, c_t.InteropTagErrorCode, c_t.InteropErrorUnexpected));
    var frame_length: usize = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_writer_finish(&writer, c_t.InteropMsgError, 9, c_t.InteropFromGuest, &frame_length));
    _ = try host.feed(22, frame[0..frame_length]);
    var output: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const remote_error = try host.next_event(22, 1, &output);
    try std_t.testing.expectEqual(event_kind_t.Draining, remote_error.event.kind);
    try std_t.testing.expect((try host.peek_tx(22)) == null);

    var malformed_host = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 23,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer malformed_host.deinit();
    const hello = handshake_mod_t.hello_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .session_epoch = 8,
        .request_id = 2,
    };
    frame_length = try encode_hello(&frame, hello, "1.2.3");
    frame[6] = @intCast(c_t.InteropMsgPing & 0xff);
    frame[7] = @intCast(c_t.InteropMsgPing >> 8);
    _ = try malformed_host.feed(23, frame[0..frame_length]);
    const malformed_view = try malformed_host.reader.peek();
    try std_t.testing.expect(malformed_view.payload_validation != c_t.InteropErrorNone);
    const malformed = try malformed_host.dispatch_handshake(malformed_view, 23, 1, &output);
    try std_t.testing.expectEqual(event_kind_t.Draining, malformed.event.kind);
    try std_t.testing.expect((try malformed_host.peek_tx(23)) != null);
}

/// Test deliberately corrupted scalar state without changing owned buffers or pointers.
/// @return No value after each impossible state fails closed and all allocations are released.
/// @errors Test assertion or allocator failure.
/// @ownership Peers are local testing-allocator values and are deinitialized before return.
/// @thread_safety Runs synchronously with no external callbacks.
fn test_private_internal_fault_containment() !void {
    var guest = try peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 31,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    _ = try guest.guest_mapping_initialized(31, 1, 9, true);
    const original_record = guest.send_records[guest.send_head];
    guest.send_records[guest.send_head].kind = .None;
    try std_t.testing.expectError(error.InternalState, guest.ack_tx(31, 2, 1));
    try std_t.testing.expectEqual(phase_t.AwaitHelloSent, guest.phase());
    try std_t.testing.expectEqual(@as(usize, 1), guest.send_count);
    guest.send_records[guest.send_head] = original_record;
    guest.deinit();

    var ready_host = try test_ready_host(32);
    const original_handshake_phase = ready_host.handshake.phase;
    ready_host.liveness = null;
    try std_t.testing.expectError(error.InternalState, ready_host.tick(32, 101));
    try std_t.testing.expectEqual(phase_t.Ready, ready_host.phase());
    try std_t.testing.expect(ready_host.liveness == null);
    ready_host.handshake.phase = original_handshake_phase;
    ready_host.liveness = try liveness_mod_t.session_liveness_t.init(.Host, 32, 101);
    ready_host.deinit();

    var stale_guest = try peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 33,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    _ = try stale_guest.guest_mapping_initialized(33, 1, 10, true);
    const stale_handshake_phase = stale_guest.handshake.phase;
    stale_guest.handshake.phase = .Ready;
    const queued_hello = try stale_guest.queue.peek();
    try std_t.testing.expectError(error.WrongPhase, stale_guest.ack_tx(33, 2, queued_hello.len));
    try std_t.testing.expectEqual(phase_t.AwaitHelloSent, stale_guest.phase());
    try std_t.testing.expectEqual(phase_t.Ready, stale_guest.handshake.phase);
    stale_guest.handshake.phase = stale_handshake_phase;
    stale_guest.deinit();
}

/// Build a Host peer that reached Ready through a valid Hello, mapping callback and full Ack send.
/// @param[in] token Nonzero token for the isolated test connection.
/// @return Ready Host value owning its copied version and empty queue.
/// @errors Allocation or codec failure during the valid setup exchange.
/// @ownership Caller owns the returned peer and must call deinit.
/// @thread_safety Setup is synchronous and the returned peer is not shared.
fn test_ready_host(token: u64) !peer_t {
    var host = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = token,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 3,
    });
    errdefer host.deinit();
    var frame: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const hello = handshake_mod_t.hello_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 3,
        .session_epoch = 19,
        .request_id = 7,
    };
    const hello_length = try encode_hello(&frame, hello, "1.2.3");
    _ = try host.feed(token, frame[0..hello_length]);
    var output: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const validate = try host.next_event(token, 100, &output);
    if (validate.event.kind != .ValidateMapping) return error.TestUnexpectedState;
    _ = try host.host_mapping_validated(token, 101, 19, c_t.InteropLayoutVersion, true);
    const ack = try host.queue.peek();
    const ready = try host.ack_tx(token, 101, ack.len);
    if (ready.kind != .Ready or host.phase() != .Ready) return error.TestUnexpectedState;
    return host;
}

/// Test fixed request-ID exhaustion through the allocator's real reserve API.
/// @return No value after Full leaves peer state unchanged or timer admission drains safely.
/// @errors Test assertion or allocator failure.
/// @ownership Values use the testing allocator and are deinitialized before return.
/// @thread_safety Calls are serialized on this test thread.
fn test_private_request_allocator_exhaustion() !void {
    var guest = try peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 34,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    for (0..request_ids_mod_t.RequestCapacity) |_| _ = try guest.request_ids.reserve();
    const before = guest.request_ids;
    try std_t.testing.expectError(error.RequestIdsFull, guest.guest_mapping_initialized(34, 1, 20, true));
    try std_t.testing.expectEqualDeep(before, guest.request_ids);
    try std_t.testing.expectEqual(phase_t.InitializingMapping, guest.phase());
    try std_t.testing.expectEqual(@as(usize, 0), guest.queue.count);
    guest.deinit();

    var host = try test_ready_host(35);
    for (0..request_ids_mod_t.RequestCapacity) |_| _ = try host.request_ids.reserve();
    const action = try host.tick(35, 2101);
    try std_t.testing.expectEqual(event_kind_t.Draining, action.kind);
    try std_t.testing.expectEqual(phase_t.Draining, host.phase());
    try std_t.testing.expectEqual(@as(usize, 0), host.queue.count);
    host.deinit();
}

/// Test codec helper argument boundaries through the real allocation-free C codec.
/// @return No value after each malformed scalar is rejected as CodecFailure.
/// @errors Test assertion failure.
/// @ownership Only stack frames and non-owning protocol facts are used.
/// @thread_safety Calls are synchronous and have no shared writer state.
fn test_private_encoder_and_field_boundaries() !void {
    var frame: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    var hello = handshake_mod_t.hello_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .session_epoch = 9,
        .request_id = 3,
    };
    try std_t.testing.expectError(error.CodecFailure, encode_hello(&frame, hello, "1.2.x"));
    hello = .{ .protocol_version = c_t.InteropVersion, .version_major = 1, .layout_version = c_t.InteropLayoutVersion, .capabilities = 1 << 63, .session_epoch = 9, .request_id = 3 };
    try std_t.testing.expectError(error.CodecFailure, encode_hello(&frame, hello, "1.2.3"));
    hello.capabilities = 0;
    hello.session_epoch = 0;
    try std_t.testing.expectError(error.CodecFailure, encode_hello(&frame, hello, "1.2.3"));
    hello.session_epoch = 9;
    hello.layout_version = 0;
    try std_t.testing.expectError(error.CodecFailure, encode_hello(&frame, hello, "1.2.3"));

    var ack = handshake_mod_t.hello_ack_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .request_id = 3,
    };
    try std_t.testing.expectError(error.CodecFailure, encode_hello_ack(&frame, ack, "1.2.x"));
    ack.capabilities = 1 << 63;
    try std_t.testing.expectError(error.CodecFailure, encode_hello_ack(&frame, ack, "1.2.3"));
    ack.capabilities = 0;
    ack.layout_version = 0;
    try std_t.testing.expectError(error.CodecFailure, encode_hello_ack(&frame, ack, "1.2.3"));

    const runtime_tag: u16 = c_t.InteropTagCapabilities;
    const runtime_width: u32 = 4;
    const runtime_empty_payload: []const u8 = &.{};
    try std_t.testing.expectError(error.MalformedField, field_uint(runtime_empty_payload, runtime_tag, runtime_width));
    try std_t.testing.expectError(error.MalformedField, field_bytes(runtime_empty_payload, c_t.InteropTagAgentVersion));
}

/// Test fault containment when a matched Pong ID was retired out of band.
/// @return No value after the ownership mismatch returns InternalState, never Ready.
/// @errors Test assertion, allocation, or codec failure.
/// @ownership One local peer and all queue allocations are released by deinit.
/// @thread_safety All synthetic callbacks are serialized.
fn test_private_missing_ping_release() !void {
    var host = try test_ready_host(36);
    _ = try host.tick(36, 2101);
    const ping = try host.queue.peek();
    var header: c_t.interop_header_t = undefined;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(ping.ptr, ping.len, &header));
    try host.request_ids.release(header.request_id);

    var pong: [c_t.InteropHeaderBytes]u8 = undefined;
    var writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&writer, &pong, pong.len);
    var pong_length: usize = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_writer_finish(&writer, c_t.InteropMsgPong, header.request_id, c_t.InteropFromGuest, &pong_length));
    _ = try host.feed(36, pong[0..pong_length]);
    var output: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    try std_t.testing.expectError(error.InternalState, host.next_event(36, 2101, &output));
    try std_t.testing.expectEqual(phase_t.Ready, host.phase());
    host.deinit();
}

/// Test private dispatch at exact Ready liveness expiry with a valid feature frame.
/// @return No value after the event drains and the feature bytes are not copied.
/// @errors Test assertion, allocation, or codec failure.
/// @ownership A local peer owns its version and queue until this test deinitializes it.
/// @thread_safety No concurrent callbacks are used.
fn test_private_ready_dispatch_timeout() !void {
    var host = try test_ready_host(37);
    defer host.deinit();
    var frame: [c_t.InteropHeaderBytes]u8 = undefined;
    var writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&writer, &frame, frame.len);
    var frame_length: usize = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_writer_finish(&writer, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest, &frame_length));
    _ = try host.feed(37, frame[0..frame_length]);
    var output: [queue_mod_t.MaxFrameBytes]u8 = [_]u8{0xA5} ** queue_mod_t.MaxFrameBytes;
    const result = try host.next_event(37, 101 + liveness_mod_t.LivenessTimeoutMs, &output);
    try std_t.testing.expectEqual(event_kind_t.Draining, result.event.kind);
    try std_t.testing.expectEqual(phase_t.Draining, host.phase());
    try std_t.testing.expectEqual(@as(u8, 0xA5), output[0]);
    try std_t.testing.expectEqual(frame_length, host.pending_bytes());
}

/// Test invalid construction inputs and closed-phase queue queries at the private boundary.
/// @return No value after all malformed scalar inputs are rejected without allocation.
/// @errors Test assertion failure.
/// @ownership No successful peer allocation escapes this function.
/// @thread_safety Test-local synchronous calls only.
fn test_private_init_and_closed_guards() !void {
    const base = config_t{
        .role = .Host,
        .connection_token = 40,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    };
    var invalid = base;
    invalid.connection_token = 0;
    try std_t.testing.expectError(error.InvalidConnectionToken, peer_t.init(std_t.testing.allocator, invalid));
    invalid = base;
    invalid.local_version = "1.2.x";
    try std_t.testing.expectError(error.InvalidVersion, peer_t.init(std_t.testing.allocator, invalid));
    invalid = base;
    invalid.local_capabilities = 1 << 63;
    try std_t.testing.expectError(error.UnknownCapabilities, peer_t.init(std_t.testing.allocator, invalid));

    var peer = try peer_t.init(std_t.testing.allocator, base);
    try std_t.testing.expectError(error.Empty, peer.queue.peek());
    peer.connection_phase = .Closed;
    try std_t.testing.expectError(error.WrongPhase, peer.peek_tx(40));
    peer.connection_phase = .AwaitHello;
    peer.deinit();
}

/// Test defensive handshake parser and ErrorAndDrain paths with valid owned reader frames.
/// @return No value after malformed private views and incompatible valid wire facts drain safely.
/// @errors Test assertion, allocation, or codec failure.
/// @ownership Peers and queues are local; custom views borrow only the live reader frame.
/// @thread_safety All direct dispatch calls are serialized on this thread.
fn test_private_handshake_dispatch_faults() !void {
    var hello_frame: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    var output: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const hello = handshake_mod_t.hello_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .session_epoch = 41,
        .request_id = 7,
    };
    const hello_length = try encode_hello(&hello_frame, hello, "1.2.3");
    var malformed_host = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 41,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    _ = try malformed_host.feed(41, hello_frame[0..hello_length]);
    const live_hello_view = try malformed_host.reader.peek();
    const malformed_hello_view = reader_mod_t.frame_view_t{
        .header = live_hello_view.header,
        .frame = live_hello_view.frame,
        .payload = &.{},
        .payload_validation = c_t.InteropErrorNone,
    };
    const malformed_hello = try malformed_host.dispatch_handshake(malformed_hello_view, 41, 100, &output);
    try std_t.testing.expectEqual(event_kind_t.Draining, malformed_hello.event.kind);
    try std_t.testing.expectEqual(phase_t.Draining, malformed_host.phase());
    try std_t.testing.expect((try malformed_host.peek_tx(41)) != null);
    malformed_host.deinit();

    var incompatible_host = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 42,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    var incompatible_hello = hello;
    incompatible_hello.version_major = 2;
    const incompatible_length = try encode_hello(&hello_frame, incompatible_hello, "2.0.0");
    _ = try incompatible_host.feed(42, hello_frame[0..incompatible_length]);
    const incompatible = try incompatible_host.next_event(42, 100, &output);
    try std_t.testing.expectEqual(event_kind_t.Draining, incompatible.event.kind);
    try std_t.testing.expectEqual(phase_t.Draining, incompatible_host.phase());
    try std_t.testing.expect((try incompatible_host.peek_tx(42)) != null);
    incompatible_host.deinit();

    var guest = try peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 43,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    _ = try guest.guest_mapping_initialized(43, 1, 43, true);
    const hello_tx = try guest.queue.peek();
    _ = try guest.ack_tx(43, 2, hello_tx.len);
    const ack = handshake_mod_t.hello_ack_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 2,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .request_id = guest.pending_hello_id,
    };
    const ack_length = try encode_hello_ack(&hello_frame, ack, "2.0.0");
    _ = try guest.feed(43, hello_frame[0..ack_length]);
    const rejected_ack = try guest.next_event(43, 100, &output);
    try std_t.testing.expectEqual(event_kind_t.Draining, rejected_ack.event.kind);
    try std_t.testing.expectEqual(phase_t.Draining, guest.phase());
    try std_t.testing.expect((try guest.peek_tx(43)) != null);
    guest.deinit();

    var malformed_guest = try peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 44,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    _ = try malformed_guest.guest_mapping_initialized(44, 1, 44, true);
    const pending_hello = try malformed_guest.queue.peek();
    _ = try malformed_guest.ack_tx(44, 2, pending_hello.len);
    const good_ack = handshake_mod_t.hello_ack_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .request_id = malformed_guest.pending_hello_id,
    };
    const good_ack_length = try encode_hello_ack(&hello_frame, good_ack, "1.2.3");
    _ = try malformed_guest.feed(44, hello_frame[0..good_ack_length]);
    const live_ack_view = try malformed_guest.reader.peek();
    const malformed_ack_view = reader_mod_t.frame_view_t{
        .header = live_ack_view.header,
        .frame = live_ack_view.frame,
        .payload = &.{},
        .payload_validation = c_t.InteropErrorNone,
    };
    const malformed_ack = try malformed_guest.dispatch_handshake(malformed_ack_view, 44, 100, &output);
    try std_t.testing.expectEqual(event_kind_t.Draining, malformed_ack.event.kind);
    try std_t.testing.expectEqual(phase_t.Draining, malformed_guest.phase());
    malformed_guest.deinit();
}

/// Test drain idempotence and invalid private control-builder arguments.
/// @return No value after an already-terminal peer remains terminal and builders fail safely.
/// @errors Test assertion or allocation failure.
/// @ownership The local peer is deinitialized after all scalar state is restored.
/// @thread_safety Synchronous calls only.
fn test_private_terminal_builder_guards() !void {
    var peer = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 45,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    try std_t.testing.expect(!peer.enqueue_empty_control(0xffff, 1, .Ping));
    try std_t.testing.expect(!peer.enqueue_error(0x7fff_ffff, 1, null));
    const original_phase = peer.connection_phase;
    const original_reason = peer.handshake.drain_reason;
    const original_time = peer.last_now_ms;
    peer.abort_transport = true;
    try std_t.testing.expect(!peer.enqueue_error(c_t.InteropErrorProtocolViolation, 1, null));
    peer.abort_transport = false;
    peer.enter_draining_at(10, .LocalRequest);
    const first_reason = peer.handshake.drain_reason;
    peer.enter_draining_at(11, .WireError);
    try std_t.testing.expectEqual(phase_t.Draining, peer.phase());
    try std_t.testing.expectEqual(first_reason, peer.handshake.drain_reason);
    peer.connection_phase = original_phase;
    peer.handshake.drain_reason = original_reason;
    peer.last_now_ms = original_time;
    peer.deinit();
}

/// Exercise bounded helper branches using valid owned allocations and scalar lifecycle changes.
/// @return No value after helper boundaries preserve their documented results.
/// @errors Test assertion or allocator failure.
/// @ownership The peer owns its reader, version, and queue until deinit; temporary frames are stack-owned.
/// @thread_safety Synchronous private test probe with no shared state.
fn test_private_bounded_helper_edges() !void {
    var peer = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 46,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer peer.deinit();
    var frame: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const hello = handshake_mod_t.hello_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .session_epoch = 7,
        .request_id = 8,
    };
    const hello_length = try encode_hello(&frame, hello, "1.2.3");
    try std_t.testing.expectEqual(@as(usize, 0), peer.pending_bytes());
    _ = try peer.feed(46, frame[0..1]);
    try std_t.testing.expectEqual(@as(usize, 0), peer.pending_bytes());
    _ = try peer.feed(46, frame[1..hello_length]);
    try std_t.testing.expectEqual(hello_length, @call(.never_inline, peer_t.pending_bytes, .{&peer}));
    const payload = frame[c_t.InteropHeaderBytes..hello_length];
    const capabilities = try field_uint(payload, c_t.InteropTagCapabilities, 8);
    try std_t.testing.expectEqual(@as(u64, 0), capabilities);
    const version = try field_bytes(payload, c_t.InteropTagAgentVersion);
    try std_t.testing.expectEqualStrings("1.2.3", version);

    var too_small: [1]u8 = undefined;
    try std_t.testing.expectError(error.CodecFailure, encode_hello(&too_small, hello, "1.2.3"));
    const hello_full_length = try encode_hello(&frame, hello, "1.2.3");
    for (0..hello_full_length) |capacity| {
        try std_t.testing.expectError(error.CodecFailure, encode_hello(frame[0..capacity], hello, "1.2.3"));
    }
    var oversized_version: [1024]u8 = undefined;
    @memset(&oversized_version, 'x');
    try std_t.testing.expectError(error.CodecFailure, @call(.never_inline, encode_hello, .{ &frame, hello, &oversized_version }));
    const ack = handshake_mod_t.hello_ack_out_t{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .request_id = 8,
    };
    try std_t.testing.expectError(error.CodecFailure, encode_hello_ack(&too_small, ack, "1.2.3"));
    const ack_full_length = try encode_hello_ack(&frame, ack, "1.2.3");
    for (0..ack_full_length) |capacity| {
        try std_t.testing.expectError(error.CodecFailure, encode_hello_ack(frame[0..capacity], ack, "1.2.3"));
    }
    try std_t.testing.expectError(error.CodecFailure, @call(.never_inline, encode_hello_ack, .{ &frame, ack, &oversized_version }));

    const zero_alias_length: usize = 0;
    try std_t.testing.expect(!@call(.never_inline, peer_t.output_aliases_peer, .{ &peer, @as([*]u8, @ptrCast(&too_small)), zero_alias_length }));
    var separate_output: [4]u8 = undefined;
    try std_t.testing.expect(!@call(.never_inline, peer_t.output_aliases_peer, .{ &peer, &separate_output, separate_output.len }));
    try std_t.testing.expect(peer.enqueue_goodbye());
    const queued = peer.queue.frames[peer.queue.head].?.bytes;
    try std_t.testing.expect(@call(.never_inline, peer_t.output_aliases_peer, .{ &peer, queued.ptr, queued.len }));

    const old_phase = peer.connection_phase;
    const old_time = peer.last_now_ms;
    peer.connection_phase = .Closed;
    peer.enter_draining_at(old_time + 1, .WireError);
    try std_t.testing.expectEqual(phase_t.Closed, peer.connection_phase);
    try std_t.testing.expectEqual(old_time, peer.last_now_ms);
    peer.connection_phase = old_phase;
    const failed = peer.fail_internal(old_time + 2);
    try std_t.testing.expectEqual(event_kind_t.Draining, failed.kind);
    try std_t.testing.expectEqual(phase_t.Draining, peer.phase());

    var ready = try test_ready_host(47);
    defer ready.deinit();
    const live = ready.liveness.?;
    const before_tick = ready.last_now_ms;
    const no_ping = try ready.tick(47, before_tick + 1);
    try std_t.testing.expectEqual(event_kind_t.None, no_ping.kind);
    const ping_due_at = before_tick + liveness_mod_t.PingIntervalMs;
    _ = try ready.tick(47, ping_due_at);
    const queued_ping = try ready.queue.peek();
    var unrelated_ready_output: [4]u8 = undefined;
    try std_t.testing.expect(!@call(.never_inline, peer_t.output_aliases_peer, .{ &ready, &unrelated_ready_output, unrelated_ready_output.len }));
    const queued_ping_record = ready.queue.frames[ready.queue.head].?.bytes;
    try std_t.testing.expect(@call(.never_inline, peer_t.output_aliases_peer, .{ &ready, queued_ping_record.ptr, queued_ping_record.len }));
    ready.liveness = null;
    try std_t.testing.expectError(error.InternalState, ready.ack_tx(47, ping_due_at + 1, 1));
    ready.liveness = live;
    try std_t.testing.expectEqual(queued_ping.len - 1, (try ready.queue.peek()).len);

    var timeout_host = try test_ready_host(50);
    defer timeout_host.deinit();
    const disconnect = try timeout_host.tick(50, timeout_host.last_now_ms + liveness_mod_t.LivenessTimeoutMs);
    try std_t.testing.expectEqual(event_kind_t.Draining, disconnect.kind);
    try std_t.testing.expectEqual(phase_t.Draining, timeout_host.phase());

    var tick_without_liveness = try test_ready_host(62);
    defer tick_without_liveness.deinit();
    const tick_time_before_fault = tick_without_liveness.last_now_ms;
    const saved_tick_liveness = tick_without_liveness.liveness;
    tick_without_liveness.liveness = null;
    try std_t.testing.expectError(error.InternalState, tick_without_liveness.tick(62, tick_time_before_fault + 1));
    tick_without_liveness.liveness = saved_tick_liveness;
    try std_t.testing.expectEqual(tick_time_before_fault, tick_without_liveness.last_now_ms);

    var ack_without_liveness = try test_ready_host(63);
    defer ack_without_liveness.deinit();
    try std_t.testing.expect(ack_without_liveness.enqueue_empty_control(c_t.InteropMsgPing, 700, .Ping));
    var wrong_token_live = ack_without_liveness.liveness.?;
    wrong_token_live.connection_token += 1;
    ack_without_liveness.liveness = wrong_token_live;
    try std_t.testing.expectError(error.WrongConnectionToken, ack_without_liveness.ack_tx(63, ack_without_liveness.last_now_ms + 1, 1));

    var ack_release_mismatch = try test_ready_host(64);
    defer ack_release_mismatch.deinit();
    try std_t.testing.expect(ack_release_mismatch.enqueue_empty_control(c_t.InteropMsgPing, 701, .Ping));
    var release_live = ack_release_mismatch.liveness.?;
    const ping_at = ack_release_mismatch.last_now_ms + liveness_mod_t.PingIntervalMs;
    _ = try release_live.register_ping(64, ping_at, 702);
    const ping_expiry = ping_at + liveness_mod_t.LivenessTimeoutMs;
    release_live.last_valid_rx_ms = ping_expiry;
    ack_release_mismatch.liveness = release_live;
    ack_release_mismatch.last_now_ms = ping_at;
    try std_t.testing.expectError(error.InternalState, ack_release_mismatch.ack_tx(64, ping_expiry, 1));

    var full_pings = try test_ready_host(58);
    defer full_pings.deinit();
    var saturated = full_pings.liveness.?;
    const registration_at = full_pings.last_now_ms + liveness_mod_t.PingIntervalMs;
    for (0..liveness_mod_t.MaxOutstandingPings) |index| {
        saturated.last_registered_ping_ms = registration_at - liveness_mod_t.PingIntervalMs;
        _ = try saturated.register_ping(58, registration_at, @intCast(100 + index));
    }
    saturated.last_valid_rx_ms = registration_at;
    full_pings.liveness = saturated;
    const full_result = try full_pings.tick(58, registration_at + liveness_mod_t.PingIntervalMs);
    try std_t.testing.expectEqual(event_kind_t.Draining, full_result.kind);
    try std_t.testing.expectEqual(phase_t.Draining, full_pings.phase());

    var saturated_control = try test_ready_host(61);
    defer saturated_control.deinit();
    var queued_controls: usize = 0;
    while (queued_controls < QueueCapacity and saturated_control.enqueue_empty_control(c_t.InteropMsgPing, 1, .Ping)) : (queued_controls += 1) {}
    try std_t.testing.expect(queued_controls != 0);
    try std_t.testing.expect(!saturated_control.enqueue_empty_control(c_t.InteropMsgPing, 1, .Ping));
    var replayed_ready_hello: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const replayed_hello_length = try encode_hello(&replayed_ready_hello, hello, "1.2.3");
    _ = try saturated_control.feed(61, replayed_ready_hello[0..replayed_hello_length]);
    var saturated_output: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const rejected_replay = try saturated_control.next_event(61, saturated_control.last_now_ms, &saturated_output);
    try std_t.testing.expectEqual(event_kind_t.Draining, rejected_replay.event.kind);
    try std_t.testing.expect(saturated_control.terminal_send_failed);

    var mismatched_release = try test_ready_host(59);
    defer mismatched_release.deinit();
    var untracked = mismatched_release.liveness.?;
    const untracked_at = mismatched_release.last_now_ms + liveness_mod_t.PingIntervalMs;
    _ = try untracked.register_ping(59, untracked_at, 600);
    mismatched_release.liveness = untracked;
    try std_t.testing.expectError(error.InternalState, mismatched_release.tick(59, untracked_at + liveness_mod_t.LivenessTimeoutMs));

    var missing_live = try test_ready_host(48);
    defer missing_live.deinit();
    var control_frame: [c_t.InteropHeaderBytes]u8 = undefined;
    var control_writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&control_writer, &control_frame, control_frame.len);
    var control_length: usize = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_writer_finish(&control_writer, c_t.InteropMsgPong, 3, c_t.InteropFromGuest, &control_length));
    _ = try missing_live.feed(48, control_frame[0..control_length]);
    const control_view = try missing_live.reader.peek();
    const saved_live = missing_live.liveness;
    missing_live.liveness = null;
    try std_t.testing.expectError(error.InternalState, missing_live.dispatch_ready(control_view, 48, missing_live.last_now_ms, &frame));
    missing_live.liveness = saved_live;

    var missing_live_feature = try test_ready_host(49);
    defer missing_live_feature.deinit();
    var feature_frame: [c_t.InteropHeaderBytes]u8 = undefined;
    var feature_writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&feature_writer, &feature_frame, feature_frame.len);
    var feature_length: usize = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_writer_finish(&feature_writer, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest, &feature_length));
    _ = try missing_live_feature.feed(49, feature_frame[0..feature_length]);
    const feature_view = try missing_live_feature.reader.peek();
    const saved_feature_live = missing_live_feature.liveness;
    missing_live_feature.liveness = null;
    try std_t.testing.expectError(error.InternalState, missing_live_feature.dispatch_ready(feature_view, 49, missing_live_feature.last_now_ms, &frame));
    missing_live_feature.liveness = saved_feature_live;

    var eof_peer = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 51,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer eof_peer.deinit();
    try eof_peer.reader.finish_input();
    try std_t.testing.expectError(error.InputClosed, eof_peer.feed(51, &.{}));
    try std_t.testing.expectError(error.InputClosed, eof_peer.next_event(51, 0, &frame));

    var failed_reader = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 52,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer failed_reader.deinit();
    failed_reader.reader.failed = true;
    try std_t.testing.expectError(error.ReaderFailed, failed_reader.feed(52, &.{}));
    try std_t.testing.expectError(error.ReaderFailed, failed_reader.next_event(52, 0, &frame));

    var stale_hello = try peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 53,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer stale_hello.deinit();
    _ = try stale_hello.guest_mapping_initialized(53, 1, 7, true);
    const hello_tx = try stale_hello.queue.peek();
    const saved_hello_handshake = stale_hello.handshake;
    stale_hello.handshake.phase = .Ready;
    try std_t.testing.expectError(error.WrongPhase, stale_hello.ack_tx(53, 2, hello_tx.len));
    stale_hello.handshake = saved_hello_handshake;
    try std_t.testing.expectEqual(@as(usize, 0), stale_hello.send_count);

    var stale_guest_mapping = try peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 56,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer stale_guest_mapping.deinit();
    const saved_guest_mapping_handshake = stale_guest_mapping.handshake;
    stale_guest_mapping.handshake.phase = .Ready;
    try std_t.testing.expectError(error.WrongPhase, stale_guest_mapping.guest_mapping_initialized(56, 1, 16, true));
    stale_guest_mapping.handshake = saved_guest_mapping_handshake;

    var stale_host_mapping = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 57,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer stale_host_mapping.deinit();
    var mapping_frame: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    var mapping_output: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const mapping_hello = try encode_hello(&mapping_frame, .{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .session_epoch = 13,
        .request_id = 7,
    }, "1.2.3");
    _ = try stale_host_mapping.feed(57, mapping_frame[0..mapping_hello]);
    _ = try stale_host_mapping.next_event(57, 1, &mapping_output);
    const saved_host_mapping_handshake = stale_host_mapping.handshake;
    stale_host_mapping.handshake.phase = .Ready;
    try std_t.testing.expectError(error.WrongPhase, stale_host_mapping.host_mapping_validated(57, 2, 13, c_t.InteropLayoutVersion, true));
    stale_host_mapping.handshake = saved_host_mapping_handshake;

    var stale_ack = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 54,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer stale_ack.deinit();
    var incoming: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const incoming_hello = try encode_hello(&incoming, .{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .session_epoch = 12,
        .request_id = 6,
    }, "1.2.3");
    _ = try stale_ack.feed(54, incoming[0..incoming_hello]);
    var event_output: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    _ = try stale_ack.next_event(54, 1, &event_output);
    _ = try stale_ack.host_mapping_validated(54, 2, 12, c_t.InteropLayoutVersion, true);
    const ack_tx = try stale_ack.queue.peek();
    const saved_ack_handshake = stale_ack.handshake;
    stale_ack.handshake.phase = .Ready;
    try std_t.testing.expectError(error.WrongPhase, stale_ack.ack_tx(54, 3, ack_tx.len));
    stale_ack.handshake = saved_ack_handshake;
    try std_t.testing.expectEqual(@as(usize, 0), stale_ack.send_count);

    var missing_hello_id = try peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 55,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer missing_hello_id.deinit();
    _ = try missing_hello_id.guest_mapping_initialized(55, 1, 15, true);
    const hello_to_send = try missing_hello_id.queue.peek();
    _ = try missing_hello_id.ack_tx(55, 2, hello_to_send.len);
    const pending_id = missing_hello_id.pending_hello_id;
    const saved_ids = missing_hello_id.request_ids;
    const saved_handshake = missing_hello_id.handshake;
    const saved_phase = missing_hello_id.connection_phase;
    const saved_time = missing_hello_id.last_now_ms;
    try missing_hello_id.request_ids.release(pending_id);
    var ack_frame: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const ack_length = try encode_hello_ack(&ack_frame, .{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .request_id = pending_id,
    }, "1.2.3");
    _ = try missing_hello_id.feed(55, ack_frame[0..ack_length]);
    var copied: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    try std_t.testing.expectError(error.InternalState, missing_hello_id.next_event(55, 3, &copied));
    missing_hello_id.request_ids = saved_ids;
    missing_hello_id.handshake = saved_handshake;
    missing_hello_id.connection_phase = saved_phase;
    missing_hello_id.last_now_ms = saved_time;
    missing_hello_id.pending_hello_id = pending_id;

    var full_guest_mapping = try peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = 65,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer full_guest_mapping.deinit();
    var queued_guest_controls: usize = 0;
    while (queued_guest_controls < QueueCapacity and full_guest_mapping.enqueue_empty_control(c_t.InteropMsgPong, 1, .Pong)) : (queued_guest_controls += 1) {}
    try std_t.testing.expectEqual(QueueCapacity, queued_guest_controls);
    const guest_capacity_result = try @call(.never_inline, peer_t.guest_mapping_initialized, .{ &full_guest_mapping, 65, 1, 16, true });
    try std_t.testing.expectEqual(event_kind_t.Draining, guest_capacity_result.kind);
    try std_t.testing.expect(full_guest_mapping.terminal_send_failed);

    var full_host_mapping = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 66,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer full_host_mapping.deinit();
    var mapping_input: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const mapping_input_length = try encode_hello(&mapping_input, .{
        .protocol_version = c_t.InteropVersion,
        .version_major = 1,
        .layout_version = c_t.InteropLayoutVersion,
        .capabilities = 0,
        .session_epoch = 17,
        .request_id = 8,
    }, "1.2.3");
    _ = try full_host_mapping.feed(66, mapping_input[0..mapping_input_length]);
    var mapping_event_output: [queue_mod_t.MaxFrameBytes]u8 = undefined;
    const mapping_request = try full_host_mapping.next_event(66, 1, &mapping_event_output);
    try std_t.testing.expectEqual(event_kind_t.ValidateMapping, mapping_request.event.kind);
    var queued_host_controls: usize = 0;
    while (queued_host_controls < QueueCapacity and full_host_mapping.enqueue_empty_control(c_t.InteropMsgPing, 1, .Ping)) : (queued_host_controls += 1) {}
    try std_t.testing.expectEqual(QueueCapacity, queued_host_controls);
    const host_capacity_result = try @call(.never_inline, peer_t.host_mapping_validated, .{ &full_host_mapping, 66, 2, 17, c_t.InteropLayoutVersion, true });
    try std_t.testing.expectEqual(event_kind_t.Draining, host_capacity_result.kind);
    try std_t.testing.expect(full_host_mapping.terminal_send_failed);

    var handshake_deadline_peer = try peer_t.init(std_t.testing.allocator, .{
        .role = .Host,
        .connection_token = 67,
        .accepted_at_ms = 0,
        .local_version = "1.2.3",
        .local_capabilities = 0,
    });
    defer handshake_deadline_peer.deinit();
    var deadline_value = handshake_mod_t.HelloTimeoutMs;
    const runtime_deadline_pointer: *volatile u64 = &deadline_value;
    const runtime_deadline = runtime_deadline_pointer.*;
    const deadline_result = try @call(.never_inline, peer_t.tick, .{ &handshake_deadline_peer, 67, runtime_deadline });
    try std_t.testing.expectEqual(event_kind_t.Draining, deadline_result.kind);
}

/// Test the facility table for every frozen feature type and the admission-error mapping.
/// @return No value after control, local and unknown types are null and each facility rule holds.
/// @errors Test assertion failure.
/// @ownership Pure scalar checks; no allocation.
/// @thread_safety Synchronous test-only probe.
fn test_private_feature_requirement_table() !void {
    // Sweep every u16 through the never-inlined table so each prong runs at runtime with a
    // non-folded selector. IMPL_DESC 3.2.5 lists 70 feature types (6 control, 3 local excluded).
    var feature_types: usize = 0;
    for (0..0x1_0000) |value| {
        if (@call(.never_inline, feature_requirement, .{@as(u16, @intCast(value))})) |_| feature_types += 1;
    }
    try std_t.testing.expectEqual(@as(usize, 70), feature_types);
    const control_types = [_]u16{ c_t.InteropMsgHello, c_t.InteropMsgHelloAck, c_t.InteropMsgGoodbye, c_t.InteropMsgPing, c_t.InteropMsgPong, c_t.InteropMsgError, 0x7F00, 0x7F01, 0x7F02, 0x0000 };
    for (control_types) |message_type| try std_t.testing.expectEqual(@as(?feature_requirement_t, null), @call(.never_inline, feature_requirement, .{message_type}));
    try std_t.testing.expect(is_local_type(0x7F01));
    try std_t.testing.expect(!is_local_type(0x0700));
    const window = feature_requirement(c_t.InteropMsgWindowClose).?;
    try std_t.testing.expect(window.satisfied_by(CapWindows));
    try std_t.testing.expect(!window.satisfied_by(CapInput));
    const pointer = feature_requirement(c_t.InteropMsgInputPointerRelative).?;
    try std_t.testing.expect(!pointer.satisfied_by(CapInput));
    try std_t.testing.expect(pointer.satisfied_by(CapInput | CapRelativePointer));
    const transfer = feature_requirement(c_t.InteropMsgTransferCancel).?;
    try std_t.testing.expect(!transfer.satisfied_by(CapWindows | CapInput));
    try std_t.testing.expect(transfer.satisfied_by(CapCatalog));
    try std_t.testing.expect(transfer.satisfied_by(CapUacNative | CapWindows));
    try std_t.testing.expect(!feature_requirement(c_t.InteropMsgCatalogBegin).?.satisfied_by(CapWindows));
    try std_t.testing.expectEqual(error.QueueFull, @call(.never_inline, map_admission_error, .{error.DataCapacityReserved}));
    try std_t.testing.expectEqual(error.QueueFull, @call(.never_inline, map_admission_error, .{error.QueueFull}));
    try std_t.testing.expectEqual(error.OutOfMemory, @call(.never_inline, map_admission_error, .{error.OutOfMemory}));
    try std_t.testing.expectEqual(error.PeerClosed, @call(.never_inline, map_admission_error, .{error.QueueClosed}));
    try std_t.testing.expectEqual(error.UnsupportedFrame, @call(.never_inline, map_admission_error, .{error.UnsupportedFrame}));
    try std_t.testing.expectEqual(error.InvalidFrame, @call(.never_inline, map_admission_error, .{error.InvalidPayload}));
    try std_t.testing.expectEqual(error.InvalidFrame, @call(.never_inline, map_admission_error, .{error.InvalidFrame}));
}

/// Test-only entry point exposing private production-boundary probes to named test roots.
/// @ownership This type and its methods exist only when Zig is compiling tests.
/// @thread_safety Tests call the probes synchronously on their current test thread.
pub const test_support_t = if (@import("builtin").is_test) struct {
    /// Run private error-boundary probes so named-root coverage includes production helpers.
    /// @return No value after every private probe succeeds.
    /// @errors Test assertion or allocation failure; never mutates external resources.
    /// @ownership Tests use only `std.testing.allocator` and release all peer values.
    /// @thread_safety Call synchronously; probes do not share peer state.
    pub fn run_all() !void {
        try test_initialization_allocation_failure();
        try test_initialized_peer_release();
        try test_zero_progress_deadline_abort();
        try test_allocator_failure_sweep();
        try test_output_alias_detection();
        try test_codec_helpers();
        try test_control_and_range_boundaries();
        try test_private_handshake_dispatch_deadline();
        try test_private_handshake_error_and_malformed_payload();
        try test_private_internal_fault_containment();
        try test_private_request_allocator_exhaustion();
        try test_private_encoder_and_field_boundaries();
        try test_private_missing_ping_release();
        try test_private_ready_dispatch_timeout();
        try test_private_init_and_closed_guards();
        try test_private_handshake_dispatch_faults();
        try test_private_terminal_builder_guards();
        try test_private_bounded_helper_edges();
        try test_private_feature_requirement_table();
    }
} else struct {};

test "private peer-boundary test support" {
    try test_support_t.run_all();
}
