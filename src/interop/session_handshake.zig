const std_t = @import("std");
const c_t = @cImport({
    @cInclude("waddle/interop_protocol.h");
});

/// Allocation-free protocol-v2 connected-peer negotiation and local drain machine.
/// @ownership Caller owns the value; no pointer or OS/mapping resource is retained.
/// @thread_safety All calls for an instance are serialized by one event-loop thread.
pub const session_handshake_t = struct {
    /// Endpoint role for this connection.
    role: role_t,
    /// Current negotiation or teardown phase.
    phase: phase_t,
    /// Why negotiation was drained; null until the first drain event.
    drain_reason: ?drain_reason_t = null,
    /// Fresh nonzero token binding delayed events to this exact connection.
    connection_token: u64,
    /// Local parsed semantic-version major; zero is valid.
    local_version_major: u32,
    /// Local advertised capability mask, restricted to known bits.
    local_capabilities: u64,
    /// Monotonic time when transport acceptance began the overall deadline.
    accepted_at_ms: u64,
    /// Most recent accepted monotonic timestamp.
    last_now_ms: u64,
    /// Staged peer epoch; not externally ready until phase Ready.
    staged_epoch: u64 = 0,
    /// Staged peer capability offer; not externally ready until phase Ready.
    staged_capabilities: u64 = 0,
    /// Staged Hello request identifier.
    staged_request_id: u32 = 0,
    /// Committed epoch; cleared on drain.
    session_epoch: u64 = 0,
    /// Committed capabilities; cleared on drain.
    negotiated_capabilities: u64 = 0,

    /// Initialize one fresh Host or Guest connection machine.
    /// @param[in] role Endpoint role.
    /// @param[in] connection_token Nonzero token unique to this connection.
    /// @param[in] local_version_major Parsed local semantic-version major.
    /// @param[in] local_capabilities Known capability bits offered locally.
    /// @param[in] accepted_at_ms Monotonic accepted-transport timestamp in ms.
    /// @return Initialized Host/AwaitHello or Guest/InitializingMapping state.
    /// @errors InvalidConnectionToken for zero token; UnknownCapabilities for
    /// bits outside InteropCapabilityMask.
    /// @ownership Returned value is caller-owned; no allocation occurs.
    /// @thread_safety Caller serializes all subsequent events.
    pub fn init(
        role: role_t,
        connection_token: u64,
        local_version_major: u32,
        local_capabilities: u64,
        accepted_at_ms: u64,
    ) handshake_error_t!session_handshake_t {
        if (connection_token == 0) return error.InvalidConnectionToken;
        if (!known_capabilities(local_capabilities)) return error.UnknownCapabilities;
        return .{
            .role = role,
            .phase = if (role == .Host) .AwaitHello else .InitializingMapping,
            .connection_token = connection_token,
            .local_version_major = local_version_major,
            .local_capabilities = local_capabilities,
            .accepted_at_ms = accepted_at_ms,
            .last_now_ms = accepted_at_ms,
        };
    }

    /// Accept fully decoded guest Hello facts on the Host.
    /// @param[in,out] self Host state; commits staged facts only after validation.
    /// @param[in] connection_token Token captured by the frame connection.
    /// @param[in] now_ms Current monotonic time in ms.
    /// @param[in] hello Typed facts from a complete codec-validated Hello.
    /// @return None on staging, ErrorAndDrain for wire incompatibility, Drain
    /// when the overall deadline has expired, or local WrongPhase on Closed.
    /// @errors WrongConnectionToken or TimeWentBackwards without mutation.
    /// @ownership Facts are copied; no borrowed payload is retained.
    /// @thread_safety Caller serializes frame dispatch.
    pub fn receive_hello(
        self: *session_handshake_t,
        connection_token: u64,
        now_ms: u64,
        hello: hello_facts_t,
    ) handshake_error_t!action_t {
        if (try self.event_expired(connection_token, now_ms)) return .Drain;
        if (self.phase == .Closed) return error.WrongPhase;
        if (self.role != .Host or self.phase != .AwaitHello) {
            self.commit_time(now_ms);
            return self.error_and_drain(.ProtocolViolation);
        }
        if (hello.protocol_version != ProtocolVersion or hello.request_id == 0 or
            hello.session_epoch == 0 or !known_capabilities(hello.capabilities))
        {
            self.commit_time(now_ms);
            return self.error_and_drain(.ProtocolViolation);
        }
        if (hello.version_major == null or hello.version_major.? != self.local_version_major or
            hello.layout_version != LayoutVersion)
        {
            self.commit_time(now_ms);
            return self.error_and_drain(.VersionMismatch);
        }
        self.commit_time(now_ms);
        self.staged_epoch = hello.session_epoch;
        self.staged_capabilities = hello.capabilities;
        self.staged_request_id = hello.request_id;
        self.phase = .AwaitMapping;
        return .None;
    }

    /// Accept the mapping validator result for the staged Hello epoch.
    /// @param[in,out] self Host state in AwaitMapping.
    /// @param[in] connection_token Token captured by the validation request.
    /// @param[in] now_ms Current monotonic time in ms.
    /// @param[in] session_epoch Epoch actually validated.
    /// @param[in] layout_version Validated mapping layout version.
    /// @param[in] validation_succeeded True only after full layout validation.
    /// @return SendHelloAck with host-computed intersection, ErrorAndDrain on
    /// failed proof, or Drain on timeout.
    /// @errors WrongConnectionToken, TimeWentBackwards, or WrongPhase without mutation.
    /// @ownership Returned action is scalar-only; mapping is never retained.
    /// @thread_safety Caller serializes validation completion.
    pub fn mapping_validated(
        self: *session_handshake_t,
        connection_token: u64,
        now_ms: u64,
        session_epoch: u64,
        layout_version: u32,
        validation_succeeded: bool,
    ) handshake_error_t!action_t {
        if (try self.event_expired(connection_token, now_ms)) return .Drain;
        if (self.role != .Host or self.phase != .AwaitMapping) return error.WrongPhase;
        if (!validation_succeeded or session_epoch != self.staged_epoch or layout_version != LayoutVersion) {
            self.commit_time(now_ms);
            return self.error_and_drain(.ProtocolViolation);
        }
        self.commit_time(now_ms);
        self.phase = .AwaitHelloAckSent;
        return .{ .SendHelloAck = .{
            .protocol_version = ProtocolVersion,
            .version_major = self.local_version_major,
            .layout_version = LayoutVersion,
            .capabilities = self.staged_capabilities & self.local_capabilities,
            .request_id = self.staged_request_id,
        } };
    }

    /// Stage guest Hello after successful external layout initialization.
    /// @param[in,out] self Guest state in InitializingMapping.
    /// @param[in] connection_token Token captured by the initializer.
    /// @param[in] now_ms Current monotonic time in ms.
    /// @param[in] initialized True only after successful full-layout initialization.
    /// @param[in] session_epoch Fresh nonzero initialized mapping epoch.
    /// @param[in] request_id Caller-allocated nonzero Hello request ID.
    /// @return SendHello containing copied fields, or Drain on timeout.
    /// @errors WrongConnectionToken, TimeWentBackwards, WrongPhase,
    /// MappingInitializationFailed, InvalidSessionEpoch, or InvalidRequestId;
    /// local errors preserve all machine state.
    /// @ownership Returned fields are copied; caller retains mapping ownership.
    /// @thread_safety Caller serializes initializer completion.
    pub fn mapping_initialized(
        self: *session_handshake_t,
        connection_token: u64,
        now_ms: u64,
        initialized: bool,
        session_epoch: u64,
        request_id: u32,
    ) handshake_error_t!action_t {
        if (try self.event_expired(connection_token, now_ms)) return .Drain;
        if (self.role != .Guest or self.phase != .InitializingMapping) return error.WrongPhase;
        if (!initialized) return error.MappingInitializationFailed;
        if (session_epoch == 0) return error.InvalidSessionEpoch;
        if (request_id == 0) return error.InvalidRequestId;
        self.commit_time(now_ms);
        self.staged_epoch = session_epoch;
        self.staged_capabilities = self.local_capabilities;
        self.staged_request_id = request_id;
        self.phase = .AwaitHelloSent;
        return .{ .SendHello = .{
            .protocol_version = ProtocolVersion,
            .version_major = self.local_version_major,
            .layout_version = LayoutVersion,
            .capabilities = self.local_capabilities,
            .session_epoch = session_epoch,
            .request_id = request_id,
        } };
    }

    /// Confirm complete Hello transmission; queueing or partial send is insufficient.
    /// @param[in,out] self Guest state in AwaitHelloSent.
    /// @param[in] connection_token Token captured by the transport send.
    /// @param[in] now_ms Current monotonic time in ms.
    /// @param[in] request_id ID of the completely sent Hello.
    /// @return None after entering AwaitHelloAck, or Drain on timeout.
    /// @errors WrongConnectionToken, TimeWentBackwards, WrongPhase, or WrongRequestId
    /// without mutation.
    /// @ownership No frame storage is retained.
    /// @thread_safety Caller serializes send-completion events.
    pub fn hello_sent(
        self: *session_handshake_t,
        connection_token: u64,
        now_ms: u64,
        request_id: u32,
    ) handshake_error_t!action_t {
        if (try self.event_expired(connection_token, now_ms)) return .Drain;
        if (self.role != .Guest or self.phase != .AwaitHelloSent) return error.WrongPhase;
        if (request_id != self.staged_request_id) return error.WrongRequestId;
        self.commit_time(now_ms);
        self.phase = .AwaitHelloAck;
        return .None;
    }

    /// Accept fully decoded host HelloAck facts on the Guest.
    /// @param[in,out] self Guest state in AwaitHelloAck.
    /// @param[in] connection_token Token captured by the frame connection.
    /// @param[in] now_ms Current monotonic time in ms.
    /// @param[in] ack Typed facts from a complete codec-validated HelloAck.
    /// @return None after committing Ready, ErrorAndDrain for wire mismatch,
    /// or Drain when the overall deadline has expired; Closed returns WrongPhase.
    /// @errors WrongConnectionToken or TimeWentBackwards without mutation.
    /// @ownership Facts are copied; no borrowed payload is retained.
    /// @thread_safety Caller serializes frame dispatch.
    pub fn receive_ack(
        self: *session_handshake_t,
        connection_token: u64,
        now_ms: u64,
        ack: hello_ack_facts_t,
    ) handshake_error_t!action_t {
        if (try self.event_expired(connection_token, now_ms)) return .Drain;
        if (self.phase == .Closed) return error.WrongPhase;
        if (self.role != .Guest or self.phase != .AwaitHelloAck) {
            self.commit_time(now_ms);
            return self.error_and_drain(.ProtocolViolation);
        }
        if (ack.protocol_version != ProtocolVersion or ack.request_id != self.staged_request_id) {
            self.commit_time(now_ms);
            return self.error_and_drain(.ProtocolViolation);
        }
        if (ack.version_major == null or ack.version_major.? != self.local_version_major or
            ack.layout_version != LayoutVersion)
        {
            self.commit_time(now_ms);
            return self.error_and_drain(.VersionMismatch);
        }
        if (!known_capabilities(ack.capabilities) or
            (ack.capabilities & self.local_capabilities) != ack.capabilities)
        {
            self.commit_time(now_ms);
            return self.error_and_drain(.ProtocolViolation);
        }
        self.commit_time(now_ms);
        self.session_epoch = self.staged_epoch;
        self.negotiated_capabilities = ack.capabilities;
        self.clear_staged();
        self.phase = .Ready;
        return .None;
    }

    /// Confirm complete HelloAck transmission; queueing or partial send is insufficient.
    /// @param[in,out] self Host state in AwaitHelloAckSent.
    /// @param[in] connection_token Token captured by the transport send.
    /// @param[in] now_ms Current monotonic time in ms.
    /// @param[in] request_id ID of the completely sent HelloAck.
    /// @return None after committing Ready, or Drain on timeout.
    /// @errors WrongConnectionToken, TimeWentBackwards, WrongPhase, or WrongRequestId
    /// without mutation.
    /// @ownership No frame storage is retained.
    /// @thread_safety Caller serializes send-completion events.
    pub fn ack_sent(
        self: *session_handshake_t,
        connection_token: u64,
        now_ms: u64,
        request_id: u32,
    ) handshake_error_t!action_t {
        if (try self.event_expired(connection_token, now_ms)) return .Drain;
        if (self.role != .Host or self.phase != .AwaitHelloAckSent) return error.WrongPhase;
        if (request_id != self.staged_request_id) return error.WrongRequestId;
        self.commit_time(now_ms);
        self.session_epoch = self.staged_epoch;
        self.negotiated_capabilities = self.staged_capabilities & self.local_capabilities;
        self.clear_staged();
        self.phase = .Ready;
        return .None;
    }

    /// Advance the overall 5000 ms pre-Ready timeout.
    /// @param[in,out] self Connected-peer machine.
    /// @param[in] connection_token Token of the timer source.
    /// @param[in] now_ms Current monotonic time in ms.
    /// @return Drain exactly at the overall deadline, otherwise None.
    /// @errors WrongConnectionToken or TimeWentBackwards without mutation.
    /// @thread_safety Caller serializes timer and peer events.
    pub fn tick(self: *session_handshake_t, connection_token: u64, now_ms: u64) handshake_error_t!action_t {
        if (try self.event_expired(connection_token, now_ms)) return .Drain;
        if (self.phase == .Closed) return .Closed;
        if (self.phase == .Draining) return .Drain;
        self.commit_time(now_ms);
        return .None;
    }

    /// Begin local orderly teardown; repeated calls are idempotent.
    /// @param[in,out] self Connected-peer machine.
    /// @param[in] connection_token Token of the local stop request.
    /// @param[in] now_ms Current monotonic time in ms.
    /// @return Drain while teardown is pending; Closed without mutation if terminal.
    /// @errors WrongConnectionToken or TimeWentBackwards without mutation.
    /// @note Does not perform transport/mapping cleanup or establish drain proof.
    /// @thread_safety Caller serializes lifecycle requests.
    pub fn begin_drain(self: *session_handshake_t, connection_token: u64, now_ms: u64) handshake_error_t!action_t {
        if (try self.event_expired(connection_token, now_ms)) return .Drain;
        if (self.phase == .Closed) return .Closed;
        self.commit_time(now_ms);
        self.enter_draining(.LocalRequest);
        return .Drain;
    }

    /// Record transport loss without inferring mapping or producer teardown.
    /// @param[in,out] self Connected-peer machine.
    /// @param[in] connection_token Token of the lost transport.
    /// @param[in] now_ms Current monotonic time in ms.
    /// @return Drain or Closed if teardown is already terminal.
    /// @errors WrongConnectionToken or TimeWentBackwards without mutation.
    /// @note EOF is not evidence that producers or host buffers are detached.
    /// @thread_safety Caller serializes transport loss with peer events.
    pub fn transport_lost(self: *session_handshake_t, connection_token: u64, now_ms: u64) handshake_error_t!action_t {
        if (try self.event_expired(connection_token, now_ms)) return .Drain;
        if (self.phase == .Closed) return .Closed;
        self.commit_time(now_ms);
        self.enter_draining(.TransportLost);
        return .Drain;
    }

    /// Complete owner teardown and enter terminal Closed.
    /// @param[in,out] self Machine whose owner completed cleanup.
    /// @param[in] connection_token Token of this connection's cleanup.
    /// @param[in] now_ms Current monotonic time in ms.
    /// @return Closed after Draining; this is never host-drained proof.
    /// @errors WrongConnectionToken, TimeWentBackwards, or WrongPhase without mutation.
    /// @thread_safety Caller serializes teardown completion.
    pub fn teardown_complete(self: *session_handshake_t, connection_token: u64, now_ms: u64) handshake_error_t!action_t {
        if (try self.event_expired(connection_token, now_ms)) return .Drain;
        if (self.phase != .Draining) return error.WrongPhase;
        self.commit_time(now_ms);
        self.phase = .Closed;
        return .Closed;
    }

    fn event_expired(self: *session_handshake_t, connection_token: u64, now_ms: u64) handshake_error_t!bool {
        if (connection_token != self.connection_token) return error.WrongConnectionToken;
        if (now_ms < self.last_now_ms) return error.TimeWentBackwards;
        if (self.phase != .Ready and self.phase != .Draining and self.phase != .Closed and
            now_ms - self.accepted_at_ms >= HelloTimeoutMs)
        {
            self.commit_time(now_ms);
            self.enter_draining(.HelloTimeout);
            return true;
        }
        return false;
    }

    fn commit_time(self: *session_handshake_t, now_ms: u64) void {
        self.last_now_ms = now_ms;
    }

    fn clear_staged(self: *session_handshake_t) void {
        self.staged_epoch = 0;
        self.staged_capabilities = 0;
        self.staged_request_id = 0;
    }

    fn enter_draining(self: *session_handshake_t, reason: drain_reason_t) void {
        self.clear_staged();
        self.session_epoch = 0;
        self.negotiated_capabilities = 0;
        self.phase = .Draining;
        if (self.drain_reason == null) self.drain_reason = reason;
    }

    fn error_and_drain(self: *session_handshake_t, wire_error: wire_error_t) action_t {
        self.enter_draining(.WireError);
        return .{ .ErrorAndDrain = wire_error };
    }
};

