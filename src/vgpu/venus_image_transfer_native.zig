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
/// Owned copy normalization result. Native region keeps the source-unit extent;
/// footprints contain actual per-image texel extents for overlap checks. No pointers.
pub const copy_region_t = struct { native: c.VkImageCopy, source_extent: c.VkExtent3D, target_extent: c.VkExtent3D };
fn copy_layers(image: image_t, layers: c.VkImageSubresourceLayers, maintenance5: bool) !c.VkImageSubresourceLayers {
    var result = layers;
    if (layers.baseArrayLayer >= image.layers) return error.Invalid;
    if (layers.layerCount == c.VK_REMAINING_ARRAY_LAYERS) {
        if (!maintenance5) return error.Invalid;
        result.layerCount = image.layers - layers.baseArrayLayer;
    }
    if (result.layerCount == 0 or result.layerCount > image.layers - result.baseArrayLayer) return error.Invalid;
    return result;
}
fn copy_footprint(image: image_t, layers: c.VkImageSubresourceLayers, offset: c.VkOffset3D, size: c.VkExtent3D, shape: block_t) !c.VkExtent3D {
    if (layers.mipLevel >= image.levels or layers.mipLevel >= 32) return error.Invalid;
    var actual = size;
    const offsets = [_]i32{ offset.x, offset.y, offset.z };
    const sizes = [_]u32{ size.width, size.height, size.depth };
    const blocks = [_]u32{ shape.width, shape.height, 1 };
    var clipped: [3]u32 = undefined;
    for (image.extent, 0..) |axis, index| {
        const mip = @max(@as(u32, 1), axis >> @as(u5, @intCast(layers.mipLevel)));
        const bound = rounded(mip, blocks[index]) * blocks[index];
        if (offsets[index] < 0 or sizes[index] == 0) return error.Invalid;
        const start: u32 = @intCast(offsets[index]);
        if (start >= mip or @as(u64, start) + sizes[index] > bound) return error.Invalid;
        clipped[index] = @min(sizes[index], mip - start);
    }
    actual.width = clipped[0];
    actual.height = clipped[1];
    actual.depth = clipped[2];
    _ = try region(image, layers, offset, actual);
    return actual;
}
/// [in] copied creation metadata and native region, no retained pointers or allocation.
/// [in] maintenance5 is the actual enabled feature, required for remaining layers and
/// image-type pairs other than 2D/3D. Vulkan1.1 maintenance1 is the caller's baseline.
/// [out] owned canonical region and overlap footprints. Invalid preserves input/state.
/// Compressed bounds round to blocks; footprints clip the final partial block to the
/// actual mip. Size-compatible color formats and identical depth/stencil formats only.
/// Immutable concurrent calls are safe. Caller checks samples, usage and native owners.
pub fn copy_region(source: image_t, target: image_t, request: c.VkImageCopy, maintenance5: bool) !copy_region_t {
    if (source.image_type > 2 or target.image_type > 2 or request.srcSubresource.aspectMask != request.dstSubresource.aspectMask or request.extent.width == 0 or request.extent.height == 0 or request.extent.depth == 0) return error.Invalid;
    const first = try block(source.format, request.srcSubresource.aspectMask);
    const second = try block(target.format, request.dstSubresource.aspectMask);
    if (first.bytes != second.bytes or (request.srcSubresource.aspectMask != 1 and source.format != target.format)) return error.Invalid;
    const first_compressed = first.width != 1 or first.height != 1;
    const second_compressed = second.width != 1 or second.height != 1;
    if (first_compressed and second_compressed and (first.width != second.width or first.height != second.height)) return error.Invalid;
    var native = request;
    native.srcSubresource = try copy_layers(source, request.srcSubresource, maintenance5);
    native.dstSubresource = try copy_layers(target, request.dstSubresource, maintenance5);
    const different = source.image_type != target.image_type;
    const volume_pair = (source.image_type == 2) != (target.image_type == 2);
    const ordinary_volume_pair = volume_pair and source.image_type != 0 and target.image_type != 0;
    if (different and !ordinary_volume_pair and !maintenance5) return error.Invalid;
    const first_remaining = request.srcSubresource.layerCount == c.VK_REMAINING_ARRAY_LAYERS;
    const second_remaining = request.dstSubresource.layerCount == c.VK_REMAINING_ARRAY_LAYERS;
    if (different and first_remaining and second_remaining) {
        if (source.image_type != 2) {
            if (request.extent.depth > native.srcSubresource.layerCount) return error.Invalid;
            native.srcSubresource.layerCount = request.extent.depth;
        }
        if (target.image_type != 2) {
            if (request.extent.depth > native.dstSubresource.layerCount) return error.Invalid;
            native.dstSubresource.layerCount = request.extent.depth;
        }
    } else if ((!different or first_remaining or second_remaining) and native.srcSubresource.layerCount != native.dstSubresource.layerCount) return error.Invalid;
    if (volume_pair and request.extent.depth != (if (source.image_type != 2) native.srcSubresource.layerCount else native.dstSubresource.layerCount)) return error.Invalid;
    var source_size = request.extent;
    var target_size = request.extent;
    if (first_compressed and !second_compressed) {
        target_size.width = @intCast(rounded(request.extent.width, first.width));
        target_size.height = @intCast(rounded(request.extent.height, first.height));
    } else if (!first_compressed and second_compressed) {
        target_size.width = std.math.mul(u32, request.extent.width, second.width) catch return error.Invalid;
        target_size.height = std.math.mul(u32, request.extent.height, second.height) catch return error.Invalid;
        if (target.image_type == 0) target_size.height = 1;
    }
    // Array layers stand in for volume depth; native extent is still unchanged.
    if (source.image_type != 2) source_size.depth = 1;
    if (target.image_type != 2) target_size.depth = 1;
    if (!different and source.image_type != 2 and request.extent.depth != 1) return error.Invalid;
    if (different and !volume_pair and (source.image_type == 0 or target.image_type == 0) and request.extent.height != 1) return error.Invalid;
    return .{ .native = native, .source_extent = try copy_footprint(source, native.srcSubresource, native.srcOffset, source_size, first), .target_extent = try copy_footprint(target, native.dstSubresource, native.dstOffset, target_size, second) };
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
fn numeric_class(format: u32) u32 {
    return switch (format) {
        13, 20, 27, 34, 41, 48, 55, 62, 68, 74, 81, 88, 95, 98, 101, 104, 107, 110, 113, 116, 119 => 1,
        14, 21, 28, 35, 42, 49, 56, 63, 69, 75, 82, 89, 96, 99, 102, 105, 108, 111, 114, 117, 120 => 2,
        else => 0,
    };
}
/// Validate blit format classes and single aspect. Integer colors must share signedness;
/// normalized/float colors may convert. Depth/stencil requires identical format and nearest.
/// Compressed formats cannot blit. Host format BLIT/filter feature checks remain caller-owned.
/// Returns Invalid without allocations or ownership changes; immutable calls thread-safe.
pub fn blit_compatible(source: u32, target: u32, aspect: u32, filter: u32) !void {
    const first = try block(source, aspect);
    const second = try block(target, aspect);
    if (filter > 1 or first.width != 1 or first.height != 1 or second.width != 1 or second.height != 1) return error.Invalid;
    if (aspect == 1) {
        if (numeric_class(source) != numeric_class(target)) return error.Invalid;
    } else if (source != target or filter != 0) return error.Invalid;
}
test "blit numeric classes depth stencil and compression obey format conversion rules" {
    try blit_compatible(37, 100, 1, 1);
    try blit_compatible(13, 98, 1, 0);
    try blit_compatible(14, 99, 1, 0);
    try std.testing.expectError(error.Invalid, blit_compatible(13, 99, 1, 0));
    try std.testing.expectError(error.Invalid, blit_compatible(37, 98, 1, 1));
    try blit_compatible(130, 130, 4, 0);
    try std.testing.expectError(error.Invalid, blit_compatible(130, 130, 2, 1));
    try std.testing.expectError(error.Invalid, blit_compatible(124, 126, 2, 0));
    try std.testing.expectError(error.Invalid, blit_compatible(131, 131, 1, 0));
}

test "format aspects dimensions layer bounds and compressed row restrictions" {
    try std.testing.expectError(error.Invalid, block(0, 1));
    try std.testing.expectError(error.Invalid, block(37, 2));
    try std.testing.expectError(error.Invalid, block(127, 2));
    try std.testing.expectError(error.Invalid, block(126, 4));
    for (1..185) |format| {
        const aspect: u32 = if (format >= 124 and format <= 126) 2 else if (format >= 127 and format <= 130) 4 else 1;
        _ = try block(@intCast(format), aspect);
    }
    var image = image_t{ .format = 37, .image_type = 1, .extent = .{ 16, 16, 4 }, .levels = 33, .layers = 4 };
    const initial: c.VkBufferImageCopy = .{ .imageSubresource = .{ .aspectMask = 1, .layerCount = 1 }, .imageExtent = .{ .width = 4, .height = 4, .depth = 1 } };
    inline for (.{ "mipLevel", "baseArrayLayer", "layerCount" }) |field| {
        var copy = initial;
        @field(copy.imageSubresource, field) = if (comptime std.mem.eql(u8, field, "layerCount")) 0 else 32;
        try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    }
    var copy = initial;
    copy.imageSubresource.layerCount = 5;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    image.image_type = 3;
    try std.testing.expectError(error.Invalid, buffer_span(image, initial));
    image.image_type = 0;
    try std.testing.expectError(error.Invalid, buffer_span(image, initial));
    copy = initial;
    copy.imageExtent.height = 1;
    copy.imageOffset.y = 1;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.imageOffset.y = 0;
    copy.imageOffset.z = 1;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.imageOffset.z = 0;
    copy.imageExtent.depth = 2;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.imageExtent.depth = 1;
    _ = try buffer_span(image, copy);
    image.image_type = 1;
    copy.imageOffset.z = 1;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.imageOffset.z = 0;
    copy.imageExtent.depth = 2;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    image.image_type = 2;
    copy.imageSubresource.baseArrayLayer = 1;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 2;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    image.image_type = 1;
    image.format = 131;
    copy = initial;
    copy.bufferImageHeight = 3;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.bufferImageHeight = 5;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.bufferImageHeight = 0;
    copy.bufferRowLength = 5;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.bufferRowLength = 0;
    copy.imageOffset.y = 1;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.imageOffset.y = 0;
    copy.imageExtent.height = 3;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    image.format = 23;
    copy = initial;
    copy.bufferOffset = 4;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    try std.testing.expectError(error.Invalid, blit_compatible(0, 37, 1, 0));
    try std.testing.expectError(error.Invalid, blit_compatible(37, 0, 1, 0));
    try std.testing.expectError(error.Invalid, blit_compatible(37, 37, 1, 2));
}

test "exact depth sizes compressed x alignment zero extent and enormous array strides" {
    try std.testing.expectEqual(@as(u32, 2), (try block(124, 2)).bytes);
    try std.testing.expectEqual(@as(u32, 4), (try block(125, 2)).bytes);
    try std.testing.expectEqual(@as(u32, 1), (try block(127, 4)).bytes);
    const image = image_t{ .format = 131, .image_type = 1, .extent = .{ 16, 16, 1 }, .levels = 1, .layers = 1 };
    var copy: c.VkBufferImageCopy = .{ .imageSubresource = .{ .aspectMask = 1, .layerCount = 1 }, .imageExtent = .{ .width = 4, .height = 4, .depth = 1 } };
    copy.imageOffset.x = 1;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    copy.imageOffset.x = 0;
    copy.imageExtent.width = 0;
    try std.testing.expectError(error.Invalid, buffer_span(image, copy));
    const huge = image_t{ .format = 37, .image_type = 1, .extent = .{ 4, 4, 1 }, .levels = 1, .layers = 0xffffffff };
    copy.imageExtent.width = 4;
    copy.bufferRowLength = 0xffffffff;
    copy.bufferImageHeight = 1;
    copy.imageExtent.height = 1;
    copy.imageSubresource.layerCount = 0xffffffff;
    try std.testing.expectError(error.Invalid, buffer_span(huge, copy));
    for (1..124) |format| try blit_compatible(@intCast(format), @intCast(format), 1, 0);
}

test "runtime depth stencil classification and full array span addition overflow" {
    for (124..131) |format| {
        for ([_]u32{ 2, 4 }) |aspect| {
            if ((format == 127 and aspect == 2) or (format < 127 and aspect == 4)) {
                try std.testing.expectError(error.Invalid, block(@intCast(format), aspect));
            } else {
                _ = try block(@intCast(format), aspect);
            }
        }
    }
    const huge = image_t{ .format = 1, .image_type = 1, .extent = .{ 0xffffffff, 0xffffffff, 1 }, .levels = 1, .layers = 2 };
    const copy: c.VkBufferImageCopy = .{ .imageSubresource = .{ .aspectMask = 1, .layerCount = 2 }, .imageExtent = .{ .width = 0xffffffff, .height = 0xffffffff, .depth = 1 } };
    try std.testing.expectError(error.Invalid, buffer_span(huge, copy));
}

test "copy block-compatible partial edges preserve native extent and actual footprints" {
    const compressed = image_t{ .format = 167, .image_type = 1, .extent = .{ 7, 3, 1 }, .levels = 1, .layers = 1 }; // ASTC8x5,16bytes
    const plain = image_t{ .format = 113, .image_type = 1, .extent = .{ 1, 1, 1 }, .levels = 1, .layers = 1 };
    var request = c.VkImageCopy{ .srcSubresource = .{ .aspectMask = 1, .layerCount = 1 }, .dstSubresource = .{ .aspectMask = 1, .layerCount = 1 }, .extent = .{ .width = 7, .height = 3, .depth = 1 } };
    const first = try copy_region(compressed, plain, request, false);
    try std.testing.expectEqual(@as(u32, 7), first.native.extent.width);
    try std.testing.expectEqual(@as(u32, 1), first.target_extent.width);
    request.extent = .{ .width = 1, .height = 1, .depth = 1 };
    const reverse = try copy_region(plain, compressed, request, false);
    try std.testing.expectEqual(@as(u32, 1), reverse.native.extent.width);
    try std.testing.expectEqual(@as(u32, 7), reverse.target_extent.width);
    try std.testing.expectEqual(@as(u32, 3), reverse.target_extent.height);
    request.extent.width = 2;
    try std.testing.expectError(error.Invalid, copy_region(plain, compressed, request, false));
    request.extent = .{ .width = 8, .height = 5, .depth = 1 };
    const padded = try copy_region(compressed, plain, request, false);
    try std.testing.expectEqual(@as(u32, 7), padded.source_extent.width);
    request.extent.width = 6;
    try std.testing.expectError(error.Invalid, copy_region(compressed, plain, request, false));
    var other = compressed;
    other.format = 157;
    try std.testing.expectError(error.Invalid, copy_region(compressed, other, request, false));
    other = plain;
    other.format = 37;
    try std.testing.expectError(error.Invalid, copy_region(compressed, other, request, false));
    var depth = plain;
    depth.format = 124;
    var target = depth;
    target.format = 128;
    request.extent = .{ .width = 1, .height = 1, .depth = 1 };
    request.srcSubresource.aspectMask = 2;
    request.dstSubresource.aspectMask = 2;
    try std.testing.expectError(error.Invalid, copy_region(depth, target, request, false));
    _ = try copy_region(depth, depth, request, false);
}
test "copy slices and owned remaining layer normalization require actual enabled maintenance5" {
    const volume = image_t{ .format = 37, .image_type = 2, .extent = .{ 8, 8, 4 }, .levels = 1, .layers = 1 };
    const array = image_t{ .format = 37, .image_type = 1, .extent = .{ 8, 8, 1 }, .levels = 1, .layers = 5 };
    var request = c.VkImageCopy{ .srcSubresource = .{ .aspectMask = 1, .layerCount = 1 }, .dstSubresource = .{ .aspectMask = 1, .baseArrayLayer = 1, .layerCount = 3 }, .srcOffset = .{ .z = 1 }, .extent = .{ .width = 8, .height = 8, .depth = 3 } };
    const result = try copy_region(volume, array, request, false);
    try std.testing.expectEqual(@as(u32, 3), result.native.dstSubresource.layerCount);
    try std.testing.expectEqual(@as(u32, 3), result.source_extent.depth);
    try std.testing.expectEqual(@as(u32, 1), result.target_extent.depth);
    const reverse = c.VkImageCopy{ .srcSubresource = request.dstSubresource, .dstSubresource = request.srcSubresource, .srcOffset = request.dstOffset, .dstOffset = request.srcOffset, .extent = request.extent };
    _ = try copy_region(array, volume, reverse, false);
    request.dstSubresource.layerCount = 2;
    try std.testing.expectError(error.Invalid, copy_region(volume, array, request, false));
    request.srcSubresource.layerCount = c.VK_REMAINING_ARRAY_LAYERS;
    request.dstSubresource.layerCount = c.VK_REMAINING_ARRAY_LAYERS;
    try std.testing.expectError(error.Invalid, copy_region(volume, array, request, false));
    const remaining = try copy_region(volume, array, request, true);
    try std.testing.expectEqual(@as(u32, 3), remaining.native.dstSubresource.layerCount);
    try std.testing.expectEqual(@as(u32, c.VK_REMAINING_ARRAY_LAYERS), request.dstSubresource.layerCount);
    request.extent.depth = 5;
    try std.testing.expectError(error.Invalid, copy_region(volume, array, request, true));
    request.extent.depth = 3;
    request.srcSubresource.layerCount = 1;
    try std.testing.expectError(error.Invalid, copy_region(volume, array, request, true));
    var line = array;
    line.image_type = 0;
    line.extent[1] = 1;
    request = .{ .srcSubresource = .{ .aspectMask = 1, .layerCount = 1 }, .dstSubresource = .{ .aspectMask = 1, .layerCount = 1 }, .extent = .{ .width = 8, .height = 1, .depth = 1 } };
    try std.testing.expectError(error.Invalid, copy_region(line, array, request, false));
    _ = try copy_region(line, array, request, true);
    _ = try copy_region(array, line, request, true);
    _ = try copy_region(line, volume, request, true);
    _ = try copy_region(volume, line, request, true);
    request.extent.height = 2;
    try std.testing.expectError(error.Invalid, copy_region(line, array, request, true));
}

test "copy preflight failures are bounded and leave borrowed metadata unchanged" {
    const initial = image_t{ .format = 37, .image_type = 1, .extent = .{ 8, 8, 1 }, .levels = 1, .layers = 4 };
    const original = c.VkImageCopy{ .srcSubresource = .{ .aspectMask = 1, .layerCount = 1 }, .dstSubresource = .{ .aspectMask = 1, .layerCount = 1 }, .extent = .{ .width = 8, .height = 8, .depth = 1 } };
    for (0..19) |case| {
        var source = initial;
        var target = initial;
        var request = original;
        switch (case) {
            0 => source.image_type = 3,
            1 => target.image_type = 3,
            2 => request.dstSubresource.aspectMask = 2,
            3 => source.format = 0,
            4 => target.format = 185,
            5 => request.srcSubresource.baseArrayLayer = 4,
            6 => request.dstSubresource.layerCount = 0,
            7 => request.srcSubresource.layerCount = 5,
            8 => request.srcSubresource.mipLevel = 1,
            9 => request.dstSubresource.mipLevel = 32,
            10 => request.srcOffset.x = -1,
            11 => request.dstOffset.y = -1,
            12 => request.extent.width = 0,
            13 => request.extent.height = 0,
            14 => request.extent.depth = 0,
            15 => request.extent.depth = 2,
            16 => request.dstSubresource.layerCount = 2,
            17 => request.dstOffset.x = 8,
            18 => request.extent.width = 9,
            else => unreachable,
        }
        try std.testing.expectError(error.Invalid, copy_region(source, target, request, true));
    }
    var request = original;
    request.srcSubresource.layerCount = c.VK_REMAINING_ARRAY_LAYERS;
    request.dstSubresource.layerCount = c.VK_REMAINING_ARRAY_LAYERS;
    try std.testing.expectEqual(@as(u32, 4), (try copy_region(initial, initial, request, true)).native.srcSubresource.layerCount);
    var volume = initial;
    volume.image_type = 2;
    volume.layers = 1;
    volume.extent[2] = 4;
    request.extent.depth = 3;
    const reverse = try copy_region(initial, volume, request, true);
    try std.testing.expectEqual(@as(u32, 3), reverse.native.srcSubresource.layerCount);
    request.extent.depth = 5;
    try std.testing.expectError(error.Invalid, copy_region(initial, volume, request, true));
    var compressed = initial;
    compressed.format = 131;
    request = original;
    _ = try copy_region(compressed, compressed, request, false);
    var huge = initial;
    huge.extent = .{ 0xffffffff, 0xffffffff, 1 };
    huge.format = 113;
    compressed.format = 167;
    request.extent.width = 0xffffffff;
    try std.testing.expectError(error.Invalid, copy_region(huge, compressed, request, false));
    request.extent.width = 1;
    request.extent.height = 0xffffffff;
    try std.testing.expectError(error.Invalid, copy_region(huge, compressed, request, false));
    compressed.image_type = 0;
    compressed.extent[1] = 1;
    request.extent.height = 1;
    _ = try copy_region(huge, compressed, request, true);
}
