const std_t = @import("std");
const peer_t = @import("peer");
const c_t = @cImport({
    @cInclude("waddle/interop_protocol.h");
});

const HostToken: u64 = 0x101;
const GuestToken: u64 = 0x202;
const Epoch: u64 = 0x334455;
const Capabilities: u64 = c_t.InteropCapWindows | c_t.InteropCapInput;
const Version = "1.2.3";
const MaxTestFrame: usize = 512;

const pair_t = struct {
    host: peer_t.peer_t,
    guest: peer_t.peer_t,
};

fn init_peer_version(allocator: std_t.mem.Allocator, role: peer_t.role_t, token: u64, accepted_at_ms: u64, version: []const u8) !peer_t.peer_t {
    return peer_t.peer_t.init(allocator, .{
        .role = role,
        .connection_token = token,
        .accepted_at_ms = accepted_at_ms,
        .local_version = version,
        .local_capabilities = Capabilities,
    });
}

fn init_peer(allocator: std_t.mem.Allocator, role: peer_t.role_t, token: u64, accepted_at_ms: u64) !peer_t.peer_t {
    return init_peer_version(allocator, role, token, accepted_at_ms, Version);
}

fn init_pair(allocator: std_t.mem.Allocator, accepted_at_ms: u64) !pair_t {
    return .{
        .host = try init_peer(allocator, .Host, HostToken, accepted_at_ms),
        .guest = try init_peer(allocator, .Guest, GuestToken, accepted_at_ms),
    };
}

fn deinit_pair(pair: *pair_t) void {
    pair.host.deinit();
    pair.guest.deinit();
}

fn expect_ready_error_queue_oom(target_role: peer_t.role_t, frame_type: u16, malformed_payload: bool) !void {
    var failing = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = std_t.math.maxInt(usize) });
    var pair = pair_t{
        .host = if (target_role == .Host) try peer_t.peer_t.init(failing.allocator(), .{
            .role = .Host,
            .connection_token = HostToken,
            .accepted_at_ms = 100,
            .local_version = Version,
            .local_capabilities = Capabilities,
        }) else try init_peer(std_t.testing.allocator, .Host, HostToken, 100),
        .guest = if (target_role == .Guest) try peer_t.peer_t.init(failing.allocator(), .{
            .role = .Guest,
            .connection_token = GuestToken,
            .accepted_at_ms = 100,
            .local_version = Version,
            .local_capabilities = Capabilities,
        }) else try init_peer(std_t.testing.allocator, .Guest, GuestToken, 100),
    };
    var pair_live = true;
    defer if (pair_live) deinit_pair(&pair);
    try complete_handshake(&pair, 101);
    failing.fail_index = failing.alloc_index;

    var input: [MaxTestFrame]u8 = undefined;
    const target_is_host = target_role == .Host;
    const incoming_direction: c_t.interop_direction_t = if (target_is_host) c_t.InteropFromGuest else c_t.InteropFromHost;
    var input_length: usize = 0;
    switch (frame_type) {
        c_t.InteropMsgHello => input_length = try make_hello(&input, 1, Epoch, Version, Capabilities),
        c_t.InteropMsgHelloAck => input_length = try make_hello_ack(&input, 1, Version, Capabilities),
        c_t.InteropMsgGoodbye => input_length = try make_goodbye_id(&input, incoming_direction, 9),
        c_t.InteropMsgPing, c_t.InteropMsgPong => input_length = try make_empty(&input, frame_type, 0, incoming_direction),
        else => return error.InvalidTestFrameType,
    }
    if (malformed_payload) try replace_tlv_tag(&input, input_length, c_t.InteropTagCapabilities, 0x7ffe);
    const target = if (target_is_host) &pair.host else &pair.guest;
    const token = if (target_is_host) HostToken else GuestToken;
    _ = try target.feed(token, input[0..input_length]);
    var output: [MaxTestFrame]u8 = undefined;
    const result = try target.next_event(token, 102, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, result.event.kind);
    try std_t.testing.expect(target.terminal_send_unavailable());
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, target.phase());
    _ = try target.transport_lost(token);
    deinit_pair(&pair);
    pair_live = false;
    try std_t.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
}

fn expect_handshake_error_queue_oom(target_role: peer_t.role_t, malformed_payload: bool) !void {
    var failing = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = std_t.math.maxInt(usize) });
    var target = try peer_t.peer_t.init(failing.allocator(), .{
        .role = target_role,
        .connection_token = if (target_role == .Host) HostToken else GuestToken,
        .accepted_at_ms = 200,
        .local_version = Version,
        .local_capabilities = Capabilities,
    });
    var target_live = true;
    defer if (target_live) target.deinit();
    const token = if (target_role == .Host) HostToken else GuestToken;
    var input: [MaxTestFrame]u8 = undefined;
    var input_length: usize = 0;
    if (target_role == .Host) {
        input_length = try make_hello(&input, 17, Epoch, if (malformed_payload) Version else "2.0.0", Capabilities);
    } else {
        _ = try target.guest_mapping_initialized(token, 201, Epoch, true);
        const hello = (try target.peek_tx(token)).?;
        _ = try target.ack_tx(token, 202, hello.len);
        input_length = try make_hello_ack(&input, target.pending_hello_id, if (malformed_payload) Version else "2.0.0", Capabilities);
    }
    if (malformed_payload) try replace_tlv_tag(&input, input_length, c_t.InteropTagCapabilities, 0x7ffe);
    _ = try target.feed(token, input[0..input_length]);
    failing.fail_index = failing.alloc_index;
    var output: [MaxTestFrame]u8 = undefined;
    const result = try target.next_event(token, 203, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, result.event.kind);
    try std_t.testing.expect(target.terminal_send_unavailable());
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, target.phase());
    _ = try target.transport_lost(token);
    target.deinit();
    target_live = false;
    try std_t.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
}

fn writer_for(buffer: []u8) c_t.interop_writer_t {
    var writer: c_t.interop_writer_t = undefined;
    c_t.interop_writer_init(&writer, buffer.ptr, buffer.len);
    return writer;
}

fn write_uint(writer: *c_t.interop_writer_t, tag: u16, value: u64) !void {
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_put_uint(writer, tag, value));
}

fn finish(writer: *c_t.interop_writer_t, frame_type: u16, request_id: u32, direction: c_t.interop_direction_t, length_out: *usize) !void {
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_writer_finish(writer, frame_type, request_id, direction, length_out));
}

fn make_hello(buffer: []u8, request_id: u32, epoch: u64, version: []const u8, capabilities: u64) !usize {
    var writer = writer_for(buffer);
    try write_uint(&writer, c_t.InteropTagCapabilities, capabilities);
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_put_bytes(&writer, c_t.InteropTagAgentVersion, version.ptr, @intCast(version.len)));
    try write_uint(&writer, c_t.InteropTagLayoutVersion, c_t.InteropLayoutVersion);
    try write_uint(&writer, c_t.InteropTagSessionEpoch, epoch);
    var length: usize = 0;
    try finish(&writer, c_t.InteropMsgHello, request_id, c_t.InteropFromGuest, &length);
    return length;
}

fn make_hello_ack(buffer: []u8, request_id: u32, version: []const u8, capabilities: u64) !usize {
    var writer = writer_for(buffer);
    try write_uint(&writer, c_t.InteropTagCapabilities, capabilities);
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_put_bytes(&writer, c_t.InteropTagAgentVersion, version.ptr, @intCast(version.len)));
    try write_uint(&writer, c_t.InteropTagLayoutVersion, c_t.InteropLayoutVersion);
    var length: usize = 0;
    try finish(&writer, c_t.InteropMsgHelloAck, request_id, c_t.InteropFromHost, &length);
    return length;
}

fn make_empty(buffer: []u8, frame_type: u16, request_id: u32, direction: c_t.interop_direction_t) !usize {
    var writer = writer_for(buffer);
    var length: usize = 0;
    try finish(&writer, frame_type, request_id, direction, &length);
    return length;
}

fn make_error(buffer: []u8, request_id: u32, code: u32, direction: c_t.interop_direction_t) !usize {
    var writer = writer_for(buffer);
    try write_uint(&writer, c_t.InteropTagErrorCode, code);
    var length: usize = 0;
    try finish(&writer, c_t.InteropMsgError, request_id, direction, &length);
    return length;
}

fn make_goodbye(buffer: []u8, direction: c_t.interop_direction_t) !usize {
    return make_goodbye_id(buffer, direction, 0);
}

fn make_goodbye_id(buffer: []u8, direction: c_t.interop_direction_t, request_id: u32) !usize {
    var writer = writer_for(buffer);
    try write_uint(&writer, c_t.InteropTagStatus, c_t.InteropCancelled);
    var length: usize = 0;
    try finish(&writer, c_t.InteropMsgGoodbye, request_id, direction, &length);
    return length;
}

fn replace_tlv_tag(frame: []u8, frame_length: usize, tag: u16, replacement: u16) !void {
    var value_pointer: [*c]const u8 = null;
    var value_length: u16 = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_field_get(frame[c_t.InteropHeaderBytes..frame_length].ptr, frame_length - c_t.InteropHeaderBytes, tag, 0, &value_pointer, &value_length));
    const tag_offset = @intFromPtr(value_pointer) - @intFromPtr(frame.ptr) - 4;
    std_t.mem.writeInt(u16, @ptrCast(frame[tag_offset..].ptr), replacement, .little);
}

fn set_request_id(frame: []u8, request_id: u32) void {
    std_t.mem.writeInt(u32, frame[12..16], request_id, .little);
}

fn complete_handshake(pair: *pair_t, now_ms: u64) !void {
    const ready = try complete_handshake_event(pair, now_ms);
    try std_t.testing.expectEqual(Capabilities, ready.capabilities);
}

/// Run the full handshake for any negotiated capability offer and return the guest Ready event.
fn complete_handshake_event(pair: *pair_t, now_ms: u64) !peer_t.event_t {
    const initialized = try pair.guest.guest_mapping_initialized(GuestToken, now_ms, Epoch, true);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, initialized.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloSent, pair.guest.phase());

    const hello_ack_event = try transfer_front_fragmented(&pair.guest, GuestToken, &pair.host, HostToken, now_ms, &.{ 1, 2, 7, 3, 4 });
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, hello_ack_event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloAck, pair.guest.phase());
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHello, pair.host.phase());

    var output: [MaxTestFrame]u8 = undefined;
    const hello_event = try pair.host.next_event(HostToken, now_ms, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.ValidateMapping, hello_event.event.kind);
    try std_t.testing.expectEqual(Epoch, hello_event.event.epoch);
    try std_t.testing.expectEqual(c_t.InteropLayoutVersion, hello_event.event.layout_version);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitMapping, pair.host.phase());

    const validated = try pair.host.host_mapping_validated(HostToken, now_ms, Epoch, c_t.InteropLayoutVersion, true);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, validated.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloAckSent, pair.host.phase());
    const ack_event = try transfer_front_fragmented(&pair.host, HostToken, &pair.guest, GuestToken, now_ms, &.{ 3, 1, 2, 9 });
    try std_t.testing.expectEqual(peer_t.event_kind_t.Ready, ack_event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Ready, pair.host.phase());

    const guest_ack_event = try pair.guest.next_event(GuestToken, now_ms, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Ready, guest_ack_event.event.kind);
    try std_t.testing.expectEqual(Epoch, guest_ack_event.event.epoch);
    try std_t.testing.expectEqual(peer_t.phase_t.Ready, pair.guest.phase());
    return guest_ack_event.event;
}

fn transfer_front_fragmented(
    sender: *peer_t.peer_t,
    sender_token: u64,
    receiver: *peer_t.peer_t,
    receiver_token: u64,
    now_ms: u64,
    fragments: []const usize,
) !peer_t.event_t {
    const first = (try sender.peek_tx(sender_token)) orelse return error.NoQueuedFrame;
    const frame_length = first.len;
    var sent: usize = 0;
    var fragment_index: usize = 0;
    var last_event: peer_t.event_t = .{ .phase = sender.phase() };
    while (sent < frame_length) {
        const front = (try sender.peek_tx(sender_token)) orelse return error.NoQueuedFrame;
        const fragment_limit = fragments[fragment_index % fragments.len];
        const count = @min(@min(fragment_limit, front.len), frame_length - sent);
        const fed = try receiver.feed(receiver_token, front[0..count]);
        try std_t.testing.expectEqual(count, fed.consumed);
        last_event = try sender.ack_tx(sender_token, now_ms, count);
        sent += count;
        fragment_index += 1;
    }
    return last_event;
}

fn feed_frame_fragmented(target: *peer_t.peer_t, token: u64, bytes: []const u8, fragments: []const usize) !void {
    var offset: usize = 0;
    var index: usize = 0;
    while (offset < bytes.len) {
        const count = @min(fragments[index % fragments.len], bytes.len - offset);
        const result = try target.feed(token, bytes[offset .. offset + count]);
        try std_t.testing.expectEqual(count, result.consumed);
        offset += count;
        index += 1;
    }
}

test "two peers exchange fragmented Hello and Ack only after mapping and full sends" {
    var pair = try init_pair(std_t.testing.allocator, 10);
    defer deinit_pair(&pair);

    try std_t.testing.expectEqual(peer_t.phase_t.InitializingMapping, pair.guest.phase());
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHello, pair.host.phase());
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, (try pair.guest.guest_mapping_initialized(GuestToken, 10, Epoch, true)).kind);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloSent, pair.guest.phase());

    const hello_view = (try pair.guest.peek_tx(GuestToken)).?;
    var hello_header: c_t.interop_header_t = undefined;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(hello_view.ptr, hello_view.len, &hello_header));
    try std_t.testing.expectEqual(c_t.InteropMsgHello, hello_header.type);
    try std_t.testing.expect(hello_header.request_id != 0);
    const hello_length = hello_view.len;
    const first_bytes = hello_view[0..7];
    const first_feed = try pair.host.feed(HostToken, first_bytes);
    try std_t.testing.expectEqual(@as(usize, 7), first_feed.consumed);
    _ = try pair.guest.ack_tx(GuestToken, 11, first_bytes.len);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloSent, pair.guest.phase());
    try std_t.testing.expectEqual(@as(usize, 0), pair.host.pending_bytes());

    // Remaining fragments are copied and acknowledged; neither a queued nor a partial Hello is Ready.
    const rest = (try pair.guest.peek_tx(GuestToken)).?;
    var offset: usize = 0;
    for ([_]usize{ 1, 4, 2, 5, 3, 8, 6 }) |fragment| {
        if (offset == rest.len) break;
        const count = @min(fragment, rest.len - offset);
        const current = (try pair.guest.peek_tx(GuestToken)).?;
        const fed = try pair.host.feed(HostToken, current[0..count]);
        try std_t.testing.expectEqual(count, fed.consumed);
        _ = try pair.guest.ack_tx(GuestToken, 11, count);
        offset += count;
    }
    while (offset < hello_length - 7) {
        const current = (try pair.guest.peek_tx(GuestToken)).?;
        const count = @min(@as(usize, 5), current.len);
        const fed = try pair.host.feed(HostToken, current[0..count]);
        try std_t.testing.expectEqual(count, fed.consumed);
        _ = try pair.guest.ack_tx(GuestToken, 11, count);
        offset += count;
    }
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHello, pair.host.phase());
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloAck, pair.guest.phase());

    var output: [MaxTestFrame]u8 = undefined;
    const validate = try pair.host.next_event(HostToken, 12, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.ValidateMapping, validate.event.kind);
    try std_t.testing.expectEqual(Epoch, validate.event.epoch);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitMapping, pair.host.phase());

    // The host may not Ack or become Ready until its external mapping validator succeeds.
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, (try pair.host.host_mapping_validated(HostToken, 12, Epoch, c_t.InteropLayoutVersion, true)).kind);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloAckSent, pair.host.phase());
    const ack_send_event = try transfer_front_fragmented(&pair.host, HostToken, &pair.guest, GuestToken, 12, &.{ 1, 3, 2, 4 });
    try std_t.testing.expectEqual(peer_t.event_kind_t.Ready, ack_send_event.kind);
    const guest_ready = try pair.guest.next_event(GuestToken, 12, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Ready, guest_ready.event.kind);
    try std_t.testing.expectEqual(Epoch, guest_ready.event.epoch);
}

