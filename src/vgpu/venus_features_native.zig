//! Allocation-free native feature adaptation; hardware support and advertisement remain caller-owned.
const std = @import("std");
const wire = @import("venus_features_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Maximum distinct recognized feature structures in one borrowed chain.
pub const MaxNodes: usize = 8;
/// Maximum accessible native headers walked before rejecting the next address unread.
pub const MaxWalkNodes: usize = 64;
/// Exact pinned core native Boolean count; immutable schema constant.
pub const CoreFlags: usize = wire.CoreFlags;
/// Owned Boolean record compatible with the checked receiver decoder; no pointer ownership.
pub const node_t = wire.node_t;
/// Copied tags and borrowed known targets. Underlying initialized objects must outlive use;
/// links/headers require external synchronization. No heap ownership or retained global state.
pub const chain_t = struct {
    /// Initialized prefix length, at most MaxNodes after successful collection.
    count: usize = 0,
    /// Owned recognized structure tags in native forward-chain order.
    tags: [MaxNodes]u32 = [_]u32{0} ** MaxNodes,
    /// Borrowed mutable output objects, nonnull in the initialized prefix.
    addresses: [MaxNodes]?*c.VkBaseOutStructure = [_]?*c.VkBaseOutStructure{null} ** MaxNodes,
};
/// Collect native headers. [in] first nullable borrowed accessible initialized chain, caller
/// synchronized until snapshot retirement. Unknown payloads are never accessed. Returns an
/// owned bounded snapshot or Invalid for cycle/duplicate/quota/alignment; no fields modified.
pub fn collect_chain(first: ?*anyopaque) !chain_t {
    var result = chain_t{};
    var seen = [_]?*c.VkBaseOutStructure{null} ** MaxWalkNodes;
    var visited: usize = 0;
    var next = first;
    while (next != null) {
        if (visited == MaxWalkNodes or @intFromPtr(next.?) % @alignOf(c.VkBaseOutStructure) != 0)
            return error.Invalid;
        const node: *c.VkBaseOutStructure = @ptrCast(@alignCast(next.?));
        for (seen[0..visited]) |previous| if (previous == node) return error.Invalid;
        seen[visited] = node;
        visited += 1;
        next = @ptrCast(node.pNext);
        if (node_count(node.sType) == null) continue;
        if (result.count == MaxNodes) return error.Invalid;
        for (result.tags[0..result.count]) |tag| if (tag == node.sType) return error.Invalid;
        result.tags[result.count] = node.sType;
        result.addresses[result.count] = node;
        result.count += 1;
    }
    return result;
}
fn node_count(tag: u32) ?usize {
    return switch (tag) {
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES => 12,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES => 47,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES => 15,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT => 3,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES => 1,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT => 2,
        else => null,
    };
}
fn publish_flags(comptime native_t: type, output: *native_t, flags: []const u32) void {
    comptime var index: usize = 0;
    inline for (@typeInfo(native_t).Struct.fields) |field| {
        if (comptime !std.mem.eql(u8, field.name, "sType") and !std.mem.eql(u8, field.name, "pNext")) {
            if (field.type != c.VkBool32) @compileError("Feature member must be VkBool32");
            @field(output.*, field.name) = flags[index];
            index += 1;
        }
    }
    std.debug.assert(flags.len == index);
}
fn publish_node(output: *c.VkBaseOutStructure, flags: []const u32) void {
    switch (output.sType) {
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES => publish_flags(c.VkPhysicalDeviceVulkan11Features, @ptrCast(@alignCast(output)), flags),
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES => publish_flags(c.VkPhysicalDeviceVulkan12Features, @ptrCast(@alignCast(output)), flags),
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES => publish_flags(c.VkPhysicalDeviceVulkan13Features, @ptrCast(@alignCast(output)), flags),
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT => publish_flags(c.VkPhysicalDeviceRobustness2FeaturesEXT, @ptrCast(@alignCast(output)), flags),
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR => publish_flags(c.VkPhysicalDeviceMaintenance5FeaturesKHR, @ptrCast(@alignCast(output)), flags),
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES => publish_flags(c.VkPhysicalDeviceHostQueryResetFeatures, @ptrCast(@alignCast(output)), flags),
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES => publish_flags(c.VkPhysicalDeviceShaderDrawParametersFeatures, @ptrCast(@alignCast(output)), flags),
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT => publish_flags(c.VkPhysicalDeviceTransformFeedbackFeaturesEXT, @ptrCast(@alignCast(output)), flags),
        else => unreachable,
    }
}

const NativeTypes = .{ c.VkPhysicalDeviceVulkan11Features, c.VkPhysicalDeviceVulkan12Features, c.VkPhysicalDeviceVulkan13Features, c.VkPhysicalDeviceRobustness2FeaturesEXT, c.VkPhysicalDeviceMaintenance5FeaturesKHR, c.VkPhysicalDeviceHostQueryResetFeatures, c.VkPhysicalDeviceShaderDrawParametersFeatures, c.VkPhysicalDeviceTransformFeedbackFeaturesEXT };
const Tags = [_]u32{ c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT };
comptime {
    const core_fields = @typeInfo(c.VkPhysicalDeviceFeatures).Struct.fields;
    if (core_fields.len != CoreFlags) @compileError("Pinned core feature count changed");
    for (core_fields) |field| if (field.type != c.VkBool32) @compileError("Core feature member is not Boolean");
    for (NativeTypes, Tags) |native_t, tag| {
        const fields = @typeInfo(native_t).Struct.fields;
        if (fields.len != node_count(tag).? + 2) @compileError("Pinned feature Boolean count changed");
        for (fields[2..]) |field| if (field.type != c.VkBool32) @compileError("Native feature payload is not Boolean");
    }
    if (@sizeOf(usize) == 8) {
        if (@sizeOf(c.VkPhysicalDeviceFeatures) != 220 or @alignOf(c.VkPhysicalDeviceFeatures) != 4 or
            @sizeOf(c.VkPhysicalDeviceFeatures2) != 240 or @alignOf(c.VkPhysicalDeviceFeatures2) != 8 or
            @offsetOf(c.VkPhysicalDeviceFeatures2, "features") != 16 or @offsetOf(c.VkPhysicalDeviceFeatures2, "pNext") != 8)
            @compileError("Core Features2 native ABI changed");
        if (@sizeOf(chain_t) != 104 or @alignOf(chain_t) != 8 or @sizeOf(node_t) != 196 or
            @sizeOf([MaxWalkNodes]?*c.VkBaseOutStructure) != 512) @compileError("Native feature snapshot ABI changed");
        for (NativeTypes, .{ 64, 208, 80, 32, 24, 24, 24, 24 }) |native_t, size| {
            if (@sizeOf(native_t) != size or @alignOf(native_t) != 8) @compileError("Native Vulkan feature ABI changed");
        }
    }
}
fn native_size(tag: u32) usize {
    inline for (NativeTypes, Tags) |native_t, known| if (tag == known) return @sizeOf(native_t);
    unreachable;
}
/// Publish checked decoder records transactionally. [in] chain/nodes borrowed immutable,
/// mutually disjoint from [in,out] initialized accessible native target objects. Caller holds
/// exclusive target access and immutable headers/links. Returns Invalid with every target
/// unchanged for count/tag/shape/Boolean/null/alignment/duplicate/overlap errors. Success writes
/// only named Boolean members; headers, links, padding and unknown payloads remain unchanged.
/// No allocation, transport, locks or shared state; caller owns all storage and synchronization.
fn validate_batch(chain: *const chain_t, nodes: []const node_t) !void {
    if (chain.count > MaxNodes or nodes.len != chain.count) return error.Invalid;
    for (nodes, 0..) |node, index| {
        const count = node_count(chain.tags[index]) orelse return error.Invalid;
        if (node.type_tag != chain.tags[index] or node.flag_count != count) return error.Invalid;
        for (node.flags[0..count]) |flag| if (flag > 1) return error.Invalid;
        const target = chain.addresses[index] orelse return error.Invalid;
        const address = @intFromPtr(target);
        if (address % @alignOf(c.VkBaseOutStructure) != 0) return error.Invalid;
        if (target.sType != chain.tags[index]) return error.Invalid;
        const end = std.math.add(usize, address, native_size(node.type_tag)) catch return error.Invalid;
        for (nodes[0..index], 0..) |previous, previous_index| {
            if (previous.type_tag == node.type_tag) return error.Invalid;
            const previous_address = @intFromPtr(chain.addresses[previous_index].?);
            const previous_end = previous_address + native_size(previous.type_tag);
            if (address < previous_end and previous_address < end) return error.Invalid;
        }
    }
}
/// Publish a complete checked chain. [in] chain/nodes borrowed immutable and disjoint from
/// [in,out] accessible exclusive initialized native objects, all externally synchronized.
/// Invalid leaves every target untouched; success writes only named Boolean members.
/// No allocation, transport, pointer retention or shared state.
pub fn publish_batch(chain: *const chain_t, nodes: []const node_t) !void {
    try validate_batch(chain, nodes);
    for (nodes, 0..) |node, index| publish_node(chain.addresses[index].?, node.flags[0..node.flag_count]);
}
/// Publish core55 and extension outputs as one transaction. [in,out] output_address nullable;
/// nonnull addresses must reference an exclusive
/// initialized accessible Features2; [in] chain is collected from exactly output.pNext with
/// immutable headers/links. Borrowed core/nodes/snapshot inputs are disjoint from every output;
/// all native objects including unknown records are distinct and non-overlapping. Caller owns
/// storage and synchronization until return. Invalid for outer alignment/tag, core count/Boolean,
/// batch errors or known-target overlap with the entire outer object leaves all outputs untouched.
/// Success writes only named Boolean fields, preserving outer/chain headers, padding and canaries.
/// Fixed bounded scalar storage; no heap, locks, transport or pointer retention.
pub fn publish_features2(output_address: ?*anyopaque, chain: *const chain_t, nodes: []const node_t, core: []const u32) !void {
    try validate_features2(output_address, chain, nodes, core);
    const output: *c.VkPhysicalDeviceFeatures2 = @ptrCast(@alignCast(output_address.?));
    for (nodes, 0..) |node, index| publish_node(chain.addresses[index].?, node.flags[0..node.flag_count]);
    publish_flags(c.VkPhysicalDeviceFeatures, &output.features, core);
}

/// Preflight complete Features2 output topology without mutation. [in] output_address nullable,
/// nonnull refers to accessible initialized externally synchronized native storage. [in] chain,
/// nodes and core are initialized immutable call-borrowed records disjoint from every output;
/// chain is collected from exactly output.pNext with unchanged headers/links. Invalid for outer
/// null/alignment/tag, core extent/Boolean, known node schema/header/Boolean or pair/outer overlap.
/// Success authorizes publication only while all same-call inputs remain unchanged. No output
/// byte writes, allocation, transport, locks, global mutation or retained pointers on either result.
pub fn validate_features2(output_address: ?*anyopaque, chain: *const chain_t, nodes: []const node_t, core: []const u32) !void {
    const raw = output_address orelse return error.Invalid;
    const address = @intFromPtr(raw);
    if (address % @alignOf(c.VkPhysicalDeviceFeatures2) != 0) return error.Invalid;
    const output: *c.VkPhysicalDeviceFeatures2 = @ptrCast(@alignCast(raw));
    if (output.sType != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 or core.len != CoreFlags) return error.Invalid;
    for (core) |flag| if (flag > 1) return error.Invalid;
    try validate_batch(chain, nodes);
    const end = std.math.add(usize, address, @sizeOf(c.VkPhysicalDeviceFeatures2)) catch return error.Invalid;
    for (nodes, 0..) |node, index| {
        const target_address = @intFromPtr(chain.addresses[index].?);
        const target_end = target_address + native_size(node.type_tag); // validate_batch proved no overflow.
        if (address < target_end and target_address < end) return error.Invalid;
    }
}

// Test-only fixtures.
fn malformed_target(address: usize) *c.VkBaseOutStructure {
    @setRuntimeSafety(false); // Test fixture only: validate rejection before this pointer is dereferenced.
    return @ptrFromInt(address);
}
fn record(tag: u32) node_t {
    return .{ .type_tag = tag, .flag_count = @intCast(node_count(tag).?) };
}
fn single(target: *c.VkBaseOutStructure) chain_t {
    var chain = chain_t{ .count = 1 };
    chain.tags[0] = target.sType;
    chain.addresses[0] = target;
    return chain;
}
test "every native Boolean one-hot preserves exact header padding and canaries" {
    inline for (NativeTypes, Tags) |native_t, tag| {
        const box_t = extern struct { before: u64, native: native_t, after: u64 };
        const fields = @typeInfo(native_t).Struct.fields;
        const count = node_count(tag).?;
        try std.testing.expectEqual(count + 2, fields.len);
        for (0..count) |selected| {
            var box: box_t = undefined;
            @memset(std.mem.asBytes(&box), 0xa5);
            box.native.sType = tag;
            box.native.pNext = null;
            const chain = single(@ptrCast(&box.native));
            var node = record(tag);
            node.flags[selected] = 1;
            var expected = std.mem.asBytes(&box).*;
            inline for (fields[2..], 0..) |field, index| {
                const offset = @offsetOf(box_t, "native") + @offsetOf(native_t, field.name);
                std.mem.writeInt(u32, expected[offset..][0..4], @intFromBool(index == selected), @import("builtin").target.cpu.arch.endian());
            }
            try @call(.never_inline, publish_batch, .{ &chain, &[_]node_t{node} });
            try std.testing.expectEqualSlices(u8, &expected, std.mem.asBytes(&box));
        }
    }
}
test "collector unknown payloads quotas cycles duplicates and misalignment" {
    const unknown_t = extern struct { base: c.VkBaseOutStructure, canary: u64 };
    var unknowns = [_]unknown_t{.{ .base = .{ .sType = 0x7fffffff, .pNext = null }, .canary = 0xdeadbeef12345678 }} ** MaxWalkNodes;
    for (unknowns[0 .. MaxWalkNodes - 1], 0..) |*unknown, index| unknown.base.pNext = &unknowns[index + 1].base;
    const before = unknowns;
    try std.testing.expectEqual(@as(usize, 0), (try @call(.never_inline, collect_chain, .{&unknowns[0]})).count);
    try std.testing.expectEqualDeep(before, unknowns);
    unknowns[63].base.pNext = @ptrFromInt(8); // quota check must precede dereference/alignment of inaccessible65th.
    try std.testing.expectError(error.Invalid, @call(.never_inline, collect_chain, .{&unknowns[0]}));
    unknowns[63].base.pNext = &unknowns[0].base;
    try std.testing.expectError(error.Invalid, collect_chain(&unknowns[0]));
    unknowns[0].base.pNext = &unknowns[0].base;
    try std.testing.expectError(error.Invalid, collect_chain(&unknowns[0]));
    try std.testing.expectError(error.Invalid, @call(.never_inline, collect_chain, .{@as(?*anyopaque, @ptrFromInt(1))}));
    try std.testing.expectEqual(@as(usize, 0), (try collect_chain(null)).count);
    var first = std.mem.zeroes(c.VkPhysicalDeviceMaintenance5FeaturesKHR);
    var second = first;
    first.sType = Tags[4];
    second.sType = Tags[4];
    first.pNext = &second;
    try std.testing.expectError(error.Invalid, collect_chain(&first));
    first.pNext = &unknowns[0];
    unknowns[0].base.pNext = @ptrCast(&second);
    second.sType = Tags[5];
    second.pNext = &unknowns[1];
    unknowns[1].base.pNext = null;
    const chain = try collect_chain(&first);
    try std.testing.expectEqual(@as(usize, 2), chain.count);
    try std.testing.expectEqual(Tags[4], chain.tags[0]);
    try std.testing.expectEqual(Tags[5], chain.tags[1]);
}
test "all eight targets batch publication and malformed last rollback" {
    var natives: std.meta.Tuple(&NativeTypes) = undefined;
    var chain = chain_t{};
    var nodes: [MaxNodes]node_t = undefined;
    inline for (NativeTypes, Tags, 0..) |native_t, tag, index| {
        natives[index] = std.mem.zeroes(native_t);
        natives[index].sType = tag;
        if (index + 1 < MaxNodes) natives[index].pNext = &natives[index + 1];
        nodes[index] = record(tag);
        @memset(nodes[index].flags[0..nodes[index].flag_count], 1);
    }
    chain = try collect_chain(&natives[0]);
    try std.testing.expectEqual(@as(usize, MaxNodes), chain.count);
    var ninth = std.mem.zeroes(c.VkPhysicalDeviceMaintenance5FeaturesKHR);
    ninth.sType = Tags[4];
    natives[7].pNext = &ninth;
    try std.testing.expectError(error.Invalid, @call(.never_inline, collect_chain, .{&natives[0]}));
    natives[7].pNext = null;
    const before = natives;
    nodes[7].flags[1] = 2;
    try std.testing.expectError(error.Invalid, @call(.never_inline, publish_batch, .{ &chain, &nodes }));
    try std.testing.expectEqualDeep(before, natives);
    nodes[7].flags[1] = 1;
    try publish_batch(&chain, &nodes);
    inline for (NativeTypes, 0..) |native_t, index| {
        inline for (@typeInfo(native_t).Struct.fields[2..]) |field| try std.testing.expectEqual(@as(u32, 1), @field(natives[index], field.name));
    }
}
test "batch validates every shape tag target and Boolean before output writes" {
    var native = std.mem.zeroes(c.VkPhysicalDeviceVulkan12Features);
    native.sType = Tags[1];
    const valid = single(@ptrCast(&native));
    const valid_node = record(Tags[1]);
    var chain = valid;
    var node = valid_node;
    chain.count = MaxNodes + 1;
    try std.testing.expectError(error.Invalid, publish_batch(&chain, &.{}));
    try std.testing.expectError(error.Invalid, publish_batch(&valid, &.{}));
    chain = valid;
    chain.tags[0] = 0xffffffff;
    try std.testing.expectError(error.Invalid, publish_batch(&chain, &.{node}));
    node.type_tag = Tags[0];
    try std.testing.expectError(error.Invalid, publish_batch(&valid, &.{node}));
    node = valid_node;
    node.flag_count = 48;
    try std.testing.expectError(error.Invalid, publish_batch(&valid, &.{node}));
    node = valid_node;
    for (0..47) |index| {
        node.flags[index] = 0xffffffff;
        try std.testing.expectError(error.Invalid, publish_batch(&valid, &.{node}));
        node.flags[index] = 0;
    }
    chain = valid;
    chain.addresses[0] = null;
    try std.testing.expectError(error.Invalid, publish_batch(&chain, &.{node}));
    chain.addresses[0] = malformed_target(9);
    try std.testing.expectError(error.Invalid, publish_batch(&chain, &.{node}));
    chain = valid;
    native.sType = Tags[0];
    try std.testing.expectError(error.Invalid, publish_batch(&chain, &.{node}));
    native.sType = Tags[1];
    chain.count = 2;
    chain.tags[1] = Tags[1];
    chain.addresses[1] = chain.addresses[0];
    try std.testing.expectError(error.Invalid, publish_batch(&chain, &.{ node, node }));
    // Accessible aligned overlapping targets with distinct tags must fail without publication.
    var storage: [256]u8 align(8) = [_]u8{0} ** 256;
    const first: *c.VkBaseOutStructure = @ptrCast(&storage);
    const second: *c.VkBaseOutStructure = @ptrCast(@alignCast(&storage[16]));
    first.sType = Tags[1];
    second.sType = Tags[4];
    chain = single(first);
    chain.count = 2;
    chain.tags[1] = second.sType;
    chain.addresses[1] = second;
    const before = storage;
    try std.testing.expectError(error.Invalid, publish_batch(&chain, &.{ node, record(Tags[4]) }));
    try std.testing.expectEqualSlices(u8, &before, &storage);
    try publish_batch(&chain_t{}, &.{});
}
test "x64 native ABI ledger is identical on Linux and Windows" {
    if (@sizeOf(usize) != 8) return;
    try std.testing.expectEqual(@as(usize, 104), @sizeOf(chain_t));
    try std.testing.expectEqual(@as(usize, 196), @sizeOf(node_t));
    inline for (NativeTypes, .{ 64, 208, 80, 32, 24, 24, 24, 24 }) |native_t, size| {
        try std.testing.expectEqual(@as(usize, size), @sizeOf(native_t));
        try std.testing.expectEqual(@as(usize, 8), @alignOf(native_t));
    }
    try std.testing.expectEqual(@as(usize, 512), @sizeOf([MaxWalkNodes]?*c.VkBaseOutStructure));
}

test "all55 core one-hots preserve complete outer bytes and known adjacent output" {
    const box_t = extern struct { before: u64, output: c.VkPhysicalDeviceFeatures2, known: c.VkPhysicalDeviceMaintenance5FeaturesKHR, after: u64 };
    const core_fields = @typeInfo(c.VkPhysicalDeviceFeatures).Struct.fields;
    for (0..55) |selected| {
        var box: box_t = undefined;
        @memset(std.mem.asBytes(&box), 0xa5);
        box.output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        box.output.pNext = &box.known;
        box.known.sType = Tags[4];
        box.known.pNext = null;
        const chain = try collect_chain(box.output.pNext);
        var node = record(Tags[4]);
        node.flags[0] = 1;
        var core = [_]u32{0} ** 55;
        core[selected] = 1;
        var expected = std.mem.asBytes(&box).*;
        inline for (core_fields, 0..) |field, index| {
            const offset = @offsetOf(box_t, "output") + @offsetOf(c.VkPhysicalDeviceFeatures2, "features") + @offsetOf(c.VkPhysicalDeviceFeatures, field.name);
            std.mem.writeInt(u32, expected[offset..][0..4], @intFromBool(index == selected), @import("builtin").target.cpu.arch.endian());
        }
        const known_offset = @offsetOf(box_t, "known") + @offsetOf(c.VkPhysicalDeviceMaintenance5FeaturesKHR, "maintenance5");
        std.mem.writeInt(u32, expected[known_offset..][0..4], 1, @import("builtin").target.cpu.arch.endian());
        const preflight_before = std.mem.asBytes(&box).*;
        try @call(.never_inline, validate_features2, .{ &box.output, &chain, &[_]node_t{node}, &core });
        try std.testing.expectEqualSlices(u8, &preflight_before, std.mem.asBytes(&box));
        try @call(.never_inline, publish_features2, .{ &box.output, &chain, &[_]node_t{node}, &core });
        try std.testing.expectEqualSlices(u8, &expected, std.mem.asBytes(&box));
    }
}
test "Features2 malformed final core or chain leaves every output byte unchanged" {
    const unknown_t = extern struct { base: c.VkBaseOutStructure, canary: u64 };
    const box_t = extern struct { output: c.VkPhysicalDeviceFeatures2, unknown: unknown_t, first: c.VkPhysicalDeviceMaintenance5FeaturesKHR, middle: unknown_t, last: c.VkPhysicalDeviceHostQueryResetFeatures, tail: unknown_t, after: u64 };
    var box: box_t = undefined;
    @memset(std.mem.asBytes(&box), 0xa5);
    box.output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    box.output.pNext = &box.unknown;
    box.unknown.base.sType = 0x7fffffff;
    box.unknown.base.pNext = @ptrCast(&box.first);
    box.first.sType = Tags[4];
    box.first.pNext = &box.middle;
    box.middle.base.sType = 0x7fffffff;
    box.middle.base.pNext = @ptrCast(&box.last);
    box.last.sType = Tags[5];
    box.last.pNext = &box.tail;
    box.tail.base.sType = 0x7fffffff;
    box.tail.base.pNext = null;
    const chain = try collect_chain(box.output.pNext);
    var nodes = [_]node_t{ record(Tags[4]), record(Tags[5]) };
    var core = [_]u32{1} ** 56;
    const before = std.mem.asBytes(&box).*;
    for ([_]usize{ 0, 54, 56 }) |count| {
        try std.testing.expectError(error.Invalid, @call(.never_inline, validate_features2, .{ &box.output, &chain, &nodes, core[0..count] }));
        try std.testing.expectError(error.Invalid, @call(.never_inline, publish_features2, .{ &box.output, &chain, &nodes, core[0..count] }));
        try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    }
    for (0..55) |index| {
        for ([_]u32{ 2, 0xffffffff }) |invalid| {
            core[index] = invalid;
            try std.testing.expectError(error.Invalid, @call(.never_inline, validate_features2, .{ &box.output, &chain, &nodes, core[0..55] }));
            try std.testing.expectError(error.Invalid, @call(.never_inline, publish_features2, .{ &box.output, &chain, &nodes, core[0..55] }));
            try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
        }
        core[index] = 1;
    }
    nodes[1].flags[0] = 2;
    try std.testing.expectError(error.Invalid, validate_features2(&box.output, &chain, &nodes, core[0..55]));
    try std.testing.expectError(error.Invalid, publish_features2(&box.output, &chain, &nodes, core[0..55]));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    nodes[1] = record(Tags[5]);
    nodes[1].flag_count = 0;
    try std.testing.expectError(error.Invalid, validate_features2(&box.output, &chain, &nodes, core[0..55]));
    try std.testing.expectError(error.Invalid, publish_features2(&box.output, &chain, &nodes, core[0..55]));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    nodes[1] = record(Tags[5]);
    nodes[1].type_tag = Tags[4];
    try std.testing.expectError(error.Invalid, validate_features2(&box.output, &chain, &nodes, core[0..55]));
    try std.testing.expectError(error.Invalid, publish_features2(&box.output, &chain, &nodes, core[0..55]));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    nodes[1] = record(Tags[5]);
    box.output.sType = 0;
    const invalid_header = std.mem.asBytes(&box).*;
    try std.testing.expectError(error.Invalid, validate_features2(&box.output, &chain, &nodes, core[0..55]));
    try std.testing.expectError(error.Invalid, publish_features2(&box.output, &chain, &nodes, core[0..55]));
    try std.testing.expectEqualSlices(u8, &invalid_header, std.mem.asBytes(&box));
    box.output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    const misaligned: *anyopaque = @ptrFromInt(9);
    try std.testing.expectError(error.Invalid, @call(.never_inline, validate_features2, .{ @as(?*anyopaque, null), &chain, &nodes, core[0..55] }));
    try std.testing.expectError(error.Invalid, @call(.never_inline, publish_features2, .{ @as(?*anyopaque, null), &chain, &nodes, core[0..55] }));
    try std.testing.expectError(error.Invalid, @call(.never_inline, validate_features2, .{ misaligned, &chain, &nodes, core[0..55] }));
    try std.testing.expectError(error.Invalid, @call(.never_inline, publish_features2, .{ misaligned, &chain, &nodes, core[0..55] }));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    const unknown_before = .{ box.unknown, box.middle, box.tail };
    try publish_features2(&box.output, &chain, &nodes, core[0..55]);
    try std.testing.expectEqualDeep(unknown_before, .{ box.unknown, box.middle, box.tail });
}
test "Features2 core versus chain overlap rejection and empty maximal chains" {
    var output = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
    output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    const core = [_]u32{0} ** 55;
    try publish_features2(&output, &chain_t{}, &.{}, &core);
    const overlapping: *c.VkPhysicalDeviceMaintenance5FeaturesKHR = @ptrCast(@alignCast(&output.features));
    overlapping.sType = Tags[4];
    overlapping.pNext = null;
    output.pNext = overlapping;
    const chain = try collect_chain(output.pNext);
    const before = std.mem.asBytes(&output).*;
    try std.testing.expectError(error.Invalid, @call(.never_inline, validate_features2, .{ &output, &chain, &[_]node_t{record(Tags[4])}, &core }));
    try std.testing.expectError(error.Invalid, @call(.never_inline, publish_features2, .{ &output, &chain, &[_]node_t{record(Tags[4])}, &core }));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&output));
    var natives: std.meta.Tuple(&NativeTypes) = undefined;
    var nodes: [MaxNodes]node_t = undefined;
    inline for (NativeTypes, Tags, 0..) |native_t, tag, index| {
        natives[index] = std.mem.zeroes(native_t);
        natives[index].sType = tag;
        if (index + 1 < MaxNodes) natives[index].pNext = &natives[index + 1];
        nodes[index] = record(tag);
    }
    output.pNext = &natives[0];
    const maximal = try collect_chain(output.pNext);
    try publish_features2(&output, &maximal, &nodes, &core);
}

