//! Allocation-free command11 serialization; native parsing and support policy belong to the ICD.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Maximum queue records; immutable scalar, no storage owner or threading restriction.
pub const MaxQueues: usize = 16;
/// Maximum priorities across all records; each queue also has at most16.
pub const MaxPriorities: usize = 64;
/// Maximum unique extension names; callers independently validate actual support.
pub const MaxExtensions: usize = 32;
/// Maximum recognized unique feature nodes; promotion conflicts reduce valid combinations.
pub const MaxNodes: usize = 9;
/// Largest recognized Boolean record (core features); no native padding is serialized.
pub const MaxFlags: usize = 55;
/// Borrowed typed queue; immutable inputs live through create_device, never retained.
pub const queue_t = struct {
    /// Actual family index; caller validates host family/count support.
    family_index: u32,
    /// Nonempty borrowed finite [0,1] priorities,1..16; negative zero is preserved.
    priorities: []const f32,
};
/// Owned typed feature node; all Boolean words normalized, unused suffix zero.
pub const feature_node_t = struct {
    /// Recognized Vulkan structure tag; no native pointer or ownership.
    type_tag: u32 = 0,
    /// Exact initialized Boolean extent for this tag, bounded by MaxFlags.
    flag_count: u8 = 0,
    /// Owned requested flags in native declaration order; sharing immutable values is safe.
    flags: [MaxFlags]u32 = [_]u32{0} ** MaxFlags,
};
fn node_flag_count(tag: u32) !u8 {
    return switch (tag) {
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 => 55,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES => 12,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES => 47,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES => 15,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR => 1,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT => 2,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT => 3,
        else => error.Invalid,
    };
}
fn promotion_conflict(first: u32, second: u32) bool {
    return (first == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES and
        second == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES) or
        (first == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES and
        second == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
}
fn validate(queues: []const queue_t, extensions: []const []const u8, legacy: ?[]const u32, nodes: []const feature_node_t) !usize {
    if (queues.len == 0 or queues.len > MaxQueues or extensions.len > MaxExtensions or nodes.len > MaxNodes) return error.Invalid;
    var bytes: usize = 108;
    var priorities: usize = 0;
    for (queues, 0..) |queue, index| {
        if (queue.priorities.len == 0 or queue.priorities.len > 16) return error.Invalid;
        for (queues[0..index]) |previous| if (previous.family_index == queue.family_index) return error.Invalid;
        priorities += queue.priorities.len;
        if (priorities > MaxPriorities) return error.Invalid;
        for (queue.priorities) |priority| if (!std.math.isFinite(priority) or priority < 0 or priority > 1) return error.Invalid;
        bytes += 32 + 4 * queue.priorities.len;
    }
    for (extensions, 0..) |name, index| {
        if (name.len < 4 or name.len > 255 or !std.mem.startsWith(u8, name, "VK_")) return error.Invalid;
        for (name) |byte| if (!(std.ascii.isAlphanumeric(byte) or byte == '_')) return error.Invalid;
        for (extensions[0..index]) |previous| if (std.mem.eql(u8, previous, name)) return error.Invalid;
        bytes += 8 + std.mem.alignForward(usize, name.len + 1, 4);
    }
    if (legacy) |flags| {
        if (flags.len != MaxFlags) return error.Invalid;
        for (flags) |flag| if (flag > 1) return error.Invalid;
        bytes += 4 * MaxFlags;
    }
    for (nodes, 0..) |node, index| {
        if (node.flag_count != try node_flag_count(node.type_tag)) return error.Invalid;
        for (node.flags[0..node.flag_count]) |flag| if (flag > 1) return error.Invalid;
        for (node.flags[node.flag_count..]) |flag| if (flag != 0) return error.Invalid;
        if (legacy != null and node.type_tag == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) return error.Invalid;
        for (nodes[0..index]) |previous| {
            if (previous.type_tag == node.type_tag or promotion_conflict(previous.type_tag, node.type_tag) or
                promotion_conflict(node.type_tag, previous.type_tag)) return error.Invalid;
        }
        bytes += 12 + 4 * @as(usize, node.flag_count);
    }
    if (bytes > render.MaxBytes) return error.Limit;
    return bytes;
}
fn put(writer: *render.writer_t, comptime word_t: type, word: word_t) void {
    // Complete validation proved this entire packet fits before the first append.
    writer.put(word_t, word) catch unreachable;
}
/// Serialize owned vkCreateDevice command11. [in] IDs nonzero, distinct translated values.
/// [in] queues/names/nodes/optional legacy are accessible immutable borrowed slices for this call.
/// Returns owned8192-byte writer/prefix or Invalid for shape/value/conflicts, Limit for packet size.
/// Complete preflight precedes every write; no input mutation, retained pointer, allocation or lock.
/// Safe for concurrent independent calls; native parsing, negotiated support and device ownership
/// remain caller responsibilities. Flags are requested values, never inferred hardware capabilities.
pub fn create_device(physical_id: u64, device_id: u64, queues: []const queue_t, extensions: []const []const u8, legacy: ?[]const u32, nodes: []const feature_node_t) !render.writer_t {
    if (physical_id == 0 or device_id == 0 or physical_id == device_id) return error.Invalid;
    const total = try validate(queues, extensions, legacy, nodes);
    var writer = render.writer_t{};
    writer.header(11, physical_id) catch unreachable;
    put(&writer, u64, 1);
    put(&writer, u32, c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    for (nodes) |node| {
        put(&writer, u64, 1);
        put(&writer, u32, node.type_tag);
    }
    put(&writer, u64, 0);
    var remaining = nodes.len;
    while (remaining != 0) {
        remaining -= 1;
        for (nodes[remaining].flags[0..nodes[remaining].flag_count]) |flag| put(&writer, u32, flag);
    }
    put(&writer, u32, 0);
    put(&writer, u32, @intCast(queues.len));
    put(&writer, u64, queues.len);
    for (queues) |queue| {
        put(&writer, u32, c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        put(&writer, u64, 0);
        put(&writer, u32, 0);
        put(&writer, u32, queue.family_index);
        put(&writer, u32, @intCast(queue.priorities.len));
        put(&writer, u64, queue.priorities.len);
        for (queue.priorities) |priority| put(&writer, u32, @bitCast(priority));
    }
    put(&writer, u32, 0);
    put(&writer, u64, 0);
    put(&writer, u32, @intCast(extensions.len));
    put(&writer, u64, extensions.len);
    for (extensions) |name| {
        put(&writer, u64, name.len + 1);
        for (name) |byte| put(&writer, u8, byte);
        const padded = std.mem.alignForward(usize, name.len + 1, 4);
        for (name.len..padded) |_| put(&writer, u8, 0);
    }
    put(&writer, u64, @intFromBool(legacy != null));
    if (legacy) |flags| for (flags) |flag| put(&writer, u32, flag);
    put(&writer, u64, 0);
    put(&writer, u64, 1);
    put(&writer, u64, device_id);
    std.debug.assert(writer.used == total);
    return writer;
}

// Test-only fixtures.
// Native records/oracle adaptation; excluded from production coverage/inventory.
extern fn venus_device_test_encode(*const c.VkDeviceCreateInfo, u64, u64, [*]u8) usize;
const native_node_t = extern struct { type_tag: u32, next: ?*const anyopaque, flags: [MaxFlags]u32 };
const Tags = [_]u32{
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,                      c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,             c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
};
const Counts = [_]u8{ 55, 12, 47, 15, 1, 1, 2, 3, 1 };
comptime {
    std.debug.assert(@sizeOf(c.VkPhysicalDeviceFeatures) == 4 * MaxFlags);
    std.debug.assert(@offsetOf(native_node_t, "flags") == 16);
    const layouts = .{
        .{ c.VkPhysicalDeviceFeatures2, "features", "features", @as(usize, 1) },
        .{ c.VkPhysicalDeviceVulkan11Features, "storageBuffer16BitAccess", "shaderDrawParameters", @as(usize, 12) },
        .{ c.VkPhysicalDeviceVulkan12Features, "samplerMirrorClampToEdge", "subgroupBroadcastDynamicId", @as(usize, 47) },
        .{ c.VkPhysicalDeviceVulkan13Features, "robustImageAccess", "maintenance4", @as(usize, 15) },
        .{ c.VkPhysicalDeviceShaderDrawParametersFeatures, "shaderDrawParameters", "shaderDrawParameters", @as(usize, 1) },
        .{ c.VkPhysicalDeviceHostQueryResetFeatures, "hostQueryReset", "hostQueryReset", @as(usize, 1) },
        .{ c.VkPhysicalDeviceTransformFeedbackFeaturesEXT, "transformFeedback", "geometryStreams", @as(usize, 2) },
        .{ c.VkPhysicalDeviceRobustness2FeaturesEXT, "robustBufferAccess2", "nullDescriptor", @as(usize, 3) },
        .{ c.VkPhysicalDeviceMaintenance5FeaturesKHR, "maintenance5", "maintenance5", @as(usize, 1) },
    };
    for (layouts) |layout| {
        std.debug.assert(@offsetOf(layout[0], layout[1]) == 16);
        std.debug.assert(@offsetOf(layout[0], layout[2]) == 16 + 4 * (layout[3] - 1));
    }
}

fn expect_oracle(physical_id: u64, device_id: u64, queues: []const queue_t, extensions: []const []const u8, legacy: ?[]const u32, nodes: []const feature_node_t) !void {
    var native_queues: [MaxQueues]c.VkDeviceQueueCreateInfo = undefined;
    for (queues, 0..) |queue, index| native_queues[index] = .{
        .sType = c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue.family_index,
        .queueCount = @intCast(queue.priorities.len),
        .pQueuePriorities = queue.priorities.ptr,
    };
    var names: [MaxExtensions][256]u8 = std.mem.zeroes([MaxExtensions][256]u8);
    var name_pointers: [MaxExtensions][*c]const u8 = undefined;
    for (extensions, 0..) |name, index| {
        @memcpy(names[index][0..name.len], name);
        name_pointers[index] = &names[index];
    }
    var native_nodes: [MaxNodes]native_node_t = undefined;
    for (nodes, 0..) |node, index| native_nodes[index] = .{
        .type_tag = node.type_tag,
        .next = if (index + 1 < nodes.len) &native_nodes[index + 1] else null,
        .flags = node.flags,
    };
    const info = c.VkDeviceCreateInfo{
        .sType = c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = if (nodes.len == 0) null else &native_nodes[0],
        .queueCreateInfoCount = @intCast(queues.len),
        .pQueueCreateInfos = &native_queues,
        .enabledExtensionCount = @intCast(extensions.len),
        .ppEnabledExtensionNames = if (extensions.len == 0) null else &name_pointers,
        .pEnabledFeatures = if (legacy) |flags| @ptrCast(flags.ptr) else null,
    };
    var expected: [render.MaxBytes]u8 = undefined;
    const count = venus_device_test_encode(&info, physical_id, device_id, &expected);
    const actual = try create_device(physical_id, device_id, queues, extensions, legacy, nodes);
    try std.testing.expectEqualSlices(u8, expected[0..count], actual.bytes[0..actual.used]);
}
fn fixture_queue() [1]queue_t {
    return .{.{ .family_index = 3, .priorities = &.{0.5} }};
}
test "device packets preserve each legacy and modern flag against independent generator" {
    const queues = fixture_queue();
    try expect_oracle(7, 42, &queues, &.{}, null, &.{});
    var legacy = [_]u32{0} ** MaxFlags;
    for (0..MaxFlags) |index| {
        legacy[index] = 1;
        try expect_oracle(0x1234567800000007, 0xfedcba980000002a, &queues, &.{"VK_EXT_fixture"}, &legacy, &.{});
        legacy[index] = 0;
    }
    for (Tags, Counts) |tag, count| {
        var node = feature_node_t{ .type_tag = tag, .flag_count = count };
        for (0..count) |index| {
            node.flags[index] = 1;
            try expect_oracle(7, 42, &queues, &.{}, null, &.{node});
            node.flags[index] = 0;
        }
    }
}
test "device forward headers and reverse payloads preserve legal chain permutations" {
    const queues = fixture_queue();
    var nodes: [7]feature_node_t = undefined;
    const selected = [_]usize{ 0, 1, 2, 3, 6, 7, 8 };
    for (selected, 0..) |index, position| {
        nodes[position] = .{ .type_tag = Tags[index], .flag_count = Counts[index] };
        for (0..Counts[index]) |flag| nodes[position].flags[flag] = @intCast((flag + position) % 2);
    }
    for (0..nodes.len) |_| {
        try expect_oracle(7, 42, &queues, &.{ "VK_EXT_a", "VK_KHR_b" }, null, &nodes);
        const first = nodes[0];
        std.mem.copyForwards(feature_node_t, nodes[0 .. nodes.len - 1], nodes[1..]);
        nodes[nodes.len - 1] = first;
    }
    std.mem.reverse(feature_node_t, &nodes);
    try expect_oracle(7, 42, &queues, &.{}, null, &nodes);
    try expect_oracle(7, 42, &queues, &.{}, null, &.{
        .{ .type_tag = Tags[4], .flag_count = 1, .flags = [_]u32{1} ++ [_]u32{0} ** 54 },
        .{ .type_tag = Tags[5], .flag_count = 1 },
    });
}
test "device queue and extension boundaries fit exact owned capacity" {
    var priorities = [_]f32{ 0, -0.0, 0.5, 1 } ** 4;
    var queues: [16]queue_t = undefined;
    for (&queues, 0..) |*queue, index| queue.* = .{ .family_index = @intCast(index), .priorities = priorities[0..4] };
    try expect_oracle(7, 42, &queues, &.{}, null, &.{});
    queues[0].priorities = &priorities;
    try expect_oracle(7, 42, queues[0..1], &.{}, null, &.{});
    var names: [32][255]u8 = undefined;
    var extensions: [32][]const u8 = undefined;
    for (&names, 0..) |*name, index| {
        @memset(name, '_');
        @memcpy(name[0..3], "VK_");
        name[3] = 'A' + @as(u8, @intCast(index));
        // 32 unique alphanumeric prefixes, then long underscore suffixes.
        if (name[3] > 'Z') name[3] = '0' + @as(u8, @intCast(index - 26));
        extensions[index] = name;
    }
    const one = fixture_queue();
    extensions[30] = names[30][0..119];
    const exact = try create_device(7, 42, &one, extensions[0..31], null, &.{});
    try std.testing.expectEqual(@as(usize, 8192), exact.used);
    try expect_oracle(7, 42, &one, extensions[0..31], null, &.{});
    extensions[30] = names[30][0..115];
    try expect_oracle(7, 42, &one, extensions[0..31], null, &.{});
    extensions[30] = names[30][0..123];
    try std.testing.expectError(error.Limit, create_device(7, 42, &one, extensions[0..31], null, &.{}));
    for (&extensions, 0..) |*name, index| name.* = names[index][0..4];
    try expect_oracle(7, 42, &one, &extensions, null, &.{});
}
test "device malformed shapes reject before native conversion without input mutation" {
    const queues = fixture_queue();
    for ([_][2]u64{ .{ 0, 42 }, .{ 7, 0 }, .{ 7, 7 } }) |ids| try std.testing.expectError(error.Invalid, create_device(ids[0], ids[1], &queues, &.{}, null, &.{}));
    try std.testing.expectError(error.Invalid, create_device(7, 42, &.{}, &.{}, null, &.{}));
    var many: [17]queue_t = undefined;
    for (&many, 0..) |*queue, index| queue.* = .{ .family_index = @intCast(index), .priorities = &.{1} };
    try std.testing.expectError(error.Invalid, create_device(7, 42, &many, &.{}, null, &.{}));
    many[1].family_index = 0;
    try std.testing.expectError(error.Invalid, create_device(7, 42, many[0..2], &.{}, null, &.{}));
    for ([_][]const f32{ &.{}, &([_]f32{1} ** 17), &.{-0.1}, &.{1.1}, &.{std.math.inf(f32)}, &.{-std.math.inf(f32)}, &.{std.math.nan(f32)} }) |priorities| {
        try std.testing.expectError(error.Invalid, create_device(7, 42, &.{.{ .family_index = 0, .priorities = priorities }}, &.{}, null, &.{}));
    }
    for (&many, 0..) |*queue, index| queue.* = .{ .family_index = @intCast(index), .priorities = &([_]f32{1} ** 16) };
    try std.testing.expectError(error.Invalid, create_device(7, 42, many[0..5], &.{}, null, &.{}));
    for ([_][]const u8{ "VK_", "vk_X", "VK_\x00bad", "VK_-bad", "VK_\xff", "VK_ bad", &([_]u8{'a'} ** 256) }) |name| try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{name}, null, &.{}));
    try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{ "VK_X", "VK_X" }, null, &.{}));
    try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &([_][]const u8{"VK_X"} ** 33), null, &.{}));
    var legacy = [_]u32{0} ** MaxFlags;
    try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{}, legacy[0..54], &.{}));
    legacy[54] = 2;
    try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{}, &legacy, &.{}));
    try std.testing.expectEqual(@as(u32, 2), legacy[54]);
    legacy[54] = 0;
    try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{}, &legacy, &.{.{ .type_tag = Tags[0], .flag_count = 55 }}));
    var node = feature_node_t{ .type_tag = 0, .flag_count = 1 };
    try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{}, null, &.{node}));
    node.type_tag = Tags[1];
    node.flag_count = 255;
    try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{}, null, &.{node}));
    node.flag_count = 12;
    node.flags[0] = 2;
    try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{}, null, &.{node}));
    node.flags[0] = 0;
    node.flags[54] = 1;
    try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{}, null, &.{node}));
    node.flags[54] = 0;
    try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{}, null, &.{ node, node }));
    try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{}, null, &([_]feature_node_t{node} ** 10)));
    for ([_][2]usize{ .{ 1, 4 }, .{ 2, 5 }, .{ 4, 1 }, .{ 5, 2 } }) |indices| {
        try std.testing.expectError(error.Invalid, create_device(7, 42, &queues, &.{}, null, &.{
            .{ .type_tag = Tags[indices[0]], .flag_count = Counts[indices[0]] },
            .{ .type_tag = Tags[indices[1]], .flag_count = Counts[indices[1]] },
        }));
    }
}
test "device owned corpus retains outputs after borrowed storage changes and frees every allocation" {
    const allocator = std.testing.allocator;
    const priorities = try allocator.alloc(f32, 16);
    defer allocator.free(priorities);
    const flags = try allocator.alloc(u32, 55);
    defer allocator.free(flags);
    const name = try allocator.dupe(u8, "VK_EXT_fixture");
    defer allocator.free(name);
    @memset(flags, 0);
    @memset(priorities, 0.5);
    const queues = [_]queue_t{.{ .family_index = 1, .priorities = priorities }};
    const actual = try create_device(7, 42, &queues, &.{name}, flags, &.{});
    const original = actual.bytes;
    @memset(flags, 1);
    @memset(priorities, 1);
    @memset(name, '_');
    try std.testing.expectEqualSlices(u8, original[0..actual.used], actual.bytes[0..actual.used]);
    for (0..256) |iteration| {
        const count = iteration % 16 + 1;
        const borrowed = [_]queue_t{.{ .family_index = @intCast(iteration), .priorities = priorities[0..count] }};
        try expect_oracle(7, 42, &borrowed, &.{}, flags, &.{});
    }
}