test "local token time count and short-output errors preserve peer and caller state" {
    var pair = try init_pair(std_t.testing.allocator, 100);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 101);

    var feature: [MaxTestFrame]u8 = undefined;
    const feature_length = try make_window_retire(&feature, 55, 2);
    try feed_frame_fragmented(&pair.host, HostToken, feature[0..feature_length], &.{ 2, 1, 9 });
    const pending_before = pair.host.pending_bytes();
    try std_t.testing.expect(pending_before == feature_length);
    const phase_before = pair.host.phase();

    var short_output = [_]u8{0xA5} ** 4;
    try std_t.testing.expectError(error.OutputTooSmall, pair.host.next_event(HostToken, 200, &short_output));
    try std_t.testing.expectEqualSlices(u8, &([_]u8{0xA5} ** 4), &short_output);
    try std_t.testing.expectEqual(pending_before, pair.host.pending_bytes());
    try std_t.testing.expectEqual(phase_before, pair.host.phase());

    // A backward timestamp proves short-output preflight did not commit the attempted time.
    var output: [MaxTestFrame]u8 = undefined;
    const event = try pair.host.next_event(HostToken, 150, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.FeatureFrame, event.event.kind);
    try std_t.testing.expectEqualSlices(u8, feature[0..feature_length], output[0..event.event.copied_bytes]);

    const after = pair.host.phase();
    try std_t.testing.expectError(error.WrongConnectionToken, pair.host.tick(HostToken + 1, 201));
    try std_t.testing.expectEqual(after, pair.host.phase());
    try std_t.testing.expectError(error.TimeWentBackwards, pair.host.tick(HostToken, 149));
    try std_t.testing.expectEqual(after, pair.host.phase());
}

test "coalesced feature frames backpressure at one complete frame and preserve each frame" {
    var pair = try init_pair(std_t.testing.allocator, 300);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 301);

    var first: [MaxTestFrame]u8 = undefined;
    var second: [MaxTestFrame]u8 = undefined;
    const first_length = try make_window_retire(&first, 54, 1);
    var retire_writer = writer_for(&second);
    try write_uint(&retire_writer, c_t.InteropTagWindow, 55);
    try write_uint(&retire_writer, c_t.InteropTagGeneration, 2);
    var retire_length: usize = 0;
    try finish(&retire_writer, c_t.InteropMsgWindowRetire, 0, c_t.InteropFromGuest, &retire_length);
    var coalesced: [MaxTestFrame * 2]u8 = undefined;
    @memcpy(coalesced[0..first_length], first[0..first_length]);
    @memcpy(coalesced[first_length .. first_length + retire_length], second[0..retire_length]);

    const fed = try pair.host.feed(HostToken, coalesced[0 .. first_length + retire_length]);
    try std_t.testing.expectEqual(first_length, fed.consumed);
    var output: [MaxTestFrame]u8 = undefined;
    const first_event = try pair.host.next_event(HostToken, 302, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.FeatureFrame, first_event.event.kind);
    try std_t.testing.expectEqual(first_length, first_event.event.copied_bytes);
    try std_t.testing.expectEqualSlices(u8, first[0..first_length], output[0..first_length]);

    const remainder = coalesced[first_length .. first_length + retire_length];
    try feed_frame_fragmented(&pair.host, HostToken, remainder, &.{ 1, 4, 2, 3 });
    const second_event = try pair.host.next_event(HostToken, 303, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.FeatureFrame, second_event.event.kind);
    try std_t.testing.expectEqual(c_t.InteropMsgWindowRetire, second_event.event.message_type);
    try std_t.testing.expectEqualSlices(u8, remainder, output[0..second_event.event.copied_bytes]);
}

test "Hello and Ack readiness require full frame bytes and explicit mapping callbacks" {
    var pair = try init_pair(std_t.testing.allocator, 400);
    defer deinit_pair(&pair);
    _ = try pair.guest.guest_mapping_initialized(GuestToken, 401, Epoch, true);
    const hello = (try pair.guest.peek_tx(GuestToken)).?;
    const hello_length = hello.len;
    const unchanged_phase = pair.guest.phase();
    try std_t.testing.expectError(error.InvalidWriteCount, pair.guest.ack_tx(GuestToken, 401, hello_length + 1));
    try std_t.testing.expectEqual(unchanged_phase, pair.guest.phase());
    try std_t.testing.expectEqual(hello_length, (try pair.guest.peek_tx(GuestToken)).?.len);
    const partial = hello[0 .. hello.len - 1];
    const fed = try pair.host.feed(HostToken, partial);
    try std_t.testing.expectEqual(partial.len, fed.consumed);
    _ = try pair.guest.ack_tx(GuestToken, 402, partial.len);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloSent, pair.guest.phase());
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHello, pair.host.phase());
    try std_t.testing.expectEqual(@as(usize, 0), pair.host.pending_bytes());

    const last = (try pair.guest.peek_tx(GuestToken)).?;
    try std_t.testing.expectEqual(@as(usize, 1), last.len);
    try std_t.testing.expectEqual(@as(usize, 1), (try pair.host.feed(HostToken, last)).consumed);
    const final_hello_send = try pair.guest.ack_tx(GuestToken, 402, 1);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, final_hello_send.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloAck, pair.guest.phase());
    try std_t.testing.expectEqual(hello_length, pair.host.pending_bytes());

    var output: [MaxTestFrame]u8 = undefined;
    const hello_event = try pair.host.next_event(HostToken, 403, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.ValidateMapping, hello_event.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitMapping, pair.host.phase());
    try std_t.testing.expectError(error.WrongPhase, pair.guest.guest_mapping_initialized(GuestToken, 403, Epoch + 1, true));
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloAck, pair.guest.phase());

    _ = try pair.host.host_mapping_validated(HostToken, 403, Epoch, c_t.InteropLayoutVersion, true);
    const ack = (try pair.host.peek_tx(HostToken)).?;
    const ack_length = ack.len;
    const ack_prefix = ack[0 .. ack_length - 1];
    try std_t.testing.expectEqual(ack_prefix.len, (try pair.guest.feed(GuestToken, ack_prefix)).consumed);
    _ = try pair.host.ack_tx(HostToken, 404, ack_prefix.len);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloAckSent, pair.host.phase());
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloAck, pair.guest.phase());
    try std_t.testing.expectEqual(@as(usize, 0), pair.guest.pending_bytes());

    const ack_last = (try pair.host.peek_tx(HostToken)).?;
    try std_t.testing.expectEqual(@as(usize, 1), ack_last.len);
    try std_t.testing.expectEqual(@as(usize, 1), (try pair.guest.feed(GuestToken, ack_last)).consumed);
    const final_ack_send = try pair.host.ack_tx(HostToken, 404, 1);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Ready, final_ack_send.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Ready, pair.host.phase());
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloAck, pair.guest.phase());
    const guest_ready = try pair.guest.next_event(GuestToken, 404, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Ready, guest_ready.event.kind);
}

test "mapping failures do not bypass the external gate or mutate guest on local rejection" {
    var pair = try init_pair(std_t.testing.allocator, 450);
    defer deinit_pair(&pair);

    try std_t.testing.expectError(error.MappingInitializationFailed, pair.guest.guest_mapping_initialized(GuestToken, 451, Epoch, false));
    try std_t.testing.expectEqual(peer_t.phase_t.InitializingMapping, pair.guest.phase());
    try std_t.testing.expect((try pair.guest.peek_tx(GuestToken)) == null);
    try std_t.testing.expectError(error.InvalidSessionEpoch, pair.guest.guest_mapping_initialized(GuestToken, 451, 0, true));
    try std_t.testing.expectEqual(peer_t.phase_t.InitializingMapping, pair.guest.phase());

    _ = try pair.guest.guest_mapping_initialized(GuestToken, 451, Epoch, true);
    _ = try transfer_front_fragmented(&pair.guest, GuestToken, &pair.host, HostToken, 452, &.{ 2, 3, 1 });
    var output: [MaxTestFrame]u8 = undefined;
    const hello = try pair.host.next_event(HostToken, 453, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.ValidateMapping, hello.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitMapping, pair.host.phase());

    const failed_validation = try pair.host.host_mapping_validated(HostToken, 453, Epoch + 1, c_t.InteropLayoutVersion, true);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, failed_validation.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.host.phase());
    try std_t.testing.expectError(error.WrongPhase, pair.host.host_mapping_validated(HostToken, 454, Epoch, c_t.InteropLayoutVersion, true));
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.host.phase());
}

test "ping pong is internal, one matched reply refreshes liveness" {
    var pair = try init_pair(std_t.testing.allocator, 500);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 501);

    _ = try pair.host.tick(HostToken, 501 + c_t.InteropPingIntervalMs);
    const ping = (try pair.host.peek_tx(HostToken)).?;
    var ping_header: c_t.interop_header_t = undefined;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(ping.ptr, ping.len, &ping_header));
    try std_t.testing.expectEqual(c_t.InteropMsgPing, ping_header.type);
    try std_t.testing.expect(ping_header.request_id != 0);
    _ = try transfer_front_fragmented(&pair.host, HostToken, &pair.guest, GuestToken, 501 + c_t.InteropPingIntervalMs, &.{ 2, 5 });

    var output: [MaxTestFrame]u8 = undefined;
    const ping_event = try pair.guest.next_event(GuestToken, 501 + c_t.InteropPingIntervalMs, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, ping_event.event.kind);
    try std_t.testing.expectEqual(@as(usize, 0), pair.guest.pending_bytes());
    const pong = (try pair.guest.peek_tx(GuestToken)).?;
    var pong_header: c_t.interop_header_t = undefined;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(pong.ptr, pong.len, &pong_header));
    try std_t.testing.expectEqual(c_t.InteropMsgPong, pong_header.type);
    try std_t.testing.expectEqual(ping_header.request_id, pong_header.request_id);
    _ = try transfer_front_fragmented(&pair.guest, GuestToken, &pair.host, HostToken, 501 + c_t.InteropPingIntervalMs + 1, &.{ 1, 4 });
    const pong_event = try pair.host.next_event(HostToken, 501 + c_t.InteropPingIntervalMs + 1, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, pong_event.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Ready, pair.host.phase());
}

test "scoped Error never receives a recursive reply; malformed Error only drains" {
    var pair = try init_pair(std_t.testing.allocator, 700);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 701);

    var remote_error: [MaxTestFrame]u8 = undefined;
    const error_length = try make_error(&remote_error, 41, c_t.InteropErrorUnexpected, c_t.InteropFromGuest);
    try feed_frame_fragmented(&pair.host, HostToken, remote_error[0..error_length], &.{ 2, 1, 8 });
    var output: [MaxTestFrame]u8 = undefined;
    const event = try pair.host.next_event(HostToken, 702, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.PeerError, event.event.kind);
    try std_t.testing.expectEqual(c_t.InteropMsgError, event.event.message_type);
    try std_t.testing.expectEqual(@as(u32, 41), event.event.request_id);
    try std_t.testing.expectEqualSlices(u8, remote_error[0..error_length], output[0..event.event.copied_bytes]);
    try std_t.testing.expect((try pair.host.peek_tx(HostToken)) == null);
    try std_t.testing.expectEqual(peer_t.phase_t.Ready, pair.host.phase());

    var malformed_error: [MaxTestFrame]u8 = undefined;
    const malformed_length = try make_empty(&malformed_error, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest);
    malformed_error[6] = @intCast(c_t.InteropMsgError & 0xff);
    malformed_error[7] = @intCast(c_t.InteropMsgError >> 8);
    try feed_frame_fragmented(&pair.host, HostToken, malformed_error[0..malformed_length], &.{ 1, 2, 4 });
    const malformed_event = try pair.host.next_event(HostToken, 703, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, malformed_event.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.host.phase());
    try std_t.testing.expect((try pair.host.peek_tx(HostToken)) == null);
}

test "exact deadlines and terminal callbacks never reopen the peer" {
    var pair = try init_pair(std_t.testing.allocator, 0);
    defer deinit_pair(&pair);
    _ = try pair.guest.guest_mapping_initialized(GuestToken, 1, Epoch, true);
    const hello = (try pair.guest.peek_tx(GuestToken)).?;
    const total = hello.len;
    const prefix = hello[0 .. total - 1];
    try std_t.testing.expectEqual(prefix.len, (try pair.host.feed(HostToken, prefix)).consumed);
    _ = try pair.guest.ack_tx(GuestToken, 2, prefix.len);
    const last = (try pair.guest.peek_tx(GuestToken)).?;
    try std_t.testing.expectEqual(@as(usize, 1), last.len);
    try std_t.testing.expectEqual(@as(usize, 1), (try pair.host.feed(HostToken, last)).consumed);
    const deadline_send = try pair.guest.ack_tx(GuestToken, c_t.InteropHelloTimeoutMs, 1);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, deadline_send.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.guest.phase());
    // The OS already sent the final byte. It must be consumed, never reoffered.
    try std_t.testing.expect((try pair.guest.peek_tx(GuestToken)) == null);

    // Host has a complete Hello at the deadline; timeout wins before mapping dispatch.
    var output: [MaxTestFrame]u8 = undefined;
    const host_deadline = try pair.host.next_event(HostToken, c_t.InteropHelloTimeoutMs, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, host_deadline.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.host.phase());
    // Timeout wins before dispatch: the complete Hello remains retained until teardown.
    try std_t.testing.expectEqual(total, pair.host.pending_bytes());

    const closed = try pair.guest.transport_lost(GuestToken);
    try std_t.testing.expectEqual(peer_t.phase_t.Closed, closed.phase);
    try std_t.testing.expectEqual(peer_t.phase_t.Closed, pair.guest.phase());
    const repeated = try pair.guest.transport_lost(GuestToken);
    try std_t.testing.expectEqual(peer_t.phase_t.Closed, repeated.phase);
    try std_t.testing.expectEqual(peer_t.phase_t.Closed, pair.guest.phase());
}

test "mandatory Hello queue OOM drains and deinit frees the owned version" {
    var failing = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = 1 });
    var guest = try init_peer(failing.allocator(), .Guest, GuestToken, 900);
    try std_t.testing.expectEqual(peer_t.phase_t.InitializingMapping, guest.phase());
    const result = try guest.guest_mapping_initialized(GuestToken, 901, Epoch, true);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, result.kind);
    try std_t.testing.expect(guest.terminal_send_unavailable());
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, guest.phase());
    try std_t.testing.expect((try guest.peek_tx(GuestToken)) == null);
    guest.deinit();
    guest.deinit();
    try std_t.testing.expectEqual(peer_t.phase_t.Closed, guest.phase());
}

test "private peer composition probes cover allocator, codec, and alias edges" {
    try peer_t.test_support_t.run_all();
}

