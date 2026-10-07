//! Pure bounded CopyCommands2 normalization. Core wrappers translate opaque copied
//! handles then reuse semantically identical classic Vulkan transfer commands.
const std = @import("std");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Maximum copied modern regions; no heap/global storage.
pub const MaxRegions = 64;
/// Owned legacy buffer-copy request; opaque handles copied, never dereferenced.
pub const buffer_copy_t = struct { source: c.VkBuffer, target: c.VkBuffer, count: u32, regions: [MaxRegions]c.VkBufferCopy };
/// Owned legacy image-copy request; all input pointers removed.
pub const image_copy_t = struct { source: c.VkImage, source_layout: u32, target: c.VkImage, target_layout: u32, count: u32, regions: [MaxRegions]c.VkImageCopy };
/// Owned legacy image-blit request; complete copied named fields, no pointer retention.
pub const image_blit_t = struct { source: c.VkImage, source_layout: u32, target: c.VkImage, target_layout: u32, filter: u32, count: u32, regions: [MaxRegions]c.VkImageBlit };
/// Owned buffer/image transfer request; source/target direction follows its normalizer.
pub const buffer_image_t = struct { buffer: c.VkBuffer, image: c.VkImage, layout: u32, count: u32, regions: [MaxRegions]c.VkBufferImageCopy };
/// Owned legacy image resolve request; copied handles require core ownership resolution.
pub const image_resolve_t = struct { source: c.VkImage, source_layout: u32, target: c.VkImage, target_layout: u32, count: u32, regions: [MaxRegions]c.VkImageResolve };
fn pointer_address(field: anytype) usize {
    // Read untrusted native pointer bits as an integer before constructing an aligned
    // Zig pointer. Typed-pointer alignment assumptions must not erase this check.
    return @as(*align(1) const usize, @ptrCast(field)).*;
}
fn regions(comptime value_t: type, address: usize, count: u32, tag: u32) ![]const value_t {
    if (count == 0 or count > MaxRegions or address == 0 or address % @alignOf(value_t) != 0) return error.Invalid;
    const pointer: [*]const value_t = @ptrFromInt(address);
    const records = pointer[0..count];
    for (records) |record| if (record.sType != tag or record.pNext != null) return error.Invalid;
    return records;
}
fn valid_outer(info: anytype, tag: u32) bool {
    return info.sType == tag and info.pNext == null;
}
/// [in] info nonnull accessible immutable native record/regions through return.
/// [out] owns all copied named buffer region fields; unused suffix unspecified.
/// Invalid rejects identity/tag/chain/array quota/alignment before reading region payload.
/// Caller validates ownership, bounds, command state and core feature; no allocation,
/// output mutation, retained pointer or shared state; independent calls thread-safe.
pub fn copy_buffer(info: *const c.VkCopyBufferInfo2) !buffer_copy_t {
    if (!valid_outer(info.*, c.VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2) or info.srcBuffer == null or info.dstBuffer == null) return error.Invalid;
    const input = try regions(c.VkBufferCopy2, pointer_address(&info.pRegions), info.regionCount, c.VK_STRUCTURE_TYPE_BUFFER_COPY_2);
    var result = buffer_copy_t{ .source = info.srcBuffer, .target = info.dstBuffer, .count = info.regionCount, .regions = undefined };
    for (input, 0..) |record, index| result.regions[index] = .{ .srcOffset = record.srcOffset, .dstOffset = record.dstOffset, .size = record.size };
    return result;
}
/// Normalize CopyImage2 into owned classic named records. Borrowed nonnull input and
/// reachable arrays immutable/accessible through call; no pointers retained. Returns
/// Invalid for malformed topology/identity/quota/alignment, no allocation/shared state.
/// Caller validates exact resources, ranges/layouts and enabled core before publication.
pub fn copy_image(info: *const c.VkCopyImageInfo2) !image_copy_t {
    if (!valid_outer(info.*, c.VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2) or info.srcImage == null or info.dstImage == null) return error.Invalid;
    const input = try regions(c.VkImageCopy2, pointer_address(&info.pRegions), info.regionCount, c.VK_STRUCTURE_TYPE_IMAGE_COPY_2);
    var result = image_copy_t{ .source = info.srcImage, .source_layout = info.srcImageLayout, .target = info.dstImage, .target_layout = info.dstImageLayout, .count = info.regionCount, .regions = undefined };
    for (input, 0..) |record, index| result.regions[index] = .{ .srcSubresource = record.srcSubresource, .srcOffset = record.srcOffset, .dstSubresource = record.dstSubresource, .dstOffset = record.dstOffset, .extent = record.extent };
    return result;
}
/// Normalize BlitImage2, preserving reversed endpoints and full filter value. Borrowed
/// immutable nonnull input, no pointer retention or allocations. Invalid for malformed
/// topology/identity/quota/alignment; caller validates semantic bounds/filter and ownership.
pub fn blit_image(info: *const c.VkBlitImageInfo2) !image_blit_t {
    if (!valid_outer(info.*, c.VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2) or info.srcImage == null or info.dstImage == null) return error.Invalid;
    const input = try regions(c.VkImageBlit2, pointer_address(&info.pRegions), info.regionCount, c.VK_STRUCTURE_TYPE_IMAGE_BLIT_2);
    var result = image_blit_t{ .source = info.srcImage, .source_layout = info.srcImageLayout, .target = info.dstImage, .target_layout = info.dstImageLayout, .filter = info.filter, .count = info.regionCount, .regions = undefined };
    for (input, 0..) |record, index| result.regions[index] = .{ .srcSubresource = record.srcSubresource, .srcOffsets = record.srcOffsets, .dstSubresource = record.dstSubresource, .dstOffsets = record.dstOffsets };
    return result;
}
fn buffer_image_regions(address: usize, count: u32, result: *buffer_image_t) !void {
    const input = try regions(c.VkBufferImageCopy2, address, count, c.VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2);
    for (input, 0..) |record, index| result.regions[index] = .{ .bufferOffset = record.bufferOffset, .bufferRowLength = record.bufferRowLength, .bufferImageHeight = record.bufferImageHeight, .imageSubresource = record.imageSubresource, .imageOffset = record.imageOffset, .imageExtent = record.imageExtent };
}
/// Normalize CopyBufferToImage2 with owned copied records, preserving every named field.
/// Borrowed nonnull immutable input; Invalid for malformed topology/identity/quota/alignment.
/// No allocation/retention/shared state; caller validates exact image/buffer bounds/usages.
pub fn copy_buffer_to_image(info: *const c.VkCopyBufferToImageInfo2) !buffer_image_t {
    if (!valid_outer(info.*, c.VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2) or info.srcBuffer == null or info.dstImage == null) return error.Invalid;
    var result = buffer_image_t{ .buffer = info.srcBuffer, .image = info.dstImage, .layout = info.dstImageLayout, .count = info.regionCount, .regions = undefined };
    try buffer_image_regions(pointer_address(&info.pRegions), info.regionCount, &result);
    return result;
}
/// Normalize CopyImageToBuffer2 with owned copied records. Input borrows accessible
/// immutable arrays; no allocation/retention/output writes. Invalid for malformed topology,
/// identity/quota/alignment. Caller validates actual ownership/ranges/layouts/usages.
pub fn copy_image_to_buffer(info: *const c.VkCopyImageToBufferInfo2) !buffer_image_t {
    if (!valid_outer(info.*, c.VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2) or info.srcImage == null or info.dstBuffer == null) return error.Invalid;
    var result = buffer_image_t{ .buffer = info.dstBuffer, .image = info.srcImage, .layout = info.srcImageLayout, .count = info.regionCount, .regions = undefined };
    try buffer_image_regions(pointer_address(&info.pRegions), info.regionCount, &result);
    return result;
}
/// Normalize ResolveImage2 with owned named fields. Input and arrays borrowed immutable;
/// no allocations/pointer retention/output writes. Invalid for malformed topology/identity/
/// quota/alignment; caller verifies actual sample counts, formats, ownership and ranges.
pub fn resolve_image(info: *const c.VkResolveImageInfo2) !image_resolve_t {
    if (!valid_outer(info.*, c.VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2) or info.srcImage == null or info.dstImage == null) return error.Invalid;
    const input = try regions(c.VkImageResolve2, pointer_address(&info.pRegions), info.regionCount, c.VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2);
    var result = image_resolve_t{ .source = info.srcImage, .source_layout = info.srcImageLayout, .target = info.dstImage, .target_layout = info.dstImageLayout, .count = info.regionCount, .regions = undefined };
    for (input, 0..) |record, index| result.regions[index] = .{ .srcSubresource = record.srcSubresource, .srcOffset = record.srcOffset, .dstSubresource = record.dstSubresource, .dstOffset = record.dstOffset, .extent = record.extent };
    return result;
}

