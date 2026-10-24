const std_t = @import("std");
const c_t = @cImport({
    @cInclude("waddle/interop_protocol.h");
});

/// Host control Ping cadence in monotonic milliseconds.
pub const PingIntervalMs = c_t.InteropPingIntervalMs;
/// Inbound-frame and individual-Ping timeout in monotonic milliseconds.
pub const LivenessTimeoutMs = c_t.InteropPingTimeoutMs;
/// Maximum outstanding host Ping records owned by one connection.
pub const MaxOutstandingPings = 4;

/// Endpoint role permitted to use a Ready-session control operation.
pub const role_t = enum {
    /// Host endpoint that initiates periodic Ping messages.
    Host,
    /// Guest endpoint that echoes host Ping messages.
    Guest,
};

/// One outstanding caller-allocated request identifier and its issue time.
pub const ping_record_t = struct {
    /// Nonzero ID allocated by the caller's shared per-direction allocator.
    request_id: u32,
    /// Monotonic time at which the owner registered the Ping before enqueue.
    issued_at_ms: u64,
};

/// Scalar action returned to the transport owner; contains no frame or pin state.
pub const action_t = struct {
    /// True when the owner must close this session immediately.
    disconnect: bool = false,
    /// Host tick advice to allocate an ID, register, then enqueue one Ping.
    ping_due: bool = false,
    /// Nonzero request ID to echo in a guest Pong; null for other actions.
    send_pong: ?u32 = null,
    /// ID matched by this Pong; null for unmatched or non-Pong traffic.
    matched_pong: ?u32 = null,
    /// Fixed copied set of IDs retired by expiry, response, or disconnect.
    released_ids: [MaxOutstandingPings]u32 = [_]u32{0} ** MaxOutstandingPings,
    /// Number of valid entries at the start of released_ids.
    released_count: u8 = 0,
};

/// Errors for stale callbacks and invalid local Ready-session API usage.
pub const liveness_error_t = error{
    InvalidConnectionToken,
    WrongConnectionToken,
    TimeWentBackwards,
    SessionClosed,
    WrongRole,
    InvalidRequestId,
    PingNotDue,
    PingCapacityFull,
    DuplicateRequestId,
};