/// Endpoint role for one connected peer.
/// @ownership Scalar enum copied into caller-owned state.
pub const role_t = enum(u8) {
    /// Host session client; receives Hello and sends HelloAck.
    Host,
    /// Guest agent; initializes mapping and sends Hello.
    Guest,
};

/// Lifecycle phases for negotiation and explicit teardown.
/// @ownership Scalar enum copied into caller-owned state.
pub const phase_t = enum(u8) {
    /// Host awaits the first guest Hello.
    AwaitHello,
    /// Host accepted Hello and awaits mapping validation.
    AwaitMapping,
    /// Host staged Ack and awaits complete-send confirmation.
    AwaitHelloAckSent,
    /// Guest awaits external mapping initialization.
    InitializingMapping,
    /// Guest staged Hello and awaits complete-send confirmation.
    AwaitHelloSent,
    /// Guest sent Hello and awaits HelloAck.
    AwaitHelloAck,
    /// Negotiation is committed; Ping/Pong policy is out of scope.
    Ready,
    /// New work is blocked while the owner performs teardown.
    Draining,
    /// Owner teardown completed; this connection instance is terminal.
    Closed,
};

/// Cause recorded when negotiation first enters Draining.
/// @ownership Scalar enum copied into machine state.
pub const drain_reason_t = enum(u8) {
    /// Overall 5000 ms pre-Ready deadline expired.
    HelloTimeout,
    /// Local owner requested orderly teardown.
    LocalRequest,
    /// Transport reported EOF or an unrecoverable error.
    TransportLost,
    /// Fatal decoded Hello/Ack or mapping-validation fact.
    WireError,
};