test "Hello and Ack reject bad major IDs and capability offers" {
    const hello_cases = [_]struct { version: []const u8, capabilities: u64, zero_id: bool }{
        .{ .version = "2.0.0", .capabilities = Capabilities, .zero_id = false },
        .{ .version = Version, .capabilities = Capabilities, .zero_id = true },
    };
    for (hello_cases) |case| {
        var host = try init_peer(std_t.testing.allocator, .Host, HostToken, 10);
        defer host.deinit();
        var hello: [MaxTestFrame]u8 = undefined;
        const length = try make_hello(&hello, 1, Epoch, case.version, case.capabilities);
        if (case.zero_id) set_request_id(hello[0..length], 0);
        try feed_frame_fragmented(&host, HostToken, hello[0..length], &.{ 1, 4, 2 });
        var output: [MaxTestFrame]u8 = undefined;
        const event = try host.next_event(HostToken, 11, &output);
        try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, event.event.kind);
        try std_t.testing.expectEqual(peer_t.phase_t.Draining, host.phase());
        try std_t.testing.expect((try host.peek_tx(HostToken)) != null);
    }

    // A known capability the host did not offer is negotiated away, not rejected.
    var host = try init_peer(std_t.testing.allocator, .Host, HostToken, 15);
    defer host.deinit();
    var offered: [MaxTestFrame]u8 = undefined;
    const offered_length = try make_hello(&offered, 5, Epoch, Version, Capabilities | c_t.InteropCapAudio);
    try feed_frame_fragmented(&host, HostToken, offered[0..offered_length], &.{ 4, 2, 3 });
    var output: [MaxTestFrame]u8 = undefined;
    const offered_event = try host.next_event(HostToken, 16, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.ValidateMapping, offered_event.event.kind);
    _ = try host.host_mapping_validated(HostToken, 16, Epoch, c_t.InteropLayoutVersion, true);
    const negotiated_ack = (try host.peek_tx(HostToken)).?;
    var ack_header: c_t.interop_header_t = undefined;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(negotiated_ack.ptr, negotiated_ack.len, &ack_header));
    var negotiated_capabilities: u64 = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_field_uint(negotiated_ack.ptr + c_t.InteropHeaderBytes, ack_header.length, c_t.InteropTagCapabilities, 8, &negotiated_capabilities));
    try std_t.testing.expectEqual(Capabilities, negotiated_capabilities);
    const host_ready = try host.ack_tx(HostToken, 17, negotiated_ack.len);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Ready, host_ready.kind);

    const ack_cases = [_]struct { version: []const u8, capabilities: u64, wrong_id: bool }{
        .{ .version = "2.0.0", .capabilities = Capabilities, .wrong_id = false },
        .{ .version = Version, .capabilities = Capabilities | c_t.InteropCapAudio, .wrong_id = false },
        .{ .version = Version, .capabilities = Capabilities, .wrong_id = true },
    };
    for (ack_cases) |case| {
        var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 20);
        defer guest.deinit();
        _ = try guest.guest_mapping_initialized(GuestToken, 21, Epoch, true);
        const hello = (try guest.peek_tx(GuestToken)).?;
        var hello_header: c_t.interop_header_t = undefined;
        try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(hello.ptr, hello.len, &hello_header));
        _ = try guest.ack_tx(GuestToken, 22, hello.len);
        var ack: [MaxTestFrame]u8 = undefined;
        const ack_id = if (case.wrong_id) hello_header.request_id + 1 else hello_header.request_id;
        const ack_length = try make_hello_ack(&ack, ack_id, case.version, case.capabilities);
        try feed_frame_fragmented(&guest, GuestToken, ack[0..ack_length], &.{ 3, 1, 6 });
        var ack_output: [MaxTestFrame]u8 = undefined;
        const event = try guest.next_event(GuestToken, 23, &ack_output);
        try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, event.event.kind);
        try std_t.testing.expectEqual(peer_t.phase_t.Draining, guest.phase());
    }
}

test "token phase time closed and empty-queue errors preserve observable state" {
    var pair = try init_pair(std_t.testing.allocator, 1000);
    defer deinit_pair(&pair);
    const host_phase = pair.host.phase();
    const host_queue_count = pair.host.queue.count;
    const host_last_time = pair.host.last_now_ms;
    try std_t.testing.expectError(error.InvalidConnectionToken, pair.host.tick(0, 1001));
    try std_t.testing.expectError(error.WrongConnectionToken, pair.host.tick(HostToken + 1, 1001));
    try std_t.testing.expectError(error.WrongPhase, pair.host.host_mapping_validated(HostToken, 1001, Epoch, c_t.InteropLayoutVersion, true));
    try std_t.testing.expectError(error.WrongPhase, pair.host.guest_mapping_initialized(HostToken, 1001, Epoch, true));
    try std_t.testing.expectError(error.TimeWentBackwards, pair.host.tick(HostToken, 999));
    try std_t.testing.expectEqual(host_phase, pair.host.phase());
    try std_t.testing.expectEqual(host_queue_count, pair.host.queue.count);
    try std_t.testing.expectEqual(host_last_time, pair.host.last_now_ms);
    try std_t.testing.expectError(error.InvalidConnectionToken, pair.host.peek_tx(0));
    try std_t.testing.expectError(error.InvalidConnectionToken, pair.host.ack_tx(0, 1001, 0));
    try std_t.testing.expectError(error.NoQueuedFrame, pair.host.ack_tx(HostToken, 1001, 0));
    try std_t.testing.expectEqual(host_phase, pair.host.phase());
    try std_t.testing.expectEqual(host_last_time, pair.host.last_now_ms);

    pair.host.deinit();
    try std_t.testing.expectError(error.PeerClosed, pair.host.tick(HostToken, 1001));
    pair.host.deinit();
    try std_t.testing.expectEqual(peer_t.phase_t.Closed, pair.host.phase());
}

test "initialization rejects invalid local version and unknown capability bits" {
    try std_t.testing.expectError(error.InvalidVersion, peer_t.peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = GuestToken,
        .accepted_at_ms = 0,
        .local_version = "1.2",
        .local_capabilities = Capabilities,
    }));
    try std_t.testing.expectError(error.UnknownCapabilities, peer_t.peer_t.init(std_t.testing.allocator, .{
        .role = .Guest,
        .connection_token = GuestToken,
        .accepted_at_ms = 0,
        .local_version = Version,
        .local_capabilities = @as(u64, 1) << 63,
    }));
}

test "wire header errors latch draining and wrong-direction controls do not dispatch" {
    var host = try init_peer(std_t.testing.allocator, .Host, HostToken, 3000);
    defer host.deinit();
    var bad_header: [MaxTestFrame]u8 = undefined;
    const hello_length = try make_hello(&bad_header, 7, Epoch, Version, Capabilities);
    bad_header[0] ^= 0x80;
    const failed = try host.feed(HostToken, bad_header[0..hello_length]);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, failed.phase);
    try std_t.testing.expect(failed.terminal_error != null);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, host.phase());
    try std_t.testing.expectError(error.WrongPhase, host.feed(HostToken, bad_header[0..hello_length]));
    try std_t.testing.expect((try host.peek_tx(HostToken)) != null);

    var pair = try init_pair(std_t.testing.allocator, 3100);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 3101);
    var ping: [MaxTestFrame]u8 = undefined;
    const ping_length = try make_empty(&ping, c_t.InteropMsgPing, 9, c_t.InteropFromHost);
    try feed_frame_fragmented(&pair.host, HostToken, ping[0..ping_length], &.{ 1, 4, 2 });
    var output: [MaxTestFrame]u8 = undefined;
    const unsupported_direction = try pair.host.next_event(HostToken, 3102, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, unsupported_direction.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.host.phase());
    try std_t.testing.expect((try pair.host.peek_tx(HostToken)) != null);
}

test "Goodbye output preflight is atomic and request IDs are validated" {
    var pair = try init_pair(std_t.testing.allocator, 3200);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 3201);

    var goodbye: [MaxTestFrame]u8 = undefined;
    const valid_length = try make_goodbye(&goodbye, c_t.InteropFromGuest);
    try feed_frame_fragmented(&pair.host, HostToken, goodbye[0..valid_length], &.{ 2, 1, 3 });
    const before_pending = pair.host.pending_bytes();
    const before_time = pair.host.last_now_ms;
    try std_t.testing.expectError(error.OutputTooSmall, pair.host.next_event(HostToken, 3202, &.{}));
    try std_t.testing.expectEqual(peer_t.phase_t.Ready, pair.host.phase());
    try std_t.testing.expectEqual(before_pending, pair.host.pending_bytes());
    try std_t.testing.expectEqual(before_time, pair.host.last_now_ms);
    var output: [MaxTestFrame]u8 = undefined;
    const event = try pair.host.next_event(HostToken, 3202, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, event.event.kind);
    try std_t.testing.expectEqual(c_t.InteropMsgGoodbye, event.event.message_type);
    try std_t.testing.expectEqual(@as(usize, valid_length), event.event.copied_bytes);
    try std_t.testing.expectEqualSlices(u8, goodbye[0..valid_length], output[0..valid_length]);

    var second_pair = try init_pair(std_t.testing.allocator, 3300);
    defer deinit_pair(&second_pair);
    try complete_handshake(&second_pair, 3301);
    const invalid_length = try make_goodbye_id(&goodbye, c_t.InteropFromGuest, 81);
    try feed_frame_fragmented(&second_pair.host, HostToken, goodbye[0..invalid_length], &.{ 3, 2 });
    const invalid = try second_pair.host.next_event(HostToken, 3302, &.{});
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, invalid.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, second_pair.host.phase());
}

test "local drain aborts incomplete Hello but still accounts confirmed OS bytes" {
    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 3400);
    defer guest.deinit();
    _ = try guest.guest_mapping_initialized(GuestToken, 3401, Epoch, true);
    const hello = (try guest.peek_tx(GuestToken)).?;
    const already_sent = hello[0..4];
    _ = try guest.ack_tx(GuestToken, 3402, already_sent.len);
    const before_remaining = (try guest.peek_tx(GuestToken)).?.len;
    const drain = try guest.begin_drain(GuestToken, 3403);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, drain.kind);
    try std_t.testing.expect(drain.abort_transport);
    try std_t.testing.expect((try guest.peek_tx(GuestToken)) == null);

    const in_flight_bytes = @min(@as(usize, 3), before_remaining);
    const acknowledged = try guest.ack_tx(GuestToken, 3404, in_flight_bytes);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, acknowledged.kind);
    try std_t.testing.expect((try guest.peek_tx(GuestToken)) == null);
    try std_t.testing.expect(guest.queue.pending_bytes < before_remaining);
}

test "Goodbye and unknown payload limits drain without recursive protocol replies" {
    var pair = try init_pair(std_t.testing.allocator, 3500);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 3501);

    var unknown: [MaxTestFrame]u8 = undefined;
    const length = try make_empty(&unknown, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest);
    unknown[6] = 0xff;
    unknown[7] = 0x7f;
    try feed_frame_fragmented(&pair.host, HostToken, unknown[0..length], &.{ 1, 2, 5 });
    var output: [MaxTestFrame]u8 = undefined;
    const event = try pair.host.next_event(HostToken, 3502, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, event.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.host.phase());
    try std_t.testing.expect((try pair.host.peek_tx(HostToken)) != null);
}

test "unknown Pong refreshes receive liveness but exact silence timeout drains" {
    var pair = try init_pair(std_t.testing.allocator, 3600);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 3601);
    const silence_timeout = c_t.InteropPingTimeoutMs;
    var pong: [MaxTestFrame]u8 = undefined;
    const pong_length = try make_empty(&pong, c_t.InteropMsgPong, 0xabc, c_t.InteropFromGuest);
    try feed_frame_fragmented(&pair.host, HostToken, pong[0..pong_length], &.{ 2, 3 });
    var output: [MaxTestFrame]u8 = undefined;
    const unknown_pong = try pair.host.next_event(HostToken, 3601 + silence_timeout - 1, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, unknown_pong.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Ready, pair.host.phase());

    var feature: [MaxTestFrame]u8 = undefined;
    const feature_length = try make_empty(&feature, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest);
    try feed_frame_fragmented(&pair.host, HostToken, feature[0..feature_length], &.{ 4, 1 });
    const timeout = try pair.host.next_event(HostToken, 3601 + silence_timeout * 2 - 1, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, timeout.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.host.phase());
}

test "copy-out rejects aliases into version, peer state, and queued frames" {
    const host = try init_peer_version(std_t.testing.allocator, .Host, HostToken, 3800, "1.2.3+abcdefghijklmnopqrstuvwxy");
    const guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 3800);
    var pair = pair_t{ .host = host, .guest = guest };
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 3801);

    var remote_error: [MaxTestFrame]u8 = undefined;
    const error_length = try make_error(&remote_error, 17, c_t.InteropErrorUnexpected, c_t.InteropFromGuest);
    try std_t.testing.expect(pair.host.local_version.len >= error_length);
    try feed_frame_fragmented(&pair.host, HostToken, remote_error[0..error_length], &.{ 2, 5, 1 });
    const before_time = pair.host.last_now_ms;
    try std_t.testing.expectError(error.OutputAliasesPeer, pair.host.next_event(HostToken, 3802, pair.host.local_version));
    try std_t.testing.expectEqual(peer_t.phase_t.Ready, pair.host.phase());
    try std_t.testing.expectEqual(error_length, pair.host.pending_bytes());
    try std_t.testing.expectEqual(before_time, pair.host.last_now_ms);
    var output: [MaxTestFrame]u8 = undefined;
    _ = try pair.host.next_event(HostToken, 3802, &output);

    _ = try pair.host.tick(HostToken, 3801 + c_t.InteropPingIntervalMs);
    var feature: [MaxTestFrame]u8 = undefined;
    const feature_length = try make_empty(&feature, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest);
    try feed_frame_fragmented(&pair.host, HostToken, feature[0..feature_length], &.{ 1, 3, 2 });
    const queued_bytes = pair.host.queue.frames[pair.host.queue.head].?.bytes;
    try std_t.testing.expect(queued_bytes.len >= feature_length);
    try std_t.testing.expectError(error.OutputAliasesPeer, pair.host.next_event(HostToken, 3801 + c_t.InteropPingIntervalMs, queued_bytes[0..feature_length]));
    const peer_pointer: [*]u8 = @ptrCast(&pair.host);
    try std_t.testing.expectError(error.OutputAliasesPeer, pair.host.next_event(HostToken, 3801 + c_t.InteropPingIntervalMs, peer_pointer[0..feature_length]));
    try std_t.testing.expectEqual(feature_length, pair.host.pending_bytes());
}

test "Pong capacity failure is terminal and the full control queue drains on the next Pong" {
    const host = try init_peer(std_t.testing.allocator, .Host, HostToken, 3900);
    const guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 3900);
    var pair = pair_t{ .host = host, .guest = guest };
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 3901);

    var ping: [MaxTestFrame]u8 = undefined;
    const ping_length = try make_empty(&ping, c_t.InteropMsgPing, 44, c_t.InteropFromHost);
    var output: [MaxTestFrame]u8 = undefined;
    for (0..peer_t.QueueCapacity) |index| {
        const now_ms = 3902 + index;
        try feed_frame_fragmented(&pair.guest, GuestToken, ping[0..ping_length], &.{ 2, 1 });
        const handled = try pair.guest.next_event(GuestToken, now_ms, &output);
        try std_t.testing.expectEqual(peer_t.event_kind_t.None, handled.event.kind);
        try std_t.testing.expectEqual(peer_t.phase_t.Ready, pair.guest.phase());
    }
    try std_t.testing.expectEqual(peer_t.QueueCapacity, pair.guest.queue.count);
    try feed_frame_fragmented(&pair.guest, GuestToken, ping[0..ping_length], &.{ 1, 3 });
    const full = try pair.guest.next_event(GuestToken, 3902 + peer_t.QueueCapacity, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, full.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.guest.phase());
    try std_t.testing.expect(pair.guest.terminal_send_unavailable());
}