/// Allocation-free Ready-session liveness machine for one serialized endpoint.
/// @ownership Caller owns this value; it retains no frame, mapping, socket, or allocator.
/// @thread_safety All calls on an instance must be serialized by its event-loop owner.
pub const session_liveness_t = struct {
    /// Endpoint role fixed for this connection.
    role: role_t,
    /// Nonzero token binding delayed callbacks to this connection instance.
    connection_token: u64,
    /// Last accepted event time, used to reject a backwards monotonic clock.
    last_event_ms: u64,
    /// Last complete codec-valid inbound-frame time.
    last_valid_rx_ms: u64,
    /// Last successful host Ping registration time; initially Ready time.
    last_registered_ping_ms: u64,
    /// Four independently expiring caller-owned request IDs; null entries are free.
    pings: [MaxOutstandingPings]?ping_record_t = [_]?ping_record_t{null} ** MaxOutstandingPings,
    /// True after liveness timeout or transport loss; subsequent calls are rejected.
    closed: bool = false,

    /// Initialize a fresh machine only after the handshake reaches Ready.
    /// @param[in] role Host initiator or Guest responder.
    /// @param[in] connection_token Nonzero token unique to this connection.
    /// @param[in] ready_at_ms Monotonic time at which Ready was committed.
    /// @return Initialized caller-owned machine with empty Ping tracking.
    /// @errors InvalidConnectionToken when connection_token is zero.
    /// @ownership No allocation occurs; returned value owns no external resource.
    /// @thread_safety Subsequent events must be serialized by one owner.
    pub fn init(role: role_t, connection_token: u64, ready_at_ms: u64) liveness_error_t!session_liveness_t {
        if (connection_token == 0) return error.InvalidConnectionToken;
        return .{
            .role = role,
            .connection_token = connection_token,
            .last_event_ms = ready_at_ms,
            .last_valid_rx_ms = ready_at_ms,
            .last_registered_ping_ms = ready_at_ms,
        };
    }

    /// Advance timers and return host Ping cadence advice without allocating an ID.
    /// @param[in,out] self Live machine; timer effects commit only on successful call.
    /// @param[in] connection_token Token captured when the callback was queued.
    /// @param[in] now_ms Current monotonic timestamp.
    /// @return ping_due on a host at cadence, released IDs for expired Pings, or disconnect.
    /// @errors WrongConnectionToken, TimeWentBackwards, or SessionClosed without mutation.
    /// @ownership Returned action owns only copied scalar IDs.
    /// @thread_safety Caller serializes tick with all other events.
    pub fn tick(self: *session_liveness_t, connection_token: u64, now_ms: u64) liveness_error_t!action_t {
        try self.check_event(connection_token, now_ms);
        var next = self.*;
        var action = action_t{};
        if (next.expire_connection_if_needed(now_ms, &action)) {
            self.* = next;
            return action;
        }
        next.expire_pings(now_ms, &action);
        next.last_event_ms = now_ms;
        action.ping_due = next.role == .Host and elapsed(now_ms, next.last_registered_ping_ms) >= PingIntervalMs;
        self.* = next;
        return action;
    }

    /// Record one complete codec-valid inbound frame that is not Ping or Pong.
    /// @param[in,out] self Live machine; receive time and independent expiry commit together.
    /// @param[in] connection_token Token captured by the frame connection.
    /// @param[in] now_ms Frame receipt time from the monotonic clock.
    /// @return Expired released_ids, or disconnect at the exact six-second deadline.
    /// @errors WrongConnectionToken, TimeWentBackwards, or SessionClosed without mutation.
    /// @ownership No frame bytes are retained.
    /// @thread_safety Caller serializes frame dispatch.
    pub fn observe_valid_frame(self: *session_liveness_t, connection_token: u64, now_ms: u64) liveness_error_t!action_t {
        try self.check_event(connection_token, now_ms);
        var next = self.*;
        var action = action_t{};
        if (next.expire_connection_if_needed(now_ms, &action)) {
            self.* = next;
            return action;
        }
        next.expire_pings(now_ms, &action);
        next.last_valid_rx_ms = now_ms;
        next.last_event_ms = now_ms;
        self.* = next;
        return action;
    }

    /// Accept a decoded host Ping on the Guest and request an immediate Pong echo.
    /// @param[in,out] self Guest machine; receive time changes only on successful frame handling.
    /// @param[in] connection_token Token captured by the frame connection.
    /// @param[in] now_ms Frame receipt time from the monotonic clock.
    /// @param[in] request_id Nonzero host Ping ID to echo verbatim.
    /// @return send_pong with request_id, plus any independently expired host-local records.
    /// @errors WrongConnectionToken, TimeWentBackwards, WrongRole, InvalidRequestId,
    /// or SessionClosed; local errors preserve the entire machine.
    /// @ownership No decoded frame storage is retained.
    /// @thread_safety Caller serializes frame dispatch.
    pub fn receive_ping(self: *session_liveness_t, connection_token: u64, now_ms: u64, request_id: u32) liveness_error_t!action_t {
        try self.check_event(connection_token, now_ms);
        if (self.role != .Guest) return error.WrongRole;
        if (request_id == 0) return error.InvalidRequestId;
        var next = self.*;
        var action = action_t{};
        if (next.expire_connection_if_needed(now_ms, &action)) {
            self.* = next;
            return action;
        }
        next.expire_pings(now_ms, &action);
        next.last_valid_rx_ms = now_ms;
        next.last_event_ms = now_ms;
        action.send_pong = request_id;
        self.* = next;
        return action;
    }

    /// Register a host Ping before enqueueing it in the transport.
    /// @param[in,out] self Host machine; commits the record and cadence only on success.
    /// @param[in] connection_token Token captured by the connection owner.
    /// @param[in] now_ms Monotonic enqueue-preparation time.
    /// @param[in] request_id Nonzero unused ID from the caller's shared global allocator.
    /// Tracked IDs remain unavailable until an action explicitly returns their release.
    /// @return Released expired IDs and no send claim; caller must enqueue the Ping.
    /// @errors WrongConnectionToken, TimeWentBackwards, WrongRole, InvalidRequestId,
    /// PingNotDue, PingCapacityFull, DuplicateRequestId, or SessionClosed. Errors preserve state.
    /// @ownership No ID is minted or owned outside this fixed record.
    /// @thread_safety Caller serializes registration and enqueue ordering.
    pub fn register_ping(self: *session_liveness_t, connection_token: u64, now_ms: u64, request_id: u32) liveness_error_t!action_t {
        try self.check_event(connection_token, now_ms);
        if (self.role != .Host) return error.WrongRole;
        if (request_id == 0) return error.InvalidRequestId;
        if (elapsed(now_ms, self.last_registered_ping_ms) < PingIntervalMs) return error.PingNotDue;
        // A caller may reuse an ID only after an earlier event returned its release.
        for (self.pings) |record| {
            if (record) |pending| {
                if (pending.request_id == request_id) return error.DuplicateRequestId;
            }
        }

        var next = self.*;
        var action = action_t{};
        if (next.expire_connection_if_needed(now_ms, &action)) {
            self.* = next;
            return action;
        }
        next.expire_pings(now_ms, &action);
        var free_index: ?usize = null;
        for (next.pings, 0..) |record, index| {
            if (record == null and free_index == null) {
                free_index = index;
            }
        }
        const index = free_index orelse return error.PingCapacityFull;
        next.pings[index] = .{ .request_id = request_id, .issued_at_ms = now_ms };
        next.last_registered_ping_ms = now_ms;
        next.last_event_ms = now_ms;
        self.* = next;
        return action;
    }

    /// Accept a decoded host Pong on the Host, matching or harmlessly ignoring its ID.
    /// @param[in,out] self Host machine; a matching record is retired atomically with receive time.
    /// @param[in] connection_token Token captured by the frame connection.
    /// @param[in] now_ms Frame receipt time from the monotonic clock.
    /// @param[in] request_id Nonzero echoed guest Ping ID.
    /// @return matched_pong and released ID for a match; unmatched or late Pong still counts as traffic.
    /// @errors WrongConnectionToken, TimeWentBackwards, WrongRole, InvalidRequestId,
    /// or SessionClosed without mutation.
    /// @ownership No Pong frame storage is retained.
    /// @thread_safety Caller serializes frame dispatch.
    pub fn receive_pong(self: *session_liveness_t, connection_token: u64, now_ms: u64, request_id: u32) liveness_error_t!action_t {
        try self.check_event(connection_token, now_ms);
        if (self.role != .Host) return error.WrongRole;
        if (request_id == 0) return error.InvalidRequestId;
        var next = self.*;
        var action = action_t{};
        if (next.expire_connection_if_needed(now_ms, &action)) {
            self.* = next;
            return action;
        }
        next.expire_pings(now_ms, &action);
        for (&next.pings) |*record| {
            if (record.*) |pending| {
                if (pending.request_id == request_id) {
                    record.* = null;
                    append_released(&action, request_id);
                    action.matched_pong = request_id;
                    break;
                }
            }
        }
        next.last_valid_rx_ms = now_ms;
        next.last_event_ms = now_ms;
        self.* = next;
        return action;
    }

    /// Close after EOF or transport loss and release only this machine's Ping IDs.
    /// @param[in,out] self Machine to close; repeated loss on a closed machine is an error.
    /// @param[in] connection_token Token captured by the failed transport.
    /// @param[in] now_ms Monotonic failure-observation time.
    /// @return disconnect and a fixed copy of all outstanding IDs.
    /// @errors WrongConnectionToken, TimeWentBackwards, or SessionClosed without mutation.
    /// @ownership Does not release frame pins, infer peer drain, or own transport cleanup.
    /// @thread_safety Caller serializes transport callbacks.
    pub fn transport_lost(self: *session_liveness_t, connection_token: u64, now_ms: u64) liveness_error_t!action_t {
        try self.check_event(connection_token, now_ms);
        var next = self.*;
        var action = action_t{ .disconnect = true };
        next.release_all(&action);
        next.closed = true;
        next.last_event_ms = now_ms;
        self.* = next;
        return action;
    }

    fn check_event(self: *const session_liveness_t, connection_token: u64, now_ms: u64) liveness_error_t!void {
        if (connection_token == 0) return error.InvalidConnectionToken;
        if (connection_token != self.connection_token) return error.WrongConnectionToken;
        if (now_ms < self.last_event_ms) return error.TimeWentBackwards;
        if (self.closed) return error.SessionClosed;
    }

    fn expire_connection_if_needed(self: *session_liveness_t, now_ms: u64, action: *action_t) bool {
        if (elapsed(now_ms, self.last_valid_rx_ms) < LivenessTimeoutMs) return false;
        self.release_all(action);
        self.closed = true;
        self.last_event_ms = now_ms;
        action.disconnect = true;
        return true;
    }

    fn expire_pings(self: *session_liveness_t, now_ms: u64, action: *action_t) void {
        for (&self.pings) |*record| {
            if (record.*) |pending| {
                if (elapsed(now_ms, pending.issued_at_ms) >= LivenessTimeoutMs) {
                    record.* = null;
                    append_released(action, pending.request_id);
                }
            }
        }
    }

    fn release_all(self: *session_liveness_t, action: *action_t) void {
        for (&self.pings) |*record| {
            if (record.*) |pending| {
                record.* = null;
                append_released(action, pending.request_id);
            }
        }
    }
};

