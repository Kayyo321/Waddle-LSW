//! Allocation-free direct draw, render-pass end and bounded color readback packets.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Caller-owned8192-byte wire storage; scope-owned, no heap allocation.
pub const writer_t = render.writer_t;
fn command_header(writer: *writer_t, opcode: u32, command_id: u64) void {
    put(writer, u32, opcode);
    put(writer, u32, 1);
    put(writer, u64, command_id);
}
fn put(writer: *writer_t, comptime word_t: type, value: word_t) void {
    writer.put(word_t, value) catch unreachable;
}
/// in: command_id is nonzero resolved host command-buffer ID, never a guest handle.
/// out: owns its16 encoded bytes; Invalid rejects zero before output; allocation-free
/// and thread-safe. Caller checks recording state and active render pass before entry.
pub fn end_render_pass(command_id: u64) !writer_t {
    if (command_id == 0) return error.Invalid;
    var writer: writer_t = .{};
    command_header(&writer, 135, command_id);
    return writer;
}
/// in: command_id is resolved nonzero host ID; values are vertexCount, instanceCount,
/// firstVertex, firstInstance. Full-u32/zero scalars are legal; caller validates pipeline,
/// recording/render-pass state and vertex-buffer requirements. out: owns32 bytes;
/// Invalid rejects zero ID; allocation-free, thread-safe, no pointer retained.
pub fn draw(command_id: u64, values: [4]u32) !writer_t {
    if (command_id == 0) return error.Invalid;
    var writer: writer_t = .{};
    command_header(&writer, 106, command_id);
    for (values) |value| put(&writer, u32, value);
    return writer;
}
/// in: IDs are resolved nonzero host objects. Caller validates live ownership, usages,
/// bound memory, command state, barriers and actual image format RGBA8/BGRA8 UNORM.
/// width/height are actual mip0 dimensions1..4096; buffer_size is actual bound buffer
/// size, nonzero. regions is borrowed accessible1..64 records, tight single-color2D
/// mip0/layer0/depth1 copies. layout is GENERAL or TRANSFER_SRC_OPTIMAL.
/// out: owns48+56*regions.len bytes; invalid IDs/layout/count/metadata/ranges return
/// Invalid before serialization. No heap allocation or pointer retention; thread-safe.
pub fn copy_image_to_buffer(command_id: u64, image_id: u64, buffer_id: u64, layout: c.VkImageLayout, width: u32, height: u32, buffer_size: u64, regions: []const c.VkBufferImageCopy) !writer_t {
    if (command_id == 0 or image_id == 0 or buffer_id == 0 or buffer_size == 0 or width == 0 or height == 0 or width > 4096 or height > 4096 or regions.len == 0 or regions.len > 64) return error.Invalid;
    if (layout != c.VK_IMAGE_LAYOUT_GENERAL and layout != c.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) return error.Invalid;
    for (regions) |region| {
        if (region.bufferRowLength != 0 or region.bufferImageHeight != 0 or region.bufferOffset % 4 != 0 or region.imageSubresource.aspectMask != c.VK_IMAGE_ASPECT_COLOR_BIT or region.imageSubresource.mipLevel != 0 or region.imageSubresource.baseArrayLayer != 0 or region.imageSubresource.layerCount != 1 or region.imageOffset.x < 0 or region.imageOffset.y < 0 or region.imageOffset.z != 0 or region.imageExtent.width == 0 or region.imageExtent.height == 0 or region.imageExtent.depth != 1) return error.Invalid;
        if (@as(u64, @intCast(region.imageOffset.x)) + region.imageExtent.width > width or @as(u64, @intCast(region.imageOffset.y)) + region.imageExtent.height > height) return error.Invalid;
        const byte_count = @as(u64, region.imageExtent.width) * region.imageExtent.height * 4;
        if (region.bufferOffset > buffer_size or byte_count > buffer_size - region.bufferOffset) return error.Invalid;
    }
    // Fixed records are56 bytes, prefix48:64 regions require3632 <=8192.
    comptime {
        std.debug.assert(48 + 56 * 64 <= render.MaxBytes);
    }
    var writer: writer_t = .{};
    command_header(&writer, 116, command_id);
    put(&writer, u64, image_id);
    put(&writer, u32, layout);
    put(&writer, u64, buffer_id);
    put(&writer, u32, @intCast(regions.len));
    put(&writer, u64, regions.len);
    for (regions) |region| {
        put(&writer, u64, region.bufferOffset);
        put(&writer, u32, region.bufferRowLength);
        put(&writer, u32, region.bufferImageHeight);
        put(&writer, u32, region.imageSubresource.aspectMask);
        put(&writer, u32, region.imageSubresource.mipLevel);
        put(&writer, u32, region.imageSubresource.baseArrayLayer);
        put(&writer, u32, region.imageSubresource.layerCount);
        put(&writer, i32, region.imageOffset.x);
        put(&writer, i32, region.imageOffset.y);
        put(&writer, i32, region.imageOffset.z);
        put(&writer, u32, region.imageExtent.width);
        put(&writer, u32, region.imageExtent.height);
        put(&writer, u32, region.imageExtent.depth);
    }
    return writer;
}