test "mandatory Pong Ping and Error allocation failures stay terminal" {
    var failing_guest_allocator = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = 2 });
    var guest = try init_peer(failing_guest_allocator.allocator(), .Guest, GuestToken, 4100);
    var host = try init_peer(std_t.testing.allocator, .Host, HostToken, 4100);
    defer host.deinit();
    _ = try guest.guest_mapping_initialized(GuestToken, 4101, Epoch, true);
    _ = try transfer_front_fragmented(&guest, GuestToken, &host, HostToken, 4102, &.{ 3, 1 });
    var output: [MaxTestFrame]u8 = undefined;
    _ = try host.next_event(HostToken, 4102, &output);
    _ = try host.host_mapping_validated(HostToken, 4102, Epoch, c_t.InteropLayoutVersion, true);
    _ = try transfer_front_fragmented(&host, HostToken, &guest, GuestToken, 4103, &.{ 1, 4 });
    _ = try guest.next_event(GuestToken, 4103, &output);
    var ping: [MaxTestFrame]u8 = undefined;
    const ping_length = try make_empty(&ping, c_t.InteropMsgPing, 9, c_t.InteropFromHost);
    try feed_frame_fragmented(&guest, GuestToken, ping[0..ping_length], &.{ 2, 3 });
    const failed_pong = try guest.next_event(GuestToken, 4104, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, failed_pong.event.kind);
    try std_t.testing.expect(guest.terminal_send_unavailable());
    guest.deinit();

    var failing_host_allocator = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = 2 });
    var failing_host = try init_peer(failing_host_allocator.allocator(), .Host, HostToken, 4200);
    var normal_guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 4200);
    defer normal_guest.deinit();
    _ = try normal_guest.guest_mapping_initialized(GuestToken, 4201, Epoch, true);
    _ = try transfer_front_fragmented(&normal_guest, GuestToken, &failing_host, HostToken, 4202, &.{ 4, 1 });
    _ = try failing_host.next_event(HostToken, 4202, &output);
    _ = try failing_host.host_mapping_validated(HostToken, 4202, Epoch, c_t.InteropLayoutVersion, true);
    _ = try transfer_front_fragmented(&failing_host, HostToken, &normal_guest, GuestToken, 4203, &.{ 2, 3 });
    _ = try normal_guest.next_event(GuestToken, 4203, &output);
    const ping_failure = try failing_host.tick(HostToken, 4203 + c_t.InteropPingIntervalMs);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, ping_failure.kind);
    try std_t.testing.expect(failing_host.terminal_send_unavailable());
    failing_host.deinit();

    var failing_error_allocator = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = 1 });
    var failing_error_host = try init_peer(failing_error_allocator.allocator(), .Host, HostToken, 4300);
    defer failing_error_host.deinit();
    var unexpected: [MaxTestFrame]u8 = undefined;
    const unexpected_length = try make_empty(&unexpected, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest);
    try feed_frame_fragmented(&failing_error_host, HostToken, unexpected[0..unexpected_length], &.{ 2, 1 });
    const failed_error = try failing_error_host.next_event(HostToken, 4301, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, failed_error.event.kind);
    try std_t.testing.expect(failing_error_host.terminal_send_unavailable());
    const repeated = try failing_error_host.tick(HostToken, 4302);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, repeated.kind);
    try std_t.testing.expect(failing_error_host.terminal_send_unavailable());
}

test "deadline zero-progress acknowledgement drains without discarding OS-accounting state" {
    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 0);
    defer guest.deinit();
    _ = try guest.guest_mapping_initialized(GuestToken, 1, Epoch, true);
    const before = (try guest.peek_tx(GuestToken)).?.len;
    const timeout = try guest.ack_tx(GuestToken, c_t.InteropHelloTimeoutMs, 0);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, timeout.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, guest.phase());
    try std_t.testing.expect(guest.must_abort_transport());
    try std_t.testing.expect((try guest.peek_tx(GuestToken)) == null);

    const confirmed = try guest.ack_tx(GuestToken, c_t.InteropHelloTimeoutMs, 1);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, confirmed.kind);
    try std_t.testing.expect((try guest.peek_tx(GuestToken)) == null);
    try std_t.testing.expectEqual(before - 1, guest.queue.pending_bytes);
}

test "partial HelloAck aborted by drain still consumes its confirmed final bytes" {
    var pair = try init_pair(std_t.testing.allocator, 4400);
    defer deinit_pair(&pair);
    _ = try pair.guest.guest_mapping_initialized(GuestToken, 4401, Epoch, true);
    _ = try transfer_front_fragmented(&pair.guest, GuestToken, &pair.host, HostToken, 4402, &.{ 2, 3, 1 });
    var output: [MaxTestFrame]u8 = undefined;
    _ = try pair.host.next_event(HostToken, 4402, &output);
    _ = try pair.host.host_mapping_validated(HostToken, 4402, Epoch, c_t.InteropLayoutVersion, true);
    const ack = (try pair.host.peek_tx(HostToken)).?;
    const prefix_length = ack.len - 1;
    try std_t.testing.expectEqual(prefix_length, (try pair.guest.feed(GuestToken, ack[0..prefix_length])).consumed);
    _ = try pair.host.ack_tx(HostToken, 4403, prefix_length);
    const remaining = (try pair.host.peek_tx(HostToken)).?;
    try std_t.testing.expectEqual(@as(usize, 1), remaining.len);
    const final_byte = remaining[0];
    const drain = try pair.host.begin_drain(HostToken, 4404);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, drain.kind);
    try std_t.testing.expect(drain.abort_transport);
    try std_t.testing.expect((try pair.host.peek_tx(HostToken)) == null);
    try std_t.testing.expectEqual(@as(usize, 1), (try pair.guest.feed(GuestToken, &[_]u8{final_byte})).consumed);
    const accounted = try pair.host.ack_tx(HostToken, 4405, 1);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, accounted.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.host.phase());
    try std_t.testing.expect((try pair.host.peek_tx(HostToken)) == null);
}

test "connection token is checked before all peer callbacks and closed peers reject work" {
    var pair = try init_pair(std_t.testing.allocator, 4500);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 4501);
    const saved_phase = pair.guest.phase();
    const saved_time = pair.guest.last_now_ms;
    const saved_queue_count = pair.guest.queue.count;
    const saved_reader_bytes = pair.guest.reader.received;
    var output: [MaxTestFrame]u8 = undefined;
    try std_t.testing.expectError(error.InvalidConnectionToken, pair.guest.feed(0, &.{}));
    try std_t.testing.expectError(error.WrongConnectionToken, pair.guest.next_event(GuestToken + 1, 4502, &output));
    try std_t.testing.expectError(error.WrongConnectionToken, pair.guest.guest_mapping_initialized(GuestToken + 1, 4502, Epoch, true));
    try std_t.testing.expectError(error.WrongConnectionToken, pair.guest.host_mapping_validated(GuestToken + 1, 4502, Epoch, c_t.InteropLayoutVersion, true));
    try std_t.testing.expectError(error.WrongConnectionToken, pair.guest.peek_tx(GuestToken + 1));
    try std_t.testing.expectError(error.WrongConnectionToken, pair.guest.ack_tx(GuestToken + 1, 4502, 0));
    try std_t.testing.expectError(error.WrongConnectionToken, pair.guest.tick(GuestToken + 1, 4502));
    try std_t.testing.expectError(error.WrongConnectionToken, pair.guest.begin_drain(GuestToken + 1, 4502));
    try std_t.testing.expectError(error.WrongConnectionToken, pair.guest.transport_lost(GuestToken + 1));
    try std_t.testing.expectEqual(saved_phase, pair.guest.phase());
    try std_t.testing.expectEqual(saved_time, pair.guest.last_now_ms);
    try std_t.testing.expectEqual(saved_queue_count, pair.guest.queue.count);
    try std_t.testing.expectEqual(saved_reader_bytes, pair.guest.reader.received);

    _ = try pair.guest.transport_lost(GuestToken);
    try std_t.testing.expectEqual(peer_t.phase_t.Closed, pair.guest.phase());
    try std_t.testing.expectError(error.WrongPhase, pair.guest.feed(GuestToken, &.{}));
    try std_t.testing.expectEqual(peer_t.phase_t.Closed, pair.guest.phase());
}

test "mandatory HelloAck allocation failure drains the host" {
    var failing_allocator = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = 1 });
    var host = try init_peer(failing_allocator.allocator(), .Host, HostToken, 4600);
    defer host.deinit();
    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 4600);
    defer guest.deinit();
    _ = try guest.guest_mapping_initialized(GuestToken, 4601, Epoch, true);
    _ = try transfer_front_fragmented(&guest, GuestToken, &host, HostToken, 4602, &.{ 2, 4 });
    var output: [MaxTestFrame]u8 = undefined;
    const hello = try host.next_event(HostToken, 4602, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.ValidateMapping, hello.event.kind);
    const failed_ack = try host.host_mapping_validated(HostToken, 4602, Epoch, c_t.InteropLayoutVersion, true);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, failed_ack.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, host.phase());
    try std_t.testing.expect((try host.peek_tx(HostToken)) == null);
}

test "all handshake deadline entry points close at the boundary and stay closed" {
    const timeout_ms = c_t.InteropHelloTimeoutMs;
    var guest_at_deadline = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 5000);
    defer guest_at_deadline.deinit();
    const guest_expired = try guest_at_deadline.guest_mapping_initialized(GuestToken, 5000 + timeout_ms, Epoch, true);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, guest_expired.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, guest_at_deadline.phase());

    var deadline_pair = try init_pair(std_t.testing.allocator, 5100);
    defer deinit_pair(&deadline_pair);
    _ = try deadline_pair.guest.guest_mapping_initialized(GuestToken, 5101, Epoch, true);
    _ = try transfer_front_fragmented(&deadline_pair.guest, GuestToken, &deadline_pair.host, HostToken, 5102, &.{ 1, 3, 2 });
    var output: [MaxTestFrame]u8 = undefined;
    const validation = try deadline_pair.host.next_event(HostToken, 5102, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.ValidateMapping, validation.event.kind);
    const expired_validation = try deadline_pair.host.host_mapping_validated(HostToken, 5100 + timeout_ms, Epoch, c_t.InteropLayoutVersion, true);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, expired_validation.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, deadline_pair.host.phase());

    var ticking = try init_peer(std_t.testing.allocator, .Host, HostToken, 5200);
    defer ticking.deinit();
    const before_deadline = try ticking.tick(HostToken, 5200 + timeout_ms - 1);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, before_deadline.kind);
    const exact_deadline = try ticking.tick(HostToken, 5200 + timeout_ms);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, exact_deadline.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, ticking.phase());
    const already_drained = try ticking.tick(HostToken, 5200 + timeout_ms);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, already_drained.kind);
    _ = try ticking.transport_lost(HostToken);
    try std_t.testing.expectEqual(peer_t.phase_t.Closed, ticking.phase());
    const closed_tick = try ticking.tick(HostToken, 5200 + timeout_ms);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, closed_tick.kind);
    try std_t.testing.expectError(error.WrongPhase, ticking.next_event(HostToken, 5200 + timeout_ms, &output));
}

test "mapping callbacks reject wrong phase and role before any timeout mutation" {
    var host = try init_peer(std_t.testing.allocator, .Host, HostToken, 5300);
    defer host.deinit();
    const host_time = host.last_now_ms;
    try std_t.testing.expectError(error.WrongPhase, host.guest_mapping_initialized(HostToken, 5300 + c_t.InteropHelloTimeoutMs, Epoch, true));
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHello, host.phase());
    try std_t.testing.expectEqual(host_time, host.last_now_ms);

    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 5400);
    defer guest.deinit();
    const guest_time = guest.last_now_ms;
    try std_t.testing.expectError(error.WrongPhase, guest.host_mapping_validated(GuestToken, 5400 + c_t.InteropHelloTimeoutMs, Epoch, c_t.InteropLayoutVersion, true));
    try std_t.testing.expectEqual(peer_t.phase_t.InitializingMapping, guest.phase());
    try std_t.testing.expectEqual(guest_time, guest.last_now_ms);

    _ = try guest.guest_mapping_initialized(GuestToken, 5401, Epoch, true);
    const queued_phase = guest.phase();
    const queued_time = guest.last_now_ms;
    try std_t.testing.expectError(error.WrongPhase, guest.guest_mapping_initialized(GuestToken, 5402, Epoch + 1, true));
    try std_t.testing.expectEqual(queued_phase, guest.phase());
    try std_t.testing.expectEqual(queued_time, guest.last_now_ms);
}

test "drain is idempotent, deadline bounded, and Goodbye allocation failure is recorded" {
    var pair = try init_pair(std_t.testing.allocator, 5500);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 5501);
    const first = try pair.host.begin_drain(HostToken, 5502);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, first.kind);
    try std_t.testing.expectEqual(@as(usize, 1), pair.host.queue.count);
    const second = try pair.host.begin_drain(HostToken, 5503);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, second.kind);
    try std_t.testing.expectEqual(@as(usize, 1), pair.host.queue.count);
    _ = try pair.host.transport_lost(HostToken);
    const closed_drain = try pair.host.begin_drain(HostToken, 5503);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, closed_drain.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Closed, pair.host.phase());

    var deadline_guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 5600);
    defer deadline_guest.deinit();
    const deadline = try deadline_guest.begin_drain(GuestToken, 5600 + c_t.InteropHelloTimeoutMs);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, deadline.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, deadline_guest.phase());
    try std_t.testing.expect((try deadline_guest.peek_tx(GuestToken)) == null);

    var failing_allocator = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = 2 });
    const failing_guest = try init_peer(failing_allocator.allocator(), .Guest, GuestToken, 5700);
    const normal_host = try init_peer(std_t.testing.allocator, .Host, HostToken, 5700);
    var failing_pair = pair_t{ .host = normal_host, .guest = failing_guest };
    defer deinit_pair(&failing_pair);
    try complete_handshake(&failing_pair, 5701);
    const no_goodbye = try failing_pair.guest.begin_drain(GuestToken, 5702);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, no_goodbye.kind);
    try std_t.testing.expect(failing_pair.guest.terminal_send_unavailable());
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, failing_pair.guest.phase());
}

test "zero ACK is a no-op before deadline and while already draining" {
    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 5800);
    defer guest.deinit();
    _ = try guest.guest_mapping_initialized(GuestToken, 5801, Epoch, true);
    const before_bytes = guest.queue.pending_bytes;
    const before_time = guest.last_now_ms;
    const zero = try guest.ack_tx(GuestToken, 5802, 0);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, zero.kind);
    try std_t.testing.expectEqual(before_bytes, guest.queue.pending_bytes);
    try std_t.testing.expectEqual(before_time + 1, guest.last_now_ms);
    _ = try guest.begin_drain(GuestToken, 5803);
    const draining_zero = try guest.ack_tx(GuestToken, 5804, 0);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, draining_zero.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, guest.phase());
}

