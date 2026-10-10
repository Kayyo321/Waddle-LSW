const std = @import("std");
const c = @cImport({
    @cInclude("waddle/av_protocol.h");
});
fn get(comptime T: type, data: []const u8, offset: usize) T {
    return std.mem.readInt(T, data[offset..][0..@sizeOf(T)], .little);
}
fn put(comptime T: type, data: []u8, offset: usize, value: T) void {
    std.mem.writeInt(T, data[offset..][0..@sizeOf(T)], value, .little);
}
fn input_valid(message: *const c.av_message_t, kind: u32) bool {
    return switch (kind) {
        7 => message.flags <= 1 and message.x == 0 and message.y == 0 and message.width == 0,
        8 => message.flags <= 1 and message.x == 0 and message.y == 0 and message.width >= 1 and message.width <= 127,
        9 => message.flags == 0 and message.width == 0 and message.x >= 0 and message.x < 8192 and message.y >= 0 and message.y < 8192,
        10 => message.flags <= 1 and message.x == 0 and message.y == 0 and message.width >= 1 and message.width <= 3,
        11 => message.flags == 0 and message.width == 0 and message.x >= -1200 and message.x <= 1200 and message.y >= -1200 and message.y <= 1200 and (message.x != 0 or message.y != 0),
        12 => message.flags >= 1 and message.flags <= 3 and message.x == 0 and message.y == 0 and message.width == 0,
        else => false,
    };
}
fn valid(message: *const c.av_message_t) bool {
    if (message.type < 1 or message.type > 23 or message.flags > 3) return false;
    if (message.type <= 14 and message.lease_generation != 0) return false;
    const title: []const u8 = std.mem.sliceAsBytes(&message.title);
    const end = std.mem.indexOfScalar(u8, title, 0) orelse return false;
    if (!std.unicode.utf8ValidateSlice(title[0..end])) return false;
    if (message.type >= 15) {
        if (message.lease_generation == 0 or message.height != 0 or message.dpi != 0 or
            message.process_id != 0 or message.damage_x != 0 or message.damage_y != 0 or
            message.damage_width != 0 or message.damage_height != 0 or !std.mem.allEqual(u8, title, 0)) return false;
        if (message.type <= 17) return message.window_id == 0 and message.sequence == 0 and
            message.x == 0 and message.y == 0 and message.width == 0 and message.flags == 0 and
            (if (message.type == 15) message.buffer_index == 0 else message.buffer_index != 0);
        return message.window_id != 0 and message.sequence != 0 and message.buffer_index != 0 and
            input_valid(message, message.type - 11);
    }
    if (message.window_id == 0) return false;
    if (message.type >= 7 and message.type <= 12) {
        if (message.sequence == 0 or message.buffer_index == 0 or message.height != 0 or
            message.dpi != 0 or message.process_id != 0 or message.damage_x != 0 or
            message.damage_y != 0 or message.damage_width != 0 or message.damage_height != 0 or
            !std.mem.allEqual(u8, title, 0)) return false;
        return input_valid(message, message.type);
    }
    if (message.type == 6) return message.sequence != 0 and message.sequence <= 0xffffff and message.flags <= 1;
    if (message.type == 2 or message.type == 5) return true;
    if (message.width == 0 or message.height == 0 or message.width > 8192 or message.height > 8192 or message.dpi < 48 or message.dpi > 768) return false;
    if ((message.type == 1 or message.type == 13 or message.type == 14) and (message.process_id == 0 or message.buffer_index >= 16)) return false;
    if ((message.type == 13 or message.type == 14) and message.sequence == 0) return false;
    if (message.type == 14 and (message.damage_x != 0 or message.damage_y != 0 or message.damage_width != 0 or message.damage_height != 0)) return false;
    if (message.type == 4) {
        if (message.buffer_index >= 3 or message.sequence == 0 or message.damage_x < 0 or message.damage_y < 0 or message.damage_width == 0 or message.damage_height == 0) return false;
        if (@as(u64, @intCast(message.damage_x)) + message.damage_width > message.width or @as(u64, @intCast(message.damage_y)) + message.damage_height > message.height) return false;
    }
    return true;
}
/// in: borrowed untrusted bytes[length]; out: caller-owned message unchanged on
/// failure. Returns 0/-1; pure, thread-safe, no allocations or ownership transfer.
export fn av_control_decode(bytes: [*]const u8, length: usize, message: *c.av_message_t) c_int {
    if (length != 328) return -1;
    const data = bytes[0..length];
    if (get(u16, data, 0) != 0x574c or get(u32, data, 4) != 320) return -1;
    var result: c.av_message_t = std.mem.zeroes(c.av_message_t);
    result.type = get(u16, data, 2);
    result.window_id = get(u64, data, 8);
    result.x = get(i32, data, 16);
    result.y = get(i32, data, 20);
    result.width = get(u32, data, 24);
    result.height = get(u32, data, 28);
    result.flags = get(u32, data, 32);
    result.dpi = get(u32, data, 36);
    result.process_id = get(u32, data, 40);
    result.buffer_index = get(u32, data, 44);
    result.sequence = get(u64, data, 48);
    if (result.type >= 15 and result.type <= 23) {
        result.lease_generation = get(u64, data, 56);
        if (!std.mem.allEqual(u8, data[64..328], 0)) return -1;
    } else {
    result.damage_x = get(i32, data, 56);
    result.damage_y = get(i32, data, 60);
    result.damage_width = get(u32, data, 64);
    result.damage_height = get(u32, data, 68);
    @memcpy(std.mem.asBytes(&result.title), data[72..328]);
    }
    if (!valid(&result)) return -1;
    message.* = result;
    return 0;
}
/// in: borrowed message; out: bytes[capacity] retained by caller. Returns 0/-1;
/// no allocation, pure/thread-safe, no mutation on error; canonical zero padding.
export fn av_control_encode(message: *const c.av_message_t, bytes: [*]u8, capacity: usize) c_int {
    if (capacity < 328 or !valid(message)) return -1;
    const data = bytes[0..328];
    @memset(data, 0);
    put(u16, data, 0, 0x574c);
    put(u16, data, 2, @intCast(message.type));
    put(u32, data, 4, 320);
    put(u64, data, 8, message.window_id);
    put(i32, data, 16, message.x);
    put(i32, data, 20, message.y);
    put(u32, data, 24, message.width);
    put(u32, data, 28, message.height);
    put(u32, data, 32, message.flags);
    put(u32, data, 36, message.dpi);
    put(u32, data, 40, message.process_id);
    put(u32, data, 44, message.buffer_index);
    put(u64, data, 48, message.sequence);
    if (message.type >= 15) {
        put(u64, data, 56, message.lease_generation);
        return 0;
    }
    put(i32, data, 56, message.damage_x);
    put(i32, data, 60, message.damage_y);
    put(u32, data, 64, message.damage_width);
    put(u32, data, 68, message.damage_height);
    const title = std.mem.sliceAsBytes(&message.title);
    const end = std.mem.indexOfScalar(u8, title, 0).?;
    @memcpy(data[72..][0..end], title[0..end]);
    return 0;
}
test "canonical endian lifecycle and frame validation" {
    var message = std.mem.zeroes(c.av_message_t);
    message.window_id = 0x123456789abcdef0;
    message.type = 1;
    message.x = -100;
    message.width = 640;
    message.height = 480;
    message.dpi = 96;
    message.process_id = 123;
    var bytes: [328]u8 = undefined;
    var decoded: c.av_message_t = undefined;
    for ([_]u32{ 1, 2, 3, 4, 5 }) |kind| {
        message.type = kind;
        message.sequence = 1;
        message.damage_width = 640;
        message.damage_height = 480;
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_control_encode, .{ &message, &bytes, bytes.len }));
        try std.testing.expectEqual(@as(u8, 0xf0), bytes[8]);
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_control_decode, .{ &bytes, bytes.len, &decoded }));
        try std.testing.expectEqual(message.x, decoded.x);
        try std.testing.expectEqual(message.window_id, decoded.window_id);
    }
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_decode, .{ &bytes, 327, &decoded }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_encode, .{ &message, &bytes, 327 }));
    for ([_]usize{ 0, 2, 4 }) |offset| {
        bytes[offset] ^= 0xff;
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_decode, .{ &bytes, 328, &decoded }));
        bytes[offset] ^= 0xff;
    }
    message.type = 4;
    for ([_]u32{ 0, 8193 }) |width| {
        message.width = width;
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_encode, .{ &message, &bytes, 328 }));
    }
    message.width = 640;
    message.buffer_index = 3;
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_encode, .{ &message, &bytes, 328 }));
    message.buffer_index = 0;
    message.damage_x = -1;
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_encode, .{ &message, &bytes, 328 }));
    message.damage_x = 1;
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_encode, .{ &message, &bytes, 328 }));
    message.damage_x = 0;
    message.title = .{1} ** 256;
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_encode, .{ &message, &bytes, 328 }));
    message.title = .{0} ** 256;
    message.title[0] = @bitCast(@as(u8, 0xff));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_encode, .{ &message, &bytes, 328 }));
}

