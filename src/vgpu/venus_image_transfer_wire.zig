//! Allocation-free image transfer packets. ICD validates ownership, usages, format/block
//! geometry, mip/layer extents and buffer bounds before submitting these structural codecs.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Owned bounded packet, no pointers retained; exclusive writer ownership belongs to caller.
pub const writer_t = render.writer_t;
/// Maximum regions per transfer; all maximum packets fit in writer_t.
pub const MaxRegions = 64;
fn put(writer: *writer_t, comptime value_t: type, value: value_t) void {
    writer.put(value_t, value) catch unreachable;
}
fn header(opcode: u32, command: u64) writer_t {
    var writer: writer_t = .{};
    put(&writer, u32, opcode);
    put(&writer, u32, 1);
    put(&writer, u64, command);
    return writer;
}
fn source_layout(layout: u32) bool {
    return layout == c.VK_IMAGE_LAYOUT_GENERAL or layout == c.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}
fn target_layout(layout: u32) bool {
    return layout == c.VK_IMAGE_LAYOUT_GENERAL or layout == c.VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
}
fn layers_valid(value: c.VkImageSubresourceLayers) bool {
    return value.aspectMask != 0 and value.aspectMask & ~@as(u32, 7) == 0 and value.layerCount != 0 and
        value.baseArrayLayer <= std.math.maxInt(u32) - value.layerCount;
}
fn encode_layers(writer: *writer_t, value: c.VkImageSubresourceLayers) void {
    put(writer, u32, value.aspectMask);
    put(writer, u32, value.mipLevel);
    put(writer, u32, value.baseArrayLayer);
    put(writer, u32, value.layerCount);
}
fn encode_offset(writer: *writer_t, value: c.VkOffset3D) void {
    put(writer, i32, value.x);
    put(writer, i32, value.y);
    put(writer, i32, value.z);
}
fn encode_extent(writer: *writer_t, value: c.VkExtent3D) void {
    put(writer, u32, value.width);
    put(writer, u32, value.height);
    put(writer, u32, value.depth);
}
fn extent_valid(value: c.VkExtent3D) bool {
    return value.width != 0 and value.height != 0 and value.depth != 0;
}
fn offset_valid(value: c.VkOffset3D) bool {
    return value.x >= 0 and value.y >= 0 and value.z >= 0;
}
fn transfer_prefix(opcode: u32, command: u64, source: u64, source_image_layout: u32, target: u64, target_image_layout: u32, count: usize) writer_t {
    var writer = header(opcode, command);
    put(&writer, u64, source);
    put(&writer, u32, source_image_layout);
    put(&writer, u64, target);
    put(&writer, u32, target_image_layout);
    put(&writer, u32, @intCast(count));
    put(&writer, u64, count);
    return writer;
}
/// [in] translated nonzero host IDs and borrowed 1..64 regions; canonical transfer layouts.
/// Returns complete owned packet or Invalid without allocation. Caller validates source/target
/// live ownership, compatible formats, exact subresources/bounds and command recording state.
/// Independent calls are thread-safe; no borrowed pointer outlives the call.
pub fn copy_image(command: u64, source: u64, source_image_layout: u32, target: u64, target_image_layout: u32, regions: []const c.VkImageCopy) !writer_t {
    return copy_image_inner(command, source, source_image_layout, target, target_image_layout, regions, false);
}
/// [in] native regions already normalized by image_transfer_native.copy_region, with
/// actual image ownership, bounds, block geometry and 3D/array layer correspondence
/// checked by caller. Unequal layer counts are legal for these validated copies.
/// Returns an owned unchanged native packet or Invalid; borrows only for this call,
/// allocation-free and thread-safe. Structural guards still apply to every region.
pub fn copy_image_validated(command: u64, source: u64, source_image_layout: u32, target: u64, target_image_layout: u32, regions: []const c.VkImageCopy) !writer_t {
    return copy_image_inner(command, source, source_image_layout, target, target_image_layout, regions, true);
}
fn copy_image_inner(command: u64, source: u64, source_image_layout: u32, target: u64, target_image_layout: u32, regions: []const c.VkImageCopy, geometry_validated: bool) !writer_t {
    if (command == 0 or source == 0 or target == 0 or !source_layout(source_image_layout) or !target_layout(target_image_layout) or regions.len == 0 or regions.len > MaxRegions) return error.Invalid;
    for (regions) |region| if (!layers_valid(region.srcSubresource) or !layers_valid(region.dstSubresource) or
        region.srcSubresource.aspectMask != region.dstSubresource.aspectMask or (!geometry_validated and region.srcSubresource.layerCount != region.dstSubresource.layerCount) or
        !offset_valid(region.srcOffset) or !offset_valid(region.dstOffset) or !extent_valid(region.extent)) return error.Invalid;
    var writer = transfer_prefix(113, command, source, source_image_layout, target, target_image_layout, regions.len);
    for (regions) |region| {
        encode_layers(&writer, region.srcSubresource);
        encode_offset(&writer, region.srcOffset);
        encode_layers(&writer, region.dstSubresource);
        encode_offset(&writer, region.dstOffset);
        encode_extent(&writer, region.extent);
    }
    return writer;
}
/// [in] nonzero host IDs, canonical layouts and borrowed 1..64 blit regions. Reversed
/// endpoints are legal, enabling flips; nonnegative endpoints and nonzero axis lengths
/// required. Caller validates image bounds, sample count and format/filter support.
/// Returns owned packet or Invalid, allocation-free, no retained pointers, thread-safe.
pub fn blit_image(command: u64, source: u64, source_image_layout: u32, target: u64, target_image_layout: u32, regions: []const c.VkImageBlit, filter: u32) !writer_t {
    if (command == 0 or source == 0 or target == 0 or !source_layout(source_image_layout) or !target_layout(target_image_layout) or regions.len == 0 or regions.len > MaxRegions or filter > c.VK_FILTER_LINEAR) return error.Invalid;
    for (regions) |region| {
        if (!layers_valid(region.srcSubresource) or !layers_valid(region.dstSubresource) or region.srcSubresource.aspectMask != region.dstSubresource.aspectMask or region.srcSubresource.layerCount != region.dstSubresource.layerCount) return error.Invalid;
        inline for (.{ region.srcOffsets, region.dstOffsets }) |offsets| {
            if (!offset_valid(offsets[0]) or !offset_valid(offsets[1]) or offsets[0].x == offsets[1].x or offsets[0].y == offsets[1].y or offsets[0].z == offsets[1].z) return error.Invalid;
        }
    }
    var writer = transfer_prefix(114, command, source, source_image_layout, target, target_image_layout, regions.len);
    for (regions) |region| {
        encode_layers(&writer, region.srcSubresource);
        put(&writer, u64, 2);
        for (region.srcOffsets) |offset| encode_offset(&writer, offset);
        encode_layers(&writer, region.dstSubresource);
        put(&writer, u64, 2);
        for (region.dstOffsets) |offset| encode_offset(&writer, offset);
    }
    put(&writer, u32, filter);
    return writer;
}
/// Encode buffer upload to image; IDs nonzero and regions borrowed 1..64 records.
/// Caller validates texel/block alignment, row strides, buffer spans and image bounds.
/// Returns complete owned packet or Invalid, no allocation/retention, thread-safe.
pub fn copy_buffer_to_image(command: u64, source: u64, target: u64, layout: u32, regions: []const c.VkBufferImageCopy) !writer_t {
    if (command == 0 or source == 0 or target == 0 or !target_layout(layout) or regions.len == 0 or regions.len > MaxRegions) return error.Invalid;
    for (regions) |region| if (!layers_valid(region.imageSubresource) or !offset_valid(region.imageOffset) or !extent_valid(region.imageExtent) or
        (region.bufferRowLength != 0 and region.bufferRowLength < region.imageExtent.width) or
        (region.bufferImageHeight != 0 and region.bufferImageHeight < region.imageExtent.height)) return error.Invalid;
    var writer = header(115, command);
    put(&writer, u64, source);
    put(&writer, u64, target);
    put(&writer, u32, layout);
    put(&writer, u32, @intCast(regions.len));
    put(&writer, u64, regions.len);
    for (regions) |region| {
        put(&writer, u64, region.bufferOffset);
        put(&writer, u32, region.bufferRowLength);
        put(&writer, u32, region.bufferImageHeight);
        encode_layers(&writer, region.imageSubresource);
        encode_offset(&writer, region.imageOffset);
        encode_extent(&writer, region.imageExtent);
    }
    return writer;
}
/// Encode image readback to buffer; IDs nonzero and regions borrowed 1..64 records.
/// Caller validates texel/block alignment, row strides, buffer spans and image bounds.
/// Returns complete owned packet or Invalid, no allocation/retention, thread-safe.
pub fn copy_image_to_buffer(command: u64, source: u64, target: u64, layout: u32, regions: []const c.VkBufferImageCopy) !writer_t {
    if (command == 0 or source == 0 or target == 0 or !source_layout(layout) or regions.len == 0 or regions.len > MaxRegions) return error.Invalid;
    for (regions) |region| if (!layers_valid(region.imageSubresource) or !offset_valid(region.imageOffset) or !extent_valid(region.imageExtent) or
        (region.bufferRowLength != 0 and region.bufferRowLength < region.imageExtent.width) or
        (region.bufferImageHeight != 0 and region.bufferImageHeight < region.imageExtent.height)) return error.Invalid;
    var writer = header(116, command);
    put(&writer, u64, source);
    put(&writer, u32, layout);
    put(&writer, u64, target);
    put(&writer, u32, @intCast(regions.len));
    put(&writer, u64, regions.len);
    for (regions) |region| {
        put(&writer, u64, region.bufferOffset);
        put(&writer, u32, region.bufferRowLength);
        put(&writer, u32, region.bufferImageHeight);
        encode_layers(&writer, region.imageSubresource);
        encode_offset(&writer, region.imageOffset);
        encode_extent(&writer, region.imageExtent);
    }
    return writer;
}
fn ranges_valid(ranges: []const c.VkImageSubresourceRange, aspects: u32) bool {
    if (ranges.len == 0 or ranges.len > MaxRegions) return false;
    for (ranges) |range| if (range.aspectMask == 0 or range.aspectMask & ~aspects != 0 or range.levelCount == 0 or range.layerCount == 0) return false;
    return true;
}
fn encode_ranges(writer: *writer_t, ranges: []const c.VkImageSubresourceRange) void {
    put(writer, u32, @intCast(ranges.len));
    put(writer, u64, ranges.len);
    for (ranges) |range| {
        put(writer, u32, range.aspectMask);
        put(writer, u32, range.baseMipLevel);
        put(writer, u32, range.levelCount);
        put(writer, u32, range.baseArrayLayer);
        put(writer, u32, range.layerCount);
    }
}
/// Clear color raw four-word union, preserving float/integer bit patterns. IDs nonzero,
/// layout canonical target and ranges borrowed1..64 color records. Caller validates
/// exact image ranges, format union interpretation and ownership. Owned result or Invalid;
/// allocation-free, no pointer retention, thread-safe.
pub fn clear_color(command: u64, image: u64, layout: u32, color: [4]u32, ranges: []const c.VkImageSubresourceRange) !writer_t {
    if (command == 0 or image == 0 or !target_layout(layout) or !ranges_valid(ranges, c.VK_IMAGE_ASPECT_COLOR_BIT)) return error.Invalid;
    var writer = header(119, command);
    put(&writer, u64, image);
    put(&writer, u32, layout);
    put(&writer, u64, 1);
    put(&writer, u32, 2); // Pinned union tag: uint32 representation.
    put(&writer, u64, 4);
    for (color) |word| put(&writer, u32, word);
    encode_ranges(&writer, ranges);
    return writer;
}
/// Clear normalized finite depth and full32-bit stencil, borrowed1..64 depth/stencil
/// ranges. Caller validates format aspects and actual image bounds. Nonzero IDs and
/// target layout required. Owned result or Invalid, allocation-free and thread-safe.
pub fn clear_depth_stencil(command: u64, image: u64, layout: u32, value: c.VkClearDepthStencilValue, ranges: []const c.VkImageSubresourceRange) !writer_t {
    if (command == 0 or image == 0 or !target_layout(layout) or !std.math.isFinite(value.depth) or value.depth < 0 or value.depth > 1 or !ranges_valid(ranges, c.VK_IMAGE_ASPECT_DEPTH_BIT | c.VK_IMAGE_ASPECT_STENCIL_BIT)) return error.Invalid;
    var writer = header(120, command);
    put(&writer, u64, image);
    put(&writer, u32, layout);
    put(&writer, u64, 1);
    put(&writer, u32, @bitCast(value.depth));
    put(&writer, u32, value.stencil);
    encode_ranges(&writer, ranges);
    return writer;
}
/// Resolve multisampled source to single-sample target. Caller validates actual sample
/// counts, format compatibility and all image subresource/extents. IDs nonzero, layouts
/// canonical, regions borrowed1..64 color records; owned result or Invalid. No allocations.
pub fn resolve_image(command: u64, source: u64, source_image_layout: u32, target: u64, target_image_layout: u32, regions: []const c.VkImageResolve) !writer_t {
    if (command == 0 or source == 0 or target == 0 or !source_layout(source_image_layout) or !target_layout(target_image_layout) or regions.len == 0 or regions.len > MaxRegions) return error.Invalid;
    for (regions) |region| if (!layers_valid(region.srcSubresource) or !layers_valid(region.dstSubresource) or region.srcSubresource.aspectMask != c.VK_IMAGE_ASPECT_COLOR_BIT or region.dstSubresource.aspectMask != c.VK_IMAGE_ASPECT_COLOR_BIT or region.srcSubresource.layerCount != region.dstSubresource.layerCount or !offset_valid(region.srcOffset) or !offset_valid(region.dstOffset) or !extent_valid(region.extent)) return error.Invalid;
    var writer = transfer_prefix(122, command, source, source_image_layout, target, target_image_layout, regions.len);
    for (regions) |region| {
        encode_layers(&writer, region.srcSubresource);
        encode_offset(&writer, region.srcOffset);
        encode_layers(&writer, region.dstSubresource);
        encode_offset(&writer, region.dstOffset);
        encode_extent(&writer, region.extent);
    }
    return writer;
}