test "reader input alias and pending frames are safe across draining and closure" {
    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 5900);
    defer guest.deinit();
    const before_phase = guest.phase();
    const before_received = guest.reader.received;
    try std_t.testing.expectError(error.InputAliasesReader, guest.feed(GuestToken, guest.reader.buffer[0..1]));
    try std_t.testing.expectEqual(before_phase, guest.phase());
    try std_t.testing.expectEqual(before_received, guest.reader.received);

    var pair = try init_pair(std_t.testing.allocator, 6000);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 6001);
    var feature: [MaxTestFrame]u8 = undefined;
    const feature_length = try make_empty(&feature, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest);
    try feed_frame_fragmented(&pair.host, HostToken, feature[0..feature_length], &.{ 2, 1 });
    _ = try pair.host.begin_drain(HostToken, 6002);
    var output: [MaxTestFrame]u8 = undefined;
    const draining = try pair.host.next_event(HostToken, 6003, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, draining.event.kind);
    try std_t.testing.expectEqual(feature_length, pair.host.pending_bytes());

    try feed_frame_fragmented(&pair.guest, GuestToken, feature[0..feature_length], &.{ 1, 2 });
    _ = try pair.guest.transport_lost(GuestToken);
    try std_t.testing.expectError(error.WrongPhase, pair.guest.next_event(GuestToken, 6002, &output));
    try std_t.testing.expectEqual(feature_length, pair.guest.pending_bytes());
}

test "pre-Ready wire errors are never answered recursively and invalid frames drain" {
    var host_error = try init_peer(std_t.testing.allocator, .Host, HostToken, 6100);
    defer host_error.deinit();
    var error_frame: [MaxTestFrame]u8 = undefined;
    const error_length = try make_error(&error_frame, 73, c_t.InteropErrorProtocolViolation, c_t.InteropFromGuest);
    try feed_frame_fragmented(&host_error, HostToken, error_frame[0..error_length], &.{ 3, 2 });
    var output: [MaxTestFrame]u8 = undefined;
    const remote_error = try host_error.next_event(HostToken, 6101, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, remote_error.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, host_error.phase());
    try std_t.testing.expect((try host_error.peek_tx(HostToken)) == null);

    var malformed_error_host = try init_peer(std_t.testing.allocator, .Host, HostToken, 6200);
    defer malformed_error_host.deinit();
    var malformed_error: [MaxTestFrame]u8 = undefined;
    const malformed_length = try make_empty(&malformed_error, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest);
    malformed_error[6] = @intCast(c_t.InteropMsgError & 0xff);
    malformed_error[7] = @intCast(c_t.InteropMsgError >> 8);
    try feed_frame_fragmented(&malformed_error_host, HostToken, malformed_error[0..malformed_length], &.{ 1, 4 });
    const malformed_result = try malformed_error_host.next_event(HostToken, 6201, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, malformed_result.event.kind);
    try std_t.testing.expect((try malformed_error_host.peek_tx(HostToken)) == null);

    var malformed_non_error_host = try init_peer(std_t.testing.allocator, .Host, HostToken, 6300);
    defer malformed_non_error_host.deinit();
    var wrong_direction: [MaxTestFrame]u8 = undefined;
    const wrong_direction_length = try make_empty(&wrong_direction, c_t.InteropMsgPing, 5, c_t.InteropFromHost);
    try feed_frame_fragmented(&malformed_non_error_host, HostToken, wrong_direction[0..wrong_direction_length], &.{ 2, 3 });
    const malformed_non_error = try malformed_non_error_host.next_event(HostToken, 6301, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, malformed_non_error.event.kind);
    const terminal_error = (try malformed_non_error_host.peek_tx(HostToken)).?;
    var terminal_header: c_t.interop_header_t = undefined;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(terminal_error.ptr, terminal_error.len, &terminal_header));
    try std_t.testing.expectEqual(c_t.InteropMsgError, terminal_header.type);

    var wrong_type_host = try init_peer(std_t.testing.allocator, .Host, HostToken, 6400);
    defer wrong_type_host.deinit();
    var feature: [MaxTestFrame]u8 = undefined;
    const feature_length = try make_empty(&feature, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest);
    try feed_frame_fragmented(&wrong_type_host, HostToken, feature[0..feature_length], &.{ 1, 2, 4 });
    const wrong_type = try wrong_type_host.next_event(HostToken, 6401, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, wrong_type.event.kind);
    try std_t.testing.expect((try wrong_type_host.peek_tx(HostToken)) != null);
}

test "premature Ack and replayed Hello or Ack cannot reopen or bypass handshake" {
    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 6500);
    defer guest.deinit();
    _ = try guest.guest_mapping_initialized(GuestToken, 6501, Epoch, true);
    const hello = (try guest.peek_tx(GuestToken)).?;
    var hello_header: c_t.interop_header_t = undefined;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(hello.ptr, hello.len, &hello_header));
    var premature_ack: [MaxTestFrame]u8 = undefined;
    const premature_length = try make_hello_ack(&premature_ack, hello_header.request_id, Version, Capabilities);
    try feed_frame_fragmented(&guest, GuestToken, premature_ack[0..premature_length], &.{ 2, 5 });
    var output: [MaxTestFrame]u8 = undefined;
    const premature = try guest.next_event(GuestToken, 6502, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, premature.event.kind);
    try std_t.testing.expect(guest.must_abort_transport());
    try std_t.testing.expect((try guest.peek_tx(GuestToken)) == null);

    const host_ready = try init_peer(std_t.testing.allocator, .Host, HostToken, 6600);
    const guest_ready = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 6600);
    var pair = pair_t{ .host = host_ready, .guest = guest_ready };
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 6601);

    var replay_hello: [MaxTestFrame]u8 = undefined;
    const replay_hello_length = try make_hello(&replay_hello, 99, Epoch + 1, Version, Capabilities);
    try feed_frame_fragmented(&pair.host, HostToken, replay_hello[0..replay_hello_length], &.{ 3, 2 });
    const replayed = try pair.host.next_event(HostToken, 6602, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, replayed.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.host.phase());

    var ack_replay = try init_pair(std_t.testing.allocator, 6700);
    defer deinit_pair(&ack_replay);
    try complete_handshake(&ack_replay, 6701);
    var replay_ack: [MaxTestFrame]u8 = undefined;
    const ack_replay_length = try make_hello_ack(&replay_ack, 1, Version, Capabilities);
    try feed_frame_fragmented(&ack_replay.guest, GuestToken, replay_ack[0..ack_replay_length], &.{ 1, 4, 2 });
    const replayed_ack = try ack_replay.guest.next_event(GuestToken, 6702, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, replayed_ack.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, ack_replay.guest.phase());
}

test "zero Ping and Pong identifiers are rejected on their receiving roles" {
    var pair = try init_pair(std_t.testing.allocator, 6800);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 6801);
    var ping: [MaxTestFrame]u8 = undefined;
    const ping_length = try make_empty(&ping, c_t.InteropMsgPing, 1, c_t.InteropFromHost);
    set_request_id(ping[0..ping_length], 0);
    try feed_frame_fragmented(&pair.guest, GuestToken, ping[0..ping_length], &.{ 2, 3 });
    var output: [MaxTestFrame]u8 = undefined;
    const rejected_ping = try pair.guest.next_event(GuestToken, 6802, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, rejected_ping.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.guest.phase());

    var pong_pair = try init_pair(std_t.testing.allocator, 6900);
    defer deinit_pair(&pong_pair);
    try complete_handshake(&pong_pair, 6901);
    var pong: [MaxTestFrame]u8 = undefined;
    const pong_length = try make_empty(&pong, c_t.InteropMsgPong, 1, c_t.InteropFromGuest);
    set_request_id(pong[0..pong_length], 0);
    try feed_frame_fragmented(&pong_pair.host, HostToken, pong[0..pong_length], &.{ 4, 2 });
    const rejected_pong = try pong_pair.host.next_event(HostToken, 6902, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, rejected_pong.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pong_pair.host.phase());
}

test "backwards timestamps are local unchanged errors for every timed callback" {
    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 7000);
    defer guest.deinit();
    try std_t.testing.expectError(error.TimeWentBackwards, guest.guest_mapping_initialized(GuestToken, 6999, Epoch, true));
    try std_t.testing.expectEqual(peer_t.phase_t.InitializingMapping, guest.phase());
    _ = try guest.guest_mapping_initialized(GuestToken, 7001, Epoch, true);
    const queued_phase = guest.phase();
    const queued_bytes = guest.queue.pending_bytes;
    const queued_records = guest.send_count;
    try std_t.testing.expectError(error.TimeWentBackwards, guest.ack_tx(GuestToken, 7000, 0));
    try std_t.testing.expectError(error.TimeWentBackwards, guest.tick(GuestToken, 7000));
    try std_t.testing.expectError(error.TimeWentBackwards, guest.begin_drain(GuestToken, 7000));
    var output: [MaxTestFrame]u8 = undefined;
    try std_t.testing.expectError(error.TimeWentBackwards, guest.next_event(GuestToken, 7000, &output));
    try std_t.testing.expectError(error.TimeWentBackwards, guest.host_mapping_validated(GuestToken, 7000, Epoch, c_t.InteropLayoutVersion, true));
    try std_t.testing.expectEqual(queued_phase, guest.phase());
    try std_t.testing.expectEqual(queued_bytes, guest.queue.pending_bytes);
    try std_t.testing.expectEqual(queued_records, guest.send_count);
    try std_t.testing.expectEqual(@as(u64, 7001), guest.last_now_ms);

    var host_pair = try init_pair(std_t.testing.allocator, 7100);
    defer deinit_pair(&host_pair);
    _ = try host_pair.guest.guest_mapping_initialized(GuestToken, 7101, Epoch, true);
    _ = try transfer_front_fragmented(&host_pair.guest, GuestToken, &host_pair.host, HostToken, 7102, &.{ 4, 1 });
    _ = try host_pair.host.next_event(HostToken, 7103, &output);
    const host_phase = host_pair.host.phase();
    const host_time = host_pair.host.last_now_ms;
    try std_t.testing.expectError(error.TimeWentBackwards, host_pair.host.host_mapping_validated(HostToken, 7102, Epoch, c_t.InteropLayoutVersion, true));
    try std_t.testing.expectEqual(host_phase, host_pair.host.phase());
    try std_t.testing.expectEqual(host_time, host_pair.host.last_now_ms);
}

test "already-draining callbacks remain terminal and deinit rejects every operation" {
    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 7200);
    defer guest.deinit();
    _ = try guest.guest_mapping_initialized(GuestToken, 7200 + c_t.InteropHelloTimeoutMs, Epoch, true);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, guest.phase());
    const drain_again = try guest.begin_drain(GuestToken, 7200 + c_t.InteropHelloTimeoutMs);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, drain_again.kind);
    const tick_again = try guest.tick(GuestToken, 7200 + c_t.InteropHelloTimeoutMs);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, tick_again.kind);
    const empty_drain = try guest.next_event(GuestToken, 7200 + c_t.InteropHelloTimeoutMs, &.{});
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, empty_drain.event.kind);
    _ = try guest.transport_lost(GuestToken);
    try std_t.testing.expectError(error.WrongPhase, guest.next_event(GuestToken, 7200 + c_t.InteropHelloTimeoutMs, &.{}));
    guest.deinit();
    try std_t.testing.expectEqual(@as(usize, 0), guest.pending_bytes());
    try std_t.testing.expectError(error.PeerClosed, guest.feed(GuestToken, &.{}));
    try std_t.testing.expectError(error.PeerClosed, guest.next_event(GuestToken, 7200 + c_t.InteropHelloTimeoutMs, &.{}));
    try std_t.testing.expectError(error.PeerClosed, guest.peek_tx(GuestToken));
    try std_t.testing.expectError(error.PeerClosed, guest.ack_tx(GuestToken, 7200 + c_t.InteropHelloTimeoutMs, 0));
    try std_t.testing.expectError(error.PeerClosed, guest.tick(GuestToken, 7200 + c_t.InteropHelloTimeoutMs));
    try std_t.testing.expectError(error.PeerClosed, guest.begin_drain(GuestToken, 7200 + c_t.InteropHelloTimeoutMs));
    try std_t.testing.expectError(error.PeerClosed, guest.transport_lost(GuestToken));
    try std_t.testing.expectError(error.PeerClosed, guest.host_mapping_validated(GuestToken, 7200 + c_t.InteropHelloTimeoutMs, Epoch, c_t.InteropLayoutVersion, true));
    try std_t.testing.expectError(error.PeerClosed, guest.guest_mapping_initialized(GuestToken, 7200 + c_t.InteropHelloTimeoutMs, Epoch, true));
    guest.deinit();
}

test "replayed control ID cases and malformed Goodbye status drain" {
    var pair = try init_pair(std_t.testing.allocator, 7300);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 7301);
    var bad_goodbye: [MaxTestFrame]u8 = undefined;
    const length = try make_goodbye(&bad_goodbye, c_t.InteropFromGuest);
    std_t.mem.writeInt(u32, bad_goodbye[20..24], 0xffff, .little);
    try feed_frame_fragmented(&pair.host, HostToken, bad_goodbye[0..length], &.{ 3, 2 });
    var output: [MaxTestFrame]u8 = undefined;
    const invalid_status = try pair.host.next_event(HostToken, 7302, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, invalid_status.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.host.phase());
    try std_t.testing.expect((try pair.host.peek_tx(HostToken)) != null);
}

test "ready-session ACK of Ping bytes accounts sends before exact liveness expiry" {
    var pair = try init_pair(std_t.testing.allocator, 7400);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 7401);
    const ready_at = pair.host.last_now_ms;
    _ = try pair.host.tick(HostToken, ready_at + c_t.InteropPingIntervalMs);
    const ping = (try pair.host.peek_tx(HostToken)).?;
    const partial = ping[0..1];
    _ = try pair.host.ack_tx(HostToken, ready_at + c_t.InteropPingIntervalMs + 1, partial.len);
    const remainder = (try pair.host.peek_tx(HostToken)).?;
    const remaining_count = remainder.len;
    const final_event = try pair.host.ack_tx(HostToken, ready_at + c_t.InteropPingTimeoutMs, remaining_count);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, final_event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, pair.host.phase());
    try std_t.testing.expect((try pair.host.peek_tx(HostToken)) == null);
    try std_t.testing.expectEqual(@as(usize, 0), pair.host.queue.pending_bytes);
}

test "empty transport and terminal queue APIs preserve their public errors" {
    var peer = try init_peer(std_t.testing.allocator, .Host, HostToken, 7500);
    defer peer.deinit();
    try std_t.testing.expectEqual(@as(usize, 0), peer.pending_bytes());
    var output: [MaxTestFrame]u8 = undefined;
    const idle = try peer.next_event(HostToken, 7500, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, idle.event.kind);
    try std_t.testing.expectEqual(@as(usize, 0), idle.required_bytes);
    try std_t.testing.expect((try peer.peek_tx(HostToken)) == null);
    try std_t.testing.expectError(error.NoQueuedFrame, peer.ack_tx(HostToken, 7500, 0));

    _ = try peer.transport_lost(HostToken);
    try std_t.testing.expectError(error.WrongPhase, peer.peek_tx(HostToken));
    try std_t.testing.expectError(error.WrongPhase, peer.ack_tx(HostToken, 7500, 0));

    const malformed = peer_t.config_t{
        .role = .Guest,
        .connection_token = GuestToken,
        .accepted_at_ms = 0,
        .local_version = "1.invalid.0",
        .local_capabilities = Capabilities,
    };
    try std_t.testing.expectError(error.InvalidVersion, peer_t.peer_t.init(std_t.testing.allocator, malformed));
}