test "Features2 preflight preserves every maximal native payload and byte" {
    const box_t = extern struct {
        before: u64,
        output: c.VkPhysicalDeviceFeatures2,
        vulkan11: c.VkPhysicalDeviceVulkan11Features,
        vulkan12: c.VkPhysicalDeviceVulkan12Features,
        vulkan13: c.VkPhysicalDeviceVulkan13Features,
        robustness2: c.VkPhysicalDeviceRobustness2FeaturesEXT,
        maintenance5: c.VkPhysicalDeviceMaintenance5FeaturesKHR,
        host_query_reset: c.VkPhysicalDeviceHostQueryResetFeatures,
        shader_draw_parameters: c.VkPhysicalDeviceShaderDrawParametersFeatures,
        transform_feedback: c.VkPhysicalDeviceTransformFeedbackFeaturesEXT,
        after: u64,
    };
    var box: box_t = undefined;
    @memset(std.mem.asBytes(&box), 0xa5);
    box.output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    box.output.pNext = &box.vulkan11;
    const native_names = .{ "vulkan11", "vulkan12", "vulkan13", "robustness2", "maintenance5", "host_query_reset", "shader_draw_parameters", "transform_feedback" };
    var nodes: [MaxNodes]node_t = undefined;
    inline for (native_names, Tags, 0..) |name, tag, index| {
        const native = &@field(box, name);
        native.sType = tag;
        native.pNext = if (index + 1 < MaxNodes) &@field(box, native_names[index + 1]) else null;
        nodes[index] = record(tag);
    }
    const chain = try collect_chain(box.output.pNext);
    const core = [_]u32{0} ** CoreFlags;
    const before = std.mem.asBytes(&box).*;
    try @call(.never_inline, validate_features2, .{ &box.output, &chain, &nodes, &core });
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    try @call(.never_inline, publish_features2, .{ &box.output, &chain, &nodes, &core });
    try std.testing.expect(!std.mem.eql(u8, &before, std.mem.asBytes(&box)));
}