// Test-only fixtures.
extern fn venus_graphics_command_test_end(output: [*]u8) usize;
extern fn venus_graphics_command_test_draw(values: *const [4]u32, output: [*]u8) usize;
extern fn venus_graphics_command_test_copy(layout: c.VkImageLayout, count: u32, regions: [*]const c.VkBufferImageCopy, output: [*]u8) usize;
fn region_fixture() c.VkBufferImageCopy {
    return .{ .imageSubresource = .{ .aspectMask = 1, .layerCount = 1 }, .imageExtent = .{ .width = 64, .height = 64, .depth = 1 } };
}
fn compare(writer: writer_t, oracle: []const u8) !void {
    try std.testing.expectEqual(oracle.len, writer.used);
    try std.testing.expectEqualSlices(u8, oracle, writer.bytes[0..writer.used]);
}
test "end and draw packets match pinned encoder across all scalar extremes" {
    var oracle: [8192]u8 = undefined;
    try compare(try end_render_pass(8), oracle[0..venus_graphics_command_test_end(&oracle)]);
    for ([_][4]u32{ .{ 3, 1, 0, 0 }, .{ 0, 0, 0, 0 }, .{ std.math.maxInt(u32), std.math.maxInt(u32), std.math.maxInt(u32), std.math.maxInt(u32) } }) |values| {
        try compare(try draw(8, values), oracle[0..venus_graphics_command_test_draw(&values, &oracle)]);
    }
    try std.testing.expectError(error.Invalid, end_render_pass(0));
    var zero_command: u64 = 0;
    try std.testing.expectError(error.Invalid, draw(@as(*volatile u64, &zero_command).*, .{ 3, 1, 0, 0 }));
}
test "ordinary and maximum image readback packets match independent encoder" {
    var regions: [64]c.VkBufferImageCopy = undefined;
    for (&regions, 0..) |*region, index| {
        region.* = region_fixture();
        region.bufferOffset = index * 16384;
    }
    var oracle: [8192]u8 = undefined;
    for ([_]usize{ 1, 2, 64 }) |count| {
        for ([_]c.VkImageLayout{ c.VK_IMAGE_LAYOUT_GENERAL, c.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL }) |layout| {
            const writer = try copy_image_to_buffer(8, 42, 43, layout, 64, 64, 16384 * 64, regions[0..count]);
            try std.testing.expectEqual(48 + 56 * count, writer.used);
            try compare(writer, oracle[0..venus_graphics_command_test_copy(layout, @intCast(count), &regions, &oracle)]);
        }
    }
}
test "readback metadata and every independent copied record field reject malformed inputs" {
    const region = region_fixture();
    for (0..3) |index| {
        var ids = [_]u64{ 8, 42, 43 };
        ids[index] = 0;
        try std.testing.expectError(error.Invalid, copy_image_to_buffer(ids[0], ids[1], ids[2], 1, 64, 64, 16384, &.{region}));
    }
    for ([_]u32{ 0, 4097, std.math.maxInt(u32) }) |value| {
        try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, value, 64, 16384, &.{region}));
        try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, value, 16384, &.{region}));
    }
    for ([_]u64{ 0, 16383 }) |value| try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, 64, value, &.{region}));
    var invalid_layout: c.VkImageLayout = 0;
    try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, @as(*volatile c.VkImageLayout, &invalid_layout).*, 64, 64, 16384, &.{region}));
    try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, 64, 16384, &.{}));
    const too_many = [_]c.VkBufferImageCopy{region} ** 65;
    try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, 64, 16384, &too_many));
    inline for (.{ "bufferOffset", "bufferRowLength", "bufferImageHeight" }) |name| {
        var invalid = region;
        @field(invalid, name) = 1;
        try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, 64, 16384, &.{invalid}));
    }
    inline for (.{ "aspectMask", "mipLevel", "baseArrayLayer", "layerCount" }) |name| {
        var invalid = region;
        @field(invalid.imageSubresource, name) = 123;
        try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, 64, 16384, &.{invalid}));
    }
    inline for (.{ "x", "y", "z" }) |name| {
        var invalid = region;
        @field(invalid.imageOffset, name) = -1;
        try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, 64, 16384, &.{invalid}));
        @field(invalid.imageOffset, name) = std.math.maxInt(i32);
        try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, 64, 16384, &.{invalid}));
    }
    inline for (.{ "width", "height", "depth" }) |name| {
        var invalid = region;
        @field(invalid.imageExtent, name) = 0;
        try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, 64, 16384, &.{invalid}));
        @field(invalid.imageExtent, name) = std.math.maxInt(u32);
        try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, 64, 16384, &.{invalid}));
    }
    var invalid = region;
    invalid.bufferOffset = 16388;
    try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, 64, 16384, &.{invalid}));
    invalid.bufferOffset = 4;
    try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 64, 64, 16384, &.{invalid}));
}
test "readback nonzero offsets exact destination fit and maximum image extent" {
    var region = region_fixture();
    region.imageOffset = .{ .x = 16, .y = 8 };
    region.imageExtent = .{ .width = 32, .height = 32, .depth = 1 };
    region.bufferOffset = 64;
    var oracle: [8192]u8 = undefined;
    try compare(try copy_image_to_buffer(8, 42, 43, 1, 64, 64, 4160, &.{region}), oracle[0..venus_graphics_command_test_copy(1, 1, &.{region}, &oracle)]);
    region.imageOffset = .{};
    region.imageExtent = .{ .width = 4096, .height = 4096, .depth = 1 };
    try compare(try copy_image_to_buffer(8, 42, 43, 6, 4096, 4096, std.math.maxInt(u64), &.{region}), oracle[0..venus_graphics_command_test_copy(6, 1, &.{region}, &oracle)]);
    region.bufferOffset = std.math.maxInt(u64) - 3;
    try std.testing.expectError(error.Invalid, copy_image_to_buffer(8, 42, 43, 1, 4096, 4096, std.math.maxInt(u64), &.{region}));
}
