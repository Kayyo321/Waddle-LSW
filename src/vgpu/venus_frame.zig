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

const ReleaseBytes: usize = 32;
const ReleaseMagic: u32 = 0x57565231;
const release_t = c.venus_release_t;
fn valid_release(release: *const release_t) bool {
    return release.context != 0 and release.frame != 0 and
        (release.status == c.RingOk or release.status == c.RingInvalid or
        release.status == c.RingCancelled or release.status == c.RingClosed);
}
/// in: nullable immutable private record; out: nullable disjoint bytes[length].
/// Returns 0 success/-1 local error, preserving bytes on failure. No allocation,
/// ownership or retained pointers; thread-safe for disjoint private outputs.
export fn venus_release_encode(input: ?*const release_t, output: ?[*]u8, length: usize) c_int {
    const release = input orelse return -1;
    const destination = output orelse return -1;
    if (length != ReleaseBytes or !valid_release(release)) return -1;
    const bytes = destination[0..ReleaseBytes];
    @memset(bytes, 0);
    put(u32, bytes, 0, ReleaseMagic);
    put(u32, bytes, 4, 1);
    put(u64, bytes, 8, release.context);
    put(u64, bytes, 16, release.frame);
    put(i32, bytes, 24, release.status);
    return 0;
}
/// out: nullable private record zeroed on failure; in: nullable bytes[length].
/// Returns 0 success/-1 NULL/-2 wire error. Allocation-free, thread-safe, no
/// retained pointers. Sender credentials and live lease matching are separate.
export fn venus_release_decode(output: ?*release_t, input: ?[*]const u8, length: usize) c_int {
    const release = output orelse return -1;
    release.* = std.mem.zeroes(release_t);
    const source = input orelse return -1;
    if (length != ReleaseBytes) return -2;
    const bytes = source[0..ReleaseBytes];
    if (get(u32, bytes, 0) != ReleaseMagic or get(u32, bytes, 4) != 1 or
        get(u32, bytes, 28) != 0) return -2;
    const value = release_t{
        .context = get(u64, bytes, 8),
        .frame = get(u64, bytes, 16),
        .status = get(i32, bytes, 24),
    };
    if (!valid_release(&value)) return -2;
    release.* = value;
    return 0;
}
fn expect_release_encode(
    status: c_int,
    release: ?*const release_t,
    bytes: ?[*]u8,
    length: usize,
) !void {
    const result = @call(.never_inline, venus_release_encode, .{ release, bytes, length });
    try std.testing.expectEqual(status, result);
}
fn expect_release(status: c_int, release: ?*release_t, bytes: ?[*]const u8, length: usize) !void {
    const result = @call(.never_inline, venus_release_decode, .{ release, bytes, length });
    try std.testing.expectEqual(status, result);
    if (status != 0) if (release) |value| {
        try std.testing.expectEqual(@as(u64, 0), value.context);
        try std.testing.expectEqual(@as(u64, 0), value.frame);
        try std.testing.expectEqual(@as(c_int, 0), value.status);
    };
}
test "release exact ABI, all identities/statuses, padding, NULL and bounds" {
    const bytes = try std.testing.allocator.alloc(u8, ReleaseBytes);
    defer std.testing.allocator.free(bytes);
    var release = release_t{ .context = 17, .frame = std.math.maxInt(u64), .status = c.RingOk };
    var decoded: release_t = undefined;
    try expect_release_encode(-1, null, bytes.ptr, bytes.len);
    try expect_release_encode(-1, &release, null, bytes.len);
    try expect_release(-1, null, bytes.ptr, bytes.len);
    try expect_release(-1, &decoded, null, bytes.len);
    for ([_]usize{ 0, 31, 33, std.math.maxInt(usize) }) |length| {
        @memset(bytes, 0x5a);
        try expect_release_encode(-1, &release, bytes.ptr, length);
        for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0x5a), byte);
        try expect_release(-2, &decoded, bytes.ptr, length);
    }
    for ([_]c_int{ c.RingOk, c.RingInvalid, c.RingCancelled, c.RingClosed }) |status| {
        release.status = status;
        try expect_release_encode(0, &release, bytes.ptr, bytes.len);
        try expect_release(0, &decoded, bytes.ptr, bytes.len);
        try std.testing.expectEqual(release.context, decoded.context);
        try std.testing.expectEqual(release.frame, decoded.frame);
        try std.testing.expectEqual(release.status, decoded.status);
    }
    for ([_]usize{ 0, 4, 28, 29, 30, 31 }) |offset| {
        bytes[offset] ^= 0x80;
        try expect_release(-2, &decoded, bytes.ptr, bytes.len);
        bytes[offset] ^= 0x80;
    }
    const InvalidStatuses = [_]c_int{
        c.RingAgain, c.RingCorrupt,          c.RingLimit,            c.RingTimeout,
        1,           std.math.minInt(c_int), std.math.maxInt(c_int),
    };
    for (InvalidStatuses) |status| {
        release.status = status;
        @memset(bytes, 0x5a);
        try expect_release_encode(-1, &release, bytes.ptr, bytes.len);
        for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0x5a), byte);
        release.status = c.RingOk;
        try expect_release_encode(0, &release, bytes.ptr, bytes.len);
        put(i32, bytes, 24, status);
        try expect_release(-2, &decoded, bytes.ptr, bytes.len);
    }
    for ([_]usize{ 8, 16 }) |offset| {
        release.context = if (offset == 8) 0 else 17;
        release.frame = if (offset == 16) 0 else 1;
        try expect_release_encode(-1, &release, bytes.ptr, bytes.len);
        release.context = 17;
        release.frame = 1;
        try expect_release_encode(0, &release, bytes.ptr, bytes.len);
        put(u64, bytes, offset, 0);
        try expect_release(-2, &decoded, bytes.ptr, bytes.len);
    }
}