/// Frozen wire errors emitted for fatal pre-Ready protocol facts.
/// @ownership Scalar enum copied into ErrorAndDrain actions.
pub const wire_error_t = enum(u32) {
    /// Frozen protocol error for invalid Hello facts or failed mapping validation.
    ProtocolViolation = c_t.InteropErrorProtocolViolation,
    /// Frozen version error for incompatible agent major or layout version.
    VersionMismatch = c_t.InteropErrorVersionMismatch,
};

/// Scalar facts parsed from a complete codec-validated guest Hello.
/// @ownership By-value facts; no frame or borrowed field storage is retained.
pub const hello_facts_t = struct {
    /// Header protocol version, expected to be 2.
    protocol_version: u16,
    /// Parsed SemVer major; null means malformed version string.
    version_major: ?u32,
    /// Advertised shared-memory layout version, expected to be 3.
    layout_version: u32,
    /// Guest capability offer, restricted to known bits.
    capabilities: u64,
    /// Nonzero guest-initialized mapping epoch.
    session_epoch: u64,
    /// Guest request ID, required nonzero by endpoint policy.
    request_id: u32,
};

/// Scalar facts parsed from a complete codec-validated host HelloAck.
/// @ownership By-value facts; no frame or borrowed field storage is retained.
pub const hello_ack_facts_t = struct {
    /// Header protocol version, expected to be 2.
    protocol_version: u16,
    /// Parsed SemVer major; null means malformed version string.
    version_major: ?u32,
    /// Negotiated shared-memory layout version, expected to be 3.
    layout_version: u32,
    /// Host-computed intersection; guest checks only known subset of its offer.
    capabilities: u64,
    /// Echo of the Guest Hello request ID.
    request_id: u32,
};

