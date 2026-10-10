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
};
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
/// Collect accessible initialized native headers. [in] first nullable borrowed head, immutable through snapshot use.
/// Caller guarantees accessible full known SDK objects, accessible unknown headers and exclusive synchronization.
/// Returns owned snapshot or Invalid for alignment/overflow/cycle/duplicate/quota. No output mutation, allocation,
/// payload reads, pointer probing or global state; unknown objects may contain exactly one native header.
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
        }
        next = link;
    }
    return result;
}
fn equal_snapshot(left: *const chain_t, right: *const chain_t) bool {
    if (left.first != right.first or left.count != right.count or left.walk_count != right.walk_count) return false;
    for (left.tags[0..left.count], right.tags[0..right.count]) |a, b| if (a != b) return false;
    for (left.addresses[0..left.count], right.addresses[0..right.count]) |a, b| if (a != b) return false;
    for (left.observations[0..left.walk_count], right.observations[0..right.walk_count]) |a, b| {
        if (a.address != b.address or a.type_tag != b.type_tag or a.next != b.next) return false;
    }
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
/// Caller synchronizes headers/links and guarantees unknown full payloads disjoint (their sizes are never guessed).
/// Returns Invalid with every output byte unchanged on root/profile/topology/range/alias/payload failure.
/// Success writes only named payload fields; headers, links, native padding, unknown objects and canaries remain intact.
/// No allocation, pointer retention, transport, locks or shared state; distinct caller-owned transactions are thread-safe.
pub fn publish_properties2(output: ?*anyopaque, chain: *const chain_t, value: *const wire.result_t) !void {
    const address = if (output) |pointer| @intFromPtr(pointer) else return error.Invalid;
    const root_range = try checked_range(address, @sizeOf(c.VkPhysicalDeviceProperties2));
    const chain_range = try checked_range(@intFromPtr(chain), @sizeOf(chain_t));
    const value_range = try checked_range(@intFromPtr(value), @sizeOf(wire.result_t));
    if (intersects(root_range, chain_range) or intersects(root_range, value_range)) return error.Invalid;
    if (chain.count > MaxNodes or chain.walk_count > MaxWalkNodes) return error.Invalid;
    var ranges: [MaxNodes]range_t = undefined;
    for (chain.tags[0..chain.count], chain.addresses[0..chain.count], 0..) |tag, target, index| {
        ranges[index] = try checked_range(target, node_size(tag) orelse return error.Invalid);
        if (intersects(ranges[index], root_range) or intersects(ranges[index], chain_range) or intersects(ranges[index], value_range)) return error.Invalid;
        for (ranges[0..index]) |previous| if (intersects(ranges[index], previous)) return error.Invalid;
    }
    for (chain.observations[0..chain.walk_count]) |observed| {
        const header_range = try checked_range(observed.address, @sizeOf(c.VkBaseOutStructure));
        if (node_size(observed.type_tag) == null) {
            if (intersects(root_range, header_range)) return error.Invalid;
            for (ranges[0..chain.count]) |target| if (intersects(target, header_range)) return error.Invalid;
        }
    }
    const root: *c.VkPhysicalDeviceProperties2 = @ptrFromInt(address);
    if (root.sType != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2) return error.Invalid;
    const actual = try collect_chain(@ptrCast(root.pNext));
    if (!equal_snapshot(chain, &actual)) return error.Invalid;
    wire.validate_result(chain.tags[0..chain.count], value) catch return error.Invalid;
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
