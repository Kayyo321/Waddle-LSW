const std = @import("std");
const c = @cImport({
    @cInclude("av_protocol.h");
});
fn get(comptime T: type, data: []const u8, offset: usize) T {
    return std.mem.readInt(T, data[offset..][0..@sizeOf(T)], .little);
}
fn put(comptime T: type, data: []u8, offset: usize, value: T) void {
    std.mem.writeInt(T, data[offset..][0..@sizeOf(T)], value, .little);
}
fn valid(message: *const c.av_message_t) bool {
    if (message.window_id == 0 or message.type < 1 or message.type > 12 or message.flags > 3) return false;
    const title: []const u8 = std.mem.sliceAsBytes(&message.title);
    const end = std.mem.indexOfScalar(u8, title, 0) orelse return false;
    if (!std.unicode.utf8ValidateSlice(title[0..end])) return false;
    if (message.type >= 7) {
        if (message.sequence == 0 or message.buffer_index == 0 or message.height != 0 or
            message.dpi != 0 or message.process_id != 0 or message.damage_x != 0 or
            message.damage_y != 0 or message.damage_width != 0 or message.damage_height != 0 or
            !std.mem.allEqual(u8, title, 0)) return false;
        return switch (message.type) {
            7 => message.flags <= 1 and message.x == 0 and message.y == 0 and message.width == 0,
            8 => message.flags <= 1 and message.x == 0 and message.y == 0 and message.width >= 1 and message.width <= 127,
            9 => message.flags == 0 and message.width == 0 and message.x >= 0 and message.x < 8192 and message.y >= 0 and message.y < 8192,
            10 => message.flags <= 1 and message.x == 0 and message.y == 0 and message.width >= 1 and message.width <= 3,
            11 => message.flags == 0 and message.width == 0 and message.x >= -1200 and message.x <= 1200 and message.y >= -1200 and message.y <= 1200 and (message.x != 0 or message.y != 0),
            12 => message.flags >= 1 and message.flags <= 3 and message.x == 0 and message.y == 0 and message.width == 0,
            else => false,
        };
    }
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
export fn legacy_av_control_decode(bytes: [*]const u8, length: usize, message: *c.av_message_t) c_int {
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
export fn legacy_av_control_encode(message: *const c.av_message_t, bytes: [*]u8, capacity: usize) c_int {
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