/// Typed outbound Hello scalars for the transport owner to serialize.
/// @ownership Copied value; caller owns full version-string and frame storage.
pub const hello_out_t = struct {
    /// Protocol version for the frame header.
    protocol_version: u16,
    /// Local version major; adapter serializes the configured full version string.
    version_major: u32,
    /// Shared-memory layout version.
    layout_version: u32,
    /// Guest's configured capability offer.
    capabilities: u64,
    /// Fresh epoch initialized before sending Hello.
    session_epoch: u64,
    /// Caller-allocated nonzero request ID.
    request_id: u32,
};

/// Typed outbound HelloAck scalars for the transport owner to serialize.
/// @ownership Copied value; caller owns full version-string and frame storage.
pub const hello_ack_out_t = struct {
    /// Protocol version for the frame header.
    protocol_version: u16,
    /// Local version major; adapter serializes the configured full version string.
    version_major: u32,
    /// Shared-memory layout version.
    layout_version: u32,
    /// Host-computed capability intersection.
    capabilities: u64,
    /// Echo of the Guest Hello request ID.
    request_id: u32,
};

/// Copied action returned to transport and lifecycle owners.
/// @ownership Scalar-only tagged union; no borrowed memory crosses the boundary.
pub const action_t = union(enum) {
    /// Accepted event needs no immediate external action.
    None: void,
    /// Serialize and completely send the contained Hello fields.
    SendHello: hello_out_t,
    /// Serialize and completely send the contained HelloAck fields.
    SendHelloAck: hello_ack_out_t,
    /// Best-effort send this frozen Error, then perform owner teardown.
    ErrorAndDrain: wire_error_t,
    /// Block negotiation and ask the owner to perform local cleanup.
    Drain: void,
    /// Owner cleanup is complete for this connection instance.
    Closed: void,
};

/// Local errors that preserve the complete machine state.
/// @ownership Compile-time error set; no runtime storage.
pub const handshake_error_t = error{
    /// Init received a zero connection token.
    InvalidConnectionToken,
    /// Init received bits outside InteropCapabilityMask.
    UnknownCapabilities,
    /// Event token does not identify this connection.
    WrongConnectionToken,
    /// Event timestamp is older than last accepted monotonic time.
    TimeWentBackwards,
    /// Local event is invalid in the current role or phase.
    WrongPhase,
    /// Local send completion ID differs from the staged outbound request ID.
    WrongRequestId,
    /// Local mapping initializer supplied a zero epoch.
    InvalidSessionEpoch,
    /// Local mapping initializer supplied a zero Hello request ID.
    InvalidRequestId,
    /// External mapping initialization failed and may be retried.
    MappingInitializationFailed,
};

