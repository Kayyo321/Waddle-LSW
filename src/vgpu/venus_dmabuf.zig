const std = @import("std");
const MaxTableBytes: usize = 65536;
const MaxIndexBytes: usize = 8192;
const Argb8888: u32 = 0x34325241;
const Xrgb8888: u32 = 0x34325258;
const Abgr8888: u32 = 0x34324241;
const Xbgr8888: u32 = 0x34324258;
const Nv12: u32 = 0x3231564e;
const plane_t = extern struct { offset: u32, stride: u32, size: u64, extent: u64 };
const layout_t = extern struct {
    width: u32,
    height: u32,
    fourcc: u32,
    plane_count: u32,
    modifier: u64,
    planes: [4]plane_t,
};
comptime {
    std.debug.assert(@sizeOf(plane_t) == 24 and @sizeOf(layout_t) == 120);
}

/// in: nullable immutable private layout; returns 0 valid, -1 invalid.
/// Pure, allocation-free, no ownership transfer; thread-safe on immutable data.
pub export fn venus_dmabuf_layout_validate(input: ?*const layout_t) c_int {
    const layout = input orelse return -1;
    if (layout.width == 0 or layout.width > 16384 or layout.height == 0 or layout.height > 16384) return -1;
    const planar = switch (layout.fourcc) {
        Argb8888, Xrgb8888, Abgr8888, Xbgr8888 => false,
        Nv12 => true,
        else => return -1,
    };
    if (layout.plane_count != @as(u32, if (planar) 2 else 1)) return -1;
    if (planar and (layout.width & 1 != 0 or layout.height & 1 != 0)) return -1;
    for (layout.planes, 0..) |plane, index| {
        if (index >= layout.plane_count) {
            if (plane.offset != 0 or plane.stride != 0 or plane.size != 0 or plane.extent != 0) return -1;
            continue;
        }
        if (plane.stride == 0 or plane.stride > std.math.maxInt(i32) or plane.size == 0 or plane.extent == 0 or plane.extent > 1073741824) return -1;
        if (plane.offset > plane.extent or plane.size > plane.extent - plane.offset) return -1;
        if (layout.modifier == 0) {
            const row_bytes: u64 = @as(u64, layout.width) * @as(u64, if (planar) 1 else 4);
            const rows: u64 = if (planar and index == 1) layout.height / 2 else layout.height;
            if (plane.stride < row_bytes or (rows - 1) * plane.stride + row_bytes > plane.size) return -1;
        }
    }
    return 0;
}

/// in: nullable immutable private table/tranche byte slices and requested pair;
/// returns 0 matched, 1 absent, -1 NULL, -2 invalid lengths/indices. Validates all
/// indices before success. Pure/thread-safe/allocation-free, no retained storage.
pub export fn venus_dmabuf_feedback_match(table_pointer: ?[*]const u8, table_bytes: usize, index_pointer: ?[*]const u8, index_bytes: usize, fourcc: u32, modifier: u64) c_int {
    const table_start = table_pointer orelse return -1;
    const index_start = index_pointer orelse return -1;
    if (table_bytes == 0 or table_bytes > MaxTableBytes or table_bytes % 16 != 0 or index_bytes == 0 or index_bytes > MaxIndexBytes or index_bytes % 2 != 0) return -2;
    const table = table_start[0..table_bytes];
    const indices = index_start[0..index_bytes];
    var matched = false;
    var offset: usize = 0;
    while (offset < indices.len) : (offset += 2) {
        const index = std.mem.readInt(u16, indices[offset..][0..2], .little);
        if (index >= table.len / 16) return -2;
        const entry = table[@as(usize, index) * 16 ..][0..16];
        if (std.mem.readInt(u32, entry[0..4], .little) == fourcc and std.mem.readInt(u64, entry[8..16], .little) == modifier) matched = true;
    }
    return if (matched) 0 else 1;
}

const damage_t = extern struct { x: i32, y: i32, width: i32, height: i32 };
/// in: image extent and nullable immutable damage[count]; returns 0 valid,
/// -1 null/invalid geometry. Pure/thread-safe/allocation-free, no retained data.
pub export fn venus_dmabuf_damage_validate(width: u32, height: u32, pointer: ?[*]const damage_t, count: usize) c_int {
    const damage = pointer orelse return -1;
    if (width == 0 or width > 16384 or height == 0 or height > 16384 or count == 0 or count > 64) return -1;
    for (damage[0..count]) |rectangle| {
        if (rectangle.x < 0 or rectangle.y < 0 or rectangle.width <= 0 or rectangle.height <= 0) return -1;
        if (@as(i64, rectangle.x) + rectangle.width > width or @as(i64, rectangle.y) + rectangle.height > height) return -1;
    }
    return 0;
}

