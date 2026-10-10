//! Guest-owned descriptor-set update templates. No native caller pointers are retained.
const std = @import("std");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Immutable entry quota, shared with the mixed descriptor update serializer.
pub const MaxEntries: usize = 64;
/// Immutable aggregate descriptor quota, matching the owned sparse descriptor ledger.
pub const MaxDescriptors: usize = 128;
/// Immutable maximum accessible pData extent. Oversized offsets/strides fail before reads.
pub const MaxDataBytes: usize = 65536;
/// Owned snapshot of template entries; layout token is borrowed and checked by the ICD.
/// No allocation or retained caller pointer; caller serializes lifetime and mutations.
pub const snapshot_t = struct {
    entries: [MaxEntries]c.VkDescriptorUpdateTemplateEntry = undefined,
    entry_count: usize = 0,
    descriptor_count: usize = 0,
    data_bytes: usize = 0,
};
/// Owned update records and payloads. Writes borrow only this object's payload storage.
/// Caller keeps this object at its original address until synchronous update returns;
/// copying a populated object requires expansion again. No heap/native ownership.
pub const expanded_t = struct {
    writes: [MaxEntries]c.VkWriteDescriptorSet = undefined,
    images: [MaxDescriptors]c.VkDescriptorImageInfo = undefined,
    buffers: [MaxDescriptors]c.VkDescriptorBufferInfo = undefined,
    texels: [MaxDescriptors]c.VkBufferView = undefined,
    write_count: usize = 0,
};
fn payload_size(descriptor_type: u32) !usize {
    return switch (descriptor_type) {
        0...3, 10 => @sizeOf(c.VkDescriptorImageInfo),
        4, 5 => @sizeOf(c.VkBufferView),
        6...9 => @sizeOf(c.VkDescriptorBufferInfo),
        else => error.Invalid,
    };
}
fn extent(entry: c.VkDescriptorUpdateTemplateEntry) !usize {
    const size = try payload_size(entry.descriptorType);
    if (entry.descriptorCount == 0 or entry.descriptorCount > MaxDescriptors or entry.dstArrayElement > std.math.maxInt(u32) - entry.descriptorCount) return error.Invalid;
    const last = std.math.mul(usize, entry.stride, entry.descriptorCount - 1) catch return error.Invalid;
    const start = std.math.add(usize, entry.offset, last) catch return error.Invalid;
    const end = std.math.add(usize, start, size) catch return error.Invalid;
    if (end > MaxDataBytes) return error.Limit;
    return end;
}
/// Copy checked descriptor-set template entries from a borrowed native create record.
/// [in] info nonnull accessible for call; entry array accessible count<=64 or null if0.
/// Descriptor-set layout/type/compatibility and enabled features are checked by the ICD.
/// Returns owned pointer-free snapshot, Invalid for malformed/unsupported input or Limit
/// for quotas. No allocations, partial result or shared mutable state.
pub fn snapshot(info: *const c.VkDescriptorUpdateTemplateCreateInfo) !snapshot_t {
    if (info.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO or info.pNext != null or info.flags != 0 or info.templateType != c.VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) return error.Invalid;
    if (info.descriptorUpdateEntryCount > MaxEntries) return error.Limit;
    const address = @as(*align(1) const usize, @ptrCast(&info.pDescriptorUpdateEntries)).*;
    if (info.descriptorUpdateEntryCount != 0 and (address == 0 or address % @alignOf(c.VkDescriptorUpdateTemplateEntry) != 0)) return error.Invalid;
    var result: snapshot_t = .{};
    result.entry_count = info.descriptorUpdateEntryCount;
    if (result.entry_count == 0) return result;
    const entries: [*]const c.VkDescriptorUpdateTemplateEntry = @ptrFromInt(address);
    for (entries[0..result.entry_count], 0..) |entry, index| {
        result.data_bytes = @max(result.data_bytes, try extent(entry));
        result.descriptor_count = std.math.add(usize, result.descriptor_count, entry.descriptorCount) catch return error.Limit;
        if (result.descriptor_count > MaxDescriptors) return error.Limit;
        result.entries[index] = entry;
    }
    return result;
}
/// Expand an owned snapshot into native writes using the caller's exact byte offsets/strides.
/// [in] definition immutable checked snapshot; set borrowed nonnull token; data borrowed
/// accessible for definition.data_bytes (nullable only for an empty template), may be unaligned.
/// [out] output exclusive stable-address storage until update returns; no pData pointers retained.
/// Returns Invalid/Limit before reading data for corrupt snapshots, overflow or null input.
/// Core validates exact set/layout compatibility, native resource handles and nullDescriptor.
/// No allocations; disjoint outputs may be expanded concurrently.
pub fn expand(definition: *const snapshot_t, set: c.VkDescriptorSet, data: ?*const anyopaque, output: *expanded_t) !void {
    output.write_count = 0;
    if (set == null or definition.entry_count > MaxEntries or definition.descriptor_count > MaxDescriptors or definition.data_bytes > MaxDataBytes) return error.Invalid;
    var count: usize = 0;
    var needed: usize = 0;
    for (definition.entries[0..definition.entry_count]) |entry| {
        needed = @max(needed, try extent(entry));
        count += entry.descriptorCount;
        if (count > MaxDescriptors) return error.Limit;
    }
    if (count != definition.descriptor_count or needed != definition.data_bytes) return error.Invalid;
    if (needed == 0) return;
    const address = if (data) |pointer| @intFromPtr(pointer) else return error.Invalid;
    _ = std.math.add(usize, address, needed) catch return error.Invalid;
    const bytes: [*]const u8 = @ptrFromInt(address);
    var cursor: usize = 0;
    for (definition.entries[0..definition.entry_count], 0..) |entry, index| {
        var write = std.mem.zeroes(c.VkWriteDescriptorSet);
        write.sType = c.VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = entry.dstBinding;
        write.dstArrayElement = entry.dstArrayElement;
        write.descriptorCount = entry.descriptorCount;
        write.descriptorType = entry.descriptorType;
        const size = try payload_size(entry.descriptorType);
        for (0..entry.descriptorCount) |element| {
            const source = bytes[entry.offset + element * entry.stride ..][0..size];
            const destination = switch (entry.descriptorType) {
                0...3, 10 => std.mem.asBytes(&output.images[cursor + element]),
                4, 5 => std.mem.asBytes(&output.texels[cursor + element]),
                6...9 => std.mem.asBytes(&output.buffers[cursor + element]),
                else => unreachable,
            };
            @memcpy(destination, source);
        }
        switch (entry.descriptorType) {
            0...3, 10 => write.pImageInfo = @ptrCast(&output.images[cursor]),
            4, 5 => write.pTexelBufferView = @ptrCast(&output.texels[cursor]),
            6...9 => write.pBufferInfo = @ptrCast(&output.buffers[cursor]),
            else => unreachable,
        }
        output.writes[index] = write;
        cursor += entry.descriptorCount;
    }
    output.write_count = definition.entry_count;
}
fn create_info(entries: []const c.VkDescriptorUpdateTemplateEntry) c.VkDescriptorUpdateTemplateCreateInfo {
    var info = std.mem.zeroes(c.VkDescriptorUpdateTemplateCreateInfo);
    info.sType = c.VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO;
    info.descriptorUpdateEntryCount = @intCast(entries.len);
    info.pDescriptorUpdateEntries = entries.ptr;
    return info;
}
test "templates copy entries and expand mixed unaligned offset stride data without retaining pData" {
    const allocator = std.testing.allocator;
    var entries = [_]c.VkDescriptorUpdateTemplateEntry{
        .{ .dstBinding = 3, .dstArrayElement = 1, .descriptorCount = 2, .descriptorType = 1, .offset = 1, .stride = 32 },
        .{ .dstBinding = 8, .descriptorCount = 1, .descriptorType = 7, .offset = 70, .stride = 0 },
        .{ .dstBinding = 9, .descriptorCount = 2, .descriptorType = 4, .offset = 100, .stride = 0 },
    };
    var info = create_info(&entries);
    const owned = try snapshot(&info);
    entries[0].offset = MaxDataBytes;
    const bytes = try allocator.alloc(u8, owned.data_bytes);
    defer allocator.free(bytes);
    @memset(bytes, 0);
    const image = c.VkDescriptorImageInfo{ .sampler = @ptrFromInt(11), .imageView = @ptrFromInt(22), .imageLayout = 1 };
    const buffer = c.VkDescriptorBufferInfo{ .buffer = @ptrFromInt(33), .offset = 64, .range = 128 };
    const texel: c.VkBufferView = @ptrFromInt(44);
    @memcpy(bytes[1..][0..@sizeOf(@TypeOf(image))], std.mem.asBytes(&image));
    @memcpy(bytes[33..][0..@sizeOf(@TypeOf(image))], std.mem.asBytes(&image));
    @memcpy(bytes[70..][0..@sizeOf(@TypeOf(buffer))], std.mem.asBytes(&buffer));
    @memcpy(bytes[100..][0..@sizeOf(@TypeOf(texel))], std.mem.asBytes(&texel));
    var output: expanded_t = .{};
    try expand(&owned, @ptrFromInt(55), bytes.ptr, &output);
    @memset(bytes, 0);
    try std.testing.expectEqual(@as(usize, 3), output.write_count);
    try std.testing.expectEqual(@as(usize, 22), @intFromPtr(output.writes[0].pImageInfo[1].imageView.?));
    try std.testing.expectEqual(@as(u64, 128), output.writes[1].pBufferInfo[0].range);
    try std.testing.expectEqual(@as(usize, 44), @intFromPtr(output.writes[2].pTexelBufferView[1].?));
    try std.testing.expectEqual(@as(u32, 1), output.writes[0].dstArrayElement);
}
test "template limits overflow malformed snapshots and empty updates reject before data reads" {
    var entry = c.VkDescriptorUpdateTemplateEntry{ .descriptorType = 6, .descriptorCount = 128, .offset = 0, .stride = 32 };
    var info = create_info(@as(*const [1]c.VkDescriptorUpdateTemplateEntry, @ptrCast(&entry)));
    var owned = try snapshot(&info);
    var output: expanded_t = .{};
    try std.testing.expectError(error.Invalid, expand(&owned, @ptrFromInt(1), null, &output));
    entry.stride = std.math.maxInt(usize);
    try std.testing.expectError(error.Invalid, snapshot(&info));
    entry.stride = 0;
    entry.offset = MaxDataBytes;
    try std.testing.expectError(error.Limit, snapshot(&info));
    entry.offset = 0;
    entry.descriptorCount = 129;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    entry.descriptorCount = 1;
    entry.descriptorType = 100;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    owned.data_bytes += 1;
    try std.testing.expectError(error.Invalid, expand(&owned, @ptrFromInt(1), @ptrFromInt(1), &output));
    info.descriptorUpdateEntryCount = 0;
    info.pDescriptorUpdateEntries = null;
    const empty = try snapshot(&info);
    try expand(&empty, @ptrFromInt(1), null, &output);
    try std.testing.expectEqual(@as(usize, 0), output.write_count);
    info.templateType = c.VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS;
    try std.testing.expectError(error.Invalid, snapshot(&info));
}