/// Frozen protocol-v2 wire version.
pub const ProtocolVersion: u16 = @intCast(c_t.InteropVersion);
/// Frozen shared-memory layout version.
pub const LayoutVersion: u32 = @intCast(c_t.InteropLayoutVersion);
/// Overall connected-peer pre-Ready deadline in milliseconds.
pub const HelloTimeoutMs: u64 = @intCast(c_t.InteropHelloTimeoutMs);
/// Known protocol capability bits.
pub const CapabilityMask: u64 = c_t.InteropCapabilityMask;

fn known_capabilities(capabilities: u64) bool {
    return (capabilities & CapabilityMask) == capabilities;
}

// Test-only fixtures.
fn expect_callback_errors_preserve(
    machine: *session_handshake_t,
    callback: anytype,
    token: u64,
    now_ms: u64,
    extra: anytype,
) !void {
    const before = machine.*;
    try std_t.testing.expectError(
        error.WrongConnectionToken,
        @call(.auto, callback, .{ machine, token + 1, now_ms } ++ extra),
    );
    try std_t.testing.expectEqualDeep(before, machine.*);
    try std_t.testing.expectError(
        error.TimeWentBackwards,
        @call(.auto, callback, .{ machine, token, now_ms - 1 } ++ extra),
    );
    try std_t.testing.expectEqualDeep(before, machine.*);
}

test "host validates Hello and mapping before Ack becomes Ready" {
    var machine = try session_handshake_t.init(.Host, 41, 2, 0b1101, 100);
    const hello = hello_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 2,
        .layout_version = LayoutVersion,
        .capabilities = 0b1011,
        .session_epoch = 900,
        .request_id = 7,
    };
    try std_t.testing.expectEqual(action_t.None, try machine.receive_hello(41, 101, hello));
    try std_t.testing.expectEqual(phase_t.AwaitMapping, machine.phase);
    try std_t.testing.expectEqual(@as(u64, 0), machine.session_epoch);
    const action = try machine.mapping_validated(41, 102, 900, LayoutVersion, true);
    try std_t.testing.expectEqual(phase_t.AwaitHelloAckSent, machine.phase);
    try std_t.testing.expectEqual(@as(u64, 0), machine.negotiated_capabilities);
    switch (action) {
        .SendHelloAck => |ack| {
            try std_t.testing.expectEqual(@as(u32, 7), ack.request_id);
            try std_t.testing.expectEqual(@as(u64, 0b1001), ack.capabilities);
            try std_t.testing.expectEqual(@as(u32, 2), ack.version_major);
        },
        else => return error.TestUnexpectedResult,
    }
    try std_t.testing.expectEqual(action_t.None, try machine.ack_sent(41, 103, 7));
    try std_t.testing.expectEqual(phase_t.Ready, machine.phase);
    try std_t.testing.expectEqual(@as(u64, 900), machine.session_epoch);
    try std_t.testing.expectEqual(@as(u64, 0b1001), machine.negotiated_capabilities);
}

test "guest waits for complete Hello and accepts capability subset" {
    var machine = try session_handshake_t.init(.Guest, 42, 3, 0b111, 200);
    const action = try machine.mapping_initialized(42, 201, true, 901, 11);
    try std_t.testing.expectEqual(phase_t.AwaitHelloSent, machine.phase);
    switch (action) {
        .SendHello => |hello| {
            try std_t.testing.expectEqual(@as(u64, 901), hello.session_epoch);
            try std_t.testing.expectEqual(@as(u32, 11), hello.request_id);
            try std_t.testing.expectEqual(@as(u64, 0b111), hello.capabilities);
        },
        else => return error.TestUnexpectedResult,
    }
    try std_t.testing.expectError(error.WrongRequestId, machine.hello_sent(42, 202, 12));
    try std_t.testing.expectEqual(phase_t.AwaitHelloSent, machine.phase);
    try std_t.testing.expectEqual(action_t.None, try machine.hello_sent(42, 202, 11));
    const ack = hello_ack_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 3,
        .layout_version = LayoutVersion,
        .capabilities = 0b001,
        .request_id = 11,
    };
    try std_t.testing.expectEqual(action_t.None, try machine.receive_ack(42, 203, ack));
    try std_t.testing.expectEqual(phase_t.Ready, machine.phase);
    try std_t.testing.expectEqual(@as(u64, 901), machine.session_epoch);
    try std_t.testing.expectEqual(@as(u64, 1), machine.negotiated_capabilities);
}

test "wire incompatibilities return frozen errors and clear readiness" {
    var host = try session_handshake_t.init(.Host, 51, 2, CapabilityMask, 0);
    var hello = hello_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 2,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .session_epoch = 1,
        .request_id = 1,
    };
    hello.protocol_version = 99;
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try host.receive_hello(51, 1, hello));
    try std_t.testing.expectEqual(phase_t.Draining, host.phase);
    try std_t.testing.expectEqual(@as(u64, 0), host.staged_epoch);

    var version_host = try session_handshake_t.init(.Host, 52, 2, 0, 0);
    hello.protocol_version = ProtocolVersion;
    hello.layout_version = 1;
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .VersionMismatch }, try version_host.receive_hello(52, 1, hello));

    var map_host = try session_handshake_t.init(.Host, 53, 2, 0, 0);
    hello.layout_version = LayoutVersion;
    _ = try map_host.receive_hello(53, 1, hello);
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try map_host.mapping_validated(53, 2, 1, LayoutVersion, false));
}