/// in: borrowed decimal bytes[length]; out: value unchanged on failure. Returns
/// 0/-1 for bounded nonzero u32, no allocation, pure/thread-safe.
export fn av_number_parse(bytes: [*]const u8, length: usize, value: *u32) c_int {
    if (length == 0 or length > 10) return -1;
    var result: u64 = 0;
    for (bytes[0..length]) |digit| {
        if (digit < '0' or digit > '9') return -1;
        result = result * 10 + digit - '0';
        if (result > std.math.maxInt(u32)) return -1;
    }
    if (result == 0) return -1;
    value.* = @intCast(result);
    return 0;
}
test "bounded native session numbers" {
    var value: u32 = 99;
    for ([_][]const u8{ "", "0", "-1", "+1", "1x", "4294967296", "00000000001" }) |invalid| {
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_number_parse, .{ invalid.ptr, invalid.len, &value }));
        try std.testing.expectEqual(@as(u32, 99), value);
    }
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_number_parse, .{ "4294967295", 10, &value }));
    try std.testing.expectEqual(std.math.maxInt(u32), value);
}

test "invalid lifecycle and frame fields never alter encoded output" {
    const valid_frame: c.av_message_t = .{ .type = 4, .window_id = 42, .x = 0, .y = 0, .width = 640, .height = 480, .flags = 0, .dpi = 96, .process_id = 123, .buffer_index = 0, .sequence = 1, .damage_x = 0, .damage_y = 0, .damage_width = 640, .damage_height = 480, .title = .{0} ** 256, .lease_generation = 0 };
    for (0..20) |index| {
        var message = valid_frame;
        switch (index) {
            0 => message.window_id = 0,
            1 => message.type = 0,
            2 => message.type = 14,
            3 => message.flags = 4,
            4 => message.height = 0,
            5 => message.height = 8193,
            6 => message.dpi = 47,
            7 => message.dpi = 769,
            8 => {
                message.type = 1;
                message.process_id = 0;
            },
            9 => message.sequence = 0,
            10 => message.damage_y = -1,
            11 => message.damage_width = 0,
            12 => message.damage_height = 0,
            13 => message.damage_y = 1,
            14 => message.width = 0,
            15 => message.width = 8193,
            16 => message.damage_x = -1,
            17 => message.damage_x = 2147483647,
            18 => message.buffer_index = 3,
            19 => {
                message.type = 1;
                message.buffer_index = 16;
            },
            else => unreachable,
        }
        var bytes = [_]u8{0xa5} ** 328;
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_encode, .{ &message, &bytes, bytes.len }));
        try std.testing.expectEqualSlices(u8, &([_]u8{0xa5} ** 328), &bytes);
    }
}

