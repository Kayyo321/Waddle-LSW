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
fn valid(message: *const c.av_message_t) bool {
    if (message.window_id == 0 or message.type < 1 or message.type > 6 or message.flags > 3) return false;
    const title: []const u8 = std.mem.sliceAsBytes(&message.title);
    const end = std.mem.indexOfScalar(u8, title, 0) orelse return false;
    if (!std.unicode.utf8ValidateSlice(title[0..end])) return false;
    if (message.type == 6) return message.sequence != 0 and message.sequence <= 0xffffff and message.flags <= 1;
    if (message.type == 2 or message.type == 5) return true;
    if (message.width == 0 or message.height == 0 or message.width > 8192 or message.height > 8192 or message.dpi < 48 or message.dpi > 768) return false;
    if (message.type == 1 and (message.process_id == 0 or message.buffer_index >= 16)) return false;
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
    result.damage_x = get(i32, data, 56);
    result.damage_y = get(i32, data, 60);
    result.damage_width = get(u32, data, 64);
    result.damage_height = get(u32, data, 68);
    @memcpy(std.mem.asBytes(&result.title), data[72..328]);
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
    const valid_frame: c.av_message_t = .{ .type = 4, .window_id = 42, .x = 0, .y = 0, .width = 640, .height = 480, .flags = 0, .dpi = 96, .process_id = 123, .buffer_index = 0, .sequence = 1, .damage_x = 0, .damage_y = 0, .damage_width = 640, .damage_height = 480, .title = .{0} ** 256 };
    for (0..20) |index| {
        var message = valid_frame;
        switch (index) {
            0 => message.window_id = 0,
            1 => message.type = 0,
            2 => message.type = 7,
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