test "Hello rejects zero identifiers epochs and unknown capabilities" {
    var hello = hello_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 2,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .session_epoch = 1,
        .request_id = 1,
    };
    var no_id = try session_handshake_t.init(.Host, 54, 2, 0, 0);
    hello.request_id = 0;
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try no_id.receive_hello(54, 1, hello));

    var no_epoch = try session_handshake_t.init(.Host, 55, 2, 0, 0);
    hello.request_id = 1;
    hello.session_epoch = 0;
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try no_epoch.receive_hello(55, 1, hello));

    var unknown_caps = try session_handshake_t.init(.Host, 56, 2, 0, 0);
    hello.session_epoch = 1;
    hello.capabilities = CapabilityMask | (1 << 20);
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try unknown_caps.receive_hello(56, 1, hello));

    var wrong_major = try session_handshake_t.init(.Host, 57, 2, 0, 0);
    hello.capabilities = 0;
    hello.version_major = 3;
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .VersionMismatch }, try wrong_major.receive_hello(57, 1, hello));

    var malformed_major = try session_handshake_t.init(.Host, 58, 2, 0, 0);
    hello.version_major = null;
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .VersionMismatch }, try malformed_major.receive_hello(58, 1, hello));
}

test "guest rejects ack capability supersets and malformed version" {
    var guest = try session_handshake_t.init(.Guest, 61, 2, 0b0011, 0);
    _ = try guest.mapping_initialized(61, 1, true, 8, 9);
    _ = try guest.hello_sent(61, 2, 9);
    var ack = hello_ack_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 2,
        .layout_version = LayoutVersion,
        .capabilities = 0b0100,
        .request_id = 9,
    };
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try guest.receive_ack(61, 3, ack));
    try std_t.testing.expectEqual(phase_t.Draining, guest.phase);
    try std_t.testing.expectEqual(@as(u64, 0), guest.session_epoch);

    var wrong_major = try session_handshake_t.init(.Guest, 62, 2, 3, 0);
    _ = try wrong_major.mapping_initialized(62, 1, true, 8, 9);
    _ = try wrong_major.hello_sent(62, 2, 9);
    ack.capabilities = 1;
    ack.version_major = null;
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .VersionMismatch }, try wrong_major.receive_ack(62, 3, ack));

    var unknown_caps = try session_handshake_t.init(.Guest, 66, 2, CapabilityMask, 0);
    _ = try unknown_caps.mapping_initialized(66, 1, true, 8, 9);
    _ = try unknown_caps.hello_sent(66, 2, 9);
    ack.version_major = 2;
    ack.capabilities = CapabilityMask | (1 << 20);
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try unknown_caps.receive_ack(66, 3, ack));
}

test "guest ack protocol correlation and layout violations use their frozen errors" {
    var guest = try session_handshake_t.init(.Guest, 63, 2, 3, 0);
    _ = try guest.mapping_initialized(63, 1, true, 8, 9);
    _ = try guest.hello_sent(63, 2, 9);
    var ack = hello_ack_facts_t{
        .protocol_version = 99,
        .version_major = 2,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .request_id = 9,
    };
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try guest.receive_ack(63, 3, ack));

    var wrong_id = try session_handshake_t.init(.Guest, 64, 2, 3, 0);
    _ = try wrong_id.mapping_initialized(64, 1, true, 8, 9);
    _ = try wrong_id.hello_sent(64, 2, 9);
    ack.protocol_version = ProtocolVersion;
    ack.request_id = 10;
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try wrong_id.receive_ack(64, 3, ack));

    var wrong_layout = try session_handshake_t.init(.Guest, 65, 2, 3, 0);
    _ = try wrong_layout.mapping_initialized(65, 1, true, 8, 9);
    _ = try wrong_layout.hello_sent(65, 2, 9);
    ack.request_id = 9;
    ack.layout_version = 1;
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .VersionMismatch }, try wrong_layout.receive_ack(65, 3, ack));

    var wrong_major = try session_handshake_t.init(.Guest, 67, 2, 3, 0);
    _ = try wrong_major.mapping_initialized(67, 1, true, 8, 9);
    _ = try wrong_major.hello_sent(67, 2, 9);
    ack.layout_version = LayoutVersion;
    ack.version_major = 3;
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .VersionMismatch }, try wrong_major.receive_ack(67, 3, ack));
}

test "local failures preserve the complete value and reject stale tokens" {
    var machine = try session_handshake_t.init(.Guest, 71, 1, 0, 10);
    const before = machine;
    try std_t.testing.expectError(error.WrongConnectionToken, machine.mapping_initialized(72, 11, true, 2, 3));
    try std_t.testing.expectEqualDeep(before, machine);
    try std_t.testing.expectError(error.TimeWentBackwards, machine.mapping_initialized(71, 9, true, 2, 3));
    try std_t.testing.expectEqualDeep(before, machine);
    try std_t.testing.expectError(error.MappingInitializationFailed, machine.mapping_initialized(71, 11, false, 2, 3));
    try std_t.testing.expectEqualDeep(before, machine);
    try std_t.testing.expectError(error.InvalidSessionEpoch, machine.mapping_initialized(71, 11, true, 0, 3));
    try std_t.testing.expectEqualDeep(before, machine);
    try std_t.testing.expectError(error.InvalidRequestId, machine.mapping_initialized(71, 11, true, 2, 0));
    try std_t.testing.expectEqualDeep(before, machine);
    try std_t.testing.expectError(error.WrongPhase, machine.hello_sent(71, 11, 1));
    try std_t.testing.expectEqualDeep(before, machine);
}

test "deadline is overall and exact at uint64 boundary" {
    var machine = try session_handshake_t.init(.Guest, 81, 1, 0, 100);
    try std_t.testing.expectError(error.WrongConnectionToken, machine.tick(80, 5100));
    try std_t.testing.expectEqual(phase_t.InitializingMapping, machine.phase);
    try std_t.testing.expectEqual(action_t.None, try machine.tick(81, 5099));
    try std_t.testing.expectEqual(action_t.Drain, try machine.tick(81, 5100));
    try std_t.testing.expectEqual(phase_t.Draining, machine.phase);
    try std_t.testing.expectEqual(drain_reason_t.HelloTimeout, machine.drain_reason.?);
    try std_t.testing.expectEqual(action_t.Drain, try machine.tick(81, 5101));

    var near_max = try session_handshake_t.init(.Host, 82, 1, 0, std_t.math.maxInt(u64) - 5000);
    try std_t.testing.expectEqual(action_t.None, try near_max.tick(82, std_t.math.maxInt(u64) - 1));
    try std_t.testing.expectEqual(action_t.Drain, try near_max.tick(82, std_t.math.maxInt(u64)));
}