/// Validate canonical lowercase sysfs UUID bytes; borrowed slice, no allocation,
/// output or ownership transfer. Pure and thread-safe; false means invalid syntax.
fn av_uuid_valid(value: []const u8) bool {
    if (value.len != 36) return false;
    for (value, 0..) |byte, index| {
        if (index == 8 or index == 13 or index == 18 or index == 23) {
            if (byte != '-') return false;
        } else if (!std.ascii.isHex(byte) or (byte >= 'A' and byte <= 'F')) return false;
    }
    return true;
}

/// in: nonnull borrowed bytes[length]; returns 0 valid canonical UUID, -1 invalid.
/// Pure, thread-safe, allocation-free, no output or ownership transfer.
export fn av_gpu_uuid_validate(bytes: [*]const u8, length: usize) c_int {
    if (length != 36) return -1;
    return if (av_uuid_valid(bytes[0..length])) 0 else -1;
}

/// in: borrowed bytes[length]; returns 0 valid PCI address, -1 invalid. Pure,
/// thread-safe, allocation-free; no output or ownership transfer.
export fn av_gpu_bdf_validate(bytes: [*]const u8, length: usize) c_int {
    if (length != 12) return -1;
    const value = bytes[0..length];
    if (value[4] != ':' or value[7] != ':' or value[10] != '.' or value[11] < '0' or value[11] > '7') return -1;
    for ([_]usize{ 0, 1, 2, 3, 5, 6, 8, 9 }) |index| {
        if (!std.ascii.isHex(value[index])) return -1;
    }
    return if ((std.fmt.parseInt(u8, value[8..10], 16) catch return -1) < 32) 0 else -1;
}
test "PCI paths reject traversal and invalid slot/function" {
    for ([_][]const u8{ "", "../../driver", "0000:00:20.0", "0000:00:00.8", "0000:0g:00.0", "0000-00:00.0" }) |value|
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_gpu_bdf_validate, .{ value.ptr, value.len }));
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_gpu_bdf_validate, .{ "0000:0E:1f.7", 12 }));
}