test "damage bounds reject every invalid field before publication" {
    var damage = [_]damage_t{.{ .x = 0, .y = 0, .width = 32, .height = 16 }} ** 64;
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_dmabuf_damage_validate, .{ @as(u32, 32), @as(u32, 16), null, @as(usize, 1) }));
    for ([_]usize{ 0, 65, std.math.maxInt(usize) }) |count|
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_dmabuf_damage_validate, .{ @as(u32, 32), @as(u32, 16), &damage, count }));
    for ([_]u32{ 0, 16385 }) |size| {
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_dmabuf_damage_validate, .{ size, @as(u32, 16), &damage, @as(usize, 1) }));
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_dmabuf_damage_validate, .{ @as(u32, 32), size, &damage, @as(usize, 1) }));
    }
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_dmabuf_damage_validate, .{ @as(u32, 32), @as(u32, 16), &damage, @as(usize, 64) }));
    for (0..4) |field| {
        damage[0] = .{ .x = 0, .y = 0, .width = 32, .height = 16 };
        switch (field) {
            0 => damage[0].x = -1,
            1 => damage[0].y = -1,
            2 => damage[0].width = 0,
            else => damage[0].height = 0,
        }
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_dmabuf_damage_validate, .{ @as(u32, 32), @as(u32, 16), &damage, @as(usize, 1) }));
    }
    damage[0] = .{ .x = std.math.maxInt(i32), .y = 0, .width = std.math.maxInt(i32), .height = 1 };
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_dmabuf_damage_validate, .{ @as(u32, 32), @as(u32, 16), &damage, @as(usize, 1) }));
    damage[0] = .{ .x = 0, .y = 16, .width = 1, .height = 1 };
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_dmabuf_damage_validate, .{ @as(u32, 32), @as(u32, 16), &damage, @as(usize, 1) }));
}

fn packed_layout() layout_t {
    return .{ .width = 32, .height = 16, .fourcc = Argb8888, .plane_count = 1, .modifier = 0, .planes = .{ .{ .offset = 0, .stride = 128, .size = 2048, .extent = 4096 }, std.mem.zeroes(plane_t), std.mem.zeroes(plane_t), std.mem.zeroes(plane_t) } };
}
fn check_layout(expected: c_int, layout: *const layout_t) !void {
    try std.testing.expectEqual(expected, @call(.never_inline, venus_dmabuf_layout_validate, .{layout}));
}
test "all image geometry and active/inactive plane boundaries" {
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_dmabuf_layout_validate, .{null}));
    var layout = packed_layout();
    try check_layout(0, &layout);
    for ([_]u32{ Argb8888, Xrgb8888, Abgr8888, Xbgr8888 }) |format| {
        layout.fourcc = format;
        try check_layout(0, &layout);
    }
    layout.fourcc = 0;
    try check_layout(-1, &layout);
    layout = packed_layout();
    for ([_]u32{ 0, 16385, std.math.maxInt(u32) }) |size| {
        layout.width = size;
        try check_layout(-1, &layout);
        layout.width = 32;
        layout.height = size;
        try check_layout(-1, &layout);
        layout.height = 16;
    }
    for ([_]u32{ 0, 2, 4, std.math.maxInt(u32) }) |count| {
        layout.plane_count = count;
        try check_layout(-1, &layout);
    }
    layout = packed_layout();
    for (0..4) |field| {
        switch (field) {
            0 => layout.planes[1].offset = 1,
            1 => layout.planes[1].stride = 1,
            2 => layout.planes[1].size = 1,
            else => layout.planes[1].extent = 1,
        }
        try check_layout(-1, &layout);
        layout.planes[1] = std.mem.zeroes(plane_t);
    }
    for ([_]u32{ 0, 127, 2147483648, std.math.maxInt(u32) }) |stride| {
        layout.planes[0].stride = stride;
        try check_layout(-1, &layout);
    }
    layout = packed_layout();
    for ([_]u64{ 0, 2047, 4097, std.math.maxInt(u64) }) |size| {
        layout.planes[0].size = size;
        try check_layout(-1, &layout);
    }
    layout = packed_layout();
    for ([_]u64{ 0, 2047, 1073741825, std.math.maxInt(u64) }) |extent| {
        layout.planes[0].extent = extent;
        try check_layout(-1, &layout);
    }
    layout = packed_layout();
    for ([_]u32{ 2049, 4097, std.math.maxInt(u32) }) |offset| {
        layout.planes[0].offset = offset;
        try check_layout(-1, &layout);
    }
    layout.planes[0].offset = 2048;
    try check_layout(0, &layout);
    layout = packed_layout();
    layout.modifier = 0x00ffffffffffffff;
    layout.planes[0].size = 1;
    try check_layout(0, &layout); // Queried nonlinear size, no invented row arithmetic.
    layout.modifier = 0x0100000000000001;
    try check_layout(0, &layout);
    layout = packed_layout();
    layout.width = 16384;
    layout.height = 16384;
    layout.planes[0] = .{ .offset = 0, .stride = 65536, .size = 1073741824, .extent = 1073741824 };
    try check_layout(0, &layout);
    layout = packed_layout();
    layout.fourcc = Nv12;
    try check_layout(-1, &layout);
    layout.plane_count = 2;
    layout.planes[0] = .{ .offset = 0, .stride = 32, .size = 512, .extent = 4096 };
    layout.planes[1] = .{ .offset = 512, .stride = 32, .size = 256, .extent = 4096 };
    try check_layout(0, &layout);
    layout.width = 31;
    try check_layout(-1, &layout);
    layout.width = 32;
    layout.height = 15;
    try check_layout(-1, &layout);
    layout.height = 16;
    layout.planes[1].size = 255;
    try check_layout(-1, &layout);
}

