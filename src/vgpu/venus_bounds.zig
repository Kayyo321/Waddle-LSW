const std = @import("std");
const HeaderBytes: usize = 192;
const MinCapacity: u32 = 64;
const MaxCapacity: u32 = 16777216;

fn read_u32(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}

/// in: nonnull borrowed mapping[length], immutable metadata during attachment.
/// Returns capacity or zero for invalid extent/ABI/padding; no allocation,
/// mutation, or alignment requirement. Thread-safe for immutable metadata.
export fn venus_bounds_capacity(mapping: [*]const u8, length: usize) u32 {
    if (length < HeaderBytes) return 0;
    const bytes = mapping[0..HeaderBytes];
    if (read_u32(bytes, 0) != 0x57565231 or read_u32(bytes, 4) != 1 or
        read_u32(bytes, 12) != HeaderBytes) return 0;
    const capacity = read_u32(bytes, 8);
    if (capacity < MinCapacity or capacity > MaxCapacity or
        capacity & (capacity - 1) != 0 or capacity > length - HeaderBytes) return 0;
    for (bytes[20..64]) |byte| if (byte != 0) return 0;
    for (bytes[68..128]) |byte| if (byte != 0) return 0;
    for (bytes[132..192]) |byte| if (byte != 0) return 0;
    return capacity;
}

/// out: nonnull borrowed payload[capacity]; in: validated capacity, cursor,
/// nonoverlapping source[length], 1..capacity length in exclusively free space.
/// Producer-only, allocation-free; publishes no state, returns no errors.
export fn venus_bounds_write(payload: [*]u8, capacity: u32, cursor: u32, source: [*]const u8, length: usize) void {
    const bytes = payload[0..capacity];
    const offset: usize = cursor & (capacity - 1);
    const first = @min(length, bytes.len - offset);
    @memcpy(bytes[offset..][0..first], source[0..first]);
    @memcpy(bytes[0 .. length - first], source[first..length]);
}

/// in: nonnull borrowed published payload[capacity], validated capacity, cursor,
/// length in 1..capacity; out: nonoverlapping borrowed destination[length].
/// Consumer-only, allocation-free; releases no state, returns no errors.
export fn venus_bounds_read(payload: [*]const u8, capacity: u32, cursor: u32, destination: [*]u8, length: usize) void {
    const bytes = payload[0..capacity];
    const offset: usize = cursor & (capacity - 1);
    const first = @min(length, bytes.len - offset);
    @memcpy(destination[0..first], bytes[offset..][0..first]);
    @memcpy(destination[first..length], bytes[0 .. length - first]);
}

test "bounds reject truncated headers and wrapped spans preserve byte order" {
    const bytes = try std.testing.allocator.alloc(u8, HeaderBytes + MinCapacity);
    defer std.testing.allocator.free(bytes);
    @memset(bytes, 0);
    try std.testing.expectEqual(@as(u32, 0), @call(.never_inline, venus_bounds_capacity, .{ bytes.ptr, HeaderBytes - 1 }));
    std.mem.writeInt(u32, bytes[0..4], 0x57565231, .little);
    std.mem.writeInt(u32, bytes[4..8], 1, .little);
    std.mem.writeInt(u32, bytes[8..12], MinCapacity, .little);
    std.mem.writeInt(u32, bytes[12..16], HeaderBytes, .little);
    try std.testing.expectEqual(MinCapacity, venus_bounds_capacity(bytes.ptr, bytes.len));
    const Source = [_]u8{ 1, 2, 3, 4, 5, 6, 7, 8 };
    var result: [8]u8 = undefined;
    venus_bounds_write(bytes.ptr + HeaderBytes, MinCapacity, 60, &Source, Source.len);
    venus_bounds_read(bytes.ptr + HeaderBytes, MinCapacity, 60, &result, result.len);
    try std.testing.expectEqualSlices(u8, &Source, &result);
}

test "each metadata field and reserved byte rejects corruption" {
    const bytes = try std.testing.allocator.alloc(u8, HeaderBytes + MinCapacity);
    defer std.testing.allocator.free(bytes);
    @memset(bytes, 0);
    std.mem.writeInt(u32, bytes[0..4], 0x57565231, .little);
    std.mem.writeInt(u32, bytes[4..8], 1, .little);
    std.mem.writeInt(u32, bytes[8..12], MinCapacity, .little);
    std.mem.writeInt(u32, bytes[12..16], HeaderBytes, .little);
    for (0..HeaderBytes) |offset| {
        if ((offset >= 16 and offset < 20) or (offset >= 64 and offset < 68) or
            (offset >= 128 and offset < 132)) continue;
        bytes[offset] ^= 0x80;
        try std.testing.expectEqual(@as(u32, 0), @call(.never_inline, venus_bounds_capacity, .{ bytes.ptr, bytes.len }));
        bytes[offset] ^= 0x80;
    }
    for ([_]u32{ 0, 32, 65, MaxCapacity + 1, 0xffffffff }) |capacity| {
        std.mem.writeInt(u32, bytes[8..12], capacity, .little);
        try std.testing.expectEqual(@as(u32, 0), @call(.never_inline, venus_bounds_capacity, .{ bytes.ptr, bytes.len }));
    }
    std.mem.writeInt(u32, bytes[8..12], MinCapacity, .little);
    try std.testing.expectEqual(@as(u32, 0), @call(.never_inline, venus_bounds_capacity, .{ bytes.ptr, bytes.len - 1 }));
    try std.testing.expectEqual(MinCapacity, @call(.never_inline, venus_bounds_capacity, .{ bytes.ptr, bytes.len }));
}

test "bounded copy handles every start and length and maximum capacity" {
    const bytes = try std.testing.allocator.alloc(u8, HeaderBytes + MaxCapacity);
    defer std.testing.allocator.free(bytes);
    @memset(bytes[0..HeaderBytes], 0);
    std.mem.writeInt(u32, bytes[0..4], 0x57565231, .little);
    std.mem.writeInt(u32, bytes[4..8], 1, .little);
    std.mem.writeInt(u32, bytes[8..12], MaxCapacity, .little);
    std.mem.writeInt(u32, bytes[12..16], HeaderBytes, .little);
    try std.testing.expectEqual(MaxCapacity, @call(.never_inline, venus_bounds_capacity, .{ bytes.ptr, bytes.len }));
    var source: [64]u8 = undefined;
    var result: [64]u8 = undefined;
    for (&source, 0..) |*byte, index| byte.* = @intCast(index);
    for (0..64) |offset| {
        for (1..65) |length| {
            @call(.never_inline, venus_bounds_write, .{ bytes.ptr + HeaderBytes, MinCapacity, @as(u32, @intCast(offset)), &source, length });
            @call(.never_inline, venus_bounds_read, .{ bytes.ptr + HeaderBytes, MinCapacity, @as(u32, @intCast(offset)), &result, length });
            try std.testing.expectEqualSlices(u8, source[0..length], result[0..length]);
        }
    }
}
