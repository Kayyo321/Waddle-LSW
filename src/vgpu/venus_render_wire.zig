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

/// Encode core shader creation from bounded SPIR-V. [in] info/code borrowed and accessible for call.
/// [in] IDs translated nonzero identities, never dereferenced. Returns owned packet or Invalid/Limit.
/// No allocation, locks or retained pointers. Validates structural word bounds; host validates semantics.
pub fn create_shader_module(info: *const c.VkShaderModuleCreateInfo, device_id: u64, module_id: u64) !writer_t {
    if (info.sType != c.VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO or info.pNext != null or
        info.flags != 0 or info.pCode == null or info.codeSize < 20 or info.codeSize % 4 != 0) return error.Invalid;
    if (info.codeSize > MaxBytes - 80) return error.Limit;
    const words = info.pCode[0 .. info.codeSize / 4];
    if (words[0] != 0x07230203 or words[1] & 0xff0000ff != 0 or
        (words[1] >> 16) & 0xff != 1 or (words[1] >> 8) & 0xff > 6 or
        words[3] == 0 or words[4] != 0) return error.Invalid;
    var cursor: usize = 5;
    while (cursor < words.len) {
        const count = words[cursor] >> 16;
        if (count == 0 or count > words.len - cursor) return error.Invalid;
        cursor += count;
    }
    var writer = writer_t{};
    try writer.header(59, device_id);
    try writer.put(u64, 1);
    try writer.put(u32, c.VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    try writer.put(u64, 0);
    try writer.put(u32, 0);
    try writer.put(u64, info.codeSize);
    try writer.put(u64, words.len);
    for (words) |word| try writer.put(u32, word);
    try finish_create(&writer, module_id);
    return writer;
}
extern fn venus_render_test_shader(*const c.VkShaderModuleCreateInfo, [*]u8) usize;
test "shader packets match pinned oracle and reject structural corruption" {
    var words = [_]u32{ 0x07230203, 0x00010000, 0, 1, 0, 0x00010000 };
    var info: c.VkShaderModuleCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words.len * 4, .pCode = &words };
    var expected: [MaxBytes]u8 = undefined;
    const writer = try create_shader_module(&info, 7, 42);
    const count = venus_render_test_shader(&info, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
    const original = words;
    for ([_]usize{ 0, 1, 3, 4, 5 }) |index| {
        words = original;
        words[index] = if (index == 3 or index == 5) 0 else 0xffffffff;
        try std.testing.expectError(error.Invalid, create_shader_module(&info, 7, 42));
    }
    words = original;
    words[5] = 0x00020000;
    try std.testing.expectError(error.Invalid, create_shader_module(&info, 7, 42));
    words = original;
    info.codeSize = MaxBytes;
    try std.testing.expectError(error.Limit, create_shader_module(&info, 7, 42));
    info.codeSize = 19;
    try std.testing.expectError(error.Invalid, create_shader_module(&info, 7, 42));
    info.codeSize = 21;
    try std.testing.expectError(error.Invalid, create_shader_module(&info, 7, 42));
    info.codeSize = 24;
    info.pCode = null;
    try std.testing.expectError(error.Invalid, create_shader_module(&info, 7, 42));
    info.pCode = &words;
    info.flags = 1;
    try std.testing.expectError(error.Invalid, create_shader_module(&info, 7, 42));
    info.flags = 0;
    info.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, create_shader_module(&info, 7, 42));
}
test "shader exact maximum packet fits and one word beyond fails before dereference" {
    var words: [(MaxBytes - 80) / 4]u32 = [_]u32{0x00010000} ** ((MaxBytes - 80) / 4);
    words[0] = 0x07230203;
    words[1] = 0x00010600;
    words[2] = 0;
    words[3] = 1;
    words[4] = 0;
    var info: c.VkShaderModuleCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words.len * 4, .pCode = &words };
    var expected: [MaxBytes]u8 = undefined;
    const writer = try create_shader_module(&info, 7, 42);
    try std.testing.expectEqual(MaxBytes, writer.used);
    const count = venus_render_test_shader(&info, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
    info.codeSize += 4;
    try std.testing.expectError(error.Limit, create_shader_module(&info, 7, 42));
}

