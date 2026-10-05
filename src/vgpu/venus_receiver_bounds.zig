const std = @import("std");

/// in: proposed command capacity and reply extent; returns one for bounded
/// powers of two (64..16MiB commands, 4096..16MiB replies), zero otherwise.
/// Pure, thread-safe, allocation-free; validates before any pointer arithmetic.
export fn venus_receiver_limits(capacity: u32, reply_bytes: u64) c_int {
    if (capacity < 64 or capacity > 16777216 or capacity & (capacity - 1) != 0) return 0;
    if (reply_bytes < 4096 or reply_bytes > 16777216 or reply_bytes & (reply_bytes - 1) != 0) return 0;
    return 1;
}

/// out: nullable actual output[capacity]; in: nullable immutable input[length],
/// disjoint buffers and validated capacity. Returns 0 success, -1 null/invalid
/// length; errors preserve output. No allocation; thread-safe disjoint buffers.
export fn venus_receiver_command_copy(output: ?[*]u8, capacity: u32, input: ?[*]const u8, length: usize) c_int {
    const destination = output orelse return -1;
    const source = input orelse return -1;
    if (length < 8 or length > capacity or length & 3 != 0) return -1;
    @memcpy(destination[0..length], source[0..length]);
    return 0;
}

/// out: nullable private output[length]; in: nullable actual input[extent],
/// validated extent at most 16MiB, proposed offset/length. Input is synchronized
/// and disjoint from output. Returns 0 success, -1 null/invalid range; no mutation
/// on error. Pure/thread-safe disjoint buffers, allocation-free bounded slices.
export fn venus_receiver_reply_copy(output: ?[*]u8, input: ?[*]const u8, extent: u64, offset: u64, length: usize) c_int {
    const destination = output orelse return -1;
    const source = input orelse return -1;
    if (offset > extent or length == 0 or length > extent - offset) return -1;
    @memcpy(destination[0..length], source[@intCast(offset)..][0..length]);
    return 0;
}

test "receiver bounds validate every error before touching private output" {
    for ([_]u32{ 0, 32, 65, 16777217 }) |capacity|
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_receiver_limits, .{ capacity, @as(u64, 4096) }));
    for ([_]u64{ 0, 4095, 4097, 16777217 }) |extent|
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_receiver_limits, .{ @as(u32, 64), extent }));
    try std.testing.expectEqual(@as(c_int, 1), @call(.never_inline, venus_receiver_limits, .{ @as(u32, 64), @as(u64, 4096) }));
    try std.testing.expectEqual(@as(c_int, 1), @call(.never_inline, venus_receiver_limits, .{ @as(u32, 16777216), @as(u64, 16777216) }));
    const bytes = try std.testing.allocator.alloc(u8, 64);
    defer std.testing.allocator.free(bytes);
    const Source = [_]u8{ 1, 2, 3, 4, 5, 6, 7, 8 };
    @memset(bytes, 0x5a);
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_receiver_command_copy, .{ null, @as(u32, 64), &Source, @as(usize, 8) }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_receiver_command_copy, .{ bytes.ptr, @as(u32, 64), null, @as(usize, 8) }));
    for ([_]usize{ 0, 7, 9, 65, std.math.maxInt(usize) }) |length|
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_receiver_command_copy, .{ bytes.ptr, @as(u32, 64), &Source, length }));
    for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0x5a), byte);
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_receiver_command_copy, .{ bytes.ptr, @as(u32, 64), &Source, @as(usize, 8) }));
    try std.testing.expectEqualSlices(u8, &Source, bytes[0..8]);
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_receiver_reply_copy, .{ null, &Source, @as(u64, 8), @as(u64, 0), @as(usize, 8) }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_receiver_reply_copy, .{ bytes.ptr, null, @as(u64, 8), @as(u64, 0), @as(usize, 8) }));
    for ([_]u64{ 0, 8, 9, std.math.maxInt(u64) }) |offset| {
        @memset(bytes, 0x5a);
        for ([_]usize{ 0, 9, std.math.maxInt(usize) }) |length|
            try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_receiver_reply_copy, .{ bytes.ptr, &Source, @as(u64, 8), offset, length }));
        for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0x5a), byte);
    }
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_receiver_reply_copy, .{ bytes.ptr, &Source, @as(u64, 8), @as(u64, 4), @as(usize, 4) }));
    try std.testing.expectEqualSlices(u8, Source[4..8], bytes[0..4]);
}
