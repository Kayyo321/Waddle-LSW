//! Bounded descriptor pool/set packets; caller validates native handle ownership and GPU lifetimes.
const std = @import("std");
const render_wire = @import("venus_render_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Maximum packet extent; borrowed transport must accept this complete initialized prefix.
pub const MaxBytes: usize = render_wire.MaxBytes;
/// Maximum sets or write/copy records; fixed scalar bound before array traversal.
pub const MaxSets: usize = 64;
/// Maximum buffers per write; descriptor count is exactly the borrowed buffer slice length.
pub const MaxBuffers: usize = 64;
/// Maximum pool descriptor capacity; bounds aggregate core pool size requests.
pub const MaxPoolDescriptors: u32 = 65536;
/// Owned allocation-free packet; bytes/used and all lifetimes match the rendering codec writer.
pub const writer_t = render_wire.writer_t;
/// Translated buffer descriptor; no native pointer or private application handle crosses this boundary.
/// Caller owns values, verifies buffer usage/range/parent and keeps allocation live until GPU retirement.
pub const buffer_info_t = struct {
    /// Nonzero receiver buffer identity; caller verifies same device and bound memory.
    buffer_id: u64,
    /// Byte offset; caller verifies native alignment and buffer extent.
    offset: u64,
    /// Positive byte range or VK_WHOLE_SIZE; caller resolves whole-size bounds against the buffer.
    range: u64,
};
/// Canonical buffer write borrowed until encode returns; no allocation/retention or thread sharing.
/// Caller validates original sType/pNext, layout binding/type/count, parent and pending-use exclusion.
pub const buffer_write_t = struct {
    /// Nonzero translated destination set identity; caller retains set ownership.
    set_id: u64,
    /// Native layout binding index, validated by the caller against exact layout metadata.
    binding: u32,
    /// First descriptor element; count addition checked locally, layout extent by caller.
    array_element: u32,
    /// Core uniform/storage buffer types6..9 only; unsupported descriptor families explicitly reject.
    descriptor_type: u32,
    /// Nonempty borrowed accessible immutable buffer records, one per descriptor; maximum64.
    buffers: []const buffer_info_t,
};
/// Canonical descriptor copy; caller verifies both same-device sets, types, exact binding ranges and GPU quiescence.
/// No pointers, allocation or ownership transfer; same-binding overlap rejects before packet construction.
pub const copy_t = struct {
    /// Nonzero translated source set identity, externally retained.
    source_set: u64,
    /// Source binding index verified against layout metadata by caller.
    source_binding: u32,
    /// First source element; checked count addition and overlap before encoding.
    source_element: u32,
    /// Nonzero translated destination set identity, externally retained.
    destination_set: u64,
    /// Destination binding index verified against layout metadata by caller.
    destination_binding: u32,
    /// First destination element; checked count addition and overlap before encoding.
    destination_element: u32,
    /// Positive descriptor count1..64; both layout bounds checked by caller.
    count: u32,
};
fn put(writer: *writer_t, comptime word_t: type, word: word_t) void {
    writer.put(word_t, word) catch unreachable;
}
/// Encode native pool creation with core flags and bounded quota.
/// @param[in] info Nonnull borrowed canonical native record/size array; pNext unsupported.
/// @param[in] device_id/pool_id Nonzero translated receiver identities, no pointers retained.
/// @return Owned packet; Invalid rejects scalar/array/quota/identity before encoding.
/// @note Allocation-free, thread-safe on disjoint inputs; caller owns pool until exact native destroy.
pub fn create_pool(info: *const c.VkDescriptorPoolCreateInfo, device_id: u64, pool_id: u64) !writer_t {
    if (device_id == 0 or pool_id == 0 or info.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO or
        info.pNext != null or info.flags & ~@as(u32, c.VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) != 0 or
        info.maxSets == 0 or info.maxSets > MaxSets or info.poolSizeCount > MaxSets or
        (info.poolSizeCount != 0 and info.pPoolSizes == null)) return error.Invalid;
    var total: u32 = 0;
    if (info.poolSizeCount != 0) for (info.pPoolSizes[0..info.poolSizeCount]) |size| {
        if (size.type > 10 or size.descriptorCount == 0 or size.descriptorCount > MaxPoolDescriptors) return error.Invalid;
        total += size.descriptorCount;
        if (total > MaxPoolDescriptors) return error.Invalid;
    };
    // Canonical counts prove80+8*64<=8192 before the first scalar append.
    std.debug.assert(80 + 8 * @as(usize, info.poolSizeCount) <= MaxBytes);
    var writer = writer_t{};
    writer.header(74, device_id) catch unreachable;
    put(&writer, u64, 1);
    put(&writer, u32, c.VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    put(&writer, u64, 0);
    put(&writer, u32, info.flags);
    put(&writer, u32, info.maxSets);
    put(&writer, u32, info.poolSizeCount);
    put(&writer, u64, info.poolSizeCount);
    if (info.poolSizeCount != 0) for (info.pPoolSizes[0..info.poolSizeCount]) |size| {
        put(&writer, u32, size.type);
        put(&writer, u32, size.descriptorCount);
    };
    put(&writer, u64, 0);
    put(&writer, u64, 1);
    put(&writer, u64, pool_id);
    return writer;
}
/// Encode a batch of distinct reserved descriptor set identities.
/// @param[in] info Nonnull canonical borrowed native record; original handles never encoded.
/// @param[in] layouts/set_ids Borrowed same-length translated IDs1..64, exact descriptorSetCount.
/// @param[in] device_id/pool_id Nonzero translated parent identities, caller validates capacity and ancestry.
/// @return Owned packet or Invalid; failed packet never published, no allocations/ownership transfer.
/// @note Thread-safe on immutable disjoint inputs; ICD publishes reserved sets only after exact host reply.
pub fn allocate_sets(info: *const c.VkDescriptorSetAllocateInfo, layouts: []const u64, device_id: u64, pool_id: u64, set_ids: []const u64) !writer_t {
    if (device_id == 0 or pool_id == 0 or info.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO or
        info.pNext != null or info.descriptorPool == null or info.descriptorSetCount == 0 or
        info.descriptorSetCount > MaxSets or info.pSetLayouts == null or
        layouts.len != info.descriptorSetCount or set_ids.len != layouts.len) return error.Invalid;
    for (layouts) |id| if (id == 0) return error.Invalid;
    for (set_ids, 0..) |id, index| {
        if (id == 0 or std.mem.indexOfScalar(u64, set_ids[0..index], id) != null) return error.Invalid;
    }
    std.debug.assert(64 + 16 * layouts.len <= MaxBytes);
    var writer = writer_t{};
    writer.header(77, device_id) catch unreachable;
    put(&writer, u64, 1);
    put(&writer, u32, c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    put(&writer, u64, 0);
    put(&writer, u64, pool_id);
    put(&writer, u32, info.descriptorSetCount);
    put(&writer, u64, layouts.len);
    for (layouts) |id| put(&writer, u64, id);
    put(&writer, u64, set_ids.len);
    for (set_ids) |id| put(&writer, u64, id);
    return writer;
}
/// Encode buffer descriptors and descriptor copies with complete validation before packet construction.
/// @param[in] device_id Nonzero translated device identity, caller validates every referenced parent.
/// @param[in] writes/copies Borrowed immutable decoded records, each slice at most64; zero-length valid.
/// @return Owned packet; Invalid malformed identities/types/ranges; Limit aggregate exceeds8192 bytes.
/// @note Allocation-free/thread-safe on disjoint immutable inputs. Caller tracks resources and excludes GPU use.
pub fn update_sets(device_id: u64, writes: []const buffer_write_t, copies: []const copy_t) !writer_t {
    if (device_id == 0 or writes.len > MaxSets or copies.len > MaxSets) return error.Invalid;
    var bytes: usize = 40;
    for (writes) |write| {
        if (write.set_id == 0 or write.descriptor_type < 6 or write.descriptor_type > 9 or
            write.buffers.len == 0 or write.buffers.len > MaxBuffers or
            write.array_element > std.math.maxInt(u32) - write.buffers.len) return error.Invalid;
        for (write.buffers) |buffer| {
            if (buffer.buffer_id == 0 or buffer.range == 0 or buffer.offset == std.math.maxInt(u64) or
                (buffer.range != std.math.maxInt(u64) and buffer.range > std.math.maxInt(u64) - buffer.offset)) return error.Invalid;
        }
        bytes += 60 + 24 * write.buffers.len;
    }
    for (copies) |copy| {
        if (copy.source_set == 0 or copy.destination_set == 0 or copy.count == 0 or copy.count > MaxBuffers or
            copy.source_element > std.math.maxInt(u32) - copy.count or
            copy.destination_element > std.math.maxInt(u32) - copy.count) return error.Invalid;
        if (copy.source_set == copy.destination_set and copy.source_binding == copy.destination_binding and
            copy.source_element < copy.destination_element + copy.count and copy.destination_element < copy.source_element + copy.count) return error.Invalid;
        bytes += 48;
    }
    if (bytes > MaxBytes) return error.Limit;
    var writer = writer_t{};
    writer.header(79, device_id) catch unreachable;
    put(&writer, u32, @intCast(writes.len));
    put(&writer, u64, writes.len);
    for (writes) |write| {
        put(&writer, u32, c.VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        put(&writer, u64, 0);
        put(&writer, u64, write.set_id);
        put(&writer, u32, write.binding);
        put(&writer, u32, write.array_element);
        put(&writer, u32, @intCast(write.buffers.len));
        put(&writer, u32, write.descriptor_type);
        put(&writer, u64, 0);
        put(&writer, u64, write.buffers.len);
        for (write.buffers) |buffer| {
            put(&writer, u64, buffer.buffer_id);
            put(&writer, u64, buffer.offset);
            put(&writer, u64, buffer.range);
        }
        put(&writer, u64, 0);
    }
    put(&writer, u32, @intCast(copies.len));
    put(&writer, u64, copies.len);
    for (copies) |copy| {
        put(&writer, u32, c.VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET);
        put(&writer, u64, 0);
        put(&writer, u64, copy.source_set);
        put(&writer, u32, copy.source_binding);
        put(&writer, u32, copy.source_element);
        put(&writer, u64, copy.destination_set);
        put(&writer, u32, copy.destination_binding);
        put(&writer, u32, copy.destination_element);
        put(&writer, u32, copy.count);
    }
    std.debug.assert(writer.used == bytes);
    return writer;
}

// Test-only fixtures.
extern fn venus_descriptor_test_pool(*const c.VkDescriptorPoolCreateInfo, [*]u8) usize;
extern fn venus_descriptor_test_allocate(*const c.VkDescriptorSetAllocateInfo, [*]const u64, [*]const u64, [*]u8) usize;
extern fn venus_descriptor_test_update(u32, [*c]const c.VkWriteDescriptorSet, u32, [*c]const c.VkCopyDescriptorSet, [*]u8) usize;

fn pool_fixture() c.VkDescriptorPoolCreateInfo {
    return .{ .sType = c.VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 64 };
}
test "pool quota and maximum records match independent pinned encoder" {
    var info = pool_fixture();
    var expected: [MaxBytes]u8 = undefined;
    var writer = try create_pool(&info, 7, 42);
    var count = venus_descriptor_test_pool(&info, &expected);
    try std.testing.expectEqual(@as(usize, 80), writer.used);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
    const sizes = [_]c.VkDescriptorPoolSize{.{ .type = 7, .descriptorCount = 1024 }} ** 64;
    info.flags = c.VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.poolSizeCount = sizes.len;
    info.pPoolSizes = &sizes;
    writer = try create_pool(&info, 7, 42);
    count = venus_descriptor_test_pool(&info, &expected);
    try std.testing.expectEqual(@as(usize, 592), writer.used);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
}
test "pool malformed scalars array bounds and aggregate quota reject before traversal" {
    const initial = pool_fixture();
    try std.testing.expectError(error.Invalid, create_pool(&initial, 0, 42));
    try std.testing.expectError(error.Invalid, create_pool(&initial, 7, 0));
    inline for (.{ "sType", "flags", "maxSets", "poolSizeCount" }) |member| {
        var info = initial;
        @field(info, member) = 0xffffffff;
        info.pPoolSizes = @ptrFromInt(4);
        try std.testing.expectError(error.Invalid, create_pool(&info, 7, 42));
    }
    var info = initial;
    info.maxSets = 0;
    try std.testing.expectError(error.Invalid, create_pool(&info, 7, 42));
    info = initial;
    info.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, create_pool(&info, 7, 42));
    info = initial;
    info.poolSizeCount = 1;
    try std.testing.expectError(error.Invalid, create_pool(&info, 7, 42));
    var sizes = [_]c.VkDescriptorPoolSize{.{ .type = 7, .descriptorCount = 1 }} ** 2;
    info.pPoolSizes = &sizes;
    sizes[0].type = 11;
    try std.testing.expectError(error.Invalid, create_pool(&info, 7, 42));
    sizes[0].type = 7;
    sizes[0].descriptorCount = 0;
    try std.testing.expectError(error.Invalid, create_pool(&info, 7, 42));
    sizes[0].descriptorCount = MaxPoolDescriptors + 1;
    try std.testing.expectError(error.Invalid, create_pool(&info, 7, 42));
    info.poolSizeCount = 2;
    sizes[0].descriptorCount = MaxPoolDescriptors;
    sizes[1].descriptorCount = 1;
    try std.testing.expectError(error.Invalid, create_pool(&info, 7, 42));
}
test "allocation identities and exact maximum packet match independent pinned encoder" {
    var native_layouts = [_]c.VkDescriptorSetLayout{@ptrFromInt(43)} ** 64;
    const layouts = [_]u64{43} ** 64;
    var sets: [64]u64 = undefined;
    for (&sets, 0..) |*id, index| id.* = index + 44;
    var info: c.VkDescriptorSetAllocateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = @ptrFromInt(42), .descriptorSetCount = 64, .pSetLayouts = &native_layouts };
    var expected: [MaxBytes]u8 = undefined;
    var writer = try allocate_sets(&info, &layouts, 7, 42, &sets);
    var count = venus_descriptor_test_allocate(&info, &layouts, &sets, &expected);
    try std.testing.expectEqual(@as(usize, 1088), writer.used);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
    info.descriptorSetCount = 1;
    writer = try allocate_sets(&info, layouts[0..1], 7, 42, sets[0..1]);
    count = venus_descriptor_test_allocate(&info, &layouts, &sets, &expected);
    try std.testing.expectEqual(@as(usize, 80), writer.used);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
}
test "allocation malformed pointers lengths and repeated reserved identities reject" {
    const native_layouts = [_]c.VkDescriptorSetLayout{@ptrFromInt(43)} ** 2;
    const initial: c.VkDescriptorSetAllocateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = @ptrFromInt(42), .descriptorSetCount = 2, .pSetLayouts = &native_layouts };
    const layouts = [_]u64{ 43, 43 };
    const sets = [_]u64{ 44, 45 };
    try std.testing.expectError(error.Invalid, allocate_sets(&initial, &layouts, 0, 42, &sets));
    try std.testing.expectError(error.Invalid, allocate_sets(&initial, &layouts, 7, 0, &sets));
    var info = initial;
    info.sType = 0;
    try std.testing.expectError(error.Invalid, allocate_sets(&info, &layouts, 7, 42, &sets));
    info = initial;
    info.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, allocate_sets(&info, &layouts, 7, 42, &sets));
    info = initial;
    info.descriptorPool = null;
    try std.testing.expectError(error.Invalid, allocate_sets(&info, &layouts, 7, 42, &sets));
    info = initial;
    info.pSetLayouts = null;
    try std.testing.expectError(error.Invalid, allocate_sets(&info, &layouts, 7, 42, &sets));
    for ([_]u32{ 0, 65 }) |count| {
        info = initial;
        info.descriptorSetCount = count;
        try std.testing.expectError(error.Invalid, allocate_sets(&info, &layouts, 7, 42, &sets));
    }
    info = initial;
    info.descriptorSetCount = 1;
    try std.testing.expectError(error.Invalid, allocate_sets(&info, &layouts, 7, 42, &sets));
    try std.testing.expectError(error.Invalid, allocate_sets(&initial, layouts[0..1], 7, 42, &sets));
    try std.testing.expectError(error.Invalid, allocate_sets(&initial, &layouts, 7, 42, sets[0..1]));
    try std.testing.expectError(error.Invalid, allocate_sets(&initial, &.{ 0, 43 }, 7, 42, &sets));
    try std.testing.expectError(error.Invalid, allocate_sets(&initial, &layouts, 7, 42, &.{ 0, 45 }));
    try std.testing.expectError(error.Invalid, allocate_sets(&initial, &layouts, 7, 42, &.{ 44, 44 }));
}

