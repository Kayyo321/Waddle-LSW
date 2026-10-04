const std = @import("std");

/// Caller-owned header result; no allocation or shared state, C ABI layout.
const guest_frame_t = extern struct {
    length: u32,
    crc: u32,
    type: u16,
};

fn get16(bytes: []const u8) u16 {
    return std.mem.readInt(u16, bytes[0..2], .little);
}
fn get32(bytes: []const u8) u32 {
    return std.mem.readInt(u32, bytes[0..4], .little);
}
fn get64(bytes: []const u8) u64 {
    return std.mem.readInt(u64, bytes[0..8], .little);
}

/// in: borrowed nonnull header[length], expected sequence; out: caller-owned frame.
/// Returns 0 for valid 32-byte header, -1 otherwise. No allocation; thread-safe.
export fn guest_header_validate(header: [*]const u8, length: usize, sequence: u32,
    frame: *guest_frame_t) c_int {
    if (length != 32) return -1;
    const bytes = header[0..length];
    if (get32(bytes) != 0x57444c43 or get16(bytes[4..]) != 1 or
        get64(bytes[8..]) != 1 or get32(bytes[20..]) != 0 or
        get32(bytes[24..]) != sequence or get32(bytes[16..]) > 1048576) return -1;
    const kind = get16(bytes[6..]);
    switch (kind) {
        1, 3, 4, 5, 7, 9 => {},
        else => return -1,
    }
    frame.* = .{ .length = get32(bytes[16..]), .crc = get32(bytes[28..]), .type = kind };
    return 0;
}

/// in: borrowed nonnull data[length]; returns IEEE CRC-32. No mutation, allocation
/// or shared state; thread-safe. Empty input is allowed and returns zero.
export fn guest_crc32(data: [*]const u8, length: usize) u32 {
    return std.hash.Crc32.hash(data[0..length]);
}

test "headers reject corruption before payload allocation" {
    var header = [_]u8{0} ** 32;
    std.mem.writeInt(u32, header[0..4], 0x57444c43, .little);
    std.mem.writeInt(u16, header[4..6], 1, .little);
    std.mem.writeInt(u16, header[6..8], 1, .little);
    std.mem.writeInt(u64, header[8..16], 1, .little);
    std.mem.writeInt(u32, header[24..28], 1, .little);
    var frame: guest_frame_t = undefined;
    try std.testing.expectEqual(@as(c_int, 0), guest_header_validate(&header, 32, 1, &frame));
    try std.testing.expectEqual(@as(c_int, -1), guest_header_validate(&header, 31, 1, &frame));
    try std.testing.expectEqual(@as(c_int, -1), guest_header_validate(&header, 32, 2, &frame));
    for ([_]usize{ 0, 4, 8, 20, 24 }) |offset| {
        header[offset] ^= 2;
        try std.testing.expectEqual(@as(c_int, -1), guest_header_validate(&header, 32, 1, &frame));
        header[offset] ^= 2;
    }
    std.mem.writeInt(u32, header[16..20], 1048577, .little);
    try std.testing.expectEqual(@as(c_int, -1), guest_header_validate(&header, 32, 1, &frame));
    std.mem.writeInt(u32, header[16..20], 1048576, .little);
    for ([_]u16{ 1, 3, 4, 5, 7, 9, 2, 6, 8, 255, 0 }) |kind| {
        std.mem.writeInt(u16, header[6..8], kind, .little);
        try std.testing.expectEqual(@as(c_int, if (kind == 1 or kind == 3 or kind == 4 or kind == 5 or kind == 7 or kind == 9) 0 else -1), guest_header_validate(&header, 32, 1, &frame));
    }
    try std.testing.expectEqual(@as(u32, 0xcbf43926), guest_crc32("123456789", 9));
    try std.testing.expectEqual(@as(u32, 0), guest_crc32("", 0));
}