// Test-only generated encoder oracle, never linked into production runtime.
extern fn venus_image_transfer_test_encode(u32, u32, *const anyopaque, *const anyopaque, [*]u8) usize;
fn compare(writer: writer_t, kind: u32, count: usize, regions: *const anyopaque, clear: *const anyopaque) !void {
    var expected: [8192]u8 = undefined;
    const used = venus_image_transfer_test_encode(kind, @intCast(count), regions, clear, &expected);
    try std.testing.expectEqual(used, writer.used);
    try std.testing.expectEqualSlices(u8, expected[0..used], writer.bytes[0..writer.used]);
}
const fixture_t = struct {
    fn function(comptime target: anytype) *const @TypeOf(target) {
        var pointer: *const @TypeOf(target) = target;
        return @as(*volatile @TypeOf(pointer), &pointer).*;
    }
};
test "all seven transfer commands match independent pinned encoder at maximum count" {
    const layers = c.VkImageSubresourceLayers{ .aspectMask = 1, .mipLevel = 3, .baseArrayLayer = 4, .layerCount = 2 };
    const extent = c.VkExtent3D{ .width = 16, .height = 32, .depth = 1 };
    const offset = c.VkOffset3D{ .x = 2, .y = 3, .z = 0 };
    const copies = [_]c.VkImageCopy{.{ .srcSubresource = layers, .srcOffset = offset, .dstSubresource = layers, .dstOffset = offset, .extent = extent }} ** MaxRegions;
    const blits = [_]c.VkImageBlit{.{ .srcSubresource = layers, .srcOffsets = .{ .{ .x = 16, .y = 32, .z = 1 }, .{ .x = 0, .y = 0, .z = 0 } }, .dstSubresource = layers, .dstOffsets = .{ .{ .x = 0, .y = 0, .z = 0 }, .{ .x = 32, .y = 16, .z = 1 } } }} ** MaxRegions;
    const uploads = [_]c.VkBufferImageCopy{.{ .bufferOffset = 1024, .bufferRowLength = 64, .bufferImageHeight = 64, .imageSubresource = layers, .imageOffset = offset, .imageExtent = extent }} ** MaxRegions;
    const resolves = [_]c.VkImageResolve{.{ .srcSubresource = layers, .srcOffset = offset, .dstSubresource = layers, .dstOffset = offset, .extent = extent }} ** MaxRegions;
    const color_ranges = [_]c.VkImageSubresourceRange{.{ .aspectMask = 1, .baseMipLevel = 2, .levelCount = 3, .baseArrayLayer = 4, .layerCount = 5 }} ** MaxRegions;
    const depth_ranges = [_]c.VkImageSubresourceRange{.{ .aspectMask = 6, .baseMipLevel = 2, .levelCount = 3, .baseArrayLayer = 4, .layerCount = 5 }} ** MaxRegions;
    const color = [4]u32{ 0x7fc01234, 0xffffffff, 0x3f800000, 0x80000000 };
    const depth = c.VkClearDepthStencilValue{ .depth = 0.75, .stencil = 0xffffffff };
    for ([_]usize{ 1, 2, MaxRegions }) |count| {
        try compare(try fixture_t.function(copy_image)(8, 42, 1, 43, 7, copies[0..count]), 0, count, &copies, &color);
        try compare(try fixture_t.function(blit_image)(8, 42, 1, 43, 7, blits[0..count], 1), 1, count, &blits, &color);
        try compare(try fixture_t.function(copy_buffer_to_image)(8, 42, 43, 7, uploads[0..count]), 2, count, &uploads, &color);
        try compare(try fixture_t.function(copy_image_to_buffer)(8, 42, 43, 1, uploads[0..count]), 6, count, &uploads, &color);
        try compare(try fixture_t.function(clear_color)(8, 42, 7, color, color_ranges[0..count]), 3, count, &color_ranges, &color);
        try compare(try fixture_t.function(clear_depth_stencil)(8, 42, 7, depth, depth_ranges[0..count]), 4, count, &depth_ranges, &depth);
        try compare(try fixture_t.function(resolve_image)(8, 42, 1, 43, 7, resolves[0..count]), 5, count, &resolves, &color);
    }
}
test "invalid transfer shape rejected before serialization" {
    const layers = c.VkImageSubresourceLayers{ .aspectMask = 1, .layerCount = 1 };
    const copy = c.VkImageCopy{ .srcSubresource = layers, .dstSubresource = layers, .extent = .{ .width = 1, .height = 1, .depth = 1 } };
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(0, 42, 1, 43, 7, &.{copy}));
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(8, 42, 7, 43, 7, &.{copy}));
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(8, 42, 1, 43, 7, &.{}));
    const too_many = [_]c.VkImageCopy{copy} ** (MaxRegions + 1);
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(8, 42, 1, 43, 7, &too_many));
    var invalid = copy;
    invalid.srcSubresource.layerCount = 0;
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(8, 42, 1, 43, 7, &.{invalid}));
    invalid = copy;
    invalid.dstSubresource.aspectMask = 2;
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(8, 42, 1, 43, 7, &.{invalid}));
    invalid = copy;
    invalid.dstOffset.x = -1;
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(8, 42, 1, 43, 7, &.{invalid}));
    invalid = copy;
    invalid.extent.depth = 0;
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(8, 42, 1, 43, 7, &.{invalid}));
    const range = c.VkImageSubresourceRange{ .aspectMask = 1, .levelCount = 1, .layerCount = 1 };
    try std.testing.expectError(error.Invalid, fixture_t.function(clear_color)(8, 42, 6, .{ 0, 0, 0, 0 }, &.{range}));
    try std.testing.expectError(error.Invalid, fixture_t.function(clear_depth_stencil)(8, 42, 7, .{ .depth = 0.5 }, &.{range}));
    const depth_range = c.VkImageSubresourceRange{ .aspectMask = 2, .levelCount = 1, .layerCount = 1 };
    try std.testing.expectError(error.Invalid, fixture_t.function(clear_depth_stencil)(8, 42, 7, .{ .depth = std.math.nan(f32) }, &.{depth_range}));
    try std.testing.expectError(error.Invalid, fixture_t.function(clear_depth_stencil)(8, 42, 7, .{ .depth = 1.1 }, &.{depth_range}));
}