test "complete feedback bounds and pair query including ignored protocol padding" {
    const table = try std.testing.allocator.alloc(u8, MaxTableBytes);
    defer std.testing.allocator.free(table);
    const indices = try std.testing.allocator.alloc(u8, MaxIndexBytes);
    defer std.testing.allocator.free(indices);
    @memset(table, 0xa5); // Protocol padding is unused, not reserved-zero.
    for (0..4096) |index| {
        std.mem.writeInt(u32, table[index * 16 ..][0..4], Argb8888, .little);
        std.mem.writeInt(u64, table[index * 16 + 8 ..][0..8], index, .little);
        std.mem.writeInt(u16, indices[index * 2 ..][0..2], @intCast(index), .little);
    }
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_dmabuf_feedback_match, .{ null, table.len, indices.ptr, indices.len, Argb8888, @as(u64, 0) }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_dmabuf_feedback_match, .{ table.ptr, table.len, null, indices.len, Argb8888, @as(u64, 0) }));
    for ([_]usize{ 0, 1, 15, 17, 65537, std.math.maxInt(usize) }) |length|
        try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_dmabuf_feedback_match, .{ table.ptr, length, indices.ptr, indices.len, Argb8888, @as(u64, 0) }));
    for ([_]usize{ 0, 1, 3, 8193, std.math.maxInt(usize) }) |length|
        try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_dmabuf_feedback_match, .{ table.ptr, table.len, indices.ptr, length, Argb8888, @as(u64, 0) }));
    for (0..4096) |index|
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_dmabuf_feedback_match, .{ table.ptr, table.len, indices.ptr + index * 2, @as(usize, 2), Argb8888, @as(u64, index) }));
    try std.testing.expectEqual(@as(c_int, 1), @call(.never_inline, venus_dmabuf_feedback_match, .{ table.ptr, table.len, indices.ptr, indices.len, Xrgb8888, @as(u64, 0) }));
    try std.testing.expectEqual(@as(c_int, 1), @call(.never_inline, venus_dmabuf_feedback_match, .{ table.ptr, table.len, indices.ptr, indices.len, Argb8888, std.math.maxInt(u64) }));
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_dmabuf_feedback_match, .{ table.ptr, table.len, indices.ptr, indices.len, Argb8888, @as(u64, 0) }));
    for ([_]u16{ 4096, std.math.maxInt(u16) }) |bad_index| {
        std.mem.writeInt(u16, indices[8190..8192], bad_index, .little);
        try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_dmabuf_feedback_match, .{ table.ptr, table.len, indices.ptr, indices.len, Argb8888, @as(u64, 0) }));
    }
}