/// out: nullable disjoint private identity zeroed on failure; in: immutable
/// nullable bytes[length], exactly16 lowercase hex characters. Returns0 success,
/// -1 NULL/shape/zero. Allocation-free/thread-safe, no retained pointer/ownership.
export fn venus_frame_context_decode(output: ?*u64, input: ?[*]const u8, length: usize) c_int {
    const identity = output orelse return -1;
    identity.* = 0;
    const bytes = input orelse return -1;
    if (length != 16) return -1;
    var value: u64 = 0;
    for (bytes[0..16]) |byte| {
        const digit: u64 = switch (byte) {
            '0'...'9' => byte - '0',
            'a'...'f' => byte - 'a' + 10,
            else => return -1,
        };
        value = (value << 4) | digit;
    }
    if (value == 0) return -1;
    identity.* = value;
    return 0;
}
fn expect_context(status: c_int, identity: ?*u64, bytes: ?[*]const u8, length: usize) !void {
    const result = @call(.never_inline, venus_frame_context_decode, .{ identity, bytes, length });
    try std.testing.expectEqual(status, result);
    if (status != 0) if (identity) |output| try std.testing.expectEqual(@as(u64, 0), output.*);
}
test "fixed context argument rejects all malformed and zero identities" {
    const bytes = try std.testing.allocator.alloc(u8, 16);
    defer std.testing.allocator.free(bytes);
    var identity: u64 = 9;
    @memcpy(bytes, "0123456789abcdef");
    try expect_context(-1, null, bytes.ptr, 16);
    try expect_context(-1, &identity, null, 16);
    for ([_]usize{ 0, 15, 17, std.math.maxInt(usize) }) |length|
        try expect_context(-1, &identity, bytes.ptr, length);
    try expect_context(0, &identity, bytes.ptr, 16);
    try std.testing.expectEqual(@as(u64, 0x0123456789abcdef), identity);
    @memset(bytes, 'f');
    try expect_context(0, &identity, bytes.ptr, 16);
    try std.testing.expectEqual(std.math.maxInt(u64), identity);
    @memset(bytes, '0');
    try expect_context(-1, &identity, bytes.ptr, 16);
    for ([_]u8{ 'A', 'F', '/', ':', '`', 'g', 'x', '+', '-', ' ', 0, 255 }) |byte| {
        for (0..16) |index| {
            bytes[index] = byte;
            try expect_context(-1, &identity, bytes.ptr, 16);
            bytes[index] = '0';
        }
    }
    bytes[15] = '1';
    try expect_context(0, &identity, bytes.ptr, 16);
    try std.testing.expectEqual(@as(u64, 1), identity);
}