test "image copy subresource mismatch offsets and clear depth upper bound reject" {
    const layers: c.VkImageSubresourceLayers = .{ .aspectMask = 1, .layerCount = 1 };
    const initial: c.VkImageCopy = .{ .srcSubresource = layers, .dstSubresource = layers, .extent = .{ .width = 1, .height = 1, .depth = 1 } };
    var region = initial;
    region.dstSubresource.layerCount = 0;
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(7, 42, 6, 43, 7, &.{region}));
    region = initial;
    region.dstSubresource.aspectMask = 2;
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(7, 42, 6, 43, 7, &.{region}));
    region = initial;
    region.dstSubresource.layerCount = 2;
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(7, 42, 6, 43, 7, &.{region}));
    region = initial;
    region.dstOffset.z = -1;
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(7, 42, 6, 43, 7, &.{region}));
    const range: c.VkImageSubresourceRange = .{ .aspectMask = 2, .levelCount = 1, .layerCount = 1 };
    try std.testing.expectError(error.Invalid, fixture_t.function(clear_depth_stencil)(7, 42, 7, .{ .depth = 2, .stencil = 0 }, &.{range}));
    var invalid_range = range;
    invalid_range.layerCount = 0;
    try std.testing.expectError(error.Invalid, fixture_t.function(clear_depth_stencil)(7, 42, 7, .{ .depth = 1, .stencil = 0 }, &.{invalid_range}));
    const resolve: c.VkImageResolve = .{ .srcSubresource = layers, .dstSubresource = .{ .aspectMask = 2, .layerCount = 1 }, .extent = initial.extent };
    try std.testing.expectError(error.Invalid, fixture_t.function(resolve_image)(7, 42, 6, 43, 7, &.{resolve}));
}