fn elapsed(now_ms: u64, then_ms: u64) u64 {
    return now_ms - then_ms;
}

fn append_released(action: *action_t, request_id: u32) void {
    const index: usize = action.released_count;
    action.released_ids[index] = request_id;
    action.released_count += 1;
}

// Test-only fixtures.
test "host cadence registration and matched pong handoff" {
    var host = try session_liveness_t.init(.Host, 1, 10);
    try std_t.testing.expect(!(try host.tick(1, 2009)).ping_due);
    try std_t.testing.expect((try host.tick(1, 2010)).ping_due);
    const registered = try host.register_ping(1, 2010, 77);
    try std_t.testing.expectEqual(@as(u8, 0), registered.released_count);
    try std_t.testing.expectError(error.PingNotDue, host.register_ping(1, 2011, 78));
    const pong = try host.receive_pong(1, 2500, 77);
    try std_t.testing.expectEqual(@as(?u32, 77), pong.matched_pong);
    try std_t.testing.expectEqual(@as(u8, 1), pong.released_count);
    try std_t.testing.expectEqual(@as(u32, 77), pong.released_ids[0]);
    try std_t.testing.expectEqual(@as(?u32, null), (try host.receive_pong(1, 2501, 77)).matched_pong);
}

test "guest echoes only nonzero Ping and never asks to ping" {
    var guest = try session_liveness_t.init(.Guest, 2, 100);
    try std_t.testing.expect(!(try guest.tick(2, 100)).ping_due);
    const before = guest;
    try std_t.testing.expectError(error.InvalidRequestId, guest.receive_ping(2, 101, 0));
    try std_t.testing.expectEqualDeep(before, guest);
    const action = try guest.receive_ping(2, 102, 0xfeed);
    try std_t.testing.expectEqual(@as(?u32, 0xfeed), action.send_pong);
    try std_t.testing.expectEqual(@as(u64, 102), guest.last_valid_rx_ms);
}