test "every CopyCommands2 family preserves independent named fields at maximum quota" {
    var buffer_regions: [64]c.VkBufferCopy2 = undefined;
    var image_regions: [64]c.VkImageCopy2 = undefined;
    var blit_regions: [64]c.VkImageBlit2 = undefined;
    var upload_regions: [64]c.VkBufferImageCopy2 = undefined;
    var resolve_regions: [64]c.VkImageResolve2 = undefined;
    const layers = c.VkImageSubresourceLayers{ .aspectMask = 1, .mipLevel = 2, .baseArrayLayer = 3, .layerCount = 4 };
    for (0..64) |index| {
        const offset = c.VkOffset3D{ .x = @intCast(index), .y = 17, .z = 23 };
        const extent = c.VkExtent3D{ .width = @intCast(index + 1), .height = 11, .depth = 7 };
        buffer_regions[index] = .{ .sType = c.VK_STRUCTURE_TYPE_BUFFER_COPY_2, .srcOffset = index * 16, .dstOffset = index * 32, .size = 64 + index };
        image_regions[index] = .{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_COPY_2, .srcSubresource = layers, .srcOffset = offset, .dstSubresource = layers, .dstOffset = .{ .x = 8, .y = 9, .z = 10 }, .extent = extent };
        blit_regions[index] = .{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_BLIT_2, .srcSubresource = layers, .srcOffsets = .{ offset, .{ .x = 0, .y = 0, .z = 0 } }, .dstSubresource = layers, .dstOffsets = .{ .{ .x = 1, .y = 2, .z = 3 }, .{ .x = 4, .y = 5, .z = 6 } } };
        upload_regions[index] = .{ .sType = c.VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, .bufferOffset = index * 64, .bufferRowLength = 512, .bufferImageHeight = 256, .imageSubresource = layers, .imageOffset = offset, .imageExtent = extent };
        resolve_regions[index] = .{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2, .srcSubresource = layers, .srcOffset = offset, .dstSubresource = layers, .dstOffset = .{ .x = 8, .y = 9, .z = 10 }, .extent = extent };
    }
    const buffer = try copy_buffer(&.{ .sType = c.VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2, .srcBuffer = @ptrFromInt(42), .dstBuffer = @ptrFromInt(43), .regionCount = 64, .pRegions = &buffer_regions });
    const image = try copy_image(&.{ .sType = c.VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2, .srcImage = @ptrFromInt(42), .dstImage = @ptrFromInt(43), .srcImageLayout = 1, .dstImageLayout = 7, .regionCount = 64, .pRegions = &image_regions });
    const blit = try blit_image(&.{ .sType = c.VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2, .srcImage = @ptrFromInt(42), .dstImage = @ptrFromInt(43), .srcImageLayout = 1, .dstImageLayout = 7, .filter = 1, .regionCount = 64, .pRegions = &blit_regions });
    const upload = try copy_buffer_to_image(&.{ .sType = c.VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, .srcBuffer = @ptrFromInt(42), .dstImage = @ptrFromInt(43), .dstImageLayout = 7, .regionCount = 64, .pRegions = &upload_regions });
    const download = try copy_image_to_buffer(&.{ .sType = c.VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, .srcImage = @ptrFromInt(43), .dstBuffer = @ptrFromInt(42), .srcImageLayout = 1, .regionCount = 64, .pRegions = &upload_regions });
    const resolve = try resolve_image(&.{ .sType = c.VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2, .srcImage = @ptrFromInt(42), .dstImage = @ptrFromInt(43), .srcImageLayout = 1, .dstImageLayout = 7, .regionCount = 64, .pRegions = &resolve_regions });
    try std.testing.expectEqual(@as(usize, 42), @intFromPtr(buffer.source.?));
    try std.testing.expectEqual(@as(u32, 64), resolve.count);
    for (0..64) |index| {
        try std.testing.expectEqual(@as(u64, index * 16), buffer.regions[index].srcOffset);
        try std.testing.expectEqual(@as(u64, index * 32), buffer.regions[index].dstOffset);
        try std.testing.expectEqual(@as(u64, 64 + index), buffer.regions[index].size);
        inline for (.{ image.regions[index], resolve.regions[index] }) |region| {
            try std.testing.expectEqualDeep(layers, region.srcSubresource);
            try std.testing.expectEqualDeep(layers, region.dstSubresource);
            try std.testing.expectEqual(@as(i32, @intCast(index)), region.srcOffset.x);
            try std.testing.expectEqual(@as(i32, 9), region.dstOffset.y);
            try std.testing.expectEqual(@as(u32, @intCast(index + 1)), region.extent.width);
        }
        try std.testing.expectEqual(@as(i32, @intCast(index)), blit.regions[index].srcOffsets[0].x);
        try std.testing.expectEqual(@as(i32, 0), blit.regions[index].srcOffsets[1].y);
        try std.testing.expectEqual(@as(i32, 6), blit.regions[index].dstOffsets[1].z);
        try std.testing.expectEqualDeep(upload.regions[index], download.regions[index]);
        try std.testing.expectEqual(@as(u64, index * 64), upload.regions[index].bufferOffset);
        try std.testing.expectEqual(@as(u32, 512), upload.regions[index].bufferRowLength);
        try std.testing.expectEqualDeep(layers, upload.regions[index].imageSubresource);
    }
}
test "malformed modern topology rejects before unread quota pointers and frees native input owners" {
    const records = try std.testing.allocator.alloc(c.VkBufferCopy2, 64);
    defer std.testing.allocator.free(records);
    for (records) |*record| record.* = .{ .sType = c.VK_STRUCTURE_TYPE_BUFFER_COPY_2, .size = 16 };
    var info = c.VkCopyBufferInfo2{ .sType = c.VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2, .srcBuffer = @ptrFromInt(42), .dstBuffer = @ptrFromInt(43), .regionCount = 64, .pRegions = records.ptr };
    _ = try copy_buffer(&info);
    records[63].pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, copy_buffer(&info));
    records[63].pNext = null;
    records[63].sType = 0;
    try std.testing.expectError(error.Invalid, copy_buffer(&info));
    records[63].sType = c.VK_STRUCTURE_TYPE_BUFFER_COPY_2;
    info.regionCount = 65;
    info.pRegions = @ptrFromInt(8);
    try std.testing.expectError(error.Invalid, copy_buffer(&info));
    info.regionCount = 1;
    var misaligned: usize = 1;
    @memcpy(std.mem.asBytes(&info.pRegions), std.mem.asBytes(&misaligned));
    try std.testing.expectError(error.Invalid, copy_buffer(&info));
    info.pRegions = null;
    try std.testing.expectError(error.Invalid, copy_buffer(&info));
    info.pRegions = records.ptr;
    info.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, copy_buffer(&info));
    info.pNext = null;
    info.srcBuffer = null;
    try std.testing.expectError(error.Invalid, copy_buffer(&info));
}