test "bad header latches wire drain and records a best-effort protocol error" {
    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 7600);
    defer guest.deinit();
    var frame: [MaxTestFrame]u8 = undefined;
    const length = try make_empty(&frame, c_t.InteropMsgPing, 1, c_t.InteropFromHost);
    frame[4] = 0xff; // Unsupported wire version; the header is still a complete bounded frame.
    const fed = try @call(.never_inline, peer_t.peer_t.feed, .{ &guest, GuestToken, frame[0..length] });
    try std_t.testing.expect(fed.terminal_error != null);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, fed.phase);
    try std_t.testing.expect(!fed.terminal_send_failed);
    try std_t.testing.expectEqual(@as(usize, 0), guest.pending_bytes());
    const queued = (try guest.peek_tx(GuestToken)).?;
    var header: c_t.interop_header_t = undefined;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(queued.ptr, queued.len, &header));
    try std_t.testing.expectEqual(c_t.InteropMsgError, header.type);
}

test "mapping callbacks reject failed epoch and layout results without readiness" {
    var failed_guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 7700);
    defer failed_guest.deinit();
    const mapping_succeeded = false;
    try std_t.testing.expectError(error.MappingInitializationFailed, @call(.never_inline, peer_t.peer_t.guest_mapping_initialized, .{ &failed_guest, GuestToken, 7701, Epoch, mapping_succeeded }));
    try std_t.testing.expectEqual(peer_t.phase_t.InitializingMapping, failed_guest.phase());
    try std_t.testing.expectError(error.InvalidSessionEpoch, failed_guest.guest_mapping_initialized(GuestToken, 7701, 0, true));
    try std_t.testing.expectEqual(peer_t.phase_t.InitializingMapping, failed_guest.phase());

    var host = try init_peer(std_t.testing.allocator, .Host, HostToken, 7800);
    defer host.deinit();
    var hello: [MaxTestFrame]u8 = undefined;
    const hello_length = try make_hello(&hello, 9, Epoch, Version, Capabilities);
    _ = try host.feed(HostToken, hello[0..hello_length]);
    var output: [MaxTestFrame]u8 = undefined;
    const request = try host.next_event(HostToken, 7801, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.ValidateMapping, request.event.kind);
    const failed = try @call(.never_inline, peer_t.peer_t.host_mapping_validated, .{ &host, HostToken, 7802, Epoch + 1, c_t.InteropLayoutVersion, true });
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, failed.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, host.phase());
    try std_t.testing.expect((try host.peek_tx(HostToken)) != null);
}

test "bad handshake envelopes and failed host validation latch draining" {
    var malformed_host = try init_peer(std_t.testing.allocator, .Host, HostToken, 8100);
    defer malformed_host.deinit();
    var malformed_hello: [MaxTestFrame]u8 = undefined;
    const malformed_length = try make_hello(&malformed_hello, 1, Epoch, Version, Capabilities);
    var version_pointer: [*c]const u8 = null;
    var version_length: u16 = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_field_get(malformed_hello[c_t.InteropHeaderBytes..malformed_length].ptr, malformed_length - c_t.InteropHeaderBytes, c_t.InteropTagAgentVersion, 0, &version_pointer, &version_length));
    const version_tag_offset = @intFromPtr(version_pointer) - @intFromPtr(&malformed_hello) - 4;
    std_t.mem.writeInt(u16, @ptrCast(malformed_hello[version_tag_offset..].ptr), 0x7ffe, .little);
    _ = try malformed_host.feed(HostToken, malformed_hello[0..malformed_length]);
    var output: [MaxTestFrame]u8 = undefined;
    const rejected = try malformed_host.next_event(HostToken, 8101, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, rejected.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, malformed_host.phase());

    var refused_host = try init_peer(std_t.testing.allocator, .Host, HostToken, 8200);
    defer refused_host.deinit();
    var hello: [MaxTestFrame]u8 = undefined;
    const hello_length = try make_hello(&hello, 2, Epoch, Version, Capabilities);
    _ = try refused_host.feed(HostToken, hello[0..hello_length]);
    try std_t.testing.expectEqual(peer_t.event_kind_t.ValidateMapping, (try refused_host.next_event(HostToken, 8201, &output)).event.kind);
    const refused = try refused_host.host_mapping_validated(HostToken, 8202, Epoch, c_t.InteropLayoutVersion, false);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, refused.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, refused_host.phase());

    var wrong_direction = try init_peer(std_t.testing.allocator, .Host, HostToken, 8300);
    defer wrong_direction.deinit();
    var wrong_header: [MaxTestFrame]u8 = undefined;
    const wrong_direction_length = try make_hello(&wrong_header, 3, Epoch, Version, Capabilities);
    wrong_header[0] ^= 0x80;
    const bad_header = try @call(.never_inline, peer_t.peer_t.feed, .{ &wrong_direction, HostToken, wrong_header[0..wrong_direction_length] });
    try std_t.testing.expect(bad_header.terminal_error != null);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, wrong_direction.phase());
    try std_t.testing.expect((try wrong_direction.peek_tx(HostToken)) != null);
}

test "malformed required Hello and Ack fields are rejected at each handshake boundary" {
    var host = try init_peer(std_t.testing.allocator, .Host, HostToken, 8400);
    defer host.deinit();
    var hello: [MaxTestFrame]u8 = undefined;
    const hello_length = try make_hello(&hello, 21, Epoch, Version, Capabilities);
    try replace_tlv_tag(&hello, hello_length, c_t.InteropTagCapabilities, 0x7ffe);
    _ = try host.feed(HostToken, hello[0..hello_length]);
    var output: [MaxTestFrame]u8 = undefined;
    const rejected_hello = try host.next_event(HostToken, 8401, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, rejected_hello.event.kind);
    try std_t.testing.expect((try host.peek_tx(HostToken)) != null);

    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 8500);
    defer guest.deinit();
    _ = try guest.guest_mapping_initialized(GuestToken, 8501, Epoch, true);
    const queued_hello = (try guest.peek_tx(GuestToken)).?;
    _ = try guest.ack_tx(GuestToken, 8502, queued_hello.len);
    var ack: [MaxTestFrame]u8 = undefined;
    const ack_length = try make_hello_ack(&ack, guest.pending_hello_id, Version, Capabilities);
    try replace_tlv_tag(&ack, ack_length, c_t.InteropTagCapabilities, 0x7ffe);
    _ = try guest.feed(GuestToken, ack[0..ack_length]);
    const rejected_ack = try guest.next_event(GuestToken, 8503, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, rejected_ack.event.kind);
    try std_t.testing.expect((try guest.peek_tx(GuestToken)) != null);
}

test "partial bodies stay hidden and queued-frame output aliases are rejected" {
    var partial_guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 8600);
    defer partial_guest.deinit();
    var hello: [MaxTestFrame]u8 = undefined;
    const hello_length = try make_hello(&hello, 23, Epoch, Version, Capabilities);
    _ = try @call(.never_inline, peer_t.peer_t.feed, .{ &partial_guest, GuestToken, hello[0 .. c_t.InteropHeaderBytes + 1] });
    try std_t.testing.expectEqual(@as(usize, 0), @call(.never_inline, peer_t.peer_t.pending_bytes, .{&partial_guest}));
    _ = try @call(.never_inline, peer_t.peer_t.feed, .{ &partial_guest, GuestToken, hello[c_t.InteropHeaderBytes + 1 .. hello_length] });
    try std_t.testing.expectEqual(hello_length, @call(.never_inline, peer_t.peer_t.pending_bytes, .{&partial_guest}));

    var pair = try init_pair(std_t.testing.allocator, 8700);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 8701);
    _ = try pair.host.tick(HostToken, 8701 + 2000);
    var feature: [MaxTestFrame]u8 = undefined;
    const feature_length = try make_empty(&feature, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest);
    _ = try pair.host.feed(HostToken, feature[0..feature_length]);
    const queued = pair.host.queue.frames[pair.host.queue.head].?.bytes;
    try std_t.testing.expectError(error.OutputAliasesPeer, pair.host.next_event(HostToken, 8701 + 2000, queued));
}

test "best effort Ready protocol errors report allocator exhaustion without leaking" {
    try expect_ready_error_queue_oom(.Guest, c_t.InteropMsgPing, false);
    try expect_ready_error_queue_oom(.Host, c_t.InteropMsgPong, false);
    try expect_ready_error_queue_oom(.Host, c_t.InteropMsgHello, false);
    try expect_ready_error_queue_oom(.Guest, c_t.InteropMsgHelloAck, false);
    try expect_ready_error_queue_oom(.Host, c_t.InteropMsgGoodbye, false);
    try expect_ready_error_queue_oom(.Host, c_t.InteropMsgHello, true);
}

test "best effort Hello and Ack incompatibility replies report allocator exhaustion" {
    try expect_handshake_error_queue_oom(.Host, false);
    try expect_handshake_error_queue_oom(.Guest, false);
    try expect_handshake_error_queue_oom(.Host, true);
    try expect_handshake_error_queue_oom(.Guest, true);
}

test "host mapping refusal preserves drain when its error reply cannot allocate" {
    var failing = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = std_t.math.maxInt(usize) });
    var host = try peer_t.peer_t.init(failing.allocator(), .{
        .role = .Host,
        .connection_token = HostToken,
        .accepted_at_ms = 300,
        .local_version = Version,
        .local_capabilities = Capabilities,
    });
    var host_live = true;
    defer if (host_live) host.deinit();
    var hello: [MaxTestFrame]u8 = undefined;
    const hello_length = try make_hello(&hello, 19, Epoch, Version, Capabilities);
    _ = try host.feed(HostToken, hello[0..hello_length]);
    var output: [MaxTestFrame]u8 = undefined;
    try std_t.testing.expectEqual(peer_t.event_kind_t.ValidateMapping, (try host.next_event(HostToken, 301, &output)).event.kind);
    failing.fail_index = failing.alloc_index;
    const refused = try host.host_mapping_validated(HostToken, 302, Epoch, c_t.InteropLayoutVersion, false);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, refused.kind);
    try std_t.testing.expect(host.terminal_send_unavailable());
    _ = try host.transport_lost(HostToken);
    host.deinit();
    host_live = false;
    try std_t.testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
}

test "valid HelloAck envelope with invalid SemVer drains as incompatible" {
    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 8000);
    defer guest.deinit();
    _ = try guest.guest_mapping_initialized(GuestToken, 8001, Epoch, true);
    const hello = (try guest.peek_tx(GuestToken)).?;
    _ = try guest.ack_tx(GuestToken, 8002, hello.len);
    var ack: [MaxTestFrame]u8 = undefined;
    const ack_length = try make_hello_ack(&ack, guest.pending_hello_id, "11.2.3", Capabilities);
    var version_ptr: [*c]const u8 = null;
    var version_length: u16 = 0;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_field_get(ack[c_t.InteropHeaderBytes..].ptr, ack_length - c_t.InteropHeaderBytes, c_t.InteropTagAgentVersion, 0, &version_ptr, &version_length));
    try std_t.testing.expectEqual(@as(u16, 6), version_length);
    @constCast(version_ptr)[0] = '0'; // Mutate a valid envelope into the SemVer 2 leading-zero case.
    _ = try guest.feed(GuestToken, ack[0..ack_length]);
    var output: [MaxTestFrame]u8 = undefined;
    const rejected = try guest.next_event(GuestToken, 8003, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, rejected.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, guest.phase());
}

// ---------------------------------------------------------------------------
// Outbound feature admission (enqueue_feature, IMPL_DESC 8.20 and 3.2.7).
// ---------------------------------------------------------------------------

/// Observable state that every refused feature admission must leave byte-identical.
const snapshot_t = struct {
    phase: peer_t.phase_t,
    queued: usize,
    pending_bytes: usize,
    send_count: usize,
    now_ms: u64,
    request_ids: usize,
    abort: bool,
};

/// Data-class admission limit: peer_t.QueueCapacity minus the 8 ReservedControlSlots of stream_send_queue.zig.
const DataCapacity: usize = 504;

fn snapshot(peer: *const peer_t.peer_t) snapshot_t {
    return .{
        .phase = peer.phase(),
        .queued = peer.queue.count,
        .pending_bytes = peer.queue.pending_bytes,
        .send_count = peer.send_count,
        .now_ms = peer.last_now_ms,
        .request_ids = peer.request_ids.count,
        .abort = peer.must_abort_transport(),
    };
}

/// Assert that one admission attempt fails with `expected` and changes no observable state.
fn expect_refused(peer: *peer_t.peer_t, token: u64, now_ms: u64, frame: []const u8, expected: anyerror) !void {
    const before = snapshot(peer);
    try std_t.testing.expectError(expected, peer.enqueue_feature(token, now_ms, frame));
    try std_t.testing.expectEqualDeep(before, snapshot(peer));
}

fn init_pair_caps(allocator: std_t.mem.Allocator, accepted_at_ms: u64, caps: u64) !pair_t {
    return .{
        .host = try peer_t.peer_t.init(allocator, .{ .role = .Host, .connection_token = HostToken, .accepted_at_ms = accepted_at_ms, .local_version = Version, .local_capabilities = caps }),
        .guest = try peer_t.peer_t.init(allocator, .{ .role = .Guest, .connection_token = GuestToken, .accepted_at_ms = accepted_at_ms, .local_version = Version, .local_capabilities = caps }),
    };
}

/// Host-to-guest WindowClose (0x0104, Windows): required window and generation.
fn make_window_retire(buffer: []u8, window: u64, generation: u64) !usize {
    var writer = writer_for(buffer);
    try write_uint(&writer, c_t.InteropTagWindow, window);
    try write_uint(&writer, c_t.InteropTagGeneration, generation);
    var length: usize = 0;
    try finish(&writer, c_t.InteropMsgWindowRetire, 0, c_t.InteropFromGuest, &length);
    return length;
}

fn make_window_close(buffer: []u8, request_id: u32, window: u64, generation: u64) !usize {
    var writer = writer_for(buffer);
    try write_uint(&writer, c_t.InteropTagWindow, window);
    try write_uint(&writer, c_t.InteropTagGeneration, generation);
    var length: usize = 0;
    try finish(&writer, c_t.InteropMsgWindowClose, request_id, c_t.InteropFromHost, &length);
    return length;
}

/// Guest-to-host WindowRetire (0x0102, Windows): required window and generation.
/// Host-to-guest InputRelease (0x020E, Input): window, generation, serial, lease epoch.
fn make_input_release(buffer: []u8, window: u64, generation: u64) !usize {
    var writer = writer_for(buffer);
    try write_uint(&writer, c_t.InteropTagWindow, window);
    try write_uint(&writer, c_t.InteropTagGeneration, generation);
    try write_uint(&writer, c_t.InteropTagSerial, 5);
    try write_uint(&writer, c_t.InteropTagLeaseEpoch, 6);
    var length: usize = 0;
    try finish(&writer, c_t.InteropMsgInputRelease, 0, c_t.InteropFromHost, &length);
    return length;
}

/// Host-to-guest DndLeave (0x0502, DragDrop): window and generation.
fn make_dnd_leave(buffer: []u8, window: u64, generation: u64) !usize {
    var writer = writer_for(buffer);
    try write_uint(&writer, c_t.InteropTagWindow, window);
    try write_uint(&writer, c_t.InteropTagGeneration, generation);
    var length: usize = 0;
    try finish(&writer, c_t.InteropMsgDndLeave, 0, c_t.InteropFromHost, &length);
    return length;
}