test "exact any-frame deadline closes before a boundary frame" {
    var guest = try session_liveness_t.init(.Guest, 3, 100);
    _ = try guest.observe_valid_frame(3, 6099);
    const before = guest;
    try std_t.testing.expectError(error.TimeWentBackwards, guest.observe_valid_frame(3, 6098));
    try std_t.testing.expectEqualDeep(before, guest);
    const action = try guest.observe_valid_frame(3, 12099);
    try std_t.testing.expect(action.disconnect);
    try std_t.testing.expect(guest.closed);
    try std_t.testing.expectError(error.SessionClosed, guest.tick(3, 12100));
}

test "ping records expire independently during continuous ordinary traffic" {
    var host = try session_liveness_t.init(.Host, 4, 0);
    _ = try host.register_ping(4, 2000, 11);
    _ = try host.observe_valid_frame(4, 3000);
    _ = try host.register_ping(4, 4000, 12);
    _ = try host.observe_valid_frame(4, 5000);
    _ = try host.observe_valid_frame(4, 7000);
    const expired = try host.observe_valid_frame(4, 8000);
    try std_t.testing.expectEqual(@as(u8, 1), expired.released_count);
    try std_t.testing.expectEqual(@as(u32, 11), expired.released_ids[0]);
    try std_t.testing.expectEqual(@as(u64, 8000), host.last_valid_rx_ms);
    const later = try host.tick(4, 10000);
    try std_t.testing.expectEqual(@as(u8, 1), later.released_count);
    try std_t.testing.expectEqual(@as(u32, 12), later.released_ids[0]);
    try std_t.testing.expect(!later.disconnect);
}