/// Encode core descriptor layout without immutable samplers. [in] info/bindings borrowed for call.
/// [in] IDs translated nonzero host identities. Returns owned packet or Invalid/Limit.
/// No allocations, retained pointers or shared state; caller tracks layout lifetime and host capabilities.
pub fn create_descriptor_layout(info: *const c.VkDescriptorSetLayoutCreateInfo, device_id: u64, layout_id: u64) !writer_t {
    if (info.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO or info.pNext != null or
        info.flags != 0 or info.bindingCount > 64 or (info.bindingCount != 0 and info.pBindings == null)) return error.Invalid;
    if (info.bindingCount != 0) for (info.pBindings[0..info.bindingCount], 0..) |binding, index| {
        if (binding.descriptorType > 10 or binding.descriptorCount == 0 or binding.descriptorCount > 1024 or
            binding.stageFlags == 0 or binding.stageFlags & ~@as(u32, 0x3f) != 0 or binding.pImmutableSamplers != null) return error.Invalid;
        for (info.pBindings[0..index]) |previous| if (previous.binding == binding.binding) return error.Invalid;
    };
    var writer = writer_t{};
    try writer.header(72, device_id);
    try writer.put(u64, 1);
    try writer.put(u32, c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    try writer.put(u64, 0);
    try writer.put(u32, 0);
    try writer.put(u32, info.bindingCount);
    try writer.put(u64, info.bindingCount);
    if (info.bindingCount != 0) for (info.pBindings[0..info.bindingCount]) |binding| {
        try writer.put(u32, binding.binding);
        try writer.put(u32, binding.descriptorType);
        try writer.put(u32, binding.descriptorCount);
        try writer.put(u32, binding.stageFlags);
        try writer.put(u64, 0);
    };
    try finish_create(&writer, layout_id);
    return writer;
}
/// Encode pipeline layout. [in] info/ranges borrowed; set_ids translated IDs replace native handles.
/// [in] device_id/layout_id translated nonzero identities. Returns owned packet or Invalid/Limit.
/// No allocation/retention; validates core128-byte push constants; caller tracks child layout ownership.
pub fn create_pipeline_layout(info: *const c.VkPipelineLayoutCreateInfo, set_ids: []const u64, device_id: u64, layout_id: u64) !writer_t {
    if (info.sType != c.VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO or info.pNext != null or
        info.flags != 0 or info.setLayoutCount > 16 or set_ids.len != info.setLayoutCount or
        info.pushConstantRangeCount > 32 or
        (info.pushConstantRangeCount != 0 and info.pPushConstantRanges == null)) return error.Invalid;
    for (set_ids) |id| if (id == 0) return error.Invalid;
    if (info.pushConstantRangeCount != 0) for (info.pPushConstantRanges[0..info.pushConstantRangeCount], 0..) |range, index| {
        if (range.stageFlags == 0 or range.stageFlags & ~@as(u32, 0x3f) != 0 or range.size == 0 or
            range.offset % 4 != 0 or range.size % 4 != 0 or range.offset >= 128 or range.size > 128 - range.offset) return error.Invalid;
        for (info.pPushConstantRanges[0..index]) |previous| if (range.stageFlags & previous.stageFlags != 0) return error.Invalid;
    };
    var writer = writer_t{};
    try writer.header(68, device_id);
    try writer.put(u64, 1);
    try writer.put(u32, c.VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    try writer.put(u64, 0);
    try writer.put(u32, 0);
    try writer.put(u32, info.setLayoutCount);
    try writer.put(u64, set_ids.len);
    for (set_ids) |id| try writer.put(u64, id);
    try writer.put(u32, info.pushConstantRangeCount);
    try writer.put(u64, info.pushConstantRangeCount);
    if (info.pushConstantRangeCount != 0) for (info.pPushConstantRanges[0..info.pushConstantRangeCount]) |range| {
        try writer.put(u32, range.stageFlags);
        try writer.put(u32, range.offset);
        try writer.put(u32, range.size);
    };
    try finish_create(&writer, layout_id);
    return writer;
}
extern fn venus_render_test_descriptor_layout(*const c.VkDescriptorSetLayoutCreateInfo, [*]u8) usize;
extern fn venus_render_test_pipeline_layout(*const c.VkPipelineLayoutCreateInfo, [*]u8) usize;
test "descriptor and pipeline layouts match pinned oracle" {
    const binding: c.VkDescriptorSetLayoutBinding = .{ .binding = 3, .descriptorType = c.VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = c.VK_SHADER_STAGE_COMPUTE_BIT };
    var descriptor: c.VkDescriptorSetLayoutCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding };
    var expected: [MaxBytes]u8 = undefined;
    var writer = try create_descriptor_layout(&descriptor, 7, 42);
    var count = venus_render_test_descriptor_layout(&descriptor, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
    const set_layouts = [_]c.VkDescriptorSetLayout{@ptrFromInt(42)};
    const ranges = [_]c.VkPushConstantRange{.{ .stageFlags = 32, .size = 128 }};
    var pipeline: c.VkPipelineLayoutCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layouts, .pushConstantRangeCount = 1, .pPushConstantRanges = &ranges };
    writer = try create_pipeline_layout(&pipeline, &.{42}, 7, 43);
    count = venus_render_test_pipeline_layout(&pipeline, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
    descriptor.bindingCount = 0;
    writer = try create_descriptor_layout(&descriptor, 7, 42);
    count = venus_render_test_descriptor_layout(&descriptor, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
    pipeline.setLayoutCount = 0;
    pipeline.pushConstantRangeCount = 0;
    writer = try create_pipeline_layout(&pipeline, &.{}, 7, 43);
    count = venus_render_test_pipeline_layout(&pipeline, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
}
test "layout malformed arrays duplicate bindings and push ranges reject" {
    var bindings = [_]c.VkDescriptorSetLayoutBinding{ .{ .binding = 0, .descriptorType = 7, .descriptorCount = 1, .stageFlags = 32 }, .{ .binding = 0, .descriptorType = 7, .descriptorCount = 1, .stageFlags = 32 } };
    var info: c.VkDescriptorSetLayoutCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = &bindings };
    try std.testing.expectError(error.Invalid, create_descriptor_layout(&info, 7, 42));
    info.bindingCount = 1;
    inline for (.{ "descriptorType", "descriptorCount", "stageFlags" }) |name| {
        const previous = @field(bindings[0], name);
        @field(bindings[0], name) = 0xffffffff;
        try std.testing.expectError(error.Invalid, create_descriptor_layout(&info, 7, 42));
        @field(bindings[0], name) = previous;
    }
    bindings[0].pImmutableSamplers = @ptrFromInt(8);
    try std.testing.expectError(error.Invalid, create_descriptor_layout(&info, 7, 42));
    bindings[0].pImmutableSamplers = null;
    info.pBindings = null;
    try std.testing.expectError(error.Invalid, create_descriptor_layout(&info, 7, 42));
    info.bindingCount = 65;
    try std.testing.expectError(error.Invalid, create_descriptor_layout(&info, 7, 42));
    var ranges = [_]c.VkPushConstantRange{ .{ .stageFlags = 32, .size = 4 }, .{ .stageFlags = 32, .size = 4 } };
    var pipeline: c.VkPipelineLayoutCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 2, .pPushConstantRanges = &ranges };
    try std.testing.expectError(error.Invalid, create_pipeline_layout(&pipeline, &.{}, 7, 43));
    pipeline.pushConstantRangeCount = 1;
    inline for (.{ "stageFlags", "offset", "size" }) |name| {
        const previous = @field(ranges[0], name);
        @field(ranges[0], name) = 0xffffffff;
        try std.testing.expectError(error.Invalid, create_pipeline_layout(&pipeline, &.{}, 7, 43));
        @field(ranges[0], name) = previous;
    }
    try std.testing.expectError(error.Invalid, create_pipeline_layout(&pipeline, &.{42}, 7, 43));
    pipeline.setLayoutCount = 1;
    try std.testing.expectError(error.Invalid, create_pipeline_layout(&pipeline, &.{0}, 7, 43));
    pipeline.setLayoutCount = 0;
    pipeline.pPushConstantRanges = null;
    try std.testing.expectError(error.Invalid, create_pipeline_layout(&pipeline, &.{}, 7, 43));
}