/// Bidirectional ClipboardClear (0x0402, Clipboard): clipboard epoch.
fn make_clipboard_clear(buffer: []u8, direction: c_t.interop_direction_t, epoch: u64) !usize {
    var writer = writer_for(buffer);
    try write_uint(&writer, c_t.InteropTagClipboardEpoch, epoch);
    var length: usize = 0;
    try finish(&writer, c_t.InteropMsgClipboardClear, 0, direction, &length);
    return length;
}

/// Bidirectional TransferCancel (0x0F03): transfer id and status, facility-gated by origin.
fn make_transfer_cancel(buffer: []u8, direction: c_t.interop_direction_t, transfer: u64) !usize {
    var writer = writer_for(buffer);
    try write_uint(&writer, c_t.InteropTagTransfer, transfer);
    try write_uint(&writer, c_t.InteropTagStatus, c_t.InteropCancelled);
    var length: usize = 0;
    try finish(&writer, c_t.InteropMsgTransferCancel, 0, direction, &length);
    return length;
}

/// Guest-to-host empty CatalogBegin (0x0600, Catalog).
fn make_catalog_begin(buffer: []u8) !usize {
    return make_empty(buffer, c_t.InteropMsgCatalogBegin, 0, c_t.InteropFromGuest);
}

/// Write a protocol-v2 header with an arbitrary type and payload length, bypassing the codec.
fn write_raw_header(frame: []u8, frame_type: u16, request_id: u32, payload_length: u32) void {
    std_t.mem.writeInt(u32, frame[0..4], c_t.InteropMagic, .little);
    std_t.mem.writeInt(u16, frame[4..6], c_t.InteropVersion, .little);
    std_t.mem.writeInt(u16, frame[6..8], frame_type, .little);
    std_t.mem.writeInt(u32, frame[8..12], payload_length, .little);
    std_t.mem.writeInt(u32, frame[12..16], request_id, .little);
}

/// Set the critical bit that every required frozen field carries on the wire.
fn critical_tag(tag: u16) u16 {
    return tag | @as(u16, @intCast(c_t.InteropCriticalTag));
}

/// Append one raw TLV (tag, u16 length, value, zero padding) and return the new payload offset.
fn append_tlv(frame: []u8, offset: usize, tag: u16, value: []const u8) usize {
    std_t.mem.writeInt(u16, frame[offset..][0..2], tag, .little);
    std_t.mem.writeInt(u16, frame[offset + 2 ..][0..2], @intCast(value.len), .little);
    @memcpy(frame[offset + 4 ..][0..value.len], value);
    const padded = (value.len + 3) & ~@as(usize, 3);
    @memset(frame[offset + 4 + value.len .. offset + 4 + padded], 0);
    return offset + 4 + padded;
}

test "feature frames are admitted by producing role with exact bytes and no request ID reservation" {
    var pair = try init_pair(std_t.testing.allocator, 400);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 401);

    var host_frame: [MaxTestFrame]u8 = undefined;
    const host_length = try make_window_close(&host_frame, 77, 0x55, 2);
    const host_event = try pair.host.enqueue_feature(HostToken, 402, host_frame[0..host_length]);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, host_event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Ready, host_event.phase);
    try std_t.testing.expectEqual(@as(u64, 402), pair.host.last_now_ms);
    try std_t.testing.expectEqual(@as(usize, 0), pair.host.request_ids.count);
    try std_t.testing.expectEqualSlices(u8, host_frame[0..host_length], (try pair.host.peek_tx(HostToken)).?);

    var guest_frame: [MaxTestFrame]u8 = undefined;
    const guest_length = try make_window_retire(&guest_frame, 0x55, 2);
    _ = try pair.guest.enqueue_feature(GuestToken, 402, guest_frame[0..guest_length]);
    try std_t.testing.expectEqualSlices(u8, guest_frame[0..guest_length], (try pair.guest.peek_tx(GuestToken)).?);

    _ = try transfer_front_fragmented(&pair.host, HostToken, &pair.guest, GuestToken, 402, &.{ 3, 5 });
    var output: [MaxTestFrame]u8 = undefined;
    const received = try pair.guest.next_event(GuestToken, 402, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.FeatureFrame, received.event.kind);
    try std_t.testing.expectEqual(c_t.InteropMsgWindowClose, received.event.message_type);
    try std_t.testing.expectEqual(@as(u32, 77), received.event.request_id);
    try std_t.testing.expectEqualSlices(u8, host_frame[0..host_length], output[0..received.event.copied_bytes]);

    _ = try transfer_front_fragmented(&pair.guest, GuestToken, &pair.host, HostToken, 402, &.{7});
    const back = try pair.host.next_event(HostToken, 402, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.FeatureFrame, back.event.kind);
    try std_t.testing.expectEqual(c_t.InteropMsgWindowRetire, back.event.message_type);
    try std_t.testing.expectEqualSlices(u8, guest_frame[0..guest_length], output[0..back.event.copied_bytes]);
}

test "control local unknown and wrong-direction feature types are refused without changing state" {
    var pair = try init_pair(std_t.testing.allocator, 400);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 401);

    var frame: [MaxTestFrame]u8 = undefined;
    const guest_only = try make_window_retire(&frame, 1, 1);
    try expect_refused(&pair.host, HostToken, 402, frame[0..guest_only], error.UnsupportedFrame);
    const host_only = try make_window_close(&frame, 0, 1, 1);
    try expect_refused(&pair.guest, GuestToken, 402, frame[0..host_only], error.UnsupportedFrame);
    const catalog_guest = try make_catalog_begin(&frame);
    try expect_refused(&pair.host, HostToken, 402, frame[0..catalog_guest], error.UnsupportedFrame);

    const hello = try make_hello(&frame, 9, Epoch, Version, Capabilities);
    try expect_refused(&pair.guest, GuestToken, 402, frame[0..hello], error.UnsupportedFrame);
    const ping = try make_empty(&frame, c_t.InteropMsgPing, 1, c_t.InteropFromHost);
    try expect_refused(&pair.host, HostToken, 402, frame[0..ping], error.UnsupportedFrame);
    const pong = try make_empty(&frame, c_t.InteropMsgPong, 1, c_t.InteropFromGuest);
    try expect_refused(&pair.guest, GuestToken, 402, frame[0..pong], error.UnsupportedFrame);
    const error_length = try make_error(&frame, 0, c_t.InteropErrorUnexpected, c_t.InteropFromHost);
    try expect_refused(&pair.host, HostToken, 402, frame[0..error_length], error.UnsupportedFrame);
    const goodbye = try make_goodbye(&frame, c_t.InteropFromGuest);
    try expect_refused(&pair.guest, GuestToken, 402, frame[0..goodbye], error.UnsupportedFrame);
    const local = try make_empty(&frame, c_t.InteropMsgLocalStop, 0, c_t.InteropFromLocal);
    try expect_refused(&pair.host, HostToken, 402, frame[0..local], error.UnsupportedFrame);

    write_raw_header(&frame, 0x0FFF, 0, 0);
    try expect_refused(&pair.host, HostToken, 402, frame[0..c_t.InteropHeaderBytes], error.UnsupportedFrame);
    write_raw_header(&frame, 0x0110, 0, 0);
    try expect_refused(&pair.guest, GuestToken, 402, frame[0..c_t.InteropHeaderBytes], error.UnsupportedFrame);
    try std_t.testing.expect((try pair.host.peek_tx(HostToken)) == null);
}

test "malformed feature headers payloads and oversize frames are InvalidFrame before any mutation" {
    var pair = try init_pair(std_t.testing.allocator, 400);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 401);

    var frame: [MaxTestFrame]u8 = undefined;
    const valid = try make_window_close(&frame, 0, 3, 4);
    try expect_refused(&pair.host, HostToken, 402, frame[0..0], error.InvalidFrame);
    try expect_refused(&pair.host, HostToken, 402, frame[0 .. c_t.InteropHeaderBytes - 1], error.InvalidFrame);
    try expect_refused(&pair.host, HostToken, 402, frame[0 .. valid + 1], error.InvalidFrame);
    try expect_refused(&pair.host, HostToken, 402, frame[0 .. valid - 4], error.InvalidFrame);

    var corrupted = frame;
    corrupted[0] ^= 0xff;
    try expect_refused(&pair.host, HostToken, 402, corrupted[0..valid], error.InvalidFrame);
    corrupted = frame;
    corrupted[4] = 1;
    try expect_refused(&pair.host, HostToken, 402, corrupted[0..valid], error.InvalidFrame);
    corrupted = frame;
    std_t.mem.writeInt(u32, corrupted[8..12], 6, .little);
    try expect_refused(&pair.host, HostToken, 402, corrupted[0 .. valid + 2], error.InvalidFrame);

    var oversize: [c_t.InteropMaxFrameBytes + 8]u8 = undefined;
    write_raw_header(&oversize, c_t.InteropMsgWindowClose, 0, c_t.InteropMaxPayloadBytes);
    @memset(oversize[c_t.InteropHeaderBytes..], 0);
    try expect_refused(&pair.host, HostToken, 402, oversize[0..], error.InvalidFrame);

    var payload: [MaxTestFrame]u8 = undefined;
    var offset = append_tlv(&payload, 0, critical_tag(c_t.InteropTagWindow), &[_]u8{ 3, 0, 0, 0, 0, 0, 0, 0 });
    var raw: [MaxTestFrame]u8 = undefined;
    write_raw_header(&raw, c_t.InteropMsgWindowClose, 0, @intCast(offset));
    @memcpy(raw[c_t.InteropHeaderBytes..][0..offset], payload[0..offset]);
    try expect_refused(&pair.host, HostToken, 402, raw[0 .. c_t.InteropHeaderBytes + offset], error.InvalidFrame);

    offset = append_tlv(&payload, 0, critical_tag(c_t.InteropTagWindow), &[_]u8{ 3, 0, 0, 0, 0, 0, 0, 0 });
    offset = append_tlv(&payload, offset, critical_tag(c_t.InteropTagWindow), &[_]u8{ 3, 0, 0, 0, 0, 0, 0, 0 });
    write_raw_header(&raw, c_t.InteropMsgWindowClose, 0, @intCast(offset));
    @memcpy(raw[c_t.InteropHeaderBytes..][0..offset], payload[0..offset]);
    try expect_refused(&pair.host, HostToken, 402, raw[0 .. c_t.InteropHeaderBytes + offset], error.InvalidFrame);

    offset = append_tlv(&payload, 0, critical_tag(c_t.InteropTagWindow), &[_]u8{ 3, 0, 0, 0, 0, 0, 0, 0 });
    offset = append_tlv(&payload, offset, critical_tag(c_t.InteropTagGeneration), &[_]u8{ 4, 0, 0, 0, 0, 0, 0, 0 });
    offset = append_tlv(&payload, offset, 0x8000 | 0x7F00, &[_]u8{ 0, 0, 0, 0 });
    write_raw_header(&raw, c_t.InteropMsgWindowClose, 0, @intCast(offset));
    @memcpy(raw[c_t.InteropHeaderBytes..][0..offset], payload[0..offset]);
    try expect_refused(&pair.host, HostToken, 402, raw[0 .. c_t.InteropHeaderBytes + offset], error.UnsupportedFrame);

    var accepted: [MaxTestFrame]u8 = undefined;
    offset = append_tlv(&payload, 0, critical_tag(c_t.InteropTagWindow), &[_]u8{ 3, 0, 0, 0, 0, 0, 0, 0 });
    offset = append_tlv(&payload, offset, critical_tag(c_t.InteropTagGeneration), &[_]u8{ 4, 0, 0, 0, 0, 0, 0, 0 });
    offset = append_tlv(&payload, offset, 0x7F00, &[_]u8{ 9, 9, 9, 9 });
    write_raw_header(&accepted, c_t.InteropMsgWindowClose, 0, @intCast(offset));
    @memcpy(accepted[c_t.InteropHeaderBytes..][0..offset], payload[0..offset]);
    _ = try pair.host.enqueue_feature(HostToken, 402, accepted[0 .. c_t.InteropHeaderBytes + offset]);
    try std_t.testing.expectEqual(@as(usize, 1), pair.host.queue.count);
}

test "every non-Ready phase refuses feature frames before validation or mutation" {
    var frame: [MaxTestFrame]u8 = undefined;
    const valid = try make_window_close(&frame, 0, 1, 1);

    var host = try init_peer(std_t.testing.allocator, .Host, HostToken, 400);
    defer host.deinit();
    try expect_refused(&host, HostToken, 401, frame[0..valid], error.WrongPhase);
    try expect_refused(&host, HostToken, 401, frame[0..0], error.WrongPhase);

    var guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 400);
    defer guest.deinit();
    try expect_refused(&guest, GuestToken, 401, frame[0..valid], error.WrongPhase);
    _ = try guest.guest_mapping_initialized(GuestToken, 401, Epoch, true);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloSent, guest.phase());
    try expect_refused(&guest, GuestToken, 402, frame[0..valid], error.WrongPhase);
    const hello = (try guest.peek_tx(GuestToken)).?;
    _ = try guest.ack_tx(GuestToken, 403, hello.len);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloAck, guest.phase());
    try expect_refused(&guest, GuestToken, 404, frame[0..valid], error.WrongPhase);

    var incoming: [MaxTestFrame]u8 = undefined;
    const hello_length = try make_hello(&incoming, 7, Epoch, Version, Capabilities);
    var output: [MaxTestFrame]u8 = undefined;
    var awaiting = try init_peer(std_t.testing.allocator, .Host, HostToken, 400);
    defer awaiting.deinit();
    _ = try awaiting.feed(HostToken, incoming[0..hello_length]);
    _ = try awaiting.next_event(HostToken, 401, &output);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitMapping, awaiting.phase());
    try expect_refused(&awaiting, HostToken, 402, frame[0..valid], error.WrongPhase);
    _ = try awaiting.host_mapping_validated(HostToken, 402, Epoch, c_t.InteropLayoutVersion, true);
    try std_t.testing.expectEqual(peer_t.phase_t.AwaitHelloAckSent, awaiting.phase());
    try expect_refused(&awaiting, HostToken, 403, frame[0..valid], error.WrongPhase);

    var draining = try init_pair(std_t.testing.allocator, 400);
    defer deinit_pair(&draining);
    try complete_handshake(&draining, 401);
    _ = try draining.host.begin_drain(HostToken, 402);
    try expect_refused(&draining.host, HostToken, 403, frame[0..valid], error.WrongPhase);
    _ = try draining.host.transport_lost(HostToken);
    try expect_refused(&draining.host, HostToken, 404, frame[0..valid], error.WrongPhase);

    draining.host.deinit();
    try std_t.testing.expectError(error.PeerClosed, draining.host.enqueue_feature(HostToken, 405, frame[0..valid]));
}

test "token time and Ready deadline boundaries are exact for feature admission" {
    var pair = try init_pair(std_t.testing.allocator, 400);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 401);

    var frame: [MaxTestFrame]u8 = undefined;
    const valid = try make_window_close(&frame, 0, 1, 1);
    try expect_refused(&pair.host, GuestToken, 402, frame[0..valid], error.WrongConnectionToken);
    try expect_refused(&pair.host, 0, 402, frame[0..valid], error.InvalidConnectionToken);
    try expect_refused(&pair.host, HostToken, 400, frame[0..valid], error.TimeWentBackwards);

    const expiry = 401 + c_t.InteropPingTimeoutMs;
    _ = try pair.host.enqueue_feature(HostToken, expiry - 1, frame[0..valid]);
    try std_t.testing.expectEqual(@as(usize, 1), pair.host.queue.count);

    const drained = try pair.host.enqueue_feature(HostToken, expiry, frame[0..valid]);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, drained.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, drained.phase);
    try std_t.testing.expectEqual(@as(usize, 1), pair.host.queue.count);
    try std_t.testing.expect(!pair.host.must_abort_transport());
    try std_t.testing.expectError(error.WrongPhase, pair.host.enqueue_feature(HostToken, expiry + 1, frame[0..valid]));
}