test "all transfer entrypoints reject missing owners layouts quotas and negative y offsets" {
    const layers: c.VkImageSubresourceLayers = .{ .aspectMask = 1, .layerCount = 1 };
    const offset: c.VkOffset3D = .{ .x = 0, .y = 0, .z = 0 };
    const extent: c.VkExtent3D = .{ .width = 1, .height = 1, .depth = 1 };
    const blit: c.VkImageBlit = .{ .srcSubresource = layers, .dstSubresource = layers, .srcOffsets = .{ offset, .{ .x = 1, .y = 1, .z = 1 } }, .dstOffsets = .{ offset, .{ .x = 1, .y = 1, .z = 1 } } };
    const upload: c.VkBufferImageCopy = .{ .imageSubresource = layers, .imageExtent = extent };
    const resolve: c.VkImageResolve = .{ .srcSubresource = layers, .dstSubresource = layers, .extent = extent };
    const color_range: c.VkImageSubresourceRange = .{ .aspectMask = 1, .levelCount = 1, .layerCount = 1 };
    const depth_range: c.VkImageSubresourceRange = .{ .aspectMask = 2, .levelCount = 1, .layerCount = 1 };
    for ([_][3]u64{ .{ 0, 42, 43 }, .{ 8, 0, 43 }, .{ 8, 42, 0 } }) |ids| {
        try std.testing.expectError(error.Invalid, fixture_t.function(blit_image)(ids[0], ids[1], 6, ids[2], 7, &.{blit}, 0));
        try std.testing.expectError(error.Invalid, fixture_t.function(copy_buffer_to_image)(ids[0], ids[1], ids[2], 7, &.{upload}));
        try std.testing.expectError(error.Invalid, fixture_t.function(copy_image_to_buffer)(ids[0], ids[1], ids[2], 6, &.{upload}));
        try std.testing.expectError(error.Invalid, fixture_t.function(resolve_image)(ids[0], ids[1], 6, ids[2], 7, &.{resolve}));
    }
    try std.testing.expectError(error.Invalid, fixture_t.function(blit_image)(8, 42, 7, 43, 7, &.{blit}, 0));
    try std.testing.expectError(error.Invalid, fixture_t.function(blit_image)(8, 42, 6, 43, 6, &.{blit}, 0));
    try std.testing.expectError(error.Invalid, fixture_t.function(resolve_image)(8, 42, 7, 43, 7, &.{resolve}));
    try std.testing.expectError(error.Invalid, fixture_t.function(resolve_image)(8, 42, 6, 43, 6, &.{resolve}));
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_buffer_to_image)(8, 42, 43, 6, &.{upload}));
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image_to_buffer)(8, 42, 43, 7, &.{upload}));
    for ([_][2]u64{ .{ 0, 42 }, .{ 8, 0 } }) |ids| {
        try std.testing.expectError(error.Invalid, fixture_t.function(clear_color)(ids[0], ids[1], 7, .{ 0, 0, 0, 0 }, &.{color_range}));
        try std.testing.expectError(error.Invalid, fixture_t.function(clear_depth_stencil)(ids[0], ids[1], 7, .{ .depth = 0.5 }, &.{depth_range}));
    }
    try std.testing.expectError(error.Invalid, fixture_t.function(clear_color)(8, 42, 7, .{ 0, 0, 0, 0 }, &.{}));
    const too_many = [_]c.VkImageSubresourceRange{color_range} ** 65;
    try std.testing.expectError(error.Invalid, fixture_t.function(clear_color)(8, 42, 7, .{ 0, 0, 0, 0 }, &too_many));
    try std.testing.expectError(error.Invalid, fixture_t.function(clear_depth_stencil)(8, 42, 6, .{ .depth = 0.5 }, &.{depth_range}));
    var copy: c.VkImageCopy = .{ .srcSubresource = layers, .dstSubresource = layers, .extent = extent, .srcOffset = .{ .x = 0, .y = -1, .z = 0 } };
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(8, 42, 6, 43, 7, &.{copy}));
    copy.srcOffset.y = 0;
    copy.srcOffset.z = -1;
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image)(8, 42, 6, 43, 7, &.{copy}));
}

test "validated slice array copy packet preserves native region against encoder" {
    var request = c.VkImageCopy{ .srcSubresource = .{ .aspectMask = 1, .layerCount = 1 }, .dstSubresource = .{ .aspectMask = 1, .baseArrayLayer = 2, .layerCount = 3 }, .srcOffset = .{ .z = 1 }, .extent = .{ .width = 8, .height = 8, .depth = 3 } };
    const clear = [_]u32{0} ** 4;
    try compare(try fixture_t.function(copy_image_validated )(8, 42, 1, 43, 7, &.{request}), 0, 1, &request, &clear);
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image )(8, 42, 1, 43, 7, &.{request}));
    request.dstSubresource.layerCount = 0;
    try std.testing.expectError(error.Invalid, fixture_t.function(copy_image_validated )(8, 42, 1, 43, 7, &.{request}));
}
