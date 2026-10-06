//! Allocation-free core image wire encoding. Callers validate object ownership and GPU lifetimes.
const std = @import("std");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Maximum owned encoded packet bytes; no allocation or mutable shared storage.
pub const MaxBytes: usize = 8192;
/// Owned bounded scratch writer. All methods borrow exclusive self for the call; no pointers retained.
/// Thread-safe for distinct writers; Invalid/Limit errors never escape a partial packet to the peer.
pub const writer_t = struct {
    /// Owned initialized prefix plus unused scratch; lifetime equals writer lifetime.
    bytes: [MaxBytes]u8 = undefined,
    /// Initialized prefix length, always bounded by bytes.len.
    used: usize = 0,
    /// Append a little-endian scalar. [in,out] self exclusive; [in] word by value.
    /// Returns Limit without writing when capacity exhausted; no allocation or ownership transfer.
    pub fn put(self: *writer_t, comptime word_t: type, word: word_t) !void {
        const size = @sizeOf(word_t);
        if (self.used > self.bytes.len or size > self.bytes.len - self.used) return error.Limit;
        std.mem.writeInt(word_t, self.bytes[self.used..][0..size], word, .little);
        self.used += size;
    }
    /// Append synchronous command header. [in,out] self exclusive; [in] id nonzero host ID.
    /// Returns Invalid for zero ID or Limit; no pointers retained, allocation or locking.
    pub fn header(self: *writer_t, command_id: u32, id: u64) !void {
        if (id == 0) return error.Invalid;
        try self.put(u32, command_id);
        try self.put(u32, 1);
        try self.put(u64, id);
    }
};
fn finish_create(writer: *writer_t, id: u64) !void {
    if (id == 0) return error.Invalid;
    try writer.put(u64, 0);
    try writer.put(u64, 1);
    try writer.put(u64, id);
}
fn range_valid(range: c.VkImageSubresourceRange) bool {
    return range.aspectMask != 0 and range.aspectMask & ~@as(u32, 7) == 0 and
        range.levelCount != 0 and range.layerCount != 0;
}
fn encode_range(writer: *writer_t, range: c.VkImageSubresourceRange) !void {
    try writer.put(u32, range.aspectMask);
    try writer.put(u32, range.baseMipLevel);
    try writer.put(u32, range.levelCount);
    try writer.put(u32, range.baseArrayLayer);
    try writer.put(u32, range.layerCount);
}
/// Encode core vkCreateImage. [in] info nonnull accessible native record, families borrowed for call.
/// [in] device_id/image_id nonzero translated host identities, never dereferenced.
/// Returns owned packet or Invalid/Limit; no allocation, ownership transfer or shared state.
/// Caller validates host-supported format, dimension limits and configured queue families before use.
pub fn create_image(info: *const c.VkImageCreateInfo, device_id: u64, image_id: u64) !writer_t {
    if (info.sType != c.VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO or info.pNext != null or
        info.flags != 0 or info.imageType > 2 or info.format <= 0 or info.format > 184 or
        info.extent.width == 0 or info.extent.height == 0 or info.extent.depth == 0 or
        info.mipLevels == 0 or info.arrayLayers == 0 or info.samples == 0 or
        info.samples > 64 or info.samples & (info.samples - 1) != 0 or info.tiling > 1 or
        info.usage == 0 or info.usage & ~@as(u32, 0xff) != 0 or
        (info.initialLayout != c.VK_IMAGE_LAYOUT_UNDEFINED and
        info.initialLayout != c.VK_IMAGE_LAYOUT_PREINITIALIZED)) return error.Invalid;
    if ((info.imageType == 0 and (info.extent.height != 1 or info.extent.depth != 1)) or
        (info.imageType == 1 and info.extent.depth != 1) or
        (info.imageType == 2 and info.arrayLayers != 1)) return error.Invalid;
    var family_count: u32 = 0;
    if (info.sharingMode == c.VK_SHARING_MODE_CONCURRENT) {
        family_count = info.queueFamilyIndexCount;
        if (family_count < 2 or family_count > 16 or info.pQueueFamilyIndices == null) return error.Invalid;
        for (info.pQueueFamilyIndices[0..family_count], 0..) |family, index| {
            if (std.mem.indexOfScalar(u32, info.pQueueFamilyIndices[0..index], family) != null) return error.Invalid;
        }
    } else if (info.sharingMode != c.VK_SHARING_MODE_EXCLUSIVE) return error.Invalid;
    var writer = writer_t{};
    try writer.header(54, device_id);
    try writer.put(u64, 1);
    try writer.put(u32, c.VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    try writer.put(u64, 0);
    try writer.put(u32, info.flags);
    try writer.put(u32, info.imageType);
    try writer.put(u32, @intCast(info.format));
    try writer.put(u32, info.extent.width);
    try writer.put(u32, info.extent.height);
    try writer.put(u32, info.extent.depth);
    try writer.put(u32, info.mipLevels);
    try writer.put(u32, info.arrayLayers);
    try writer.put(u32, info.samples);
    try writer.put(u32, info.tiling);
    try writer.put(u32, info.usage);
    try writer.put(u32, info.sharingMode);
    try writer.put(u32, family_count);
    try writer.put(u64, family_count);
    if (family_count != 0) for (info.pQueueFamilyIndices[0..family_count]) |family| try writer.put(u32, family);
    try writer.put(u32, info.initialLayout);
    try finish_create(&writer, image_id);
    return writer;
}
/// Encode core vkCreateImageView. [in] info borrowed canonical record; native image handle ignored.
/// [in] IDs nonzero translated host identities. Returns owned packet or Invalid/Limit.
/// No allocation/retention; caller validates format compatibility and range against image metadata.
pub fn create_image_view(info: *const c.VkImageViewCreateInfo, device_id: u64, image_id: u64, view_id: u64) !writer_t {
    if (info.sType != c.VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO or info.pNext != null or
        info.flags != 0 or info.viewType > 6 or info.format <= 0 or info.format > 184 or
        image_id == 0 or !range_valid(info.subresourceRange)) return error.Invalid;
    const components = [_]u32{ info.components.r, info.components.g, info.components.b, info.components.a };
    for (components) |component| if (component > 6) return error.Invalid;
    var writer = writer_t{};
    try writer.header(57, device_id);
    try writer.put(u64, 1);
    try writer.put(u32, c.VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    try writer.put(u64, 0);
    try writer.put(u32, 0);
    try writer.put(u64, image_id);
    try writer.put(u32, info.viewType);
    try writer.put(u32, @intCast(info.format));
    for (components) |component| try writer.put(u32, component);
    try encode_range(&writer, info.subresourceRange);
    try finish_create(&writer, view_id);
    return writer;
}
/// Append core image barrier struct. [in,out] writer exclusive; [in] info borrowed; image_id translated.
/// Returns Invalid/Limit; caller discards partial packet on error. No pointers retained or allocations.
/// Caller validates queue families, bound memory and ranges, and records lifetime references.
pub fn image_barrier(writer: *writer_t, info: *const c.VkImageMemoryBarrier, image_id: u64) !void {
    if (info.sType != c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER or info.pNext != null or image_id == 0 or
        (info.srcAccessMask | info.dstAccessMask) & ~@as(u32, 0x1ffff) != 0 or
        info.oldLayout > 8 or info.newLayout > 8 or info.newLayout == c.VK_IMAGE_LAYOUT_UNDEFINED or
        info.newLayout == c.VK_IMAGE_LAYOUT_PREINITIALIZED or !range_valid(info.subresourceRange)) return error.Invalid;
    try writer.put(u32, c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    try writer.put(u64, 0);
    try writer.put(u32, info.srcAccessMask);
    try writer.put(u32, info.dstAccessMask);
    try writer.put(u32, info.oldLayout);
    try writer.put(u32, info.newLayout);
    try writer.put(u32, info.srcQueueFamilyIndex);
    try writer.put(u32, info.dstQueueFamilyIndex);
    try writer.put(u64, image_id);
    try encode_range(writer, info.subresourceRange);
}

extern fn venus_render_test_image(*const c.VkImageCreateInfo, [*]u8) usize;
extern fn venus_render_test_view(*const c.VkImageViewCreateInfo, [*]u8) usize;
extern fn venus_render_test_barrier(*const c.VkImageMemoryBarrier, [*]u8) usize;
fn image_fixture() c.VkImageCreateInfo {
    return .{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = c.VK_IMAGE_TYPE_2D, .format = c.VK_FORMAT_R8G8B8A8_UNORM, .extent = .{ .width = 32, .height = 16, .depth = 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = 1, .usage = c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
}
test "image packets match independent pinned serializer for sharing modes" {
    var info = image_fixture();
    var expected: [MaxBytes]u8 = undefined;
    const exclusive = try create_image(&info, 7, 42);
    var count = venus_render_test_image(&info, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], exclusive.bytes[0..exclusive.used]);
    const families = [_]u32{ 2, 5 };
    info.sharingMode = c.VK_SHARING_MODE_CONCURRENT;
    info.queueFamilyIndexCount = families.len;
    info.pQueueFamilyIndices = &families;
    const concurrent = try create_image(&info, 7, 42);
    count = venus_render_test_image(&info, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], concurrent.bytes[0..concurrent.used]);
}
test "image scalar and pointer boundaries reject malformed records" {
    const initial = image_fixture();
    inline for (.{ "sType", "imageType", "format", "samples", "tiling", "usage", "sharingMode", "initialLayout" }) |name| {
        var info = initial;
        @field(info, name) = 0xffffffff;
        try std.testing.expectError(error.Invalid, create_image(&info, 7, 42));
    }
    inline for (.{ "mipLevels", "arrayLayers", "samples", "usage" }) |name| {
        var info = initial;
        @field(info, name) = 0;
        try std.testing.expectError(error.Invalid, create_image(&info, 7, 42));
    }
    var info = initial;
    info.extent.depth = 2;
    try std.testing.expectError(error.Invalid, create_image(&info, 7, 42));
    info = initial;
    info.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, create_image(&info, 7, 42));
    info = initial;
    info.flags = 1;
    try std.testing.expectError(error.Invalid, create_image(&info, 7, 42));
    info = initial;
    info.samples = 3;
    try std.testing.expectError(error.Invalid, create_image(&info, 7, 42));
    info = initial;
    info.sharingMode = c.VK_SHARING_MODE_CONCURRENT;
    for ([_]u32{ 0, 1, 17 }) |count| {
        info.queueFamilyIndexCount = count;
        try std.testing.expectError(error.Invalid, create_image(&info, 7, 42));
    }
    info.queueFamilyIndexCount = 2;
    try std.testing.expectError(error.Invalid, create_image(&info, 7, 42));
    const repeated = [_]u32{ 1, 1 };
    info.pQueueFamilyIndices = &repeated;
    try std.testing.expectError(error.Invalid, create_image(&info, 7, 42));
    try std.testing.expectError(error.Invalid, create_image(&initial, 0, 42));
    try std.testing.expectError(error.Invalid, create_image(&initial, 7, 0));
}
test "view and image barrier packets match independent pinned serializer" {
    var expected: [MaxBytes]u8 = undefined;
    const range: c.VkImageSubresourceRange = .{ .aspectMask = 1, .levelCount = 1, .layerCount = 1 };
    var info: c.VkImageViewCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = c.VK_IMAGE_VIEW_TYPE_2D, .format = c.VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = range };
    const writer = try create_image_view(&info, 7, 42, 43);
    const count = venus_render_test_view(&info, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
    var barrier: c.VkImageMemoryBarrier = .{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .newLayout = c.VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = 0xffffffff, .dstQueueFamilyIndex = 0xffffffff, .subresourceRange = range };
    var encoded = writer_t{};
    try image_barrier(&encoded, &barrier, 42);
    const barrier_count = venus_render_test_barrier(&barrier, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..barrier_count], encoded.bytes[0..encoded.used]);
    info.components.r = 7;
    try std.testing.expectError(error.Invalid, create_image_view(&info, 7, 42, 43));
    info.components.r = 0;
    info.subresourceRange.aspectMask = 8;
    try std.testing.expectError(error.Invalid, create_image_view(&info, 7, 42, 43));
    barrier.newLayout = c.VK_IMAGE_LAYOUT_UNDEFINED;
    try std.testing.expectError(error.Invalid, image_barrier(&encoded, &barrier, 42));
    barrier.newLayout = c.VK_IMAGE_LAYOUT_GENERAL;
    barrier.dstAccessMask = 0x20000;
    try std.testing.expectError(error.Invalid, image_barrier(&encoded, &barrier, 42));
}
test "writer rejects exhaustion before scalar writes without allocating" {
    var writer = writer_t{ .used = MaxBytes - 3 };
    try std.testing.expectError(error.Limit, writer.put(u32, 1));
    try std.testing.expectEqual(@as(usize, MaxBytes - 3), writer.used);
    writer.used = MaxBytes + 1;
    try std.testing.expectError(error.Limit, writer.put(u32, 1));
}
test "view and barrier negative scalar boundaries reject before serialization" {
    const range: c.VkImageSubresourceRange = .{ .aspectMask = 1, .levelCount = 1, .layerCount = 1 };
    const initial: c.VkImageViewCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = 1, .format = 37, .subresourceRange = range };
    inline for (.{ "sType", "flags", "viewType", "format" }) |name| {
        var info = initial;
        @field(info, name) = 0xffffffff;
        try std.testing.expectError(error.Invalid, create_image_view(&info, 7, 42, 43));
    }
    var info = initial;
    info.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, create_image_view(&info, 7, 42, 43));
    try std.testing.expectError(error.Invalid, create_image_view(&initial, 7, 0, 43));
    inline for (.{ "aspectMask", "levelCount", "layerCount" }) |name| {
        info = initial;
        @field(info.subresourceRange, name) = 0;
        try std.testing.expectError(error.Invalid, create_image_view(&info, 7, 42, 43));
    }
    const initial_barrier: c.VkImageMemoryBarrier = .{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .newLayout = 1, .subresourceRange = range };
    var writer = writer_t{};
    inline for (.{ "sType", "oldLayout", "newLayout", "srcAccessMask" }) |name| {
        var barrier = initial_barrier;
        @field(barrier, name) = 0xffffffff;
        try std.testing.expectError(error.Invalid, image_barrier(&writer, &barrier, 42));
    }
    var barrier = initial_barrier;
    barrier.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, image_barrier(&writer, &barrier, 42));
    barrier = initial_barrier;
    barrier.newLayout = c.VK_IMAGE_LAYOUT_PREINITIALIZED;
    try std.testing.expectError(error.Invalid, image_barrier(&writer, &barrier, 42));
    try std.testing.expectError(error.Invalid, image_barrier(&writer, &initial_barrier, 0));
    try std.testing.expectEqual(@as(usize, 0), writer.used);
    barrier = initial_barrier;
    writer.used = MaxBytes - 2;
    try std.testing.expectError(error.Limit, image_barrier(&writer, &barrier, 42));
}
