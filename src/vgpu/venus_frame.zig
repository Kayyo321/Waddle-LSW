const std = @import("std");
const c = @cImport({
    @cInclude("waddle/venus_frame.h");
});
const dmabuf = @import("venus_dmabuf.zig");
const FrameBytes: usize = 1216;
const FrameMagic: u32 = 0x57564431;
const frame_t = c.venus_frame_t;

fn valid_frame(frame: *const frame_t) bool {
    if (frame.context == 0 or frame.frame == 0 or frame.damage_count == 0 or frame.damage_count > 64) return false;
    if (dmabuf.venus_dmabuf_layout_validate(@ptrCast(&frame.layout)) != 0 or
        dmabuf.venus_dmabuf_damage_validate(frame.layout.width, frame.layout.height, @ptrCast(&frame.damage), frame.damage_count) != 0) return false;
    for (frame.resource_ids, 0..) |id, index| {
        if (index < frame.layout.plane_count) {
            if (id < 2 or id > 65) return false;
        } else if (id != 0) return false;
    }
    for (frame.damage[frame.damage_count..]) |damage|
        if (damage.x != 0 or damage.y != 0 or damage.width != 0 or damage.height != 0) return false;
    return true;
}
fn put(comptime integer_t: type, bytes: []u8, offset: usize, value: integer_t) void {
    std.mem.writeInt(integer_t, bytes[offset..][0..@sizeOf(integer_t)], value, .little);
}
fn get(comptime integer_t: type, bytes: []const u8, offset: usize) integer_t {
    return std.mem.readInt(integer_t, bytes[offset..][0..@sizeOf(integer_t)], .little);
}
/// in: nullable immutable private frame; out: nullable disjoint bytes[length].
/// Returns 0 or -1 local metadata/length/NULL. Preserves encoded output on failure.
/// Pure/thread-safe/allocation-free; all fixed offsets follow venus_frame.h ABI.
export fn venus_frame_encode(input: ?*const frame_t, output: ?[*]u8, length: usize) c_int {
    const frame = input orelse return -1;
    const destination = output orelse return -1;
    if (length != FrameBytes or !valid_frame(frame)) return -1;
    const bytes = destination[0..FrameBytes];
    @memset(bytes, 0);
    put(u32, bytes, 0, FrameMagic);
    put(u32, bytes, 4, 1);
    put(u64, bytes, 8, frame.context);
    put(u64, bytes, 16, frame.frame);
    put(u32, bytes, 24, frame.layout.plane_count);
    put(u32, bytes, 28, frame.damage_count);
    put(u32, bytes, 32, frame.layout.width);
    put(u32, bytes, 36, frame.layout.height);
    put(u32, bytes, 40, frame.layout.fourcc);
    put(u64, bytes, 48, frame.layout.modifier);
    for (frame.layout.planes, 0..) |plane, index| {
        const offset = 56 + index * 32;
        put(u32, bytes, offset, frame.resource_ids[index]);
        put(u32, bytes, offset + 4, plane.offset);
        put(u32, bytes, offset + 8, plane.stride);
        put(u64, bytes, offset + 16, plane.size);
        put(u64, bytes, offset + 24, plane.extent);
    }
    for (frame.damage, 0..) |damage, index| {
        const offset = 192 + index * 16;
        put(i32, bytes, offset, damage.x);
        put(i32, bytes, offset + 4, damage.y);
        put(i32, bytes, offset + 8, damage.width);
        put(i32, bytes, offset + 12, damage.height);
    }
    return 0;
}
/// out: nullable private frame zeroed on failure; in: nullable immutable bytes[length].
/// Returns 0, -1 NULL, -2 wire/metadata errors. Pure/thread-safe/allocation-free.
export fn venus_frame_decode(output: ?*frame_t, input: ?[*]const u8, length: usize) c_int {
    const frame = output orelse return -1;
    frame.* = std.mem.zeroes(frame_t);
    const source = input orelse return -1;
    if (length != FrameBytes) return -2;
    const bytes = source[0..FrameBytes];
    if (get(u32, bytes, 0) != FrameMagic or get(u32, bytes, 4) != 1 or get(u32, bytes, 44) != 0 or get(u64, bytes, 184) != 0) return -2;
    var value = std.mem.zeroes(frame_t);
    value.context = get(u64, bytes, 8);
    value.frame = get(u64, bytes, 16);
    value.layout.plane_count = get(u32, bytes, 24);
    value.damage_count = get(u32, bytes, 28);
    value.layout.width = get(u32, bytes, 32);
    value.layout.height = get(u32, bytes, 36);
    value.layout.fourcc = get(u32, bytes, 40);
    value.layout.modifier = get(u64, bytes, 48);
    for (&value.layout.planes, 0..) |*plane, index| {
        const offset = 56 + index * 32;
        if (get(u32, bytes, offset + 12) != 0) return -2;
        value.resource_ids[index] = get(u32, bytes, offset);
        plane.offset = get(u32, bytes, offset + 4);
        plane.stride = get(u32, bytes, offset + 8);
        plane.size = get(u64, bytes, offset + 16);
        plane.extent = get(u64, bytes, offset + 24);
    }
    for (&value.damage, 0..) |*damage, index| {
        const offset = 192 + index * 16;
        damage.x = get(i32, bytes, offset);
        damage.y = get(i32, bytes, offset + 4);
        damage.width = get(i32, bytes, offset + 8);
        damage.height = get(i32, bytes, offset + 12);
    }
    if (!valid_frame(&value)) return -2;
    frame.* = value;
    return 0;
}
fn example_frame() frame_t {
    var value = std.mem.zeroes(frame_t);
    value.context = 1;
    value.frame = 2;
    value.layout.width = 32;
    value.layout.height = 16;
    value.layout.fourcc = 0x34325241;
    value.layout.plane_count = 1;
    value.layout.planes[0] = .{ .offset = 0, .stride = 128, .size = 2048, .extent = 4096 };
    value.resource_ids[0] = 2;
    value.damage_count = 1;
    value.damage[0] = .{ .x = 0, .y = 0, .width = 32, .height = 16 };
    return value;
}
test "frame exact wire roundtrip, NULL/length and all identity/padding errors" {
    const bytes = try std.testing.allocator.alloc(u8, FrameBytes);
    defer std.testing.allocator.free(bytes);
    const original = example_frame();
    var value: frame_t = undefined;
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_frame_encode, .{ null, bytes.ptr, bytes.len }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_frame_encode, .{ &original, null, bytes.len }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_frame_decode, .{ null, bytes.ptr, bytes.len }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_frame_decode, .{ &value, null, bytes.len }));
    for ([_]usize{ 0, 1215, 1217, std.math.maxInt(usize) }) |length| {
        @memset(bytes, 0x5a);
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_frame_encode, .{ &original, bytes.ptr, length }));
        for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0x5a), byte);
        try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_frame_decode, .{ &value, bytes.ptr, length }));
        try std.testing.expectEqual(@as(u64, 0), value.context);
    }
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_frame_encode, .{ &original, bytes.ptr, bytes.len }));
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_frame_decode, .{ &value, bytes.ptr, bytes.len }));
    try std.testing.expectEqual(original.context, value.context);
    try std.testing.expectEqual(original.frame, value.frame);
    try std.testing.expectEqual(@as(i32, 32), value.damage[0].width);
    for ([_]usize{ 0, 4, 44, 68, 100, 132, 164, 184, 191 }) |offset| {
        const saved = bytes[offset];
        bytes[offset] ^= 0x80;
        try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_frame_decode, .{ &value, bytes.ptr, bytes.len }));
        try std.testing.expectEqual(@as(u64, 0), value.context);
        bytes[offset] = saved;
    }
    for (0..17) |field| {
        var bad = original;
        switch (field) {
            0 => bad.context = 0,
            1 => bad.frame = 0,
            2 => bad.damage_count = 0,
            3 => bad.damage_count = 65,
            4 => bad.layout.width = 0,
            5 => bad.damage[0].width = -1,
            6 => bad.resource_ids[0] = 1,
            7 => bad.resource_ids[0] = 66,
            8 => bad.resource_ids[1] = 2,
            9 => bad.damage[1].x = 1,
            10 => bad.damage[1].y = 1,
            11 => bad.damage[1].width = 1,
            12 => bad.damage[1].height = 1,
            13 => bad.layout.plane_count = 5,
            14 => bad.layout.planes[1].extent = 1,
            15 => bad.layout.planes[0].offset = std.math.maxInt(u32),
            else => bad.layout.planes[0].stride = 0,
        }
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_frame_encode, .{ &bad, bytes.ptr, bytes.len }));
    }
    put(u64, bytes, 8, 0);
    try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_frame_decode, .{ &value, bytes.ptr, bytes.len }));
    var full = original;
    full.damage_count = 64;
    for (&full.damage) |*damage| damage.* = full.damage[0];
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_frame_encode, .{ &full, bytes.ptr, bytes.len }));
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_frame_decode, .{ &value, bytes.ptr, bytes.len }));
}
