const std = @import("std");
const MappingBytes: usize = 65536 + 48 * 33554432;
fn write(comptime T: type, bytes: []u8, offset: usize, value: T) void {
    std.mem.writeInt(T, bytes[offset..][0..@sizeOf(T)], value, .little);
}
fn read(comptime T: type, bytes: []const u8, offset: usize) T {
    return std.mem.readInt(T, bytes[offset..][0..@sizeOf(T)], .little);
}
/// in/out: aligned borrowed mapping[length], quiescent before peer connection.
/// Returns 0/-1, no allocation, initializer-thread only. Pixels untouched.
export fn av_layout_init(mapping: [*]align(64) u8, length: usize) c_int {
    if (length < MappingBytes) return -1;
    const bytes = mapping[0..65536];
    @memset(bytes, 0);
    write(u32, bytes, 0, 0x57415631);
    write(u32, bytes, 4, 1);
    write(u64, bytes, 8, MappingBytes);
    write(u32, bytes, 128 + 8, 48000);
    write(u32, bytes, 128 + 12, 2);
    write(u32, bytes, 128 + 16, 1);
    write(u32, bytes, 128 + 20, 4096);
    for (0..16) |pool| {
        for (0..3) |index| write(u32, bytes, 4096 + (pool * 3 + index) * 64 + 4, @intCast(index));
    }
    return 0;
}
/// in: aligned borrowed immutable mapping[length]; returns 0/-1; pure/thread-safe,
/// no allocation or mutation. Validates metadata before all offset calculations.
export fn av_layout_validate(mapping: [*]align(64) const u8, length: usize) c_int {
    if (length < MappingBytes) return -1;
    const bytes = mapping[0..64];
    return if (read(u32, bytes, 0) == 0x57415631 and read(u32, bytes, 4) == 1 and read(u64, bytes, 8) == MappingBytes) 0 else -1;
}
/// in/out: aligned borrowed validated mapping[length]; in: pool/index. Returns
/// borrowed aligned 64-byte header or null; mapping outlives use; no allocation.
export fn av_layout_slot(mapping: [*]align(64) u8, length: usize, pool: u32, index: u32) ?*align(64) anyopaque {
    if (length < MappingBytes or pool >= 16 or index >= 3) return null;
    return @ptrCast(@alignCast(mapping + 4096 + (@as(usize, pool) * 3 + index) * 64));
}
/// in: pool/index bounds; returns a page-aligned pixel offset or zero on invalid
/// index. Pure/thread-safe, no ownership or allocation; validate mapping first.
export fn av_layout_pixels(pool: u32, index: u32) u64 {
    if (pool >= 16 or index >= 3) return 0;
    return 65536 + (@as(u64, pool) * 3 + index) * 33554432;
}
test "mapping bounds and immutable ABI validation" {
    // Sparse logical length; these functions only access 64 KiB of metadata.
    const bytes = try std.testing.allocator.alignedAlloc(u8, 64, 65536);
    defer std.testing.allocator.free(bytes);
    try std.testing.expectEqual(@as(c_int, -1), av_layout_init(bytes.ptr, MappingBytes - 1));
    try std.testing.expectEqual(@as(c_int, 0), av_layout_init(bytes.ptr, MappingBytes));
    try std.testing.expectEqual(@as(c_int, 0), av_layout_validate(bytes.ptr, MappingBytes));
    try std.testing.expectEqual(@as(c_int, -1), av_layout_validate(bytes.ptr, MappingBytes - 1));
    for ([_]usize{ 0, 4, 8 }) |offset| {
        bytes[offset] ^= 1;
        try std.testing.expectEqual(@as(c_int, -1), av_layout_validate(bytes.ptr, MappingBytes));
        bytes[offset] ^= 1;
    }
    try std.testing.expect(av_layout_slot(bytes.ptr, MappingBytes, 15, 2) != null);
    try std.testing.expect(av_layout_slot(bytes.ptr, MappingBytes, 16, 2) == null);
    try std.testing.expect(av_layout_slot(bytes.ptr, MappingBytes, 15, 3) == null);
    try std.testing.expect(av_layout_slot(bytes.ptr, MappingBytes - 1, 0, 0) == null);
    try std.testing.expectEqual(@as(u64, 0), av_layout_pixels(16, 0));
    try std.testing.expectEqual(@as(u64, 0), av_layout_pixels(0, 3));
    try std.testing.expectEqual(@as(u64, MappingBytes), av_layout_pixels(15, 2) + 33554432);
}
