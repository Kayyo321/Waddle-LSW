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
/// validated extent at most one GiB, proposed offset/length. Input is synchronized
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

/// in: untrusted host ID; returns one-based slot 1..64 or zero if outside 2..65.
/// Pure/thread-safe, no allocation, validates before any C ledger index access.
pub export fn venus_receiver_resource_slot(id: u32) u32 {
    return if (id >= 2 and id <= 65) id - 1 else 0;
}

/// in: proposed ID/blob/bytes/flags; returns one when bounded (2..65 ID,
/// page-aligned nonzero bytes <=one GiB, known flags, CrossDevice requires Share,
/// blob zero exactly Map); otherwise zero. Pure/thread-safe, allocation-free.
pub export fn venus_receiver_resource_request(id: u32, blob: u64, bytes: u64, flags: u32) c_int {
    if (venus_receiver_resource_slot(id) == 0 or bytes == 0 or bytes > 1073741824 or bytes & 4095 != 0) return 0;
    if (flags > 7 or (flags & 4 != 0 and flags & 2 == 0) or (blob == 0 and flags != 1)) return 0;
    return 1;
}

/// in: actual extent and proposed offset/length; returns one for nonempty bounded
/// range, zero otherwise. Pure/thread-safe/no allocation; subtraction avoids overflow.
export fn venus_receiver_resource_range(extent: u64, offset: u64, length: usize) c_int {
    return if (offset <= extent and length != 0 and length <= extent - offset) 1 else 0;
}

/// out: nullable actual memory[extent]; in: nullable immutable disjoint input[length],
/// validated extent <=one GiB and proposed offset/length. Returns 0 success, -1
/// null/invalid bounds without modifying memory. Allocation-free/thread-safe on
/// disjoint buffers; caller guarantees quiescent CPU resource mapping.
export fn venus_receiver_memory_write(memory: ?[*]u8, extent: u64, offset: u64, input: ?[*]const u8, length: usize) c_int {
    const destination = memory orelse return -1;
    const source = input orelse return -1;
    if (venus_receiver_resource_range(extent, offset, length) == 0) return -1;
    @memcpy(destination[@intCast(offset)..][0..length], source[0..length]);
    return 0;
}

test "resource policy validates ID, page extent, flag and copy boundaries" {
    for ([_]u32{ 0, 1, 66, 0xffffffff }) |id| {
        try std.testing.expectEqual(@as(u32, 0), @call(.never_inline, venus_receiver_resource_slot, .{id}));
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_receiver_resource_request, .{ id, @as(u64, 0), @as(u64, 4096), @as(u32, 1) }));
    }
    for (2..66) |id| try std.testing.expectEqual(@as(u32, @intCast(id - 1)), @call(.never_inline, venus_receiver_resource_slot, .{@as(u32, @intCast(id))}));
    for ([_]u64{ 0, 1, 4095, 4097, 1073741825, std.math.maxInt(u64) }) |bytes|
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_receiver_resource_request, .{ @as(u32, 2), @as(u64, 0), bytes, @as(u32, 1) }));
    for (0..10) |flags| {
        const valid_cpu = flags == 1;
        const valid_device = flags <= 7 and flags != 4 and flags != 5;
        try std.testing.expectEqual(@as(c_int, if (valid_cpu) 1 else 0), @call(.never_inline, venus_receiver_resource_request, .{ @as(u32, 2), @as(u64, 0), @as(u64, 4096), @as(u32, @intCast(flags)) }));
        try std.testing.expectEqual(@as(c_int, if (valid_device) 1 else 0), @call(.never_inline, venus_receiver_resource_request, .{ @as(u32, 65), @as(u64, 1), @as(u64, 1073741824), @as(u32, @intCast(flags)) }));
    }
    const bytes = try std.testing.allocator.alloc(u8, 8);
    defer std.testing.allocator.free(bytes);
    const Source = [_]u8{ 1, 2, 3, 4 };
    @memset(bytes, 0x5a);
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_receiver_memory_write, .{ null, @as(u64, 8), @as(u64, 0), &Source, @as(usize, 4) }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_receiver_memory_write, .{ bytes.ptr, @as(u64, 8), @as(u64, 0), null, @as(usize, 4) }));
    for ([_]u64{ 0, 5, 8, 9, std.math.maxInt(u64) }) |offset| {
        for ([_]usize{ 0, 4, 9, std.math.maxInt(usize) }) |length| {
            if (offset == 0 and length == 4) continue;
            try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_receiver_memory_write, .{ bytes.ptr, @as(u64, 8), offset, &Source, length }));
        }
    }
    for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0x5a), byte);
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_receiver_memory_write, .{ bytes.ptr, @as(u64, 8), @as(u64, 4), &Source, @as(usize, 4) }));
    try std.testing.expectEqualSlices(u8, &Source, bytes[4..8]);
}