test "diagnostic tokens have bounded color and occlusion state" {
    var message = std.mem.zeroes(c.av_message_t);
    message.type = 6;
    message.window_id = 1;
    var bytes: [328]u8 = undefined;
    var decoded: c.av_message_t = undefined;
    for ([_]u64{ 1, 0xffffff }) |token| {
        message.sequence = token;
        message.flags = 1;
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_control_encode, .{ &message, &bytes, bytes.len }));
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_control_decode, .{ &bytes, bytes.len, &decoded }));
        try std.testing.expectEqual(token, decoded.sequence);
    }
    for ([_]u64{ 0, 0x1000000 }) |token| {
        message.sequence = token;
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_encode, .{ &message, &bytes, bytes.len }));
    }
    message.sequence = 1;
    message.flags = 2;
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_encode, .{ &message, &bytes, bytes.len }));
}

test "mdev UUID validation rejects path and spelling ambiguity" {
    const ValidUuid = "12345678-1234-5678-9abc-123456789abc";
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_gpu_uuid_validate, .{ ValidUuid, ValidUuid.len }));
    for ([_][]const u8{ "", "../12345678-1234-5678-9abc-123456789abc", "12345678-1234-5678-9abc-123456789abC" }) |value|
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_gpu_uuid_validate, .{ value.ptr, value.len }));
    for (0..36) |index| {
        var value = ValidUuid.*;
        value[index] = 'g';
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_gpu_uuid_validate, .{ &value, value.len }));
    }
}

test "input envelopes round trip and reject every reserved byte transactionally" {
    for (7..13) |kind| {
        var event = std.mem.zeroes(c.av_message_t);
        event.type = @intCast(kind);
        event.window_id = 0x1234;
        event.sequence = 42;
        event.buffer_index = 10;
        if (kind == 7 or kind == 8 or kind == 10 or kind == 12) event.flags = 1;
        if (kind == 8 or kind == 10) event.width = 1;
        if (kind == 11) event.y = -120;
        var bytes: [328]u8 = undefined;
        var decoded = std.mem.zeroes(c.av_message_t);
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_control_encode, .{ &event, &bytes, bytes.len }));
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_control_decode, .{ &bytes, bytes.len, &decoded }));
        try std.testing.expectEqual(event.sequence, decoded.sequence);
        for (0..bytes.len) |length|
            try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_decode, .{ &bytes, length, &decoded }));
        // Every byte in reserved scalar fields and title is rejected; this also
        // exercises bytes after title's first NUL (legacy titles allow padding).
        for ([_]usize{ 28, 36, 40, 56, 60, 64, 68 }) |offset| {
            bytes[offset] = 1;
            const sentinel = decoded;
            try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_decode, .{ &bytes, bytes.len, &decoded }));
            try std.testing.expectEqualSlices(u8, std.mem.asBytes(&sentinel), std.mem.asBytes(&decoded));
            bytes[offset] = 0;
        }
        for (72..328) |offset| {
            bytes[offset] = 1;
            try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_decode, .{ &bytes, bytes.len, &decoded }));
            bytes[offset] = 0;
        }
        for (0..22) |index| {
            var invalid = event;
            switch (index) {
                0 => invalid.sequence = 0,
                1 => invalid.buffer_index = 0,
                2 => invalid.height = 1,
                3 => invalid.dpi = 1,
                4 => invalid.process_id = 1,
                5 => invalid.damage_x = 1,
                6 => invalid.damage_y = 1,
                7 => invalid.damage_width = 1,
                8 => invalid.damage_height = 1,
                9 => invalid.title[255] = 1,
                10 => invalid.flags = 4,
                11 => invalid.x = -1,
                12 => invalid.x = 8192,
                13 => invalid.y = -1,
                14 => invalid.y = 8192,
                15 => invalid.width = 128,
                16 => invalid.width = 0,
                17 => invalid.flags = 0,
                18 => invalid.x = -1201,
                19 => invalid.y = -1201,
                20 => invalid.flags = 2,
                21 => { invalid.x = 0; invalid.y = 0; },
                else => unreachable,
            }
            // Each type has different valid bounds. Only cases outside that
            // type's exact domain belong to the rejection corpus.
            if (valid(&invalid)) continue;
            var unchanged = [_]u8{0xa5} ** 328;
            try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_encode, .{ &invalid, &unchanged, unchanged.len }));
            try std.testing.expectEqualSlices(u8, &([_]u8{0xa5} ** 328), &unchanged);
        }
        for ([_]i32{ -1200, -1, 0, 1, 1200, 8191 }) |coordinate| {
            var boundary = event;
            boundary.x = coordinate;
            boundary.y = coordinate;
            const expected: c_int = if (valid(&boundary)) 0 else -1;
            try std.testing.expectEqual(expected, @call(.never_inline, av_control_encode, .{ &boundary, &bytes, bytes.len }));
        }
    }
}