fn native_write(write: buffer_write_t, buffers: [*c]const c.VkDescriptorBufferInfo) c.VkWriteDescriptorSet {
    return .{ .sType = c.VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = @ptrFromInt(write.set_id), .dstBinding = write.binding, .dstArrayElement = write.array_element, .descriptorCount = @intCast(write.buffers.len), .descriptorType = write.descriptor_type, .pBufferInfo = buffers };
}
fn native_copy(copy: copy_t) c.VkCopyDescriptorSet {
    return .{ .sType = c.VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = @ptrFromInt(copy.source_set), .srcBinding = copy.source_binding, .srcArrayElement = copy.source_element, .dstSet = @ptrFromInt(copy.destination_set), .dstBinding = copy.destination_binding, .dstArrayElement = copy.destination_element, .descriptorCount = copy.count };
}
fn write_fixture(buffers: []const buffer_info_t) buffer_write_t {
    return .{ .set_id = 44, .binding = 3, .array_element = 1, .descriptor_type = 7, .buffers = buffers };
}
fn copy_fixture() copy_t {
    return .{ .source_set = 44, .source_binding = 3, .source_element = 0, .destination_set = 45, .destination_binding = 3, .destination_element = 0, .count = 1 };
}
test "buffer descriptor families copies and empty packet match independent pinned encoder" {
    const buffers = [_]buffer_info_t{ .{ .buffer_id = 42, .offset = 0, .range = 64 }, .{ .buffer_id = 43, .offset = 16, .range = std.math.maxInt(u64) } };
    const native_buffers = [_]c.VkDescriptorBufferInfo{ .{ .buffer = @ptrFromInt(42), .range = 64 }, .{ .buffer = @ptrFromInt(43), .offset = 16, .range = std.math.maxInt(u64) } };
    var writes: [4]buffer_write_t = undefined;
    var native_writes: [4]c.VkWriteDescriptorSet = undefined;
    for (&writes, &native_writes, 0..) |*write, *native, index| {
        write.* = write_fixture(&buffers);
        write.descriptor_type = @intCast(index + 6);
        native.* = native_write(write.*, &native_buffers);
    }
    var copies = [_]copy_t{copy_fixture()} ** 3;
    copies[1].source_set = 45;
    copies[1].source_binding = 4;
    copies[2].source_set = 45;
    copies[2].destination_element = 1;
    const native_copies = [_]c.VkCopyDescriptorSet{ native_copy(copies[0]), native_copy(copies[1]), native_copy(copies[2]) };
    var expected: [MaxBytes]u8 = undefined;
    var writer = try update_sets(7, &writes, &copies);
    var count = venus_descriptor_test_update(writes.len, &native_writes, copies.len, &native_copies, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
    writer = try update_sets(7, &.{}, &.{});
    count = venus_descriptor_test_update(0, null, 0, null, &expected);
    try std.testing.expectEqual(@as(usize, 40), writer.used);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
}
test "update maximum aligned packet matches oracle and next descriptor rejects" {
    const buffers = [_]buffer_info_t{.{ .buffer_id = 42, .offset = 0, .range = 64 }} ** 64;
    const native_buffers = [_]c.VkDescriptorBufferInfo{.{ .buffer = @ptrFromInt(42), .range = 64 }} ** 64;
    var writes = [_]buffer_write_t{write_fixture(&buffers)} ** 5;
    var native_writes: [5]c.VkWriteDescriptorSet = undefined;
    writes[4].buffers = buffers[0..63];
    for (&native_writes, writes) |*native, write| native.* = native_write(write, &native_buffers);
    const copies = [_]copy_t{copy_fixture()} ** 4;
    const native_copies = [_]c.VkCopyDescriptorSet{native_copy(copy_fixture())} ** 4;
    var expected: [MaxBytes]u8 = undefined;
    const writer = try update_sets(7, &writes, &copies);
    try std.testing.expectEqual(@as(usize, 8188), writer.used);
    const count = venus_descriptor_test_update(writes.len, &native_writes, copies.len, &native_copies, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
    writes[4].buffers = &buffers;
    try std.testing.expectError(error.Limit, update_sets(7, &writes, &copies));
}
test "update malformed write scalars counts identities and overflow reject before encoding" {
    const initial_buffer: buffer_info_t = .{ .buffer_id = 42, .offset = 0, .range = 64 };
    var buffers = [_]buffer_info_t{initial_buffer} ** 65;
    const initial = write_fixture(buffers[0..1]);
    var writes = [_]buffer_write_t{initial} ** 65;
    try std.testing.expectError(error.Invalid, update_sets(0, writes[0..1], &.{}));
    try std.testing.expectError(error.Invalid, update_sets(7, &writes, &.{}));
    for ([_]u32{ 5, 10 }) |kind| {
        writes[0].descriptor_type = kind;
        try std.testing.expectError(error.Invalid, update_sets(7, writes[0..1], &.{}));
    }
    writes[0] = initial;
    writes[0].set_id = 0;
    try std.testing.expectError(error.Invalid, update_sets(7, writes[0..1], &.{}));
    writes[0] = initial;
    writes[0].buffers = &.{};
    try std.testing.expectError(error.Invalid, update_sets(7, writes[0..1], &.{}));
    writes[0].buffers = &buffers;
    try std.testing.expectError(error.Invalid, update_sets(7, writes[0..1], &.{}));
    writes[0] = initial;
    writes[0].array_element = std.math.maxInt(u32);
    try std.testing.expectError(error.Invalid, update_sets(7, writes[0..1], &.{}));
    writes[0] = initial;
    buffers[0].buffer_id = 0;
    try std.testing.expectError(error.Invalid, update_sets(7, writes[0..1], &.{}));
    buffers[0] = initial_buffer;
    buffers[0].range = 0;
    try std.testing.expectError(error.Invalid, update_sets(7, writes[0..1], &.{}));
    buffers[0] = initial_buffer;
    buffers[0].offset = std.math.maxInt(u64);
    try std.testing.expectError(error.Invalid, update_sets(7, writes[0..1], &.{}));
    buffers[0].offset -= 1;
    buffers[0].range = 2;
    try std.testing.expectError(error.Invalid, update_sets(7, writes[0..1], &.{}));
}
test "copy scalar bounds same-binding overlap and sixty-four record maximum" {
    const initial = copy_fixture();
    var copies = [_]copy_t{initial} ** 65;
    try std.testing.expectError(error.Invalid, update_sets(7, &.{}, &copies));
    inline for (.{ "source_set", "destination_set", "count" }) |member| {
        copies[0] = initial;
        @field(copies[0], member) = 0;
        try std.testing.expectError(error.Invalid, update_sets(7, &.{}, copies[0..1]));
    }
    copies[0] = initial;
    copies[0].count = 65;
    try std.testing.expectError(error.Invalid, update_sets(7, &.{}, copies[0..1]));
    inline for (.{ "source_element", "destination_element" }) |member| {
        copies[0] = initial;
        @field(copies[0], member) = std.math.maxInt(u32);
        try std.testing.expectError(error.Invalid, update_sets(7, &.{}, copies[0..1]));
    }
    copies[0] = initial;
    copies[0].destination_set = copies[0].source_set;
    try std.testing.expectError(error.Invalid, update_sets(7, &.{}, copies[0..1]));
    copies[0].count = 2;
    copies[0].source_element = 1;
    try std.testing.expectError(error.Invalid, update_sets(7, &.{}, copies[0..1]));
    copies[0].source_element = 3;
    _ = try update_sets(7, &.{}, copies[0..1]);
    copies = [_]copy_t{initial} ** 65;
    var native_copies = [_]c.VkCopyDescriptorSet{native_copy(initial)} ** 64;
    var expected: [MaxBytes]u8 = undefined;
    const writer = try update_sets(7, &.{}, copies[0..64]);
    try std.testing.expectEqual(@as(usize, 3112), writer.used);
    const count = venus_descriptor_test_update(0, null, 64, &native_copies, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..count], writer.bytes[0..writer.used]);
}
