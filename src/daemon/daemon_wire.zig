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

/// Fixed daemon status mount value; caller owns all copied bytes.
const daemon_mount_t = extern struct {
    host_path: [1024]u8,
    guest_drive: [32]u8,
    read_only: u32,
    reserved: u32,
};
/// Exact legacy daemon status value. No field contains a pointer.
const daemon_status_response_t = extern struct {
    subsystem_state: u32,
    daemon_pid: u32,
    qemu_pid: u32,
    virtiofsd_pid: u32,
    vsock_cid: u32,
    vsock_port: u32,
    uptime_sec: u64,
    memory_mb: u32,
    vcpus: u32,
    mount_count: u32,
    reserved: u32,
    mounts: [8]daemon_mount_t,
};
comptime {
    if (@sizeOf(daemon_mount_t) != 1064 or @sizeOf(daemon_status_response_t) != 8560 or
        @offsetOf(daemon_status_response_t, "mounts") != 48)
        @compileError("legacy status wire layout mismatch");
}

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

/// in: readable bytes[length]; out: disjoint aligned u32 errno. Returns0/-1.
/// Legacy diagnostic bytes are opaque. Failure preserves output; no allocation; thread-safe.
export fn daemon_wire_decode_error_response(bytes: ?[*]const u8, length: usize, out_error: ?*u32) c_int {
    if (bytes == null or out_error == null or length != 264) return -1;
    const code = read_u32(bytes.?[0..length]);
    if (code == 0 or code > 4095) return -1;
    out_error.?.* = code;
    return 0;
}

/// in: exact readable 8560-byte legacy status payload; out: disjoint aligned value.
/// Returns0/-1; failure preserves output. No allocation, retention or shared state.
/// Validates admission scalars and all active mount bounds; inactive mounts become zero.
export fn daemon_wire_decode_status_response(bytes: ?[*]const u8, length: usize, out_response: ?*daemon_status_response_t) c_int {
    if (bytes == null or out_response == null or length != 8560) return -1;
    const data = bytes.?[0..length];
    const state = read_u32(data[0..]);
    const cid = read_u32(data[16..]);
    const port = read_u32(data[20..]);
    const memory_mb = read_u32(data[32..]);
    const vcpus = read_u32(data[36..]);
    const count = read_u32(data[40..]);
    if (state > 6 or cid < 3 or cid == std_t.math.maxInt(u32) or port == 0 or
        memory_mb < 512 or memory_mb > 65536 or vcpus == 0 or vcpus > 128 or
        count > 8 or read_u32(data[44..]) != 0) return -1;
    var result = std_t.mem.zeroes(daemon_status_response_t);
    result.subsystem_state = state;
    result.daemon_pid = read_u32(data[4..]);
    result.qemu_pid = read_u32(data[8..]);
    result.virtiofsd_pid = read_u32(data[12..]);
    result.vsock_cid = cid;
    result.vsock_port = port;
    result.uptime_sec = std_t.mem.readInt(u64, data[24..32], .little);
    result.memory_mb = memory_mb;
    result.vcpus = vcpus;
    result.mount_count = count;
    for (0..count) |index| {
        const raw = data[48 + index * 1064 ..][0..1064];
        if (raw[0] == 0 or raw[1024] == 0 or
            std_t.mem.indexOfScalar(u8, raw[0..1024], 0) == null or
            std_t.mem.indexOfScalar(u8, raw[1024..1056], 0) == null or
            read_u32(raw[1056..]) > 1 or read_u32(raw[1060..]) != 0) return -1;
        @memcpy(&result.mounts[index].host_path, raw[0..1024]);
        @memcpy(&result.mounts[index].guest_drive, raw[1024..1056]);
        result.mounts[index].read_only = read_u32(raw[1056..]);
    }
    out_response.?.* = result;
    return 0;
}

// Test-only fixtures.
test "legacy error status exact bounds preserve destination" {
    var data = [_]u8{0xa5} ** 264;
    var out: u32 = 9;
    for ([_]u32{ 0, 1, 95, 4095, 4096, std_t.math.maxInt(u32) }) |code| {
        std_t.mem.writeInt(u32, data[0..4], code, .little);
        out = 9;
        const valid = code >= 1 and code <= 4095;
        try std_t.testing.expectEqual(@as(c_int, if (valid) 0 else -1), @call(.never_inline, daemon_wire_decode_error_response, .{ &data, 264, &out }));
        try std_t.testing.expectEqual(if (valid) code else 9, out);
    }
    try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_error_response, .{ null, 264, &out }));
    try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_error_response, .{ &data, 264, null }));
    for ([_]usize{ 0, 263, 265, std_t.math.maxInt(usize) }) |length| {
        try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_error_response, .{ &data, length, &out }));
        try std_t.testing.expectEqual(@as(u32, 9), out);
    }
}
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