test "drain transport loss and close are idempotent" {
    var machine = try session_handshake_t.init(.Host, 91, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try machine.begin_drain(91, 1));
    try std_t.testing.expectEqual(phase_t.Draining, machine.phase);
    try std_t.testing.expectEqual(drain_reason_t.LocalRequest, machine.drain_reason.?);
    try std_t.testing.expectEqual(action_t.Drain, try machine.begin_drain(91, 2));
    try std_t.testing.expectEqual(action_t.Closed, try machine.teardown_complete(91, 3));
    try std_t.testing.expectEqual(phase_t.Closed, machine.phase);
    try std_t.testing.expectEqual(action_t.Closed, try machine.transport_lost(91, 4));
    try std_t.testing.expectError(error.WrongPhase, machine.teardown_complete(91, 4));

    var lost = try session_handshake_t.init(.Guest, 92, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try lost.transport_lost(92, 1));
    try std_t.testing.expectEqual(action_t.Closed, try lost.teardown_complete(92, 2));
}

test "Closed is terminal for wire receive and lifecycle callbacks" {
    var host = try session_handshake_t.init(.Host, 93, 1, 0, 10);
    _ = try host.begin_drain(93, 11);
    _ = try host.teardown_complete(93, 12);
    const closed_host = host;
    const hello = hello_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .session_epoch = 1,
        .request_id = 1,
    };
    try std_t.testing.expectError(error.WrongPhase, host.receive_hello(93, 13, hello));
    try std_t.testing.expectEqualDeep(closed_host, host);
    try std_t.testing.expectError(error.WrongConnectionToken, host.receive_hello(94, 13, hello));
    try std_t.testing.expectEqualDeep(closed_host, host);
    try std_t.testing.expectError(error.TimeWentBackwards, host.receive_hello(93, 11, hello));
    try std_t.testing.expectEqualDeep(closed_host, host);
    try std_t.testing.expectEqual(action_t.Closed, try host.tick(93, 13));
    try std_t.testing.expectEqualDeep(closed_host, host);
    try std_t.testing.expectEqual(action_t.Closed, try host.begin_drain(93, 13));
    try std_t.testing.expectEqualDeep(closed_host, host);
    try std_t.testing.expectEqual(action_t.Closed, try host.transport_lost(93, 13));
    try std_t.testing.expectEqualDeep(closed_host, host);

    var guest = try session_handshake_t.init(.Guest, 95, 1, 0, 20);
    _ = try guest.transport_lost(95, 21);
    _ = try guest.teardown_complete(95, 22);
    const closed_guest = guest;
    const ack = hello_ack_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .request_id = 1,
    };
    try std_t.testing.expectError(error.WrongPhase, guest.receive_ack(95, 23, ack));
    try std_t.testing.expectEqualDeep(closed_guest, guest);
    try std_t.testing.expectError(error.TimeWentBackwards, guest.receive_ack(95, 21, ack));
    try std_t.testing.expectEqualDeep(closed_guest, guest);
    try std_t.testing.expectEqual(action_t.Closed, try guest.tick(95, 23));
    try std_t.testing.expectEqualDeep(closed_guest, guest);
    try std_t.testing.expectEqual(action_t.Closed, try guest.begin_drain(95, 23));
    try std_t.testing.expectEqualDeep(closed_guest, guest);
    try std_t.testing.expectEqual(action_t.Closed, try guest.transport_lost(95, 23));
    try std_t.testing.expectEqualDeep(closed_guest, guest);
}

test "deadline guards precede each callback-specific validation" {
    const hello = hello_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .session_epoch = 1,
        .request_id = 1,
    };
    const ack = hello_ack_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .request_id = 1,
    };

    var receive_hello_machine = try session_handshake_t.init(.Host, 131, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try receive_hello_machine.receive_hello(131, HelloTimeoutMs, hello));

    var mapping_validated_machine = try session_handshake_t.init(.Host, 132, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try mapping_validated_machine.mapping_validated(132, HelloTimeoutMs, 1, LayoutVersion, true));

    var mapping_initialized_machine = try session_handshake_t.init(.Guest, 133, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try mapping_initialized_machine.mapping_initialized(133, HelloTimeoutMs, true, 1, 1));

    var hello_sent_machine = try session_handshake_t.init(.Guest, 134, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try hello_sent_machine.hello_sent(134, HelloTimeoutMs, 1));

    var receive_ack_machine = try session_handshake_t.init(.Guest, 135, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try receive_ack_machine.receive_ack(135, HelloTimeoutMs, ack));

    var ack_sent_machine = try session_handshake_t.init(.Host, 136, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try ack_sent_machine.ack_sent(136, HelloTimeoutMs, 1));

    var tick_machine = try session_handshake_t.init(.Host, 137, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try tick_machine.tick(137, HelloTimeoutMs));

    var begin_drain_machine = try session_handshake_t.init(.Host, 138, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try begin_drain_machine.begin_drain(138, HelloTimeoutMs));

    var transport_lost_machine = try session_handshake_t.init(.Guest, 139, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try transport_lost_machine.transport_lost(139, HelloTimeoutMs));

    var teardown_machine = try session_handshake_t.init(.Host, 140, 1, 0, 0);
    try std_t.testing.expectEqual(action_t.Drain, try teardown_machine.teardown_complete(140, HelloTimeoutMs));
}