test "registration limits are transactional and reused IDs require release" {
    var host = try session_liveness_t.init(.Host, 5, 0);
    _ = try host.register_ping(5, 2000, 1);
    _ = try host.observe_valid_frame(5, 2500);
    const before = host;
    try std_t.testing.expectError(error.DuplicateRequestId, host.register_ping(5, 4000, 1));
    try std_t.testing.expectEqualDeep(before, host);
    _ = try host.register_ping(5, 4000, 2);
    _ = try host.observe_valid_frame(5, 4500);
    _ = try host.register_ping(5, 6000, 3);
    _ = try host.observe_valid_frame(5, 6500);
    _ = try host.register_ping(5, 8000, 4);
    _ = try host.observe_valid_frame(5, 8500);
    const full = try session_liveness_t.init(.Host, 15, 5000);
    var full_host = full;
    full_host.pings = .{
        ping_record_t{ .request_id = 21, .issued_at_ms = 5000 },
        ping_record_t{ .request_id = 22, .issued_at_ms = 5000 },
        ping_record_t{ .request_id = 23, .issued_at_ms = 5000 },
        ping_record_t{ .request_id = 24, .issued_at_ms = 5000 },
    };
    const before_full = full_host;
    try std_t.testing.expectError(error.PingCapacityFull, full_host.register_ping(15, 7000, 25));
    try std_t.testing.expectEqualDeep(before_full, full_host);
    const after_expiry = try host.register_ping(5, 10000, 5);
    try std_t.testing.expect(after_expiry.released_count >= 1);
    try std_t.testing.expectEqual(@as(u32, 2), after_expiry.released_ids[0]);
}

test "expired ID remains unavailable until an event returns its release" {
    var host = try session_liveness_t.init(.Host, 16, 0);
    _ = try host.register_ping(16, 2000, 41);
    _ = try host.observe_valid_frame(16, 3000);
    const pending_before = host;
    try std_t.testing.expectError(error.DuplicateRequestId, host.register_ping(16, 8000, 41));
    try std_t.testing.expectEqualDeep(pending_before, host);
    const released = try host.observe_valid_frame(16, 8000);
    try std_t.testing.expectEqual(@as(u8, 1), released.released_count);
    try std_t.testing.expectEqual(@as(u32, 41), released.released_ids[0]);
    const reused = try host.register_ping(16, 10000, 41);
    try std_t.testing.expectEqual(@as(u8, 0), reused.released_count);
    try std_t.testing.expectEqual(@as(?u32, null), reused.matched_pong);
    try std_t.testing.expectEqual(@as(u32, 41), host.pings[0].?.request_id);
}

test "multiple pongs match out of order and unknown IDs remain harmless" {
    var host = try session_liveness_t.init(.Host, 17, 0);
    _ = try host.register_ping(17, 2000, 51);
    _ = try host.register_ping(17, 4000, 52);
    _ = try host.observe_valid_frame(17, 4100);
    const unknown = try host.receive_pong(17, 4200, 99);
    try std_t.testing.expectEqual(@as(?u32, null), unknown.matched_pong);
    try std_t.testing.expectEqual(@as(u8, 0), unknown.released_count);
    const second = try host.receive_pong(17, 4300, 52);
    try std_t.testing.expectEqual(@as(?u32, 52), second.matched_pong);
    const first = try host.receive_pong(17, 4400, 51);
    try std_t.testing.expectEqual(@as(?u32, 51), first.matched_pong);
    try std_t.testing.expectEqual(@as(u32, 51), first.released_ids[0]);
    const late = try host.receive_pong(17, 4500, 52);
    try std_t.testing.expectEqual(@as(?u32, null), late.matched_pong);
    try std_t.testing.expectEqual(@as(u8, 0), late.released_count);
}

test "Ping APIs close at connection deadline and wrong-role Pong is transactional" {
    var registration_timeout = try session_liveness_t.init(.Host, 18, 0);
    _ = try registration_timeout.register_ping(18, 2000, 61);
    const registration_disconnect = try registration_timeout.register_ping(18, 6000, 62);
    try std_t.testing.expect(registration_disconnect.disconnect);
    try std_t.testing.expectEqual(@as(u8, 1), registration_disconnect.released_count);
    try std_t.testing.expectEqual(@as(u32, 61), registration_disconnect.released_ids[0]);

    var pong_timeout = try session_liveness_t.init(.Host, 19, 0);
    _ = try pong_timeout.register_ping(19, 2000, 63);
    const pong_disconnect = try pong_timeout.receive_pong(19, 6000, 63);
    try std_t.testing.expect(pong_disconnect.disconnect);
    try std_t.testing.expectEqual(@as(u8, 1), pong_disconnect.released_count);
    try std_t.testing.expectEqual(@as(?u32, null), pong_disconnect.matched_pong);

    var ping_timeout = try session_liveness_t.init(.Guest, 20, 0);
    const ping_disconnect = try ping_timeout.receive_ping(20, 6000, 64);
    try std_t.testing.expect(ping_disconnect.disconnect);
    var wrong_role_guest = try session_liveness_t.init(.Guest, 21, 6000);
    const wrong_role_before = wrong_role_guest;
    try std_t.testing.expectError(error.WrongRole, wrong_role_guest.receive_pong(21, 6001, 64));
    try std_t.testing.expectEqualDeep(wrong_role_before, wrong_role_guest);
}