test "CreateV2 golden bytes and lifecycle identity remain fixed and transactional" {
    const creation: c.av_message_t = .{ .type = 13, .window_id = 0x0807060504030201,
        .x = -2, .y = 3, .width = 640, .height = 480, .flags = 0, .dpi = 96,
        .process_id = 123, .buffer_index = 2, .sequence = 0x1817161514131211,
        .damage_x = 0, .damage_y = 0, .damage_width = 0, .damage_height = 0,
        .title = .{0} ** 256, .lease_generation = 0 };
    var golden = [_]u8{0} ** 328;
    golden[0] = 0x4c; golden[1] = 0x57; golden[2] = 13;
    golden[4] = 0x40; golden[5] = 1;
    @memcpy(golden[8..16], &[_]u8{1,2,3,4,5,6,7,8});
    @memcpy(golden[16..20], &[_]u8{0xfe,0xff,0xff,0xff});
    golden[20] = 3; golden[24] = 0x80; golden[25] = 2;
    golden[28] = 0xe0; golden[29] = 1; golden[36] = 96;
    golden[40] = 123; golden[44] = 2;
    @memcpy(golden[48..56], &[_]u8{0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18});
    var bytes: [328]u8 = undefined;
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_control_encode, .{ &creation, &bytes, bytes.len }));
    try std.testing.expectEqualSlices(u8, &golden, &bytes);
    var decoded = creation;
    for (0..328) |length| {
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_decode, .{ &bytes, length, &decoded }));
        try std.testing.expectEqualSlices(u8, std.mem.asBytes(&creation), std.mem.asBytes(&decoded));
    }
    for ([_]usize{ 0, 2, 4, 8, 24, 28, 36, 40, 48 }) |offset| {
        var corrupt = golden;
        if (offset == 48) @memset(corrupt[48..56], 0)
        else if (offset == 8) @memset(corrupt[8..16], 0)
        else if (offset == 24 or offset == 28) @memset(corrupt[offset..][0..4], 0)
        else corrupt[offset] = 0;
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_control_decode, .{ &corrupt, corrupt.len, &decoded }));
        try std.testing.expectEqualSlices(u8, std.mem.asBytes(&creation), std.mem.asBytes(&decoded));
    }
    for ([_]u32{ 2, 3, 5, 13 }) |kind| {
        var message = creation; message.type = kind;
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_control_encode, .{ &message, &bytes, bytes.len }));
        try std.testing.expectEqualSlices(u8, golden[48..56], bytes[48..56]);
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_control_decode, .{ &bytes, bytes.len, &decoded }));
        try std.testing.expectEqual(message.sequence, decoded.sequence);
    }
    // Zero legacy lifecycle stays decodable; modern dispatch rejects it by mode.
    for ([_]u32{ 1, 2, 3, 5 }) |kind| {
        var message = creation; message.type = kind; message.sequence = 0;
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_control_encode, .{ &message, &bytes, bytes.len }));
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_control_decode, .{ &bytes, bytes.len, &decoded }));
    }
}

