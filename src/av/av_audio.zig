const std = @import("std");

/// C ABI metadata; borrowed caller-owned aligned mapping. Single producer owns
/// write_head/overrun_frames, single consumer owns read_head; rest immutable.
const audio_ring_header_t = extern struct {
    write_head: u32,
    read_head: u32,
    sample_rate: u32,
    channels: u32,
    format: u32,
    capacity_frames: u32,
    overrun_frames: u32,
    reserved: [36]u8,
};

fn valid_ring(ring: *align(64) audio_ring_header_t, capacity: u32, pcm_len: usize) bool {
    return capacity >= 2 and capacity <= 1048576 and std.math.isPowerOfTwo(capacity) and
        ring.sample_rate == 48000 and ring.channels == 2 and ring.format == 1 and
        pcm_len >= @as(usize, capacity) * 4;
}

/// in/out: nonnull borrowed ring and pcm[pcm_len]; in: nonoverlapping input[input_len].
/// Returns copied frames or -1 for invalid format/length/cursors. No allocation;
/// exactly one producer; mapping immutable and alive throughout call.
export fn av_audio_write(ring: *align(64) audio_ring_header_t, pcm: [*]u8, pcm_len: usize, input: [*]const u8, input_len: usize) i64 {
    const capacity = ring.capacity_frames;
    if (!valid_ring(ring, capacity, pcm_len) or input_len % 4 != 0 or input_len / 4 > std.math.maxInt(u32)) return -1;
    const write_head = @atomicLoad(u32, &ring.write_head, .acquire);
    const read_head = @atomicLoad(u32, &ring.read_head, .acquire);
    const used = write_head -% read_head;
    if (used > capacity) return -1;
    const requested: u32 = @intCast(input_len / 4);
    const count = @min(requested, capacity - used);
    const offset = (write_head & (capacity - 1)) * 4;
    const first = @min(count * 4, capacity * 4 - offset);
    @memcpy(pcm[0..pcm_len][offset..][0..first], input[0..input_len][0..first]);
    @memcpy(pcm[0..pcm_len][0 .. count * 4 - first], input[0..input_len][first .. count * 4]);
    @atomicStore(u32, &ring.write_head, write_head +% count, .release);
    const dropped = requested - count;
    const overruns = @atomicLoad(u32, &ring.overrun_frames, .acquire);
    @atomicStore(u32, &ring.overrun_frames, overruns +% dropped, .release);
    return count;
}

/// in/out: borrowed ring; in: pcm[pcm_len], max_backlog (0 disables trim);
/// out: nonoverlapping output[output_len]. Returns copied frames or -1; fills
/// underruns with zero. Single consumer only; no allocation or ownership transfer.
export fn av_audio_read(ring: *align(64) audio_ring_header_t, pcm: [*]const u8, pcm_len: usize, output: [*]u8, output_len: usize, max_backlog: u32) i64 {
    const capacity = ring.capacity_frames;
    if (!valid_ring(ring, capacity, pcm_len) or output_len % 4 != 0 or output_len / 4 > std.math.maxInt(u32)) return -1;
    const write_head = @atomicLoad(u32, &ring.write_head, .acquire);
    var read_head = @atomicLoad(u32, &ring.read_head, .acquire);
    var used = write_head -% read_head;
    if (used > capacity) return -1;
    if (max_backlog != 0 and used > max_backlog) {
        read_head +%= used - max_backlog;
        used = max_backlog;
    }
    const count = @min(output_len / 4, used);
    const offset = (read_head & (capacity - 1)) * 4;
    const first = @min(count * 4, capacity * 4 - offset);
    @memcpy(output[0..output_len][0..first], pcm[0..pcm_len][offset..][0..first]);
    @memcpy(output[0..output_len][first .. count * 4], pcm[0..pcm_len][0 .. count * 4 - first]);
    @memset(output[count * 4 .. output_len], 0);
    @atomicStore(u32, &ring.read_head, read_head +% @as(u32, @intCast(count)), .release);
    return @intCast(count);
}

/// in: bounded pixel dimensions/stride/capacity; returns required bytes or 0 on
/// invalid bounds. Pure/thread-safe, no allocation or ownership transfer.
export fn av_video_size(width: u32, height: u32, stride: u32, capacity: u64) u64 {
    if (width == 0 or height == 0 or @as(u64, width) * 4 > stride) return 0;
    const size = @as(u64, height) * stride;
    return if (size > capacity) 0 else size;
}

