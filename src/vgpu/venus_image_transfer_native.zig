//! Pure format/block and image/buffer transfer geometry. No native identities or pointers retained.
const std = @import("std");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Copied image geometry; caller supplies validated actual image creation metadata.
pub const image_t = struct { format: u32, image_type: u32, extent: [3]u32, levels: u32, layers: u32 };
/// Format transfer block geometry, bytes refer to a single selected aspect.
pub const block_t = struct { width: u32 = 1, height: u32 = 1, bytes: u32 };
/// Resolve a core format and single aspect to block geometry. Invalid for unsupported
/// extension/multiplanar formats or incompatible aspects. No allocation or shared state.
pub fn block(format: u32, aspect: u32) !block_t {
    if (format == 0 or format > 184) return error.Invalid;
    if (format >= 124 and format <= 130) {
        const bytes: u32 = switch (aspect) {
            2 => switch (format) {
                124, 128 => 2,
                125, 126, 129, 130 => 4,
                else => return error.Invalid,
            },
            4 => if (format >= 127) 1 else return error.Invalid,
            else => return error.Invalid,
        };
        return .{ .bytes = bytes };
    }
    if (aspect != 1) return error.Invalid;
    if (format >= 157) {
        const widths = [_]u32{ 4, 5, 5, 6, 6, 8, 8, 8, 10, 10, 10, 10, 12, 12 };
        const heights = [_]u32{ 4, 4, 5, 5, 6, 5, 6, 8, 5, 6, 8, 10, 10, 12 };
        const index = (format - 157) / 2;
        return .{ .width = widths[index], .height = heights[index], .bytes = 16 };
    }
    if (format >= 131) return .{ .width = 4, .height = 4, .bytes = switch (format) {
        131...134, 139, 140, 147...150, 153, 154 => 8,
        else => 16,
    } };
    const bytes: u32 = switch (format) {
        1, 9...15 => 1,
        2...8, 16...22, 70...76 => 2,
        23...36 => 3,
        37...69, 77...83, 98...100, 122, 123 => 4,
        84...90 => 6,
        91...97, 101...103, 110...112 => 8,
        104...106 => 12,
        107...109, 113...115 => 16,
        116...118 => 24,
        119...121 => 32,
        else => return error.Invalid,
    };
    return .{ .bytes = bytes };
}
/// Validate exact mip/layer/offset/extent and compressed edge alignment. Single aspect
/// only; 1D/2D/3D shapes follow creation metadata. Invalid preserves all inputs.
/// Borrowed values copied for call; no allocation, state or pointer ownership.
pub fn region(image: image_t, layers: c.VkImageSubresourceLayers, offset: c.VkOffset3D, size: c.VkExtent3D) !block_t {
    const shape = try block(image.format, layers.aspectMask);
    if (layers.mipLevel >= image.levels or layers.mipLevel >= 32 or layers.layerCount == 0 or layers.baseArrayLayer >= image.layers or layers.layerCount > image.layers - layers.baseArrayLayer or image.image_type > 2) return error.Invalid;
    const offsets = [_]i32{ offset.x, offset.y, offset.z };
    const extents = [_]u32{ size.width, size.height, size.depth };
    var mip: [3]u32 = undefined;
    for (image.extent, 0..) |axis, index| {
        mip[index] = @max(@as(u32, 1), axis >> @as(u5, @intCast(layers.mipLevel)));
        if (offsets[index] < 0 or extents[index] == 0 or @as(u32, @intCast(offsets[index])) >= mip[index] or extents[index] > mip[index] - @as(u32, @intCast(offsets[index]))) return error.Invalid;
    }
    if (image.image_type == 0 and (offset.y != 0 or offset.z != 0 or size.height != 1 or size.depth != 1)) return error.Invalid;
    if (image.image_type == 1 and (offset.z != 0 or size.depth != 1)) return error.Invalid;
    if (image.image_type == 2 and (layers.baseArrayLayer != 0 or layers.layerCount != 1)) return error.Invalid;
    if (@as(u32, @intCast(offset.x)) % shape.width != 0 or @as(u32, @intCast(offset.y)) % shape.height != 0 or (size.width % shape.width != 0 and @as(u32, @intCast(offset.x)) + size.width != mip[0]) or (size.height % shape.height != 0 and @as(u32, @intCast(offset.y)) + size.height != mip[1])) return error.Invalid;
    return shape;
}
fn multiply(left: u64, right: u64) !u64 {
    return std.math.mul(u64, left, right) catch error.Invalid;
}
fn add(left: u64, right: u64) !u64 {
    return std.math.add(u64, left, right) catch error.Invalid;
}
fn rounded(value: u32, divisor: u32) u64 {
    return (@as(u64, value) + divisor - 1) / divisor;
}
/// Return the exclusive buffer span length for a buffer/image region with row/image
/// strides, mip/array/3D and compressed-block semantics. Caller checks bufferOffset/span
/// against actual bound buffer size. Returns Invalid for overflow/alignment/shape errors;
/// no allocation or retained metadata, safe for concurrent immutable calls.
pub fn buffer_span(image: image_t, copy: c.VkBufferImageCopy) !u64 {
    const shape = try region(image, copy.imageSubresource, copy.imageOffset, copy.imageExtent);
    const row = if (copy.bufferRowLength == 0) copy.imageExtent.width else copy.bufferRowLength;
    const height = if (copy.bufferImageHeight == 0) copy.imageExtent.height else copy.bufferImageHeight;
    if (row < copy.imageExtent.width or height < copy.imageExtent.height or (copy.bufferRowLength != 0 and row % shape.width != 0) or (copy.bufferImageHeight != 0 and height % shape.height != 0) or copy.bufferOffset % 4 != 0 or (copy.imageSubresource.aspectMask == 1 and copy.bufferOffset % shape.bytes != 0)) return error.Invalid;
    const row_bytes = try multiply(rounded(row, shape.width), shape.bytes);
    const slice_bytes = try multiply(rounded(height, shape.height), row_bytes);
    const slices = if (image.image_type == 2) copy.imageExtent.depth else copy.imageSubresource.layerCount;
    const last_slice = try multiply(slices - 1, slice_bytes);
    const last_row = try multiply(rounded(copy.imageExtent.height, shape.height) - 1, row_bytes);
    return try add(try add(last_slice, last_row), try multiply(rounded(copy.imageExtent.width, shape.width), shape.bytes));
}
test "image mip arrays padded rows volume and compressed block spans" {
    const image = image_t{ .format = 44, .image_type = 1, .extent = .{ 64, 32, 1 }, .levels = 3, .layers = 4 };
    const layers = c.VkImageSubresourceLayers{ .aspectMask = 1, .mipLevel = 1, .baseArrayLayer = 1, .layerCount = 2 };
    const copy = c.VkBufferImageCopy{ .imageSubresource = layers, .bufferRowLength = 40, .bufferImageHeight = 20, .imageExtent = .{ .width = 32, .height = 16, .depth = 1 } };
    try std.testing.expectEqual(@as(u64, 3200 + 2400 + 128), try buffer_span(image, copy));
    var volume = image;
    volume.image_type = 2;
    volume.extent = .{ 16, 8, 4 };
    volume.layers = 1;
    var request = copy;
    request.imageSubresource = .{ .aspectMask = 1, .layerCount = 1 };
    request.bufferRowLength = 0;
    request.bufferImageHeight = 0;
    request.imageExtent = .{ .width = 16, .height = 8, .depth = 4 };
    try std.testing.expectEqual(@as(u64, 2048), try buffer_span(volume, request));
    var compressed = image;
    compressed.format = 131;
    compressed.extent = .{ 7, 5, 1 };
    compressed.levels = 1;
    compressed.layers = 1;
    request.imageSubresource = .{ .aspectMask = 1, .layerCount = 1 };
    request.imageExtent = .{ .width = 7, .height = 5, .depth = 1 };
    try std.testing.expectEqual(@as(u64, 32), try buffer_span(compressed, request));
    request.imageExtent.width = 5;
    try std.testing.expectError(error.Invalid, buffer_span(compressed, request));
}
test "geometry rejects aspects mip shape strides alignment and arithmetic overflow" {
    const image = image_t{ .format = 130, .image_type = 1, .extent = .{ 16, 16, 1 }, .levels = 1, .layers = 1 };
    var copy = c.VkBufferImageCopy{ .imageSubresource = .{ .aspectMask = 2, .layerCount = 1 }, .imageExtent = .{ .width = 16, .height = 16, .depth = 1 } };
    try std.testing.expectEqual(@as(u64, 1024), try buffer_span(image, copy));
    copy.imageSubresource.aspectMask = 4;
    try std.testing.expectEqual(@as(u64, 256), try buffer_span(image, copy));
    copy.imageSubresource.aspectMask = 6;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.imageSubresource.aspectMask = 2;
    copy.bufferOffset = 1;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.bufferOffset = 0;
    copy.imageOffset.x = -1;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.imageOffset.x = 0;
    copy.imageSubresource.mipLevel = 1;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.imageSubresource.mipLevel = 0;
    copy.bufferRowLength = 15;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    var huge = image_t{ .format = 121, .image_type = 2, .extent = .{ 0xffffffff, 0xffffffff, 0xffffffff }, .levels = 1, .layers = 1 };
    copy.imageSubresource = .{ .aspectMask = 1, .layerCount = 1 };
    copy.bufferRowLength = 0;
    copy.imageExtent = .{ .width = 0xffffffff, .height = 0xffffffff, .depth = 0xffffffff };
    try std.testing.expectError(error.Invalid, buffer_span(huge, copy));
    huge.format = 185;
    try std.testing.expectError(error.Invalid, buffer_span(huge, copy));
}