test "every asynchronous callback rejects stale tokens and backward time unchanged" {
    const hello = hello_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .session_epoch = 1,
        .request_id = 1,
    };
    const ack = hello_ack_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .request_id = 1,
    };
    var machine = try session_handshake_t.init(.Host, 151, 1, 0, 10);

    try expect_callback_errors_preserve(&machine, session_handshake_t.receive_hello, 151, 9, .{hello});
    try expect_callback_errors_preserve(&machine, session_handshake_t.mapping_validated, 151, 9, .{ 1, LayoutVersion, true });
    try expect_callback_errors_preserve(&machine, session_handshake_t.mapping_initialized, 151, 9, .{ true, 1, 1 });
    try expect_callback_errors_preserve(&machine, session_handshake_t.hello_sent, 151, 9, .{1});
    try expect_callback_errors_preserve(&machine, session_handshake_t.receive_ack, 151, 9, .{ack});
    try expect_callback_errors_preserve(&machine, session_handshake_t.ack_sent, 151, 9, .{1});
    try expect_callback_errors_preserve(&machine, session_handshake_t.tick, 151, 9, .{});
    try expect_callback_errors_preserve(&machine, session_handshake_t.begin_drain, 151, 9, .{});
    try expect_callback_errors_preserve(&machine, session_handshake_t.transport_lost, 151, 9, .{});
    try expect_callback_errors_preserve(&machine, session_handshake_t.teardown_complete, 151, 9, .{});
}

test "wire handshake message in wrong role is fatal" {
    var guest = try session_handshake_t.init(.Guest, 101, 1, 0, 0);
    const hello = hello_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .session_epoch = 3,
        .request_id = 2,
    };
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try guest.receive_hello(101, 1, hello));
    var host = try session_handshake_t.init(.Host, 102, 1, 0, 0);
    const ack = hello_ack_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .request_id = 1,
    };
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try host.receive_ack(102, 1, ack));
}

test "initialization and host mapping proof guards" {
    try std_t.testing.expectError(error.InvalidConnectionToken, session_handshake_t.init(.Host, 0, 1, 0, 0));
    try std_t.testing.expectError(error.UnknownCapabilities, session_handshake_t.init(.Host, 1, 1, CapabilityMask | (1 << 20), 0));
    var host = try session_handshake_t.init(.Host, 111, 1, 0, 0);
    const hello = hello_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .session_epoch = 7,
        .request_id = 1,
    };
    try std_t.testing.expectError(error.WrongPhase, host.mapping_validated(111, 1, 7, LayoutVersion, true));
    try std_t.testing.expectEqual(phase_t.AwaitHello, host.phase);
    _ = try host.receive_hello(111, 1, hello);
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try host.mapping_validated(111, 2, 8, LayoutVersion, true));
}

test "wrong Ack send ID and invalid local phases preserve state" {
    var host = try session_handshake_t.init(.Host, 121, 1, 1, 0);
    const hello = hello_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 1,
        .session_epoch = 7,
        .request_id = 5,
    };
    _ = try host.receive_hello(121, 1, hello);
    _ = try host.mapping_validated(121, 2, 7, LayoutVersion, true);
    const before = host;
    try std_t.testing.expectError(error.WrongRequestId, host.ack_sent(121, 3, 6));
    try std_t.testing.expectEqualDeep(before, host);
    try std_t.testing.expectError(error.WrongPhase, host.hello_sent(121, 3, 5));
    try std_t.testing.expectEqualDeep(before, host);

    var ready = try session_handshake_t.init(.Host, 122, 1, 0, 0);
    const valid_hello = hello_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .session_epoch = 1,
        .request_id = 1,
    };
    _ = try ready.receive_hello(122, 1, valid_hello);
    _ = try ready.mapping_validated(122, 2, 1, LayoutVersion, true);
    _ = try ready.ack_sent(122, 3, 1);
    try std_t.testing.expectEqual(action_t.None, try ready.tick(122, 4));
    try std_t.testing.expectError(error.WrongPhase, ready.teardown_complete(122, 5));
    try std_t.testing.expectEqual(phase_t.Ready, ready.phase);
}

test "role and phase guards distinguish local events from invalid wire phase" {
    var guest_for_mapping_validation = try session_handshake_t.init(.Guest, 161, 1, 0, 0);
    const guest_before = guest_for_mapping_validation;
    try std_t.testing.expectError(
        error.WrongPhase,
        guest_for_mapping_validation.mapping_validated(161, 1, 1, LayoutVersion, true),
    );
    try std_t.testing.expectEqualDeep(guest_before, guest_for_mapping_validation);

    var host_for_mapping_initialization = try session_handshake_t.init(.Host, 162, 1, 0, 0);
    const host_before = host_for_mapping_initialization;
    try std_t.testing.expectError(
        error.WrongPhase,
        host_for_mapping_initialization.mapping_initialized(162, 1, true, 1, 1),
    );
    try std_t.testing.expectEqualDeep(host_before, host_for_mapping_initialization);

    var guest_for_ack_send = try session_handshake_t.init(.Guest, 163, 1, 0, 0);
    const guest_ack_before = guest_for_ack_send;
    try std_t.testing.expectError(error.WrongPhase, guest_for_ack_send.ack_sent(163, 1, 1));
    try std_t.testing.expectEqualDeep(guest_ack_before, guest_for_ack_send);

    var host_for_duplicate_hello = try session_handshake_t.init(.Host, 164, 1, 0, 0);
    const hello = hello_facts_t{
        .protocol_version = ProtocolVersion,
        .version_major = 1,
        .layout_version = LayoutVersion,
        .capabilities = 0,
        .session_epoch = 1,
        .request_id = 1,
    };
    try std_t.testing.expectEqual(action_t.None, try host_for_duplicate_hello.receive_hello(164, 1, hello));
    try std_t.testing.expectEqual(action_t{ .ErrorAndDrain = .ProtocolViolation }, try host_for_duplicate_hello.receive_hello(164, 2, hello));
    try std_t.testing.expectEqual(phase_t.Draining, host_for_duplicate_hello.phase);
}