test "template snapshot headers native arrays aggregate descriptors and corrupt expansion limits" {
    const entry: c.VkDescriptorUpdateTemplateEntry = .{ .descriptorType = 6, .descriptorCount = 1, .offset = 0, .stride = 0 };
    const initial = create_info(&.{entry});
    var info = initial;
    info.sType = 0;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    info = initial;
    info.pNext = &info;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    info = initial;
    info.flags = 1;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    info = initial;
    info.descriptorUpdateEntryCount = 65;
    try std.testing.expectError(error.Limit, snapshot(&info));
    info = initial;
    info.pDescriptorUpdateEntries = null;
    try std.testing.expectError(error.Invalid, snapshot(&info));
    var misaligned: usize = 3;
    @memcpy(std.mem.asBytes(&info.pDescriptorUpdateEntries), std.mem.asBytes(&misaligned));
    try std.testing.expectError(error.Invalid, snapshot(&info));
    const many = [_]c.VkDescriptorUpdateTemplateEntry{
        .{ .descriptorType = 6, .descriptorCount = 128, .offset = 0, .stride = 0 },
        entry,
    };
    info = create_info(&many);
    try std.testing.expectError(error.Limit, snapshot(&info));
    info = initial;
    const initial_definition = try snapshot(&info);
    var definition = initial_definition;
    var output: expanded_t = .{};
    try std.testing.expectError(error.Invalid, expand(&definition, null, null, &output));
    inline for (.{ "entry_count", "descriptor_count", "data_bytes" }) |field| {
        definition = initial_definition;
        @field(definition, field) = if (comptime std.mem.eql(u8, field, "entry_count")) MaxEntries + 1 else if (comptime std.mem.eql(u8, field, "descriptor_count")) MaxDescriptors + 1 else MaxDataBytes + 1;
        try std.testing.expectError(error.Invalid, expand(&definition, @ptrFromInt(1), null, &output));
    }
    definition = initial_definition;
    definition.entry_count = 2;
    definition.entries[0] = many[0];
    definition.entries[1] = entry;
    try std.testing.expectError(error.Limit, expand(&definition, @ptrFromInt(1), null, &output));
    definition = initial_definition;
    try std.testing.expectError(error.Invalid, expand(&definition, @ptrFromInt(1), @ptrFromInt(std.math.maxInt(usize) - 8), &output));
    try std.testing.expectEqual(@as(usize, 0), output.write_count);
}