test "bounded audio wrap, overflow, silence and corrupt metadata" {
    var ring: audio_ring_header_t align(64) = .{ .write_head = 0xfffffffe, .read_head = 0xfffffffe, .sample_rate = 48000, .channels = 2, .format = 1, .capacity_frames = 4, .overrun_frames = 0, .reserved = .{0} ** 36 };
    const pcm = try std.testing.allocator.alloc(u8, 16);
    defer std.testing.allocator.free(pcm);
    const input = [_]u8{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20 };
    var output: [24]u8 = undefined;
    try std.testing.expectEqual(@as(i64, 4), @call(.never_inline, av_audio_write, .{ &ring, pcm.ptr, pcm.len, &input, input.len }));
    try std.testing.expectEqual(@as(u32, 1), ring.overrun_frames);
    try std.testing.expectEqual(@as(i64, 0), @call(.never_inline, av_audio_write, .{ &ring, pcm.ptr, pcm.len, &input, 4 }));
    try std.testing.expectEqual(@as(i64, 4), @call(.never_inline, av_audio_read, .{ &ring, pcm.ptr, pcm.len, &output, output.len, 0 }));
    try std.testing.expectEqualSlices(u8, input[0..16], output[0..16]);
    try std.testing.expectEqualSlices(u8, &(.{0} ** 8), output[16..]);
    try std.testing.expectEqual(@as(i64, 4), @call(.never_inline, av_audio_write, .{ &ring, pcm.ptr, pcm.len, &input, 16 }));
    try std.testing.expectEqual(@as(i64, 2), @call(.never_inline, av_audio_read, .{ &ring, pcm.ptr, pcm.len, &output, 8, 2 }));
    try std.testing.expectEqualSlices(u8, input[8..16], output[0..8]);
    try std.testing.expectEqual(@as(i64, -1), @call(.never_inline, av_audio_write, .{ &ring, pcm.ptr, 15, &input, 4 }));
    try std.testing.expectEqual(@as(i64, -1), @call(.never_inline, av_audio_read, .{ &ring, pcm.ptr, 15, &output, 8, 0 }));
    try std.testing.expectEqual(@as(i64, -1), @call(.never_inline, av_audio_write, .{ &ring, pcm.ptr, 16, &input, 3 }));
    try std.testing.expectEqual(@as(i64, -1), @call(.never_inline, av_audio_read, .{ &ring, pcm.ptr, 16, &output, 3, 0 }));
    ring.write_head = ring.read_head +% 5;
    try std.testing.expectEqual(@as(i64, -1), @call(.never_inline, av_audio_write, .{ &ring, pcm.ptr, 16, &input, 4 }));
    try std.testing.expectEqual(@as(i64, -1), @call(.never_inline, av_audio_read, .{ &ring, pcm.ptr, 16, &output, 4, 0 }));
    ring.write_head = ring.read_head;
    for ([_]u32{ 0, 1, 3, 1048577 }) |capacity| {
        ring.capacity_frames = capacity;
        try std.testing.expectEqual(@as(i64, -1), @call(.never_inline, av_audio_write, .{ &ring, pcm.ptr, 16, &input, 4 }));
    }
    ring.capacity_frames = 4;
    ring.sample_rate = 44100;
    try std.testing.expectEqual(@as(i64, -1), @call(.never_inline, av_audio_write, .{ &ring, pcm.ptr, 16, &input, 4 }));
    ring.sample_rate = 48000;
    ring.channels = 1;
    try std.testing.expectEqual(@as(i64, -1), @call(.never_inline, av_audio_write, .{ &ring, pcm.ptr, 16, &input, 4 }));
    ring.channels = 2;
    ring.format = 2;
    try std.testing.expectEqual(@as(i64, -1), @call(.never_inline, av_audio_write, .{ &ring, pcm.ptr, 16, &input, 4 }));
}

test "pixel validation prevents overflow and zero dimensions" {
    try std.testing.expectEqual(@as(u64, 16), @call(.never_inline, av_video_size, .{ 2, 2, 8, 16 }));
    for ([_][4]u64{ .{ 0, 1, 4, 16 }, .{ 1, 0, 4, 16 }, .{ 2, 2, 7, 16 }, .{ 2, 2, 8, 15 }, .{ 0xffffffff, 2, 0xffffffff, 0xffffffffffffffff } }) |args| {
        try std.testing.expectEqual(@as(u64, 0), @call(.never_inline, av_video_size, .{ @as(u32, @intCast(args[0])), @as(u32, @intCast(args[1])), @as(u32, @intCast(args[2])), args[3] }));
    }
}

/// in: borrowed source[source_len], stride and dimensions; out: nonoverlapping
/// output[output_len]. Returns 0/-1, leaves output unchanged on error. Pure,
/// thread-safe and allocation-free; caller retains all memory.
export fn av_copy_bgra(source: [*]const u8, source_len: usize, source_stride: u32, output: [*]u8, output_len: usize, width: u32, height: u32) c_int {
    if (av_video_size(width, height, source_stride, source_len) == 0 or @as(u64, width) * 4 > std.math.maxInt(u32)) return -1;
    const stride = width * 4;
    if (av_video_size(width, height, stride, output_len) == 0) return -1;
    for (0..height) |row| {
        @memcpy(output[row * stride ..][0..stride], source[row * source_stride ..][0..stride]);
    }
    return 0;
}
test "padded BGRA rows copy without touching destination tail" {
    const source = [_]u8{ 1, 2, 3, 4, 99, 99, 99, 99, 5, 6, 7, 8, 99, 99, 99, 99 };
    var output = [_]u8{0xaa} ** 12;
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, av_copy_bgra, .{ &source, source.len, 8, &output, output.len, 1, 2 }));
    try std.testing.expectEqualSlices(u8, &.{ 1, 2, 3, 4, 5, 6, 7, 8 }, output[0..8]);
    try std.testing.expectEqual(@as(u8, 0xaa), output[8]);
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_copy_bgra, .{ &source, 15, 8, &output, 12, 1, 2 }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, av_copy_bgra, .{ &source, 16, 8, &output, 7, 1, 2 }));
}