test "CreateV3 and epoch opcodes have independent golden fields and canonical padding" {
    var create = std.mem.zeroes(c.av_message_t);
    create.type = 14; create.window_id = 7; create.sequence = 0x0807060504030201;
    create.width = 640; create.height = 480; create.dpi = 96; create.process_id = 55;
    var bytes: [328]u8 = undefined;
    var decoded = std.mem.zeroes(c.av_message_t);
    try std.testing.expectEqual(@as(c_int, 0), av_control_encode(&create, &bytes, bytes.len));
    try std.testing.expectEqual(@as(u8, 14), bytes[2]);
    try std.testing.expectEqual(@as(c_int, 0), av_control_decode(&bytes, bytes.len, &decoded));
    try std.testing.expectEqual(@as(u64, 0), decoded.lease_generation);
    for (0..4) |field| {
        var invalid = create;
        switch (field) { 0 => invalid.damage_x = 1, 1 => invalid.damage_y = 1,
            2 => invalid.damage_width = 1, 3 => invalid.damage_height = 1, else => unreachable }
        try std.testing.expectEqual(@as(c_int, -1), av_control_encode(&invalid, &bytes, bytes.len));
    }
    for (15..24) |kind| {
        var event = std.mem.zeroes(c.av_message_t);
        event.type = @intCast(kind); event.lease_generation = 0x8877665544332211;
        if (kind != 15) event.buffer_index = 0x04030201;
        if (kind >= 18) { event.window_id = 0x0807060504030201; event.sequence = 0x1817161514131211; }
        if (kind == 18 or kind == 19 or kind == 21 or kind == 23) event.flags = 1;
        if (kind == 19 or kind == 21) event.width = 1;
        if (kind == 22) event.y = -120;
        try std.testing.expectEqual(@as(c_int, 0), av_control_encode(&event, &bytes, bytes.len));
        try std.testing.expectEqual(@as(u8, @intCast(kind)), bytes[2]);
        try std.testing.expectEqualSlices(u8, &[_]u8{0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88}, bytes[56..64]);
        try std.testing.expectEqual(@as(c_int, 0), av_control_decode(&bytes, bytes.len, &decoded));
        try std.testing.expectEqual(event.lease_generation, decoded.lease_generation);
        try std.testing.expectEqual(event.sequence, decoded.sequence);
        try std.testing.expectEqual(event.buffer_index, decoded.buffer_index);
        try std.testing.expectEqual(@as(i32, 0), decoded.damage_x);
        for (64..328) |offset| {
            bytes[offset] = 1;
            const before = decoded;
            try std.testing.expectEqual(@as(c_int, -1), av_control_decode(&bytes, bytes.len, &decoded));
            try std.testing.expectEqualSlices(u8, std.mem.asBytes(&before), std.mem.asBytes(&decoded));
            bytes[offset] = 0;
        }
        for (0..19) |field| {
            var invalid = event;
            switch (field) {
                0 => invalid.lease_generation = 0, 1 => invalid.height = 1, 2 => invalid.dpi = 1,
                3 => invalid.process_id = 1, 4 => invalid.damage_x = 1, 5 => invalid.damage_y = 1,
                6 => invalid.damage_width = 1, 7 => invalid.damage_height = 1, 8 => invalid.title[255] = 1,
                9 => invalid.buffer_index = if (kind == 15) 1 else 0,
                10 => invalid.window_id = if (kind <= 17) 1 else 0,
                11 => invalid.sequence = if (kind <= 17) 1 else 0,
                12 => invalid.x = 8192, 13 => invalid.y = 8192,
                14 => invalid.width = 128, 15 => invalid.flags = 4,
                16 => invalid.type = 24, 17 => invalid.x = -1201, 18 => invalid.y = -1201,
                else => unreachable,
            }
            var untouched = [_]u8{0xa5} ** 328;
            try std.testing.expectEqual(@as(c_int, -1), av_control_encode(&invalid, &untouched, untouched.len));
            try std.testing.expectEqualSlices(u8, &([_]u8{0xa5} ** 328), &untouched);
        }
    }
    // Legacy logical epochs are rejected, but old padding remains untouched.
    for (1..15) |kind| {
        var old = create; old.type = @intCast(kind); old.lease_generation = 1;
        try std.testing.expectEqual(@as(c_int, -1), av_control_encode(&old, &bytes, bytes.len));
    }
    create.type = 13; create.damage_x = 17;
    try std.testing.expectEqual(@as(c_int, 0), av_control_encode(&create, &bytes, bytes.len));
    bytes[327] = 9;
    try std.testing.expectEqual(@as(c_int, 0), av_control_decode(&bytes, bytes.len, &decoded));
    try std.testing.expectEqual(@as(u64, 0), decoded.lease_generation);
}