test "status payload exact scalar mount bounds and transactional output" {
    var data = [_]u8{0} ** 8560;
    std_t.mem.writeInt(u32, data[0..4], 4, .little);
    std_t.mem.writeInt(u32, data[16..20], 3, .little);
    std_t.mem.writeInt(u32, data[20..24], 5242, .little);
    std_t.mem.writeInt(u32, data[32..36], 4096, .little);
    std_t.mem.writeInt(u32, data[36..40], 4, .little);
    const Sentinel = std_t.mem.zeroes(daemon_status_response_t);
    var out = Sentinel;
    try std_t.testing.expectEqual(@as(c_int, 0), @call(.never_inline, daemon_wire_decode_status_response, .{ &data, data.len, &out }));
    try std_t.testing.expectEqual(@as(u32, 3), out.vsock_cid);
    const fields = [_]struct { offset: usize, valid: u32, invalid: []const u32 }{
        .{ .offset = 0, .valid = 4, .invalid = &.{7, 0xffffffff} },
        .{ .offset = 16, .valid = 3, .invalid = &.{0, 1, 2, 0xffffffff} },
        .{ .offset = 20, .valid = 5242, .invalid = &.{0} },
        .{ .offset = 32, .valid = 4096, .invalid = &.{0, 511, 65537} },
        .{ .offset = 36, .valid = 4, .invalid = &.{0, 129} },
        .{ .offset = 40, .valid = 0, .invalid = &.{9, 0xffffffff} },
        .{ .offset = 44, .valid = 0, .invalid = &.{1} },
    };
    for (fields) |field| {
        for (field.invalid) |bad| {
            std_t.mem.writeInt(u32, data[field.offset..][0..4], bad, .little);
            out = Sentinel;
            try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_status_response, .{ &data, data.len, &out }));
            try std_t.testing.expectEqualDeep(Sentinel, out);
        }
        std_t.mem.writeInt(u32, data[field.offset..][0..4], field.valid, .little);
    }
    std_t.mem.writeInt(u32, data[40..44], 8, .little);
    for (0..8) |index| {
        const offset = 48 + index * 1064;
        data[offset] = '/';
        data[offset+1024] = 'Z';
        std_t.mem.writeInt(u32, data[offset+1056..][0..4], @intCast(index % 2), .little);
    }
    try std_t.testing.expectEqual(@as(c_int, 0), @call(.never_inline, daemon_wire_decode_status_response, .{ &data, data.len, &out }));
    try std_t.testing.expectEqual(@as(u32, 8), out.mount_count);
    for ([_]usize{ 48, 48+1024, 48+1056, 48+1060 }) |offset| {
        const saved = data[offset];
        data[offset] = if (offset == 48 or offset == 48+1024) 0 else 2;
        out = Sentinel;
        try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_status_response, .{ &data, data.len, &out }));
        try std_t.testing.expectEqualDeep(Sentinel, out);
        data[offset] = saved;
    }
    for ([_]struct { start: usize, len: usize }{ .{.start=48,.len=1024}, .{.start=1072,.len=32} }) |range| {
        const saved = data[range.start..][0..range.len];
        var copy: [1024]u8 = undefined;
        @memcpy(copy[0..range.len], saved);
        @memset(data[range.start..][0..range.len], 'x');
        out = Sentinel;
        try std_t.testing.expectEqual(@as(c_int, -1), @call(.never_inline, daemon_wire_decode_status_response, .{ &data, data.len, &out }));
        try std_t.testing.expectEqualDeep(Sentinel, out);
        @memcpy(data[range.start..][0..range.len], copy[0..range.len]);
    }
    for ([_]usize{0,8559,8561,std_t.math.maxInt(usize)}) |length| {
        out = Sentinel;
        try std_t.testing.expectEqual(@as(c_int,-1), @call(.never_inline, daemon_wire_decode_status_response, .{&data,length,&out}));
        try std_t.testing.expectEqualDeep(Sentinel,out);
    }
    try std_t.testing.expectEqual(@as(c_int,-1), @call(.never_inline, daemon_wire_decode_status_response, .{null,8560,&out}));
    try std_t.testing.expectEqual(@as(c_int,-1), @call(.never_inline, daemon_wire_decode_status_response, .{&data,8560,null}));
}
