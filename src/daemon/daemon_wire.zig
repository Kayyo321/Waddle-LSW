const std_t = @import("std");

/// Native C header result. Caller owns storage; serialized mutation, no pointers.
const daemon_header_t = extern struct {
    magic: u32,
    version: u16,
    msg_type: u16,
    sequence: u32,
    payload_len: u32,
};

/// Native C readiness result. Caller owns storage; no allocation or shared state.
const daemon_ready_response_t = extern struct { status: i32, cid: u32 };

fn read_u32(bytes: []const u8) u32 {
    return std_t.mem.readInt(u32, bytes[0..4], .little);
}

/// in: readable bytes[length]; out: disjoint nonnull aligned header. Returns0/-1.
/// Failure preserves output. No allocation/retention; thread-safe disjoint outputs.
export fn daemon_wire_decode_header(bytes: ?[*]const u8, length: usize, out_header: ?*daemon_header_t) c_int {
    if (bytes == null or out_header == null or length != 16) return -1;
    const data = bytes.?[0..length];
    const magic = read_u32(data);
    const version = std_t.mem.readInt(u16, data[4..6], .little);
    const payload_len = read_u32(data[12..]);
    if (magic != 0x57414444 or version != 1 or payload_len > 65536) return -1;
    out_header.?.* = .{
        .magic = magic,
        .version = version,
        .msg_type = std_t.mem.readInt(u16, data[6..8], .little),
        .sequence = read_u32(data[8..]),
        .payload_len = payload_len,
    };
    return 0;
}

/// in: readable bytes[length]; out: disjoint nonnull aligned u32. Returns0/-1.
/// Zero selects120000ms. Invalid input preserves output; no allocation; thread-safe.
export fn daemon_wire_decode_ready_request(bytes: ?[*]const u8, length: usize, out_timeout: ?*u32) c_int {
    if (bytes == null or out_timeout == null or length != 4) return -1;
    const timeout = read_u32(bytes.?[0..length]);
    if (timeout > 120000) return -1;
    out_timeout.?.* = if (timeout == 0) 120000 else timeout;
    return 0;
}

/// in: readable bytes[length]; out: disjoint nonnull aligned result. Returns0/-1.
/// Invalid status/CID/length preserves output; no allocation; thread-safe.
export fn daemon_wire_decode_ready_response(bytes: ?[*]const u8, length: usize, out_response: ?*daemon_ready_response_t) c_int {
    if (bytes == null or out_response == null or length != 8) return -1;
    const data = bytes.?[0..length];
    const status: i32 = @bitCast(read_u32(data));
    const cid = read_u32(data[4..]);
    if (status != 0 and status != -110 and status != -107) return -1;
    if (cid < 3 or cid == std_t.math.maxInt(u32)) return -1;
    out_response.?.* = .{ .status = status, .cid = cid };
    return 0;
}

// Test-only fixtures.
test "exact little endian header and all rejection boundaries preserve output" {
    var data = [_]u8{ 0x44, 0x44, 0x41, 0x57, 1, 0, 0x10, 0, 4, 3, 2, 1, 0, 0, 1, 0 };
    const Sentinel = daemon_header_t{ .magic = 7, .version = 7, .msg_type = 7, .sequence = 7, .payload_len = 7 };
    var out = Sentinel;
    try std_t.testing.expectEqual(@as(c_int, 0), @call(.never_inline, daemon_wire_decode_header, .{ &data, data.len, &out }));
    try std_t.testing.expectEqual(@as(u32, 0x01020304), out.sequence);
    try std_t.testing.expectEqual(@as(u16, 0x10), out.msg_type);
    try std_t.testing.expectEqual(@as(u32, 65536), out.payload_len);
    for ([_]usize{ 0, 15, 17, std_t.math.maxInt(usize) }) |length| {
        out = Sentinel;
        try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_header, .{ &data, length, &out }));
        try std_t.testing.expectEqualDeep(Sentinel, out);
    }
    for ([_]usize{ 0, 4, 12 }) |index| {
        data[index] ^= 1;
        out = Sentinel;
        try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_header, .{ &data, 16, &out }));
        try std_t.testing.expectEqualDeep(Sentinel, out);
        data[index] ^= 1;
    }
    try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_header, .{ null, 16, &out }));
    try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_header, .{ &data, 16, null }));
    data[12] = 0;
    data[14] = 0;
    try std_t.testing.expectEqual(@as(c_int, 0), @call(.never_inline, daemon_wire_decode_header, .{ &data, 16, &out }));
    try std_t.testing.expectEqual(@as(u32, 0), out.payload_len);
}

test "readiness request default maximum and malformed bytes" {
    var data: [4]u8 = undefined;
    var out: u32 = 9;
    for ([_]u32{ 0, 1, 119999, 120000, 120001, std_t.math.maxInt(u32) }) |timeout| {
        std_t.mem.writeInt(u32, &data, timeout, .little);
        out = 9;
        const result = @call(.never_inline, daemon_wire_decode_ready_request, .{ &data, 4, &out });
        try std_t.testing.expectEqual(@as(c_int, if (timeout <= 120000) 0 else -1), result);
        try std_t.testing.expectEqual(if (timeout > 120000) 9 else if (timeout == 0) 120000 else timeout, out);
    }
    try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_ready_request, .{ null, 4, &out }));
    try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_ready_request, .{ &data, 4, null }));
    for ([_]usize{ 0, 3, 5, std_t.math.maxInt(usize) }) |length| {
        try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_ready_request, .{ &data, length, &out }));
        try std_t.testing.expectEqual(@as(u32, 9), out);
    }
}

test "response validates every status CID and length before output mutation" {
    var data: [8]u8 = undefined;
    const Sentinel = daemon_ready_response_t{ .status = 77, .cid = 88 };
    var out = Sentinel;
    for ([_]i32{ 0, -110, -107, 110, 107, -1, std_t.math.minInt(i32), std_t.math.maxInt(i32) }) |status| {
        for ([_]u32{ 0, 1, 2, 3, 7, std_t.math.maxInt(u32) - 1, std_t.math.maxInt(u32) }) |cid| {
            std_t.mem.writeInt(u32, data[0..4], @bitCast(status), .little);
            std_t.mem.writeInt(u32, data[4..8], cid, .little);
            out = Sentinel;
            const valid = (status == 0 or status == -110 or status == -107) and cid >= 3 and cid != std_t.math.maxInt(u32);
            try std_t.testing.expectEqual(@as(c_int, if (valid) 0 else -1), @call(.never_inline, daemon_wire_decode_ready_response, .{ &data, 8, &out }));
            try std_t.testing.expectEqualDeep(if (valid) daemon_ready_response_t{ .status = status, .cid = cid } else Sentinel, out);
        }
    }
    try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_ready_response, .{ null, 8, &out }));
    try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_ready_response, .{ &data, 8, null }));
    for ([_]usize{ 0, 7, 9, std_t.math.maxInt(usize) }) |length| {
        try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_ready_response, .{ &data, length, &out }));
        try std_t.testing.expectEqualDeep(Sentinel, out);
    }
}