test "local errors and stale tokens preserve complete state" {
    var host = try session_liveness_t.init(.Host, 6, 20);
    const before = host;
    try std_t.testing.expectError(error.WrongRole, host.receive_ping(6, 21, 8));
    try std_t.testing.expectError(error.InvalidRequestId, host.receive_pong(6, 21, 0));
    try std_t.testing.expectError(error.WrongConnectionToken, host.tick(7, 21));
    try std_t.testing.expectError(error.InvalidConnectionToken, host.tick(0, 21));
    try std_t.testing.expectEqualDeep(before, host);
    try std_t.testing.expectError(error.InvalidConnectionToken, session_liveness_t.init(.Guest, 0, 0));
}

test "disconnect releases all pending IDs exactly once" {
    var host = try session_liveness_t.init(.Host, 7, 0);
    _ = try host.register_ping(7, 2000, 101);
    _ = try host.register_ping(7, 4000, 102);
    const lost = try host.transport_lost(7, 4001);
    try std_t.testing.expect(lost.disconnect);
    try std_t.testing.expectEqual(@as(u8, 2), lost.released_count);
    try std_t.testing.expectEqual(@as(u32, 101), lost.released_ids[0]);
    try std_t.testing.expectEqual(@as(u32, 102), lost.released_ids[1]);
    try std_t.testing.expectError(error.SessionClosed, host.transport_lost(7, 4002));
}

test "role errors at future times do not mutate or trigger timeout" {
    var guest = try session_liveness_t.init(.Guest, 8, 0);
    const before = guest;
    try std_t.testing.expectError(error.WrongRole, guest.register_ping(8, 9000, 1));
    try std_t.testing.expectEqualDeep(before, guest);
    var host = try session_liveness_t.init(.Host, 9, 0);
    const host_before = host;
    try std_t.testing.expectError(error.WrongRole, host.receive_ping(9, 9000, 1));
    try std_t.testing.expectEqualDeep(host_before, host);
}

test "elapsed comparisons remain correct near uint64 maximum" {
    const start = std_t.math.maxInt(u64) - 10_000;
    var host = try session_liveness_t.init(.Host, 10, start);
    try std_t.testing.expect((try host.tick(10, start + 1999)).ping_due == false);
    try std_t.testing.expect((try host.tick(10, start + 2000)).ping_due);
    _ = try host.register_ping(10, start + 2000, 301);
    _ = try host.observe_valid_frame(10, start + 3000);
    const expiring = try host.observe_valid_frame(10, start + 7999);
    try std_t.testing.expectEqual(@as(u8, 0), expiring.released_count);
    const expired = try host.observe_valid_frame(10, start + 8000);
    try std_t.testing.expectEqual(@as(u8, 1), expired.released_count);
    try std_t.testing.expectEqual(@as(u32, 301), expired.released_ids[0]);
}

test "deterministic long Ready traffic stays bounded and allocation free" {
    var host = try session_liveness_t.init(.Host, 11, 0);
    var now: u64 = 0;
    var next_id: u32 = 1;
    var iteration: u32 = 0;
    while (iteration < 100_000) : (iteration += 1) {
        now += 100;
        const tick_action = try host.tick(11, now);
        if (tick_action.ping_due) {
            _ = try host.register_ping(11, now, next_id);
            next_id +%= 1;
            if (next_id == 0) next_id = 1;
        }
        _ = try host.observe_valid_frame(11, now);
        try std_t.testing.expect(!host.closed);
        var count: usize = 0;
        for (host.pings) |record| if (record != null) {
            count += 1;
        };
        try std_t.testing.expect(count <= MaxOutstandingPings);
    }
}
