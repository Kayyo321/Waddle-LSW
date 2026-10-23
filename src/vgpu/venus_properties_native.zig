//! Allocation-free native Properties2 topology and transactional named-field publication.
const std = @import("std");
const wire = @import("venus_properties_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Immutable recognized node ceiling; no allocation or mutable storage.
pub const MaxNodes = wire.MaxNodes;
/// Immutable native header walk ceiling excluding the outer root.
pub const MaxWalkNodes: usize = 64;
/// Owned observed header identity; addresses borrow caller storage through snapshot retirement.
pub const observation_t = struct {
    /// Borrowed accessible header address, nonzero in the active prefix.
    address: usize = 0,
    /// Copied SDK tag, unknown tags retained without reading their payload.
    type_tag: u32 = 0,
    /// Copied forward link address, zero terminates the chain.
    next: usize = 0,
};
/// Owned bounded topology snapshot; caller synchronizes all borrowed headers and targets until publication.
pub const chain_t = struct {
    /// Original nullable chain-head address.
    first: usize = 0,
    /// Initialized recognized count0..5.
    count: usize = 0,
    /// Copied recognized canonical tags in forward order.
    tags: [MaxNodes]u32 = [_]u32{0} ** MaxNodes,
    /// Borrowed full known native object addresses, no heap ownership.
    addresses: [MaxNodes]usize = [_]usize{0} ** MaxNodes,
    /// Initialized total header count0..64, including unknown nodes.
    walk_count: usize = 0,
    /// Owned header observations in actual forward order.
    observations: [MaxWalkNodes]observation_t = [_]observation_t{.{}} ** MaxWalkNodes,
    /// Initialized promoted count0..17.
    promoted_count: usize = 0,
    /// Copied promoted canonical tags in forward order, unique.
    promoted_tags: [MaxPromoted]u32 = [_]u32{0} ** MaxPromoted,
    /// Borrowed full promoted native object addresses, no heap ownership.
    promoted_addresses: [MaxPromoted]usize = [_]usize{0} ** MaxPromoted,
};
/// Owned wire request profile: canonical chain tags in forward order, then required parent aggregates.
pub const request_t = struct {
    /// Initialized tag count0..5.
    count: usize = 0,
    /// Unique canonical tags; unused suffix zero.
    tags: [MaxNodes]u32 = [_]u32{0} ** MaxNodes,
};
/// Immutable promoted structure ceiling: the seventeen Vulkan1.1-1.3 promoted property structures.
pub const MaxPromoted: usize = 17;
// Comptime-only registry. Each promoted structure copies its named fields from one aggregate; renames
// list promoted->aggregate names that differ. Comptime proofs below require identical field types.
const promoted_t = struct { native_t: type, tag: u32, core: usize, renames: []const [2][]const u8 = &.{} };
const CoreTags = [_]u32{
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
};
const CoreVersions = [_]u32{ c.VK_API_VERSION_1_1, c.VK_API_VERSION_1_2, c.VK_API_VERSION_1_3 };
const CoreTypes = .{ c.VkPhysicalDeviceVulkan11Properties, c.VkPhysicalDeviceVulkan12Properties, c.VkPhysicalDeviceVulkan13Properties };
const CoreMembers = .{ "core11", "core12", "core13" };
const Promoted = [_]promoted_t{
    .{ .native_t = c.VkPhysicalDeviceIDProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, .core = 0 },
    .{ .native_t = c.VkPhysicalDeviceSubgroupProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .core = 0, .renames = &.{
        .{ "supportedStages", "subgroupSupportedStages" },
        .{ "supportedOperations", "subgroupSupportedOperations" },
        .{ "quadOperationsInAllStages", "subgroupQuadOperationsInAllStages" },
    } },
    .{ .native_t = c.VkPhysicalDevicePointClippingProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES, .core = 0 },
    .{ .native_t = c.VkPhysicalDeviceMultiviewProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES, .core = 0 },
    .{ .native_t = c.VkPhysicalDeviceProtectedMemoryProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES, .core = 0 },
    .{ .native_t = c.VkPhysicalDeviceMaintenance3Properties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, .core = 0 },
    .{ .native_t = c.VkPhysicalDeviceDriverProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceFloatControlsProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceDescriptorIndexingProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceDepthStencilResolveProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceSamplerFilterMinmaxProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceTimelineSemaphoreProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceSubgroupSizeControlProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceInlineUniformBlockProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceTexelBufferAlignmentProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceShaderIntegerDotProductProperties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceMaintenance4Properties, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES, .core = 2 },
};
comptime {
    @setEvalBranchQuota(200000);
    if (Promoted.len != MaxPromoted) @compileError("Promoted property inventory changed");
    for (Promoted, 0..) |entry, index| {
        if (@alignOf(entry.native_t) != 8 or @offsetOf(entry.native_t, "sType") != 0 or @offsetOf(entry.native_t, "pNext") != 8)
            @compileError("Promoted property header ABI changed");
        for (Promoted[0..index]) |previous| if (previous.tag == entry.tag) @compileError("Duplicate promoted property tag");
        for (CoreTags) |tag| if (tag == entry.tag) @compileError("Promoted tag aliases an aggregate");
        for (wire.KnownTags) |tag| if (tag == entry.tag) @compileError("Promoted tag aliases a canonical node");
        const aggregate_t = CoreTypes[entry.core];
        for (@typeInfo(entry.native_t).Struct.fields[2..]) |field| {
            var name: []const u8 = field.name;
            for (entry.renames) |pair| if (std.mem.eql(u8, pair[0], field.name)) { name = pair[1]; };
            if (!@hasField(aggregate_t, name) or @TypeOf(@field(@as(aggregate_t, undefined), name)) != field.type)
                @compileError("Promoted property field has no identical aggregate field: " ++ field.name);
        }
        for (entry.renames) |pair| if (!@hasField(entry.native_t, pair[0])) @compileError("Unused promoted rename");
    }
}
comptime {
    if (@sizeOf(usize) != 8 or @sizeOf(c.VkBaseOutStructure) != 16 or @alignOf(c.VkBaseOutStructure) != 8 or
        @offsetOf(c.VkBaseOutStructure, "sType") != 0 or @offsetOf(c.VkBaseOutStructure, "pNext") != 8 or
        @sizeOf(c.VkPhysicalDeviceProperties2) != 840 or @alignOf(c.VkPhysicalDeviceProperties2) != 8 or
        @offsetOf(c.VkPhysicalDeviceProperties2, "sType") != 0 or @offsetOf(c.VkPhysicalDeviceProperties2, "pNext") != 8 or
        @offsetOf(c.VkPhysicalDeviceProperties2, "properties") != 16)
        @compileError("Pinned x64 native Properties2/header ABI changed");
    if (@alignOf(chain_t) != 8 or @alignOf(wire.result_t) != 8) @compileError("Owned input alignment changed");
}
const range_t = struct { first: usize, end: usize };
fn checked_range(address: usize, size: usize) !range_t {
    if (address == 0 or address % @alignOf(c.VkBaseOutStructure) != 0) return error.Invalid;
    return .{ .first = address, .end = std.math.add(usize, address, size) catch return error.Invalid };
}
fn intersects(left: range_t, right: range_t) bool {
    return left.first < right.end and right.first < left.end;
}
fn node_size(tag: u32) ?usize {
    return switch (tag) {
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES => @sizeOf(c.VkPhysicalDeviceVulkan11Properties),
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES => @sizeOf(c.VkPhysicalDeviceVulkan12Properties),
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES => @sizeOf(c.VkPhysicalDeviceVulkan13Properties),
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT => @sizeOf(c.VkPhysicalDeviceRobustness2PropertiesEXT),
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES_KHR => @sizeOf(c.VkPhysicalDeviceMaintenance5Properties),
        else => null,
    };
}
fn promoted_size(tag: u32) ?usize {
    inline for (Promoted) |entry| if (tag == entry.tag) return @sizeOf(entry.native_t);
    return null;
}
fn promoted_core(tag: u32) ?usize {
    inline for (Promoted) |entry| if (tag == entry.tag) return entry.core;
    return null;
}
/// Collect accessible initialized native headers. [in] first nullable borrowed head, immutable through snapshot use.
/// Caller guarantees accessible full known and promoted SDK objects, accessible unknown headers and exclusive
/// synchronization. Returns owned snapshot or Invalid for alignment/overflow/cycle/duplicate/quota. No output
/// mutation, allocation, payload reads, pointer probing or global state; unknown objects may contain exactly one
/// native header. Promoted structures are recorded separately and never enter the canonical wire profile.
pub fn collect_chain(first: ?*anyopaque) !chain_t {
    var result = chain_t{ .first = if (first) |value| @intFromPtr(value) else 0 };
    var next = result.first;
    while (next != 0) {
        if (result.walk_count == MaxWalkNodes) return error.Invalid; // Reject the next address unread.
        _ = try checked_range(next, @sizeOf(c.VkBaseOutStructure));
        for (result.observations[0..result.walk_count]) |previous| if (previous.address == next) return error.Invalid;
        const header: *const c.VkBaseOutStructure = @ptrFromInt(next);
        const link = if (header.pNext) |value| @intFromPtr(value) else 0;
        result.observations[result.walk_count] = .{ .address = next, .type_tag = header.sType, .next = link };
        result.walk_count += 1;
        if (node_size(header.sType)) |size| {
            _ = try checked_range(next, size);
            if (result.count == MaxNodes) return error.Invalid;
            for (result.tags[0..result.count]) |tag| if (tag == header.sType) return error.Invalid;
            result.tags[result.count] = header.sType;
            result.addresses[result.count] = next;
            result.count += 1;
        } else if (promoted_size(header.sType)) |size| {
            _ = try checked_range(next, size);
            for (result.promoted_tags[0..result.promoted_count]) |tag| if (tag == header.sType) return error.Invalid;
            // Seventeen unique promoted tags bound this index; duplicates are rejected above.
            std.debug.assert(result.promoted_count < MaxPromoted);
            result.promoted_tags[result.promoted_count] = header.sType;
            result.promoted_addresses[result.promoted_count] = next;
            result.promoted_count += 1;
        }
        next = link;
    }
    return result;
}
/// Derive the wire request for one snapshot. [in] chain nonnull immutable borrowed snapshot; api_version is the
/// visible public API. Returns the canonical chain tags in forward order, then each absent parent aggregate in
/// 1.1/1.2/1.3 order for which a promoted structure is present and api_version reaches its promotion version.
/// Invalid for snapshot extents above their ceilings or a forged snapshot whose additions exceed five tags.
/// No allocation, native access, pointer retention or shared state.
pub fn request_tags(chain: *const chain_t, api_version: u32) !request_t {
    if (chain.count > MaxNodes or chain.promoted_count > MaxPromoted) return error.Invalid;
    var request = request_t{ .count = chain.count };
    @memcpy(request.tags[0..chain.count], chain.tags[0..chain.count]);
    for (CoreTags, CoreVersions, 0..) |parent, version, core| {
        if (api_version < version) continue;
        var needed = false;
        for (chain.promoted_tags[0..chain.promoted_count]) |tag| {
            if (promoted_core(tag) == core) needed = true;
        }
        if (!needed or std.mem.indexOfScalar(u32, request.tags[0..request.count], parent) != null) continue;
        if (request.count == MaxNodes) return error.Invalid;
        request.tags[request.count] = parent;
        request.count += 1;
    }
    return request;
}
fn request_index(request: *const request_t, tag: u32) usize {
    for (request.tags[0..request.count], 0..) |known, index| if (known == tag) return index;
    unreachable; // request_tags admitted the parent of every published promoted structure.
}
fn copy_promoted(comptime entry: promoted_t, output: *entry.native_t, input: *const CoreTypes[entry.core]) void {
    inline for (@typeInfo(entry.native_t).Struct.fields[2..]) |field| {
        const name = comptime name: {
            for (entry.renames) |pair| if (std.mem.eql(u8, pair[0], field.name)) break :name pair[1];
            break :name field.name;
        };
        copy_named(field.type, &@field(output, field.name), &@field(input, name));
    }
}
fn equal_snapshot(left: *const chain_t, right: *const chain_t) bool {
    if (left.first != right.first or left.count != right.count or left.walk_count != right.walk_count) return false;
    for (left.tags[0..left.count], right.tags[0..right.count]) |a, b| if (a != b) return false;
    for (left.addresses[0..left.count], right.addresses[0..right.count]) |a, b| if (a != b) return false;
    for (left.observations[0..left.walk_count], right.observations[0..right.walk_count]) |a, b| {
        if (a.address != b.address or a.type_tag != b.type_tag or a.next != b.next) return false;
    }
    if (left.promoted_count != right.promoted_count) return false;
    for (left.promoted_tags[0..left.promoted_count], right.promoted_tags[0..right.promoted_count]) |a, b| if (a != b) return false;
    for (left.promoted_addresses[0..left.promoted_count], right.promoted_addresses[0..right.promoted_count]) |a, b| if (a != b) return false;
    return true;
}
fn copy_named(comptime value_t: type, output: *value_t, input: *const value_t) void {
    switch (@typeInfo(value_t)) {
        .Struct => |structure| inline for (structure.fields) |field| {
            if (comptime !std.mem.eql(u8, field.name, "sType") and !std.mem.eql(u8, field.name, "pNext")) {
                copy_named(field.type, &@field(output, field.name), &@field(input, field.name));
            }
        },
        else => output.* = input.*, // Scalars and fixed arrays contain no native padding.
    }
}
/// Publish one complete normalized result transactionally. [in,out] output nullable outer SDK address, exclusive
/// initialized accessible storage; [in] chain/value nonnull immutable accessible borrowed inputs, disjoint from targets.
/// value must answer request_tags(chain, value.properties.apiVersion), the projected visible API version.
/// Caller synchronizes headers/links and guarantees unknown full payloads disjoint (their sizes are never guessed).
/// Returns Invalid with every output byte unchanged on root/profile/topology/range/alias/payload failure.
/// Success writes only named payload fields; promoted structures above the visible API stay unchanged. Headers,
/// links, native padding, unknown objects and canaries remain intact. No allocation, pointer retention, transport,
/// locks or shared state; distinct caller-owned transactions are thread-safe.
pub fn publish_properties2(output: ?*anyopaque, chain: *const chain_t, value: *const wire.result_t) !void {
    const address = if (output) |pointer| @intFromPtr(pointer) else return error.Invalid;
    const root_range = try checked_range(address, @sizeOf(c.VkPhysicalDeviceProperties2));
    const chain_range = try checked_range(@intFromPtr(chain), @sizeOf(chain_t));
    const value_range = try checked_range(@intFromPtr(value), @sizeOf(wire.result_t));
    if (intersects(root_range, chain_range) or intersects(root_range, value_range)) return error.Invalid;
    if (chain.count > MaxNodes or chain.walk_count > MaxWalkNodes or chain.promoted_count > MaxPromoted) return error.Invalid;
    var ranges: [MaxNodes + MaxPromoted]range_t = undefined;
    for (chain.tags[0..chain.count], chain.addresses[0..chain.count], 0..) |tag, target, index| {
        ranges[index] = try checked_range(target, node_size(tag) orelse return error.Invalid);
        if (intersects(ranges[index], root_range) or intersects(ranges[index], chain_range) or intersects(ranges[index], value_range)) return error.Invalid;
        for (ranges[0..index]) |previous| if (intersects(ranges[index], previous)) return error.Invalid;
    }
    for (chain.promoted_tags[0..chain.promoted_count], chain.promoted_addresses[0..chain.promoted_count], chain.count..) |tag, target, index| {
        ranges[index] = try checked_range(target, promoted_size(tag) orelse return error.Invalid);
        if (intersects(ranges[index], root_range) or intersects(ranges[index], chain_range) or intersects(ranges[index], value_range)) return error.Invalid;
        for (ranges[0..index]) |previous| if (intersects(ranges[index], previous)) return error.Invalid;
    }
    const target_count = chain.count + chain.promoted_count;
    for (chain.observations[0..chain.walk_count]) |observed| {
        const header_range = try checked_range(observed.address, @sizeOf(c.VkBaseOutStructure));
        if (node_size(observed.type_tag) == null and promoted_size(observed.type_tag) == null) {
            if (intersects(root_range, header_range)) return error.Invalid;
            for (ranges[0..target_count]) |target| if (intersects(target, header_range)) return error.Invalid;
        }
    }
    const root: *c.VkPhysicalDeviceProperties2 = @ptrFromInt(address);
    if (root.sType != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2) return error.Invalid;
    const actual = try collect_chain(@ptrCast(root.pNext));
    if (!equal_snapshot(chain, &actual)) return error.Invalid;
    const api_version = value.properties.apiVersion;
    const request = try request_tags(chain, api_version);
    wire.validate_result(request.tags[0..request.count], value) catch return error.Invalid;
    // Every topology/range/data check has completed. No fallible operation follows the first write.
    copy_named(c.VkPhysicalDeviceProperties, &root.properties, &value.properties);
    for (chain.tags[0..chain.count], chain.addresses[0..chain.count], value.nodes[0..chain.count]) |tag, target, *node| {
        switch (tag) {
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES => copy_named(c.VkPhysicalDeviceVulkan11Properties, @ptrFromInt(target), &node.data.core11),
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES => copy_named(c.VkPhysicalDeviceVulkan12Properties, @ptrFromInt(target), &node.data.core12),
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES => copy_named(c.VkPhysicalDeviceVulkan13Properties, @ptrFromInt(target), &node.data.core13),
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT => copy_named(c.VkPhysicalDeviceRobustness2PropertiesEXT, @ptrFromInt(target), &node.data.robustness),
            else => copy_named(c.VkPhysicalDeviceMaintenance5Properties, @ptrFromInt(target), &node.data.maintenance),
        }
    }
    for (chain.promoted_tags[0..chain.promoted_count], chain.promoted_addresses[0..chain.promoted_count]) |tag, target| {
        inline for (Promoted) |entry| {
            if (tag == entry.tag and api_version >= CoreVersions[entry.core]) {
                const parent = &value.nodes[request_index(&request, CoreTags[entry.core])];
                copy_promoted(entry, @ptrFromInt(target), &@field(parent.data, CoreMembers[entry.core]));
            }
        }
    }
}

// Test-only fixtures.
extern fn venus_properties_test_fixture([*]const u32, usize, *c.VkPhysicalDeviceProperties, [*]wire.data_t) void;
extern fn venus_properties_test_encode([*]const u32, usize, *const c.VkPhysicalDeviceProperties, [*]const wire.data_t, [*]u8) usize;
const guarded_root_t = extern struct { before: u64, value: c.VkPhysicalDeviceProperties2, after: u64 };
const guarded_node_t = extern struct { before: u64, value: wire.data_t, after: u64 };
const fixture_t = struct {
    root: guarded_root_t,
    nodes: [5]guarded_node_t,
};
fn fixture_init(fixture: *fixture_t, tags: []const u32) void {
    @memset(std.mem.asBytes(fixture), 0xa7);
    fixture.root.value.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    fixture.root.value.pNext = if (tags.len != 0) &fixture.nodes[0].value else null;
    for (tags, 0..) |tag, index| {
        const header: *c.VkBaseOutStructure = @ptrCast(&fixture.nodes[index].value);
        header.sType = tag;
        header.pNext = if (index + 1 < tags.len) @ptrCast(&fixture.nodes[index + 1].value) else null;
    }
}
fn fixture_result(tags: []const u32) !wire.result_t {
    var core: c.VkPhysicalDeviceProperties = undefined;
    var nodes: [5]wire.data_t = undefined;
    venus_properties_test_fixture(tags.ptr, tags.len, &core, &nodes);
    var bytes: [4096]u8 = undefined;
    const size = venus_properties_test_encode(tags.ptr, tags.len, &core, &nodes, &bytes);
    return wire.decode(bytes[0..size], tags);
}
fn fixture_mark(comptime value_t: type, mask: []bool, offset: usize) void {
    switch (@typeInfo(value_t)) {
        .Struct => |structure| inline for (structure.fields) |field| {
            if (comptime !std.mem.eql(u8, field.name, "sType") and !std.mem.eql(u8, field.name, "pNext")) fixture_mark(field.type, mask, offset + @offsetOf(value_t, field.name));
        },
        else => @memset(mask[offset..][0..@sizeOf(value_t)], true),
    }
}
fn fixture_preserved(comptime value_t: type, before: []const u8, after: []const u8) !void {
    var mask = [_]bool{false} ** @sizeOf(guarded_node_t);
    fixture_mark(value_t, &mask, 8);
    for (before, after, 0..) |old, new, index| if (!mask[index]) try std.testing.expectEqual(old, new);
}
fn fixture_expect(comptime value_t: type, output: *const value_t, input: *const value_t) !void {
    inline for (@typeInfo(value_t).Struct.fields) |field| {
        if (comptime !std.mem.eql(u8, field.name, "sType") and !std.mem.eql(u8, field.name, "pNext")) try std.testing.expectEqualDeep(@field(input, field.name), @field(output, field.name));
    }
}
fn fixture_check(tags: []const u32) !void {
    var fixture: fixture_t = undefined;
    fixture_init(&fixture, tags);
    const before = std.mem.asBytes(&fixture).*;
    const chain = try collect_chain(fixture.root.value.pNext);
    const result = try fixture_result(tags);
    try publish_properties2(&fixture.root.value, &chain, &result);
    try std.testing.expectEqualDeep(result.properties, fixture.root.value.properties);
    var root_mask = [_]bool{false} ** @sizeOf(guarded_root_t);
    fixture_mark(c.VkPhysicalDeviceProperties, &root_mask, @offsetOf(guarded_root_t, "value") + @offsetOf(c.VkPhysicalDeviceProperties2, "properties"));
    for (before[0..@sizeOf(guarded_root_t)], std.mem.asBytes(&fixture.root), 0..) |old, new, index| if (!root_mask[index]) try std.testing.expectEqual(old, new);
    for (tags, 0..) |tag, index| {
        const offset = @offsetOf(fixture_t, "nodes") + index * @sizeOf(guarded_node_t);
        const old = before[offset..][0..@sizeOf(guarded_node_t)];
        const new = std.mem.asBytes(&fixture.nodes[index]);
        switch (tag) {
            wire.KnownTags[0] => {
                try fixture_expect(c.VkPhysicalDeviceVulkan11Properties, &fixture.nodes[index].value.core11, &result.nodes[index].data.core11);
                try fixture_preserved(c.VkPhysicalDeviceVulkan11Properties, old, new);
            },
            wire.KnownTags[1] => {
                try fixture_expect(c.VkPhysicalDeviceVulkan12Properties, &fixture.nodes[index].value.core12, &result.nodes[index].data.core12);
                try fixture_preserved(c.VkPhysicalDeviceVulkan12Properties, old, new);
            },
            wire.KnownTags[2] => {
                try fixture_expect(c.VkPhysicalDeviceVulkan13Properties, &fixture.nodes[index].value.core13, &result.nodes[index].data.core13);
                try fixture_preserved(c.VkPhysicalDeviceVulkan13Properties, old, new);
            },
            wire.KnownTags[3] => {
                try fixture_expect(c.VkPhysicalDeviceRobustness2PropertiesEXT, &fixture.nodes[index].value.robustness, &result.nodes[index].data.robustness);
                try fixture_preserved(c.VkPhysicalDeviceRobustness2PropertiesEXT, old, new);
            },
            else => {
                try fixture_expect(c.VkPhysicalDeviceMaintenance5Properties, &fixture.nodes[index].value.maintenance, &result.nodes[index].data.maintenance);
                try fixture_preserved(c.VkPhysicalDeviceMaintenance5Properties, old, new);
            },
        }
    }
    const inactive = @offsetOf(fixture_t, "nodes") + tags.len * @sizeOf(guarded_node_t);
    try std.testing.expectEqualSlices(u8, before[inactive..], std.mem.asBytes(&fixture)[inactive..]);
}
fn fixture_profiles(tags: *[5]u32, count: usize, visited: *usize) !void {
    try fixture_check(tags[0..count]);
    visited.* += 1;
    if (count == 5) return;
    for (wire.KnownTags) |tag| {
        if (std.mem.indexOfScalar(u32, tags[0..count], tag) != null) continue;
        tags[count] = tag;
        try fixture_profiles(tags, count + 1, visited);
    }
}
fn fixture_reject(fixture: *fixture_t, output: ?*anyopaque, chain: *const chain_t, result: *const wire.result_t) !void {
    const before = std.mem.asBytes(fixture).*;
    try std.testing.expectError(error.Invalid, publish_properties2(output, chain, result));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(fixture));
}
test "Properties2 native all326 typed topology permutations preserve every header padding and canary" {
    @setEvalBranchQuota(100000);
    var tags: [5]u32 = undefined;
    var visited: usize = 0;
    try fixture_profiles(&tags, 0, &visited);
    try std.testing.expectEqual(@as(usize, 326), visited);
}
test "Properties2 native unknown header-only owners quotas cycles duplicates alignment and overflow" {
    try std.testing.expectEqual(@as(usize, 0), (try collect_chain(null)).count);
    try std.testing.expectError(error.Invalid, collect_chain(@ptrFromInt(1)));
    try std.testing.expectError(error.Invalid, collect_chain(@ptrFromInt(std.math.maxInt(usize) - 7)));
    const headers = try std.testing.allocator.alloc(c.VkBaseOutStructure, 64);
    defer std.testing.allocator.free(headers);
    for (headers, 0..) |*header, index| {
        header.* = .{ .sType = 0x7fffffff, .pNext = if (index + 1 < 64) &headers[index + 1] else null };
    }
    const snapshot = try collect_chain(headers.ptr);
    try std.testing.expectEqual(@as(usize, 64), snapshot.walk_count);
    try std.testing.expectEqual(@as(usize, 0), snapshot.count);
    headers[63].pNext = @ptrFromInt(8); // Sixty-fifth address rejected unread, even if inaccessible.
    try std.testing.expectError(error.Invalid, collect_chain(headers.ptr));
    headers[63].pNext = null;
    headers[0].pNext = &headers[0];
    try std.testing.expectError(error.Invalid, collect_chain(headers.ptr));
    var fixture: fixture_t = undefined;
    fixture_init(&fixture, &.{ wire.KnownTags[0], wire.KnownTags[0] });
    try std.testing.expectError(error.Invalid, collect_chain(fixture.root.value.pNext));
    fixture_init(&fixture, &wire.KnownTags);
    const last: *c.VkBaseOutStructure = @ptrCast(&fixture.nodes[4].value);
    const sixth = try std.testing.allocator.create(c.VkPhysicalDeviceVulkan11Properties);
    defer std.testing.allocator.destroy(sixth);
    sixth.* = std.mem.zeroes(c.VkPhysicalDeviceVulkan11Properties);
    sixth.sType = wire.KnownTags[0];
    last.pNext = @ptrCast(sixth);
    try std.testing.expectError(error.Invalid, collect_chain(fixture.root.value.pNext));
    const header = try std.testing.allocator.create(c.VkBaseOutStructure);
    defer std.testing.allocator.destroy(header);
    header.* = .{ .sType = 0x7ffffffe, .pNext = null };
    fixture_init(&fixture, &.{});
    fixture.root.value.pNext = header;
    const chain = try collect_chain(header);
    const before = std.mem.asBytes(header).*;
    const result = try fixture_result(&.{});
    try publish_properties2(&fixture.root.value, &chain, &result);
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(header));
}
test "Properties2 native every late validation and snapshot mutation leaves all bytes unchanged" {
    var fixture: fixture_t = undefined;
    fixture_init(&fixture, &wire.KnownTags);
    var chain = try collect_chain(fixture.root.value.pNext);
    var result = try fixture_result(&wire.KnownTags);
    try fixture_reject(&fixture, null, &chain, &result);
    try fixture_reject(&fixture, @ptrFromInt(@intFromPtr(&fixture.root.value) + 1), &chain, &result);
    try fixture_reject(&fixture, @ptrFromInt(std.math.maxInt(usize) - 7), &chain, &result);
    fixture.root.value.sType = 0;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    fixture.root.value.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    chain.count = 6;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain.count = 5;
    chain.walk_count = 65;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain.walk_count = 5;
    result.nodes[4].data.maintenance.polygonModePointSize = 2;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    result = try fixture_result(&wire.KnownTags);
    result.properties.limits.timestampPeriod = std.math.inf(f32);
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    result = try fixture_result(&wire.KnownTags);
    result.count = 4;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    result = try fixture_result(&wire.KnownTags);
    chain.tags[4] = 0;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain.tags[4] = wire.KnownTags[4];
    chain.addresses[4] = 0;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    chain.addresses[4] = @intFromPtr(&fixture.nodes[4].value) + 1;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    chain.addresses[4] = chain.addresses[3] + 8;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    chain.addresses[4] = @intFromPtr(&fixture.root.value);
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    chain.addresses[4] = @intFromPtr(&chain);
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    chain.addresses[4] = @intFromPtr(&result);
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    chain.observations[4].address = 1;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    chain.observations[4].type_tag = 0x7ffffffe;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    chain.observations[4].next = 8;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    chain.first = 8;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    fixture.nodes[4].value.maintenance.sType = 0x7fffffff;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    fixture_init(&fixture, &wire.KnownTags);
    chain = try collect_chain(fixture.root.value.pNext);
    const first: *c.VkBaseOutStructure = @ptrCast(&fixture.nodes[0].value);
    first.pNext = null;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    fixture_init(&fixture, &wire.KnownTags);
    chain = try collect_chain(fixture.root.value.pNext);
    chain.walk_count = 4;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    const swapped = chain.addresses[1];
    chain.addresses[1] = chain.addresses[2];
    chain.addresses[2] = swapped;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
}
test "Properties2 native input/root/known/unknown alias ranges reject before any publication" {
    var fixture: fixture_t = undefined;
    fixture_init(&fixture, &.{});
    var chain = try collect_chain(null);
    var result = try fixture_result(&.{});
    try fixture_reject(&fixture, @ptrCast(&chain), &chain, &result);
    try fixture_reject(&fixture, @ptrCast(&result), &chain, &result);
    fixture.root.value.pNext = &fixture.root.value;
    chain = chain_t{ .first = @intFromPtr(&fixture.root.value), .walk_count = 1 };
    chain.observations[0] = .{ .address = chain.first, .type_tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .next = chain.first };
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    fixture_init(&fixture, &.{wire.KnownTags[0]});
    chain = try collect_chain(fixture.root.value.pNext);
    result = try fixture_result(&.{wire.KnownTags[0]});
    // Forged unknown observation intersects full known target; rejected before actual topology reads.
    chain.walk_count = 2;
    chain.observations[1] = .{ .address = chain.addresses[0] + 16, .type_tag = 0x7fffffff, .next = 0 };
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
    chain.observations[1].address = @intFromPtr(&fixture.root.value) + 16;
    try fixture_reject(&fixture, &fixture.root.value, &chain, &result);
}
test "Properties2 native256 allocator-owned success/error transactions retire every owner" {
    for (0..256) |cycle| {
        const fixture = try std.testing.allocator.create(fixture_t);
        defer std.testing.allocator.destroy(fixture);
        var tags = wire.KnownTags;
        std.mem.rotate(u32, &tags, cycle % 5);
        fixture_init(fixture, &tags);
        const chain = try collect_chain(fixture.root.value.pNext);
        var result = try fixture_result(&tags);
        try publish_properties2(&fixture.root.value, &chain, &result);
        try std.testing.expectEqualDeep(result.properties, fixture.root.value.properties);
        result.count = 0;
        try fixture_reject(fixture, &fixture.root.value, &chain, &result);
    }
}
const PromotedBytes = size: {
    var size: usize = 0;
    for (Promoted) |entry| size = @max(size, @sizeOf(entry.native_t));
    break :size size;
};
const guarded_promoted_t = extern struct { before: u64, value: [PromotedBytes]u8 align(8), after: u64 };
const guarded_header_t = extern struct { before: u64, value: c.VkBaseOutStructure, after: u64 };
const promoted_fixture_t = struct {
    root: guarded_root_t,
    nodes: [5]guarded_node_t,
    unknown: guarded_header_t,
    promoted: [MaxPromoted]guarded_promoted_t,
};
fn promoted_header(fixture: *promoted_fixture_t, index: usize) *c.VkBaseOutStructure {
    return @ptrCast(&fixture.promoted[index].value);
}
fn known_header(fixture: *promoted_fixture_t, index: usize) *c.VkBaseOutStructure {
    return @ptrCast(&fixture.nodes[index].value);
}
fn promoted_init(fixture: *promoted_fixture_t, headers: []const *c.VkBaseOutStructure) void {
    @memset(std.mem.asBytes(fixture), 0xa7);
    fixture.root.value.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    for (0..5) |index| known_header(fixture, index).sType = wire.KnownTags[index];
    inline for (Promoted, 0..) |entry, index| promoted_header(fixture, index).sType = entry.tag;
    fixture.unknown.value.sType = 0x7fffffff;
    fixture.root.value.pNext = if (headers.len != 0) headers[0] else null;
    for (headers, 0..) |header, index| header.pNext = if (index + 1 < headers.len) headers[index + 1] else null;
}
fn promoted_expect(comptime entry: promoted_t, output: *const entry.native_t, parent: *const CoreTypes[entry.core]) !void {
    inline for (@typeInfo(entry.native_t).Struct.fields[2..]) |field| {
        const name = comptime name: {
            for (entry.renames) |pair| if (std.mem.eql(u8, pair[0], field.name)) break :name pair[1];
            break :name field.name;
        };
        try std.testing.expectEqualDeep(@field(parent, name), @field(output, field.name));
    }
}
// Every promoted slot present in chain: named fields equal the parent when admitted, all other bytes unchanged.
fn promoted_verify(fixture: *promoted_fixture_t, before: *const [@sizeOf(promoted_fixture_t)]u8, chain: *const chain_t, request: *const request_t, result: *const wire.result_t) !void {
    const api = result.properties.apiVersion;
    inline for (Promoted, 0..) |entry, index| {
        const offset = @offsetOf(promoted_fixture_t, "promoted") + index * @sizeOf(guarded_promoted_t);
        const old = before[offset..][0..@sizeOf(guarded_promoted_t)];
        const new = std.mem.asBytes(&fixture.promoted[index]);
        const present = std.mem.indexOfScalar(u32, chain.promoted_tags[0..chain.promoted_count], entry.tag) != null;
        const written = present and api >= CoreVersions[entry.core];
        var mask = [_]bool{false} ** @sizeOf(guarded_promoted_t);
        if (written) {
            fixture_mark(entry.native_t, &mask, 8);
            const parent = &result.nodes[request_index(request, CoreTags[entry.core])];
            const output: *const entry.native_t = @ptrCast(@alignCast(&fixture.promoted[index].value));
            try promoted_expect(entry, output, &@field(parent.data, CoreMembers[entry.core]));
        }
        for (old, new, 0..) |a, b, byte| if (!mask[byte]) try std.testing.expectEqual(a, b);
    }
    const unknown = @offsetOf(promoted_fixture_t, "unknown");
    try std.testing.expectEqualSlices(u8, before[unknown..][0..@sizeOf(guarded_header_t)], std.mem.asBytes(&fixture.unknown));
}
fn promoted_publish(fixture: *promoted_fixture_t, api: u32) !struct { chain: chain_t, request: request_t, result: wire.result_t } {
    const chain = try collect_chain(fixture.root.value.pNext);
    const request = try request_tags(&chain, api);
    var result = try fixture_result(request.tags[0..request.count]);
    result.properties.apiVersion = api;
    const before = std.mem.asBytes(fixture).*;
    try publish_properties2(&fixture.root.value, &chain, &result);
    try promoted_verify(fixture, &before, &chain, &request, &result);
    for (chain.tags[0..chain.count], 0..) |tag, index| {
        const slot = std.mem.indexOfScalar(u32, &wire.KnownTags, tag).?;
        const output = std.mem.asBytes(&fixture.nodes[slot].value);
        var expected = before[@offsetOf(promoted_fixture_t, "nodes") + slot * @sizeOf(guarded_node_t) + 8 ..][0..@sizeOf(wire.data_t)].*;
        switch (slot) {
            0 => copy_named(c.VkPhysicalDeviceVulkan11Properties, @ptrCast(@alignCast(&expected)), &result.nodes[index].data.core11),
            1 => copy_named(c.VkPhysicalDeviceVulkan12Properties, @ptrCast(@alignCast(&expected)), &result.nodes[index].data.core12),
            2 => copy_named(c.VkPhysicalDeviceVulkan13Properties, @ptrCast(@alignCast(&expected)), &result.nodes[index].data.core13),
            3 => copy_named(c.VkPhysicalDeviceRobustness2PropertiesEXT, @ptrCast(@alignCast(&expected)), &result.nodes[index].data.robustness),
            else => copy_named(c.VkPhysicalDeviceMaintenance5Properties, @ptrCast(@alignCast(&expected)), &result.nodes[index].data.maintenance),
        }
        try std.testing.expectEqualSlices(u8, &expected, output);
    }
    return .{ .chain = chain, .request = request, .result = result };
}
const PromotedApis = [_]u32{ c.VK_API_VERSION_1_0, c.VK_API_VERSION_1_1, c.VK_API_VERSION_1_2, c.VK_API_VERSION_1_3 | 280 };
test "Properties2 native every promoted structure copies its aggregate under each API gate and chain shape" {
    @setEvalBranchQuota(1000000);
    const fixture = try std.testing.allocator.create(promoted_fixture_t);
    defer std.testing.allocator.destroy(fixture);
    var published: usize = 0;
    inline for (Promoted, 0..) |entry, index| {
        for (PromotedApis) |api| for (0..4) |shape| {
            const promoted = promoted_header(fixture, index);
            const parent = known_header(fixture, entry.core);
            const unknown: *c.VkBaseOutStructure = &fixture.unknown.value;
            switch (shape) {
                0 => promoted_init(fixture, &.{promoted}),
                1 => promoted_init(fixture, &.{ parent, promoted }),
                2 => promoted_init(fixture, &.{ promoted, unknown, parent }),
                else => promoted_init(fixture, &.{ unknown, promoted }),
            }
            const outcome = try promoted_publish(fixture, api);
            const with_parent = shape == 1 or shape == 2;
            const admitted = api >= CoreVersions[entry.core];
            try std.testing.expectEqual(@as(usize, if (with_parent or admitted) 1 else 0), outcome.request.count);
            if (outcome.request.count == 1) try std.testing.expectEqual(CoreTags[entry.core], outcome.request.tags[0]);
            try std.testing.expectEqual(@as(usize, 1), outcome.chain.promoted_count);
            published += 1;
        };
    }
    try std.testing.expectEqual(@as(usize, MaxPromoted * 16), published);
}
test "Properties2 native combined seventeen promoted structures and five canonical nodes in rotated orders" {
    @setEvalBranchQuota(1000000);
    const fixture = try std.testing.allocator.create(promoted_fixture_t);
    defer std.testing.allocator.destroy(fixture);
    var headers: [MaxPromoted + 5]*c.VkBaseOutStructure = undefined;
    for (0..MaxPromoted) |rotation| {
        for (0..MaxPromoted) |index| headers[index] = promoted_header(fixture, index);
        std.mem.rotate(*c.VkBaseOutStructure, headers[0..MaxPromoted], rotation);
        for (PromotedApis) |api| {
            promoted_init(fixture, headers[0..MaxPromoted]);
            const outcome = try promoted_publish(fixture, api);
            var expected: usize = 0;
            for (CoreVersions) |version| expected += @intFromBool(api >= version);
            try std.testing.expectEqual(expected, outcome.request.count);
            try std.testing.expectEqual(@as(usize, MaxPromoted), outcome.chain.promoted_count);
            for (CoreTags[0..expected], outcome.request.tags[0..expected]) |tag, requested| try std.testing.expectEqual(tag, requested);
        }
        for (0..5) |index| headers[MaxPromoted + index] = known_header(fixture, index);
        std.mem.rotate(*c.VkBaseOutStructure, &headers, rotation % 11);
        promoted_init(fixture, &headers);
        const outcome = try promoted_publish(fixture, PromotedApis[3]);
        try std.testing.expectEqual(@as(usize, 5), outcome.request.count);
        try std.testing.expectEqual(@as(usize, 5), outcome.chain.count);
    }
}
fn promoted_reject(fixture: *promoted_fixture_t, chain: *const chain_t, result: *const wire.result_t) !void {
    const before = std.mem.asBytes(fixture).*;
    try std.testing.expectError(error.Invalid, publish_properties2(&fixture.root.value, chain, result));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(fixture));
}
test "Properties2 native promoted duplicates ranges aliases forged requests and stale snapshots reject unchanged" {
    const fixture = try std.testing.allocator.create(promoted_fixture_t);
    defer std.testing.allocator.destroy(fixture);
    const api = PromotedApis[3];
    // Duplicate promoted tag: the second header re-tags another slot.
    promoted_init(fixture, &.{ promoted_header(fixture, 6), promoted_header(fixture, 7) });
    promoted_header(fixture, 7).sType = comptime Promoted[6].tag;
    try std.testing.expectError(error.Invalid, collect_chain(fixture.root.value.pNext));
    // Full promoted extent overflows the address space.
    const tail = std.math.maxInt(usize) - 7 - 15;
    try std.testing.expectError(error.Invalid, checked_range(tail, @sizeOf(c.VkPhysicalDeviceDriverProperties)));
    promoted_init(fixture, &.{ promoted_header(fixture, 6), known_header(fixture, 1) });
    var chain = try collect_chain(fixture.root.value.pNext);
    var request = try request_tags(&chain, api);
    var result = try fixture_result(request.tags[0..request.count]);
    result.properties.apiVersion = api;
    // Snapshot ceilings and forged requests.
    chain.promoted_count = MaxPromoted + 1;
    try std.testing.expectError(error.Invalid, request_tags(&chain, api));
    try promoted_reject(fixture, &chain, &result);
    chain = try collect_chain(fixture.root.value.pNext);
    var forged = chain;
    forged.count = 5;
    forged.tags = [_]u32{wire.KnownTags[3]} ** 5;
    try std.testing.expectError(error.Invalid, request_tags(&forged, api));
    forged.count = 6;
    try std.testing.expectError(error.Invalid, request_tags(&forged, api));
    // Promoted target overlapping the canonical target, root, snapshot and reply.
    for ([_]usize{ chain.addresses[0] + 8, @intFromPtr(&fixture.root.value), @intFromPtr(&chain) + 8, @intFromPtr(&result) + 8 }) |address| {
        var aliased = chain;
        aliased.promoted_addresses[0] = address;
        try promoted_reject(fixture, &aliased, &result);
    }
    var misaligned = chain;
    misaligned.promoted_addresses[0] += 4;
    try promoted_reject(fixture, &misaligned, &result);
    var retagged = chain;
    retagged.promoted_tags[0] = 0x7ffffffe;
    try promoted_reject(fixture, &retagged, &result);
    var renamed = chain;
    renamed.promoted_tags[0] = comptime Promoted[7].tag;
    try promoted_reject(fixture, &renamed, &result);
    var missing = chain;
    missing.promoted_count = 0;
    try promoted_reject(fixture, &missing, &result);
    // Unknown header forged inside the promoted target.
    var unknown = chain;
    unknown.walk_count = 3;
    unknown.observations[2] = .{ .address = chain.promoted_addresses[0] + 16, .type_tag = 0x7fffffff, .next = 0 };
    try promoted_reject(fixture, &unknown, &result);
    // Two promoted targets that overlap each other.
    promoted_init(fixture, &.{ promoted_header(fixture, 6), promoted_header(fixture, 7) });
    chain = try collect_chain(fixture.root.value.pNext);
    request = try request_tags(&chain, api);
    result = try fixture_result(request.tags[0..request.count]);
    result.properties.apiVersion = api;
    var pair = chain;
    pair.promoted_addresses[1] = chain.promoted_addresses[0] + 8;
    try promoted_reject(fixture, &pair, &result);
    // Reply that omits the required parent, or answers a lower gate.
    const bare = try fixture_result(&.{});
    var lower = bare;
    lower.properties.apiVersion = api;
    try promoted_reject(fixture, &chain, &lower);
    var gated = result;
    gated.properties.apiVersion = c.VK_API_VERSION_1_1;
    try promoted_reject(fixture, &chain, &gated);
    // Live link changed after the snapshot.
    promoted_header(fixture, 6).pNext = null;
    try promoted_reject(fixture, &chain, &result);
}
test "Properties2 native256 allocator-owned promoted transactions retire every owner" {
    for (0..256) |cycle| {
        const fixture = try std.testing.allocator.create(promoted_fixture_t);
        defer std.testing.allocator.destroy(fixture);
        const index = cycle % MaxPromoted;
        promoted_init(fixture, &.{ promoted_header(fixture, index), known_header(fixture, cycle % 5) });
        _ = try promoted_publish(fixture, PromotedApis[cycle % 4]);
    }
}
