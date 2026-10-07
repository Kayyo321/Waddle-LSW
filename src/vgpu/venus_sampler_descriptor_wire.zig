//! Bounded samplers and image/texel descriptor writes; caller owns all translated resources.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const descriptors = @import("venus_descriptor_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Complete owned bounded packet, no dynamic allocation.
pub const writer_t = render.writer_t;
/// Translated image descriptor. Zero sampler/view requires appropriate immutable/null
/// semantics validated by caller; image layout must match descriptor usage.
pub const image_info_t = struct { sampler: u64 = 0, view: u64 = 0, layout: u32 = 0 };
/// One borrowed canonical write. Exactly one payload family selected by descriptor type;
/// caller validates layout range/type, ownership, pending updates and nullDescriptor feature.
pub const write_t = struct {
    set: u64,
    binding: u32,
    element: u32,
    descriptor_type: u32,
    images: []const image_info_t = &.{},
    buffers: []const descriptors.buffer_info_t = &.{},
    texels: []const u64 = &.{},
};
fn put(writer: *writer_t, comptime value_t: type, value: value_t) !void {
    try writer.put(value_t, value);
}
fn payload_count(write: write_t) !usize {
    if (write.descriptor_type > 10 or write.set == 0) return error.Invalid;
    const image = write.descriptor_type <= 3 or write.descriptor_type == 10;
    const texel = write.descriptor_type == 4 or write.descriptor_type == 5;
    const count = if (image) write.images.len else if (texel) write.texels.len else write.buffers.len;
    if (count == 0 or count > 128 or write.element > std.math.maxInt(u32) - count or
        (!image and write.images.len != 0) or (!texel and write.texels.len != 0) or ((image or texel) and write.buffers.len != 0)) return error.Invalid;
    return count;
}
/// Encode mixed core descriptor writes, each1..128 records and at most64 writes/copies.
/// All slices borrowed accessible immutable until return; IDs translated and externally
/// retained. Null identities are legal only with caller-verified null/immutable semantics.
/// Caller validates exact binding spans/usage/layout/type and update-after-bind rules.
/// Returns owned packet or Invalid/Limit; no partial packet published, allocations or retention.
pub fn update_sets(device: u64, writes: []const write_t, copies: []const descriptors.copy_t) !writer_t {
    if (device == 0 or writes.len > 64 or copies.len > 64) return error.Invalid;
    for (writes) |write| {
        _ = try payload_count(write);
        for (write.images) |image| if (write.descriptor_type != c.VK_DESCRIPTOR_TYPE_SAMPLER and image.view != 0 and
            image.layout == c.VK_IMAGE_LAYOUT_UNDEFINED) return error.Invalid;
        for (write.buffers) |buffer| if (buffer.buffer_id != 0 and (buffer.range == 0 or buffer.offset == std.math.maxInt(u64) or (buffer.range != std.math.maxInt(u64) and buffer.range > std.math.maxInt(u64) - buffer.offset))) return error.Invalid;
    }
    for (copies) |copy| {
        if (copy.source_set == 0 or copy.destination_set == 0 or copy.count == 0 or copy.count > 128 or
            copy.source_element > std.math.maxInt(u32) - copy.count or copy.destination_element > std.math.maxInt(u32) - copy.count) return error.Invalid;
        if (copy.source_set == copy.destination_set and copy.source_binding == copy.destination_binding and
            copy.source_element < copy.destination_element + copy.count and copy.destination_element < copy.source_element + copy.count) return error.Invalid;
    }
    var writer: writer_t = .{};
    try writer.header(79, device);
    try put(&writer, u32, @intCast(writes.len));
    try put(&writer, u64, writes.len);
    for (writes) |write| {
        const count = try payload_count(write);
        try put(&writer, u32, c.VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        try put(&writer, u64, 0);
        try put(&writer, u64, write.set);
        try put(&writer, u32, write.binding);
        try put(&writer, u32, write.element);
        try put(&writer, u32, @intCast(count));
        try put(&writer, u32, write.descriptor_type);
        try put(&writer, u64, write.images.len);
        for (write.images) |image| {
            try put(&writer, u64, image.sampler);
            try put(&writer, u64, image.view);
            try put(&writer, u32, image.layout);
        }
        try put(&writer, u64, write.buffers.len);
        for (write.buffers) |buffer| {
            try put(&writer, u64, buffer.buffer_id);
            try put(&writer, u64, buffer.offset);
            try put(&writer, u64, buffer.range);
        }
        try put(&writer, u64, write.texels.len);
        for (write.texels) |view| try put(&writer, u64, view);
    }
    try put(&writer, u32, @intCast(copies.len));
    try put(&writer, u64, copies.len);
    for (copies) |copy| {
        try put(&writer, u32, c.VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET);
        try put(&writer, u64, 0);
        try put(&writer, u64, copy.source_set);
        try put(&writer, u32, copy.source_binding);
        try put(&writer, u32, copy.source_element);
        try put(&writer, u64, copy.destination_set);
        try put(&writer, u32, copy.destination_binding);
        try put(&writer, u32, copy.destination_element);
        try put(&writer, u32, copy.count);
    }
    return writer;
}

// Test-only independent pinned encoder entrypoints.
extern fn venus_sampler_descriptor_test_write(*const c.VkWriteDescriptorSet, [*]u8) usize;
fn compare(writer: writer_t, expected: []const u8) !void {
    try std.testing.expectEqual(expected.len, writer.used);
    try std.testing.expectEqualSlices(u8, expected, writer.bytes[0..writer.used]);
}
test "image texel buffer writes match generated bytes" {
    var expected: [8192]u8 = undefined;
    var images: [128]image_info_t = undefined;
    var native_images: [128]c.VkDescriptorImageInfo = undefined;
    var buffers: [128]descriptors.buffer_info_t = undefined;
    var native_buffers: [128]c.VkDescriptorBufferInfo = undefined;
    var texels: [128]u64 = undefined;
    var native_texels: [128]c.VkBufferView = undefined;
    for (0..128) |index| {
        images[index] = .{ .sampler = 42 + index, .view = 242 + index, .layout = c.VK_IMAGE_LAYOUT_GENERAL };
        native_images[index] = .{ .sampler = @ptrFromInt(images[index].sampler), .imageView = @ptrFromInt(images[index].view), .imageLayout = images[index].layout };
        buffers[index] = .{ .buffer_id = 442 + index, .offset = index * 16, .range = 64 };
        native_buffers[index] = .{ .buffer = @ptrFromInt(buffers[index].buffer_id), .offset = buffers[index].offset, .range = buffers[index].range };
        texels[index] = 642 + index;
        native_texels[index] = @ptrFromInt(texels[index]);
    }
    for (0..11) |kind| for ([_]usize{ 1, 128 }) |count| {
        const image = kind <= 3 or kind == 10;
        const texel = kind == 4 or kind == 5;
        const write = write_t{ .set = 15, .binding = 3, .element = 2, .descriptor_type = @intCast(kind), .images = if (image) images[0..count] else &.{}, .buffers = if (!image and !texel) buffers[0..count] else &.{}, .texels = if (texel) texels[0..count] else &.{} };
        const native = c.VkWriteDescriptorSet{ .sType = c.VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = @ptrFromInt(15), .dstBinding = 3, .dstArrayElement = 2, .descriptorCount = @intCast(count), .descriptorType = @intCast(kind), .pImageInfo = if (image) &native_images else null, .pBufferInfo = if (!image and !texel) &native_buffers else null, .pTexelBufferView = if (texel) &native_texels else null };
        try compare(try update_sets(8, &.{write}, &.{}), expected[0..venus_sampler_descriptor_test_write(&native, &expected)]);
    };
}
test "descriptor invalid shapes reject before publication" {
    try std.testing.expectError(error.Invalid, update_sets(8, &.{.{ .set = 15, .binding = 0, .element = 0, .descriptor_type = 2 }}, &.{}));
    try std.testing.expectError(error.Invalid, update_sets(8, &.{.{ .set = 15, .binding = 0, .element = 0, .descriptor_type = 2, .images = &.{.{ .view = 1 }} }}, &.{}));
    const writes = [_]write_t{.{ .set = 15, .binding = 0, .element = 0, .descriptor_type = 1, .images = &([_]image_info_t{.{ .view = 1, .layout = 1 }} ** 128) }} ** 4;
    try std.testing.expectError(error.Limit, update_sets(8, &writes, &.{}));
}

/// Translated immutable sampler array for one native layout binding. Empty means absent;
/// caller retains all sampler owners through descriptor layout/set lifetime.
pub const immutable_samplers_t = struct { ids: []const u64 = &.{} };
fn array_elements(pointer: anytype, count: usize) []const @typeInfo(@TypeOf(pointer)).Pointer.child {
    if (count == 0) return &.{};
    return pointer[0..count];
}
/// Encode bounded core descriptor layout plus descriptor-indexing binding flags.
/// info/reachable native records immutable accessible borrowed; immutable arrays match
/// binding order and translate native sampler handles. Caller validates all corresponding
/// enabled features, host limits and retention. At most64bindings, each <=65536 descriptors;
/// no speculative large metadata allocation. Returns complete owned packet or Invalid/Limit.
pub fn create_layout(device: u64, output: u64, info: *const c.VkDescriptorSetLayoutCreateInfo, immutable: []const immutable_samplers_t) !writer_t {
    if (device == 0 or output == 0 or info.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO or
        info.flags & ~@as(u32, c.VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) != 0 or info.bindingCount > 64 or
        info.bindingCount != immutable.len or (info.bindingCount != 0 and info.pBindings == null)) return error.Invalid;
    var flags: ?*const c.VkDescriptorSetLayoutBindingFlagsCreateInfo = null;
    if (info.pNext) |pointer| {
        flags = @ptrCast(@alignCast(pointer));
        const value = flags.?;
        if (value.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO or value.pNext != null or
            (value.bindingCount != 0 and value.bindingCount != info.bindingCount) or (value.bindingCount != 0 and value.pBindingFlags == null)) return error.Invalid;
    }
    for (array_elements(info.pBindings, info.bindingCount), immutable, 0..) |binding, samplers, index| {
        if (binding.descriptorType > 10 or binding.descriptorCount > 65536 or binding.stageFlags & ~@as(u32, 0x3f) != 0 or
            (binding.descriptorCount != 0 and binding.stageFlags == 0) or (binding.pImmutableSamplers == null) != (samplers.ids.len == 0) or
            (samplers.ids.len != 0 and (samplers.ids.len != binding.descriptorCount or binding.descriptorType > 1))) return error.Invalid;
        for (samplers.ids) |id| if (id == 0) return error.Invalid;
        for (array_elements(info.pBindings, index)) |previous| if (previous.binding == binding.binding) return error.Invalid;
        if (flags) |value| if (value.bindingCount != 0) {
            const mask = value.pBindingFlags[index];
            if (mask & ~@as(u32, 15) != 0) return error.Invalid;
            if (mask & c.VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT != 0 and info.flags & c.VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT == 0) return error.Invalid;
            if (mask & c.VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT != 0) {
                if (binding.descriptorType == c.VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC or binding.descriptorType == c.VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) return error.Invalid;
                for (array_elements(info.pBindings, info.bindingCount)) |other| if (other.binding > binding.binding) return error.Invalid;
            }
        };
    }
    var writer: writer_t = .{};
    try writer.header(72, device);
    try put(&writer, u64, 1);
    try put(&writer, u32, info.sType);
    if (flags) |value| {
        try put(&writer, u64, 1);
        try put(&writer, u32, value.sType);
        try put(&writer, u64, 0);
        try put(&writer, u32, value.bindingCount);
        try put(&writer, u64, value.bindingCount);
        for (array_elements(value.pBindingFlags, value.bindingCount)) |mask| try put(&writer, u32, mask);
    } else try put(&writer, u64, 0);
    try put(&writer, u32, info.flags);
    try put(&writer, u32, info.bindingCount);
    try put(&writer, u64, info.bindingCount);
    for (array_elements(info.pBindings, info.bindingCount), immutable) |binding, samplers| {
        try put(&writer, u32, binding.binding);
        try put(&writer, u32, binding.descriptorType);
        try put(&writer, u32, binding.descriptorCount);
        try put(&writer, u32, binding.stageFlags);
        try put(&writer, u64, samplers.ids.len);
        for (samplers.ids) |id| try put(&writer, u64, id);
    }
    try put(&writer, u64, 0);
    try put(&writer, u64, 1);
    try put(&writer, u64, output);
    return writer;
}
/// Encode real descriptor-pool capacity without allocating metadata for unused capacity.
/// MaxSets<=65536, total descriptor capacity<=1048576, at most64 core pool type records.
/// Free-set and update-after-bind flags supported; caller validates feature/host limits.
/// Native info borrowed call-lifetime, IDs translated nonzero; owned result or Invalid/Limit.
pub fn create_pool(device: u64, output: u64, info: *const c.VkDescriptorPoolCreateInfo) !writer_t {
    if (device == 0 or output == 0 or info.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO or info.pNext != null or info.flags & ~@as(u32, 3) != 0 or
        info.maxSets == 0 or info.maxSets > 65536 or info.poolSizeCount > 64 or (info.poolSizeCount != 0 and info.pPoolSizes == null)) return error.Invalid;
    var total: u64 = 0;
    for (array_elements(info.pPoolSizes, info.poolSizeCount)) |size| {
        if (size.type > 10 or size.descriptorCount == 0) return error.Invalid;
        total += size.descriptorCount;
        if (total > 1048576) return error.Invalid;
    }
    var writer: writer_t = .{};
    try writer.header(74, device);
    try put(&writer, u64, 1);
    try put(&writer, u32, info.sType);
    try put(&writer, u64, 0);
    try put(&writer, u32, info.flags);
    try put(&writer, u32, info.maxSets);
    try put(&writer, u32, info.poolSizeCount);
    try put(&writer, u64, info.poolSizeCount);
    for (array_elements(info.pPoolSizes, info.poolSizeCount)) |size| {
        try put(&writer, u32, size.type);
        try put(&writer, u32, size.descriptorCount);
    }
    try put(&writer, u64, 0);
    try put(&writer, u64, 1);
    try put(&writer, u64, output);
    return writer;
}
/// Encode allocated set identities with optional variable descriptor counts. Native chain
/// borrowed accessible immutable and validated; all layout/set IDs translated same-device.
/// Caller verifies live-set registry capacity and actual layout/pool descriptor budgets.
/// At most64 actual sets per call; owned complete packet or Invalid/Limit, no allocation.
pub fn allocate_sets(device: u64, pool: u64, info: *const c.VkDescriptorSetAllocateInfo, layouts: []const u64, sets: []const u64) !writer_t {
    if (device == 0 or pool == 0 or info.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO or info.descriptorSetCount == 0 or info.descriptorSetCount > 64 or
        info.descriptorSetCount != layouts.len or layouts.len != sets.len or info.pSetLayouts == null or info.descriptorPool == null) return error.Invalid;
    var variable: ?*const c.VkDescriptorSetVariableDescriptorCountAllocateInfo = null;
    if (info.pNext) |pointer| {
        variable = @ptrCast(@alignCast(pointer));
        const value = variable.?;
        if (value.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO or value.pNext != null or
            (value.descriptorSetCount != 0 and value.descriptorSetCount != sets.len) or (value.descriptorSetCount != 0 and value.pDescriptorCounts == null)) return error.Invalid;
        for (array_elements(value.pDescriptorCounts, value.descriptorSetCount)) |count| if (count > 65536) return error.Invalid;
    }
    for (layouts) |layout| if (layout == 0) return error.Invalid;
    for (sets, 0..) |id, index| if (id == 0 or std.mem.indexOfScalar(u64, sets[0..index], id) != null) return error.Invalid;
    var writer: writer_t = .{};
    try writer.header(77, device);
    try put(&writer, u64, 1);
    try put(&writer, u32, info.sType);
    if (variable) |value| {
        try put(&writer, u64, 1);
        try put(&writer, u32, value.sType);
        try put(&writer, u64, 0);
        try put(&writer, u32, value.descriptorSetCount);
        try put(&writer, u64, value.descriptorSetCount);
        for (array_elements(value.pDescriptorCounts, value.descriptorSetCount)) |count| try put(&writer, u32, count);
    } else try put(&writer, u64, 0);
    try put(&writer, u64, pool);
    try put(&writer, u32, @intCast(layouts.len));
    try put(&writer, u64, layouts.len);
    for (layouts) |layout| try put(&writer, u64, layout);
    try put(&writer, u64, sets.len);
    for (sets) |id| try put(&writer, u64, id);
    return writer;
}

// Test-only generated extended descriptor fixtures.
extern fn venus_sampler_descriptor_test_layout(*const c.VkDescriptorSetLayoutCreateInfo, [*]u8) usize;
extern fn venus_sampler_descriptor_test_pool(*const c.VkDescriptorPoolCreateInfo, [*]u8) usize;
extern fn venus_sampler_descriptor_test_allocate(*const c.VkDescriptorSetAllocateInfo, [*]u8) usize;
test "immutable sampler indexing flags large pool and variable counts match generated bytes" {
    var expected: [8192]u8 = undefined;
    const samplers = [_]c.VkSampler{ @ptrFromInt(52), @ptrFromInt(53) };
    const bindings = [_]c.VkDescriptorSetLayoutBinding{
        .{ .binding = 0, .descriptorType = c.VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2, .stageFlags = 16, .pImmutableSamplers = &samplers },
        .{ .binding = 7, .descriptorType = c.VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 128, .stageFlags = 32 },
    };
    const masks = [_]c.VkDescriptorBindingFlags{ c.VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 15 };
    var flags = c.VkDescriptorSetLayoutBindingFlagsCreateInfo{ .sType = c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 2, .pBindingFlags = &masks };
    var info = c.VkDescriptorSetLayoutCreateInfo{ .sType = c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &flags, .flags = c.VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, .bindingCount = 2, .pBindings = &bindings };
    const immutable = [_]immutable_samplers_t{ .{ .ids = &.{ 52, 53 } }, .{} };
    try compare(try create_layout(8, 42, &info, &immutable), expected[0..venus_sampler_descriptor_test_layout(&info, &expected)]);
    flags.bindingCount = 0;
    flags.pBindingFlags = null;
    try compare(try create_layout(8, 42, &info, &immutable), expected[0..venus_sampler_descriptor_test_layout(&info, &expected)]);
    info.pNext = null;
    try compare(try create_layout(8, 42, &info, &immutable), expected[0..venus_sampler_descriptor_test_layout(&info, &expected)]);
    const sizes = [_]c.VkDescriptorPoolSize{ .{ .type = 2, .descriptorCount = 24576 }, .{ .type = 6, .descriptorCount = 98304 } };
    const pool = c.VkDescriptorPoolCreateInfo{ .sType = c.VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = 3, .maxSets = 49152, .poolSizeCount = 2, .pPoolSizes = &sizes };
    try compare(try create_pool(8, 42, &pool), expected[0..venus_sampler_descriptor_test_pool(&pool, &expected)]);
    const layouts = [_]c.VkDescriptorSetLayout{ @ptrFromInt(52), @ptrFromInt(53) };
    const counts = [_]u32{ 128, 64 };
    var variable = c.VkDescriptorSetVariableDescriptorCountAllocateInfo{ .sType = c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, .descriptorSetCount = 2, .pDescriptorCounts = &counts };
    var allocate = c.VkDescriptorSetAllocateInfo{ .sType = c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .pNext = &variable, .descriptorPool = @ptrFromInt(42), .descriptorSetCount = 2, .pSetLayouts = &layouts };
    try compare(try allocate_sets(8, 42, &allocate, &.{ 52, 53 }, &.{ 43, 44 }), expected[0..venus_sampler_descriptor_test_allocate(&allocate, &expected)]);
    variable.descriptorSetCount = 0;
    variable.pDescriptorCounts = null;
    try compare(try allocate_sets(8, 42, &allocate, &.{ 52, 53 }, &.{ 43, 44 }), expected[0..venus_sampler_descriptor_test_allocate(&allocate, &expected)]);
    allocate.pNext = null;
    try compare(try allocate_sets(8, 42, &allocate, &.{ 52, 53 }, &.{ 43, 44 }), expected[0..venus_sampler_descriptor_test_allocate(&allocate, &expected)]);
}