test "data reservation refuses at the exact boundary and recovers after acknowledgements" {
    var pair = try init_pair(std_t.testing.allocator, 400);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 401);

    var frame: [MaxTestFrame]u8 = undefined;
    for (0..DataCapacity) |index| {
        const length = try make_window_close(&frame, 0, index + 1, 1);
        _ = try pair.host.enqueue_feature(HostToken, 402, frame[0..length]);
    }
    try std_t.testing.expectEqual(DataCapacity, pair.host.queue.count);
    const full_length = try make_window_close(&frame, 0, 9999, 3);
    try expect_refused(&pair.host, HostToken, 402, frame[0..full_length], error.QueueFull);

    const ping_at = 401 + c_t.InteropPingIntervalMs;
    _ = try pair.host.tick(HostToken, ping_at);
    try std_t.testing.expectEqual(DataCapacity + 1, pair.host.queue.count);
    try expect_refused(&pair.host, HostToken, ping_at + 1, frame[0..full_length], error.QueueFull);

    var front = (try pair.host.peek_tx(HostToken)).?;
    _ = try pair.host.ack_tx(HostToken, ping_at + 2, front.len);
    try expect_refused(&pair.host, HostToken, ping_at + 3, frame[0..full_length], error.QueueFull);
    front = (try pair.host.peek_tx(HostToken)).?;
    _ = try pair.host.ack_tx(HostToken, ping_at + 4, front.len);
    _ = try pair.host.enqueue_feature(HostToken, ping_at + 5, frame[0..full_length]);
    try std_t.testing.expectEqual(DataCapacity, pair.host.queue.count);

    // Windows 1..504 were admitted in order, the Ping followed them, and Window 9999 came last.
    var next_data_window: u64 = 3;
    var expected: [MaxTestFrame]u8 = undefined;
    var saw_ping = false;
    var now: u64 = ping_at + 6;
    while (pair.host.queue.count > 0) : (now += 1) {
        const bytes = (try pair.host.peek_tx(HostToken)).?;
        var header: c_t.interop_header_t = undefined;
        try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(bytes.ptr, bytes.len, &header));
        if (header.type == c_t.InteropMsgPing) {
            try std_t.testing.expect(!saw_ping);
            saw_ping = true;
        } else if (next_data_window <= 504) {
            const length = try make_window_close(&expected, 0, next_data_window, 1);
            try std_t.testing.expectEqualSlices(u8, expected[0..length], bytes);
            next_data_window += 1;
        } else {
            try std_t.testing.expectEqualSlices(u8, frame[0..full_length], bytes);
        }
        _ = try pair.host.ack_tx(HostToken, now, bytes.len);
    }
    try std_t.testing.expectEqual(@as(u64, 505), next_data_window);
    try std_t.testing.expect(saw_ping);
    try std_t.testing.expectEqual(@as(usize, 0), pair.host.queue.pending_bytes);
}

test "feature frames keep FIFO order around Ping and Pong liveness traffic" {
    var pair = try init_pair(std_t.testing.allocator, 400);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 401);

    var first: [MaxTestFrame]u8 = undefined;
    const first_length = try make_window_close(&first, 0, 101, 1);
    _ = try pair.host.enqueue_feature(HostToken, 402, first[0..first_length]);
    const ping_event = try pair.host.tick(HostToken, 401 + c_t.InteropPingIntervalMs);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, ping_event.kind);
    var second: [MaxTestFrame]u8 = undefined;
    const second_length = try make_window_close(&second, 0, 202, 1);
    _ = try pair.host.enqueue_feature(HostToken, 401 + c_t.InteropPingIntervalMs + 1, second[0..second_length]);
    try std_t.testing.expectEqual(@as(usize, 3), pair.host.queue.count);

    var output: [MaxTestFrame]u8 = undefined;
    _ = try transfer_front_fragmented(&pair.host, HostToken, &pair.guest, GuestToken, 2403, &.{ 5, 9 });
    const delivered_first = try pair.guest.next_event(GuestToken, 2403, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.FeatureFrame, delivered_first.event.kind);
    try std_t.testing.expectEqualSlices(u8, first[0..first_length], output[0..delivered_first.event.copied_bytes]);

    _ = try transfer_front_fragmented(&pair.host, HostToken, &pair.guest, GuestToken, 2404, &.{ 4, 2 });
    const delivered_ping = try pair.guest.next_event(GuestToken, 2404, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, delivered_ping.event.kind);

    _ = try transfer_front_fragmented(&pair.host, HostToken, &pair.guest, GuestToken, 2405, &.{ 6, 3 });
    const delivered_second = try pair.guest.next_event(GuestToken, 2405, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.FeatureFrame, delivered_second.event.kind);
    try std_t.testing.expectEqualSlices(u8, second[0..second_length], output[0..delivered_second.event.copied_bytes]);

    _ = try transfer_front_fragmented(&pair.guest, GuestToken, &pair.host, HostToken, 2406, &.{5});
    const pong = try pair.host.next_event(HostToken, 2406, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.None, pong.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Ready, pair.host.phase());
    try std_t.testing.expectEqual(@as(usize, 0), pair.host.queue.count);
}

test "drain queues Goodbye behind admitted feature bytes and transport loss closes admission" {
    var pair = try init_pair(std_t.testing.allocator, 400);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 401);

    var frame: [MaxTestFrame]u8 = undefined;
    const admitted = try make_window_close(&frame, 0, 7, 1);
    _ = try pair.host.enqueue_feature(HostToken, 402, frame[0..admitted]);
    _ = try pair.host.begin_drain(HostToken, 403);
    try expect_refused(&pair.host, HostToken, 404, frame[0..admitted], error.WrongPhase);
    try std_t.testing.expect(!pair.host.must_abort_transport());

    const front = (try pair.host.peek_tx(HostToken)).?;
    try std_t.testing.expectEqualSlices(u8, frame[0..admitted], front);
    _ = try pair.host.ack_tx(HostToken, 404, front.len);
    const goodbye = (try pair.host.peek_tx(HostToken)).?;
    var header: c_t.interop_header_t = undefined;
    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(goodbye.ptr, goodbye.len, &header));
    try std_t.testing.expectEqual(c_t.InteropMsgGoodbye, header.type);
    _ = try pair.host.ack_tx(HostToken, 405, goodbye.len);
    try std_t.testing.expect((try pair.host.peek_tx(HostToken)) == null);

    _ = try pair.host.transport_lost(HostToken);
    try expect_refused(&pair.host, HostToken, 406, frame[0..admitted], error.WrongPhase);
}

test "allocation failure at feature admission preserves every observable field" {
    var frame: [MaxTestFrame]u8 = undefined;
    const length = try make_window_close(&frame, 0, 12, 1);
    for ([_]bool{ false, true }) |inject| {
        var failing = std_t.testing.FailingAllocator.init(std_t.testing.allocator, .{ .fail_index = std_t.math.maxInt(usize) });
        var pair = pair_t{
            .host = try peer_t.peer_t.init(failing.allocator(), .{ .role = .Host, .connection_token = HostToken, .accepted_at_ms = 400, .local_version = Version, .local_capabilities = Capabilities }),
            .guest = try init_peer(std_t.testing.allocator, .Guest, GuestToken, 400),
        };
        defer deinit_pair(&pair);
        try complete_handshake(&pair, 401);
        const before = snapshot(&pair.host);
        if (inject) {
            failing.fail_index = failing.alloc_index;
            try std_t.testing.expectError(error.OutOfMemory, pair.host.enqueue_feature(HostToken, 402, frame[0..length]));
            try std_t.testing.expect(failing.has_induced_failure);
            try std_t.testing.expectEqualDeep(before, snapshot(&pair.host));
            failing.fail_index = std_t.math.maxInt(usize);
        } else {
            const event = try pair.host.enqueue_feature(HostToken, 402, frame[0..length]);
            try std_t.testing.expectEqual(peer_t.event_kind_t.None, event.kind);
            try std_t.testing.expectEqual(before.queued + 1, pair.host.queue.count);
        }
    }
}

test "negotiated facilities gate each feature family and any-of transfers" {
    var narrow = try init_pair(std_t.testing.allocator, 400);
    defer deinit_pair(&narrow);
    try complete_handshake(&narrow, 401);
    var frame: [MaxTestFrame]u8 = undefined;
    const clipboard = try make_clipboard_clear(&frame, c_t.InteropFromHost, 4);
    try expect_refused(&narrow.host, HostToken, 402, frame[0..clipboard], error.FeatureNotNegotiated);
    const transfer = try make_transfer_cancel(&frame, c_t.InteropFromHost, 9);
    try expect_refused(&narrow.host, HostToken, 402, frame[0..transfer], error.FeatureNotNegotiated);
    const release = try make_input_release(&frame, 3, 3);
    _ = try narrow.host.enqueue_feature(HostToken, 402, frame[0..release]);

    var wide = try init_pair_caps(std_t.testing.allocator, 400, Capabilities | c_t.InteropCapClipboard | c_t.InteropCapCatalog);
    defer deinit_pair(&wide);
    const ready = try complete_handshake_event(&wide, 401);
    try std_t.testing.expectEqual(Capabilities | c_t.InteropCapClipboard | c_t.InteropCapCatalog, ready.capabilities);
    const host_clear = try make_clipboard_clear(&frame, c_t.InteropFromHost, 4);
    _ = try wide.host.enqueue_feature(HostToken, 402, frame[0..host_clear]);
    const guest_clear = try make_clipboard_clear(&frame, c_t.InteropFromGuest, 5);
    _ = try wide.guest.enqueue_feature(GuestToken, 402, frame[0..guest_clear]);
    const guest_transfer = try make_transfer_cancel(&frame, c_t.InteropFromGuest, 11);
    _ = try wide.guest.enqueue_feature(GuestToken, 402, frame[0..guest_transfer]);
    const catalog = try make_catalog_begin(&frame);
    _ = try wide.guest.enqueue_feature(GuestToken, 402, frame[0..catalog]);
    const dnd = try make_dnd_leave(&frame, 1, 1);
    try expect_refused(&wide.host, HostToken, 402, frame[0..dnd], error.FeatureNotNegotiated);
    try std_t.testing.expectEqual(@as(usize, 3), wide.guest.queue.count);
}

test "received feature frames outside the negotiated map drain with UnsupportedType" {
    var narrow = try init_pair(std_t.testing.allocator, 500);
    defer deinit_pair(&narrow);
    try complete_handshake(&narrow, 501);

    // Guest-produced ClipboardClear without Clipboard negotiated: IMPL_DESC 3.2.7 step 4.
    var frame: [MaxTestFrame]u8 = undefined;
    const clipboard = try make_clipboard_clear(&frame, c_t.InteropFromGuest, 5);
    _ = try narrow.host.feed(HostToken, frame[0..clipboard]);
    var output: [MaxTestFrame]u8 = undefined;
    const event = try narrow.host.next_event(HostToken, 502, &output);
    try std_t.testing.expectEqual(peer_t.event_kind_t.Draining, event.event.kind);
    try std_t.testing.expectEqual(peer_t.phase_t.Draining, narrow.host.phase());
    // The refusal is answered by one Error(UnsupportedType) frame, like codec-level rejections.
    const reply = (try narrow.host.peek_tx(HostToken)).?;
    try std_t.testing.expectEqual(@as(u16, c_t.InteropMsgError), std_t.mem.readInt(u16, reply[6..8], .little));
    try std_t.testing.expectEqual(@as(usize, 1), narrow.host.queue.count);
}

// Deterministic random sequence of admissions, partial acknowledgements, Ping ticks and
// refused frames. The model tracks FIFO window IDs for WindowClose frames and counts Pings.
test "randomized admission and partial acknowledgement sequences preserve FIFO accounting" {
    var pair = try init_pair(std_t.testing.allocator, 400);
    defer deinit_pair(&pair);
    try complete_handshake(&pair, 401);

    var prng = std_t.rand.DefaultPrng.init(0x5eed_1234);
    const random = prng.random();
    var expected_ids: [peer_t.QueueCapacity]u64 = undefined;
    var expected_head: usize = 0;
    var expected_count: usize = 0;
    var pings_pending: usize = 0;
    var next_window: u64 = 1;
    var now: u64 = 402;
    var current: [64]u8 = undefined;
    var current_length: usize = 0;
    var current_offset: usize = 0;
    var refused_count: usize = 0;

    for (0..3000) |_| {
        const choice = random.uintLessThan(u32, 100);
        if (choice < 40 and pair.host.queue.count < peer_t.QueueCapacity) {
            var admitted: [64]u8 = undefined;
            const length = try make_window_close(&admitted, 0, next_window, 1);
            const admission = pair.host.enqueue_feature(HostToken, now, admitted[0..length]);
            if (admission) |event| {
                try std_t.testing.expectEqual(peer_t.event_kind_t.None, event.kind);
                expected_ids[(expected_head + expected_count) % peer_t.QueueCapacity] = next_window;
                expected_count += 1;
                next_window += 1;
            } else |err| {
                try std_t.testing.expectEqual(error.QueueFull, err);
            }
        } else if (choice < 45) {
            try expect_refused(&pair.host, HostToken, now, current[0..0], error.InvalidFrame);
            refused_count += 1;
        } else if (choice < 50) {
            const event = try pair.host.tick(HostToken, now);
            try std_t.testing.expect(event.kind != .Draining);
            if (pair.host.queue.count > expected_count + pings_pending) pings_pending += 1;
        } else {
            if (try pair.host.peek_tx(HostToken)) |front| {
                if (current_offset == 0) current_length = front.len;
                const count = random.uintAtMost(usize, front.len);
                @memcpy(current[current_offset..][0..count], front[0..count]);
                current_offset += count;
                _ = try pair.host.ack_tx(HostToken, now, count);
                if (current_offset == current_length) {
                    var header: c_t.interop_header_t = undefined;
                    try std_t.testing.expectEqual(c_t.InteropErrorNone, c_t.interop_header_decode(&current, current_length, &header));
                    if (header.type == c_t.InteropMsgPing) {
                        pings_pending -= 1;
                    } else {
                        try std_t.testing.expectEqual(c_t.InteropMsgWindowClose, header.type);
                        try std_t.testing.expect(expected_count > 0);
                        const window = std_t.mem.readInt(u64, current[20..28], .little);
                        try std_t.testing.expectEqual(expected_ids[expected_head], window);
                        expected_head = (expected_head + 1) % peer_t.QueueCapacity;
                        expected_count -= 1;
                    }
                    current_offset = 0;
                }
            }
        }
        try std_t.testing.expectEqual(pair.host.queue.count, pair.host.send_count);
        try std_t.testing.expectEqual(expected_count + pings_pending, pair.host.queue.count);
        now += random.uintAtMost(u64, 2);
        try std_t.testing.expect(now < 401 + c_t.InteropPingTimeoutMs);
    }

    while (try pair.host.peek_tx(HostToken)) |front| {
        const count = front.len;
        _ = try pair.host.ack_tx(HostToken, now, count);
        now += 1;
        if (pair.host.queue.count == 0) break;
    }
    try std_t.testing.expectEqual(@as(usize, 0), pair.host.queue.count);
    try std_t.testing.expectEqual(@as(usize, 0), pair.host.send_count);
    try std_t.testing.expect(refused_count > 0);
}
