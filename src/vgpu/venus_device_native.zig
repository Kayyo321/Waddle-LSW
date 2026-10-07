//! Pure bounded native device-request snapshot; current optional features/extensions stay disabled.
const std = @import("std");
const wire = @import("venus_device_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Owned immutable scalar request; no borrowed pointer survives preflight. Independent records
/// are safe to share immutably. Queue priorities occupy the packed prefix of priorities;
/// counts/families occupy queue_count entries. Accepted current records contain no nodes/names.
pub const owned_request_t = struct {
    /// Actual family indexes in original queue order; caller validates host availability.
    families: [16]u32 = [_]u32{0} ** 16,
    /// Requested priorities per queue,1..16; sum equals priority_count.
    counts: [16]u32 = [_]u32{0} ** 16,
    /// Owned finite [0,1] float values in packed queue order, preserving negative zero.
    priorities: [64]f32 = [_]f32{0} ** 64,
    /// Owned55 legacy flags, all false under current policy; initialized unused storage zero.
    legacy: [55]u32 = [_]u32{0} ** 55,
    /// Owned parsed feature storage, scrubbed after current false-node selection.
    nodes: [9]wire.feature_node_t = [_]wire.feature_node_t{.{}} ** 9,
    /// Compiled registry IDs; current empty admission always leaves all entries zero.
    extension_ids: [32]u8 = [_]u8{0} ** 32,
    /// Initialized queue records,1..16 on success.
    queue_count: u8 = 0,
    /// Initialized priorities,1..64 on success.
    priority_count: u8 = 0,
    /// Selected modern records; zero on every current success.
    node_count: u8 = 0,
    /// Selected extension IDs; zero on every current success.
    extension_count: u8 = 0,
    /// Original nonnull legacy source marker; retained for owned legacy encoding.
    legacy_present: bool = false,
    /// Original Features2 source marker; false core normalizes to absent wire legacy input.
    features2_present: bool = false,
};
const MaxHeaders: usize = 64;
const preflight_scratch_t = struct {
    request: owned_request_t = .{},
    seen: [MaxHeaders]usize = [_]usize{0} ** MaxHeaders,
    header_count: u8 = 0,
};
const NativeTypes = .{
    c.VkPhysicalDeviceFeatures2,                    c.VkPhysicalDeviceVulkan11Features,
    c.VkPhysicalDeviceVulkan12Features,             c.VkPhysicalDeviceVulkan13Features,
    c.VkPhysicalDeviceShaderDrawParametersFeatures, c.VkPhysicalDeviceHostQueryResetFeatures,
    c.VkPhysicalDeviceTransformFeedbackFeaturesEXT, c.VkPhysicalDeviceRobustness2FeaturesEXT,
    c.VkPhysicalDeviceMaintenance5FeaturesKHR,
};
const Tags = [_]u32{
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,                      c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,             c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
};
const Counts = [_]u8{ 55, 12, 47, 15, 1, 1, 2, 3, 1 };
fn native_pointer(comptime native_t: type, address: ?*const anyopaque) !*const native_t {
    const raw = address orelse return error.Invalid;
    if (@intFromPtr(raw) % @alignOf(native_t) != 0) return error.Invalid;
    return @ptrCast(@alignCast(raw));
}
fn copy_flags(comptime native_t: type, input: *const native_t, output: *[55]u32) !void {
    comptime var index: usize = 0;
    inline for (@typeInfo(native_t).Struct.fields) |field| {
        if (comptime !std.mem.eql(u8, field.name, "sType") and !std.mem.eql(u8, field.name, "pNext")) {
            if (field.type != c.VkBool32) @compileError("Device feature field is not Boolean");
            const flag = @field(input.*, field.name);
            if (flag > 1) return error.Invalid;
            output[index] = flag;
            index += 1;
        }
    }
}
fn copy_node(header: *const c.VkBaseInStructure, node: *wire.feature_node_t) !bool {
    inline for (NativeTypes, Tags, Counts) |native_t, tag, count| {
        if (header.sType == tag) {
            const input = try native_pointer(native_t, @ptrCast(header));
            node.type_tag = tag;
            node.flag_count = count;
            if (comptime native_t == c.VkPhysicalDeviceFeatures2) {
                try copy_flags(c.VkPhysicalDeviceFeatures, &input.features, &node.flags);
            } else try copy_flags(native_t, input, &node.flags);
            return true;
        }
    }
    return false;
}
fn promotion_conflict(first: u32, second: u32) bool {
    return (first == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES and
        second == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES) or
        (first == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES and
        second == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES);
}
fn bounded_name(address: [*c]const u8) ![]const u8 {
    if (address == null) return error.Invalid;
    var length: usize = 0;
    while (length < 256) : (length += 1) {
        if (address[length] == 0) {
            const name = address[0..length];
            if (length < 4 or !std.mem.startsWith(u8, name, "VK_")) return error.Invalid;
            for (name) |byte| if (!(std.ascii.isAlphanumeric(byte) or byte == '_')) return error.Invalid;
            return name;
        }
    }
    return error.Invalid;
}
fn copy_queues(info: *const c.VkDeviceCreateInfo, request: *owned_request_t) !void {
    if (info.queueCreateInfoCount == 0 or info.queueCreateInfoCount > 16) return error.Invalid;
    const first = try native_pointer(c.VkDeviceQueueCreateInfo, @ptrCast(info.pQueueCreateInfos));
    const queues: [*]const c.VkDeviceQueueCreateInfo = @ptrCast(first);
    request.queue_count = @intCast(info.queueCreateInfoCount);
    for (queues[0..request.queue_count], 0..) |queue, index| {
        if (queue.sType != c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO or queue.pNext != null or
            queue.flags != 0 or queue.queueCount == 0 or queue.queueCount > 16) return error.Invalid;
        for (request.families[0..index]) |previous| if (previous == queue.queueFamilyIndex) return error.Invalid;
        const total = @as(u32, request.priority_count) + queue.queueCount;
        if (total > 64) return error.Invalid;
        const priority_first = try native_pointer(f32, @ptrCast(queue.pQueuePriorities));
        const priorities: [*]const f32 = @ptrCast(priority_first);
        for (priorities[0..queue.queueCount], 0..) |priority, priority_index| {
            if (!std.math.isFinite(priority) or priority < 0 or priority > 1) return error.Invalid;
            request.priorities[@as(usize, request.priority_count) + priority_index] = priority;
        }
        request.families[index] = queue.queueFamilyIndex;
        request.counts[index] = queue.queueCount;
        request.priority_count = @intCast(total);
    }
}
fn validate_names(info: *const c.VkDeviceCreateInfo) !void {
    if (info.enabledExtensionCount > 32) return error.Invalid;
    if (info.enabledExtensionCount == 0) return;
    const first = try native_pointer([*c]const u8, @ptrCast(info.ppEnabledExtensionNames));
    const names: [*]const [*c]const u8 = @ptrCast(first);
    for (names[0..info.enabledExtensionCount], 0..) |pointer, index| {
        const name = try bounded_name(pointer);
        for (names[0..index]) |previous| if (std.mem.eql(u8, name, try bounded_name(previous))) return error.Invalid;
    }
}
fn copy_chain(first: ?*const anyopaque, scratch: *preflight_scratch_t) !void {
    var next = first;
    while (next) |address| {
        if (scratch.header_count == MaxHeaders) return error.Invalid;
        const header = try native_pointer(c.VkBaseInStructure, address);
        const value = @intFromPtr(address);
        for (scratch.seen[0..scratch.header_count]) |previous| if (previous == value) return error.Invalid;
        scratch.seen[scratch.header_count] = value;
        scratch.header_count += 1;
        next = @ptrCast(header.pNext);
        var node = wire.feature_node_t{};
        if (!try copy_node(header, &node)) continue;
        for (scratch.request.nodes[0..scratch.request.node_count]) |previous| {
            if (previous.type_tag == node.type_tag or promotion_conflict(previous.type_tag, node.type_tag) or
                promotion_conflict(node.type_tag, previous.type_tag)) return error.Invalid;
        }
        // Nine unique recognized tags prove this bounded index; duplicates are rejected above.
        std.debug.assert(scratch.request.node_count < scratch.request.nodes.len);
        scratch.request.nodes[scratch.request.node_count] = node;
        scratch.request.node_count += 1;
        if (node.type_tag == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            scratch.request.features2_present = true;
    }
}
/// Snapshot and preflight one native request. [in] info_address nullable; nonnull inputs, arrays,
/// structs, feature objects and256-byte bounded names are accessible initialized immutable and
/// externally synchronized for this call, with valid extents/disjoint storage. Unknown accessible
/// aligned16-byte headers count toward64; payloads are never accessed. No arbitrary-address
/// accessibility proof is made. Returns owned2696-byte scalar request or Invalid for null/alignment,
/// shape/value/name/cycle/quota/duplicate/promotion/legacy-plus-Features2, LayerNotPresent at the
/// early layer check, FeatureNotPresent for any structurally valid requested true, then
/// ExtensionNotPresent for any valid name under current empty admission. Full structural validation
/// precedes those feature/extension errors. All false modern records are scrubbed/omitted; original
/// false legacy55 is retained. No caller writes, transport/query/reservation, allocation, shared
/// state, retained pointer or lock. Independent calls are thread safe. Output/handle checks belong
/// to the ICD. Ownership of returned values transfers to caller; all source borrows end at return.
pub fn preflight(info_address: ?*const anyopaque) !owned_request_t {
    const info = try native_pointer(c.VkDeviceCreateInfo, info_address);
    if (info.sType != c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO or info.flags != 0) return error.Invalid;
    if (info.enabledLayerCount != 0) return error.LayerNotPresent;
    var scratch = preflight_scratch_t{};
    try copy_queues(info, &scratch.request);
    try validate_names(info);
    try copy_chain(info.pNext, &scratch);
    scratch.request.legacy_present = info.pEnabledFeatures != null;
    if (scratch.request.legacy_present) {
        if (scratch.request.features2_present) return error.Invalid;
        const legacy = try native_pointer(c.VkPhysicalDeviceFeatures, @ptrCast(info.pEnabledFeatures));
        try copy_flags(c.VkPhysicalDeviceFeatures, legacy, &scratch.request.legacy);
    }
    for (scratch.request.legacy) |flag| if (flag != 0) return error.FeatureNotPresent;
    for (scratch.request.nodes[0..scratch.request.node_count]) |node|
        for (node.flags[0..node.flag_count]) |flag| if (flag != 0) return error.FeatureNotPresent;
    if (info.enabledExtensionCount != 0) return error.ExtensionNotPresent;
    scratch.request.nodes = [_]wire.feature_node_t{.{}} ** 9;
    scratch.request.node_count = 0;
    return scratch.request;
}
/// Immutable intersection of actual hardware and implemented ICD support. Caller
/// owns arrays/names through preflight_supported; no global storage or allocation.
pub const support_policy_t = struct {
    /// Supported legacy55 flags; each must be canonical0/1.
    legacy: [55]u32 = [_]u32{0} ** 55,
    /// Supported recognized feature nodes, unique tags and canonical flag extents.
    nodes: []const wire.feature_node_t = &.{},
    /// Canonical implemented extension names, at most32; index becomes owned ID.
    extension_names: []const []const u8 = &.{},
};
/// Snapshot structurally valid native input and admit only the supplied actual
/// hardware/implementation intersection. [in] info_address follows preflight's
/// accessible immutable native contract; policy borrowed, canonical and immutable.
/// [out] Independent owned queues/features/extension IDs with no borrowed input.
/// Invalid malformed native/policy, LayerNotPresent, FeatureNotPresent for any
/// unsupported requested true, ExtensionNotPresent for unavailable names.
/// Supported all-false chains are retained faithfully. No allocation, transport,
/// caller writes or retained pointers; thread-safe for disjoint owners.
pub fn preflight_supported(info_address: ?*const anyopaque, policy: *const support_policy_t) !owned_request_t {
    if (policy.nodes.len > wire.MaxNodes or policy.extension_names.len > wire.MaxExtensions) return error.Invalid;
    for (policy.legacy) |flag| if (flag > 1) return error.Invalid;
    for (policy.nodes, 0..) |supported, index| {
        var expected: ?u8 = null;
        for (Tags, Counts) |tag, extent| if (supported.type_tag == tag) { expected = extent; break; };
        if (expected == null or expected.? != supported.flag_count) return error.Invalid;
        for (supported.flags[0..supported.flag_count]) |flag| if (flag > 1) return error.Invalid;
        for (policy.nodes[0..index]) |previous| if (previous.type_tag == supported.type_tag) return error.Invalid;
    }
    for (policy.extension_names, 0..) |name, index| {
        if (name.len < 4 or name.len >= 256 or !std.mem.startsWith(u8, name, "VK_")) return error.Invalid;
        for (name) |byte| if (!std.ascii.isAlphanumeric(byte) and byte != '_') return error.Invalid;
        for (policy.extension_names[0..index]) |previous| if (std.mem.eql(u8, name, previous)) return error.Invalid;
    }
    const info = try native_pointer(c.VkDeviceCreateInfo, info_address);
    if (info.sType != c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO or info.flags != 0) return error.Invalid;
    if (info.enabledLayerCount != 0) return error.LayerNotPresent;
    var scratch = preflight_scratch_t{};
    try copy_queues(info, &scratch.request);
    try validate_names(info);
    try copy_chain(info.pNext, &scratch);
    scratch.request.legacy_present = info.pEnabledFeatures != null;
    if (scratch.request.legacy_present) {
        if (scratch.request.features2_present) return error.Invalid;
        const legacy = try native_pointer(c.VkPhysicalDeviceFeatures, @ptrCast(info.pEnabledFeatures));
        try copy_flags(c.VkPhysicalDeviceFeatures, legacy, &scratch.request.legacy);
    }
    for (scratch.request.legacy, policy.legacy) |requested, supported| if (requested > supported) return error.FeatureNotPresent;
    for (scratch.request.nodes[0..scratch.request.node_count]) |requested| {
        if (requested.type_tag == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            for (requested.flags[0..requested.flag_count], policy.legacy) |flag, supported| if (flag > supported) return error.FeatureNotPresent;
            continue;
        }
        var matched: ?wire.feature_node_t = null;
        for (policy.nodes) |supported| if (supported.type_tag == requested.type_tag) { matched = supported; break; };
        for (requested.flags[0..requested.flag_count], 0..) |flag, index| {
            const supported = if (matched) |value| value.flags[index] else 0;
            if (flag > supported) return error.FeatureNotPresent;
        }
    }
    if (info.enabledExtensionCount != 0) {
        const first = try native_pointer([*c]const u8, @ptrCast(info.ppEnabledExtensionNames));
        const names: [*]const [*c]const u8 = @ptrCast(first);
        for (names[0..info.enabledExtensionCount], 0..) |pointer, index| {
            const name = try bounded_name(pointer);
            var matched: ?u8 = null;
            for (policy.extension_names, 0..) |available, registry_index| if (std.mem.eql(u8, name, available)) { matched = @intCast(registry_index); break; };
            scratch.request.extension_ids[index] = matched orelse return error.ExtensionNotPresent;
        }
        scratch.request.extension_count = @intCast(info.enabledExtensionCount);
    }
    return scratch.request;
}
comptime {
    if (@sizeOf(owned_request_t) != 2696 or @alignOf(owned_request_t) != 4 or
        @sizeOf(preflight_scratch_t) != 3216 or @alignOf(preflight_scratch_t) != 8)
        @compileError("Owned native device snapshot ABI changed");
    if (@sizeOf(c.VkBaseInStructure) != 16 or @alignOf(c.VkBaseInStructure) != 8 or
        @sizeOf(c.VkPhysicalDeviceFeatures) != 220 or @alignOf(c.VkPhysicalDeviceFeatures) != 4)
        @compileError("Native device header/core ABI changed");
    for (NativeTypes, Counts) |native_t, count| {
        if (@alignOf(native_t) != 8) @compileError("Native device node alignment changed");
        if (native_t == c.VkPhysicalDeviceFeatures2) {
            if (@offsetOf(native_t, "features") != 16) @compileError("Native core node offset changed");
        } else if (@typeInfo(native_t).Struct.fields.len != count + 2) @compileError("Native device Boolean extent changed");
    }
}

// Test-only fixtures. Excluded from production coverage/inventory.
const native_node_t = extern struct {
    type_tag: u32 = 0,
    next: ?*const anyopaque = null,
    flags: [55]u32 = [_]u32{0} ** 55,
};
fn misaligned_address() usize {
    var address: usize = 1;
    std.mem.doNotOptimizeAway(&address);
    return address;
}
fn malformed_pointer(comptime native_t: type, value: *native_t, comptime field: []const u8) void {
    const address: usize = 1;
    const offset = @offsetOf(native_t, field);
    @memcpy(std.mem.asBytes(value)[offset..][0..@sizeOf(usize)], std.mem.asBytes(&address));
}
fn basic_info(queues: []const c.VkDeviceQueueCreateInfo) c.VkDeviceCreateInfo {
    return .{ .sType = c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = @intCast(queues.len), .pQueueCreateInfos = queues.ptr };
}
fn basic_queue(priorities: []const f32) c.VkDeviceQueueCreateInfo {
    return .{ .sType = c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = @intCast(priorities.len), .pQueuePriorities = priorities.ptr };
}
fn expect_invalid(info: *const c.VkDeviceCreateInfo) !void {
    const before = std.mem.asBytes(info).*;
    try std.testing.expectError(error.Invalid, preflight(info));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(info));
}
fn encode_owned(request: *const owned_request_t) !wire_result_t {
    var queues: [16]wire.queue_t = undefined;
    var offset: usize = 0;
    for (0..request.queue_count) |index| {
        const count = request.counts[index];
        queues[index] = .{ .family_index = request.families[index], .priorities = request.priorities[offset..][0..count] };
        offset += count;
    }
    return try wire.create_device(7, 42, queues[0..request.queue_count], &.{}, if (request.legacy_present) &request.legacy else null, request.nodes[0..request.node_count]);
}
const wire_result_t = @import("venus_render_wire.zig").writer_t;
extern fn venus_device_test_encode(*const c.VkDeviceCreateInfo, u64, u64, [*]u8) usize;
fn expect_native_oracle(info: *const c.VkDeviceCreateInfo, request: *const owned_request_t) !void {
    var selected = info.*;
    selected.pNext = null; // Current false modern nodes are omitted after validation.
    var expected: [8192]u8 = undefined;
    const length = venus_device_test_encode(&selected, 7, 42, &expected);
    const actual = try encode_owned(request);
    try std.testing.expectEqual(length, actual.used);
    try std.testing.expectEqualSlices(u8, expected[0..length], actual.bytes[0..actual.used]);
}

test "native device owns packed queues float bits and independently encodes snapshot" {
    var priorities = [_]f32{ 0, -0.0, 0.25, 1 };
    var queues = [_]c.VkDeviceQueueCreateInfo{ basic_queue(&priorities), basic_queue(priorities[1..3]) };
    queues[0].queueFamilyIndex = 91;
    queues[1].queueFamilyIndex = 7;
    var info = basic_info(&queues);
    const before = std.mem.asBytes(&queues).*;
    const request = try preflight(&info);
    const packet = try encode_owned(&request);
    try expect_native_oracle(&info, &request);
    try std.testing.expectEqual(@as(u8, 2), request.queue_count);
    try std.testing.expectEqual(@as(u8, 6), request.priority_count);
    try std.testing.expectEqual(@as(u32, 0x80000000), @as(u32, @bitCast(request.priorities[1])));
    try std.testing.expectEqualSlices(u32, &.{ 91, 7 }, request.families[0..2]);
    try std.testing.expectEqualSlices(u32, &.{ 4, 2 }, request.counts[0..2]);
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&queues));
    priorities = [_]f32{0.75} ** 4;
    queues[0].queueFamilyIndex = 999;
    info.queueCreateInfoCount = 0;
    const repeat = try encode_owned(&request);
    try std.testing.expectEqualSlices(u8, packet.bytes[0..packet.used], repeat.bytes[0..repeat.used]);
    try std.testing.expectEqual(@as(usize, 196), packet.used);
    try std.testing.expectEqual(@as(u8, 0), request.node_count);
    try std.testing.expectEqual(@as(u8, 0), request.extension_count);
}

test "native device rejects every legacy true and malformed Boolean without input writes" {
    const priorities = [_]f32{1};
    const queues = [_]c.VkDeviceQueueCreateInfo{basic_queue(&priorities)};
    var core = std.mem.zeroes(c.VkPhysicalDeviceFeatures);
    var info = basic_info(&queues);
    info.pEnabledFeatures = &core;
    inline for (@typeInfo(c.VkPhysicalDeviceFeatures).Struct.fields) |field| {
        @field(core, field.name) = 1;
        try std.testing.expectError(error.FeatureNotPresent, preflight(&info));
        try std.testing.expectEqual(@as(u32, 1), @field(core, field.name));
        @field(core, field.name) = 2;
        try expect_invalid(&info);
        @field(core, field.name) = 0;
    }
    const request = try preflight(&info);
    try expect_native_oracle(&info, &request);
    try std.testing.expect(request.legacy_present);
    try std.testing.expect(!request.features2_present);
    const packet = try encode_owned(&request);
    try std.testing.expectEqual(@as(usize, 364), packet.used);
    try std.testing.expectEqualSlices(u32, &([_]u32{0} ** 55), &request.legacy);
}

test "native device rejects all137 modern one-hots and malformed native named fields" {
    const priorities = [_]f32{1};
    const queues = [_]c.VkDeviceQueueCreateInfo{basic_queue(&priorities)};
    var info = basic_info(&queues);
    inline for (NativeTypes, Tags) |native_t, tag| {
        var native = std.mem.zeroes(native_t);
        native.sType = tag;
        info.pNext = &native;
        if (comptime native_t == c.VkPhysicalDeviceFeatures2) {
            inline for (@typeInfo(c.VkPhysicalDeviceFeatures).Struct.fields) |field| {
                @field(native.features, field.name) = 1;
                try std.testing.expectError(error.FeatureNotPresent, preflight(&info));
                @field(native.features, field.name) = 2;
                try expect_invalid(&info);
                @field(native.features, field.name) = 0;
            }
        } else inline for (@typeInfo(native_t).Struct.fields[2..]) |field| {
            @field(native, field.name) = 1;
            try std.testing.expectError(error.FeatureNotPresent, preflight(&info));
            @field(native, field.name) = 2;
            try expect_invalid(&info);
            @field(native, field.name) = 0;
        }
        const before = std.mem.asBytes(&native).*;
        const request = try preflight(&info);
        try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&native));
        try std.testing.expectEqual(@as(u8, 0), request.node_count);
        try std.testing.expectEqualSlices(u8, std.mem.asBytes(&([_]wire.feature_node_t{.{}} ** 9)), std.mem.asBytes(&request.nodes));
        try std.testing.expectEqual(native_t == c.VkPhysicalDeviceFeatures2, request.features2_present);
        try std.testing.expectEqual(@as(usize, 144), (try encode_owned(&request)).used);
    }
}

test "native device structural errors precede true flags names and false-node stripping" {
    const priorities = [_]f32{1};
    const queues = [_]c.VkDeviceQueueCreateInfo{basic_queue(&priorities)};
    var info = basic_info(&queues);
    var first = native_node_t{ .type_tag = Tags[1] };
    var second = native_node_t{ .type_tag = Tags[2] };
    first.flags[0] = 1;
    second.flags[46] = 2;
    first.next = &second;
    info.pNext = &first;
    try expect_invalid(&info);
    second.flags[46] = 0;
    const names = [_][*c]const u8{"VK_EXT_example"};
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &names;
    try std.testing.expectError(error.FeatureNotPresent, preflight(&info));
    first.flags[0] = 0;
    try std.testing.expectError(error.ExtensionNotPresent, preflight(&info));
    info.enabledExtensionCount = 0;
    second.type_tag = first.type_tag;
    try expect_invalid(&info);
    second.type_tag = Tags[4];
    try expect_invalid(&info);
    second.type_tag = Tags[2];
    first.type_tag = Tags[5];
    try expect_invalid(&info);
    second.next = &first;
    try expect_invalid(&info);
    second.next = null;
    first.type_tag = Tags[0];
    var core = std.mem.zeroes(c.VkPhysicalDeviceFeatures);
    info.pEnabledFeatures = &core;
    try expect_invalid(&info);
}

test "native device promotion conflicts reject both traversal orders even false" {
    const priorities = [_]f32{1};
    const queues = [_]c.VkDeviceQueueCreateInfo{basic_queue(&priorities)};
    var info = basic_info(&queues);
    for ([_][2]u32{ .{ Tags[1], Tags[4] }, .{ Tags[2], Tags[5] } }) |pair| {
        var first = native_node_t{ .type_tag = pair[0] };
        var second = native_node_t{ .type_tag = pair[1] };
        first.next = &second;
        info.pNext = &first;
        try expect_invalid(&info);
        first.next = null;
        second.next = &first;
        info.pNext = &second;
        try expect_invalid(&info);
    }
}

test "native device legal reversed chains strip every false node and preserve unknown bytes" {
    const priorities = [_]f32{1};
    const queues = [_]c.VkDeviceQueueCreateInfo{basic_queue(&priorities)};
    var info = basic_info(&queues);
    const selected = [_]u32{ Tags[0], Tags[1], Tags[2], Tags[3], Tags[6], Tags[7], Tags[8] };
    for ([_]bool{ false, true }) |reverse| {
        var nodes = [_]native_node_t{.{}} ** 15;
        for (0..7) |index| {
            nodes[2 * index].type_tag = selected[if (reverse) 6 - index else index];
            nodes[2 * index + 1].type_tag = if (index % 2 == 0) 0x7ffffff0 else c.VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
            nodes[2 * index + 1].flags = [_]u32{0xa5a5a5a5} ** 55;
        }
        nodes[14].type_tag = c.VK_STRUCTURE_TYPE_APPLICATION_INFO;
        for (nodes[0..14], 0..) |*node, index| node.next = &nodes[index + 1];
        info.pNext = &nodes[0];
        const before = std.mem.asBytes(&nodes).*;
        const result = try preflight(&info);
        try expect_native_oracle(&info, &result);
        try std.testing.expect(result.features2_present);
        try std.testing.expect(!result.legacy_present);
        try std.testing.expectEqual(@as(u8, 0), result.node_count);
        try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&nodes));
        try std.testing.expectEqual(@as(usize, 144), (try encode_owned(&result)).used);
    }
}

test "native device unknown header quota rejects unread65th and repeated addresses" {
    const priorities = [_]f32{1};
    const queues = [_]c.VkDeviceQueueCreateInfo{basic_queue(&priorities)};
    var info = basic_info(&queues);
    var headers = [_]c.VkBaseInStructure{.{ .sType = 0x7ffffff0 }} ** 64;
    for (headers[0..63], 0..) |*header, index| header.pNext = &headers[index + 1];
    info.pNext = &headers[0];
    _ = try preflight(&info);
    headers[63].pNext = @ptrFromInt(8); // Aligned but inaccessible: quota must precede read.
    try expect_invalid(&info);
    headers[63].pNext = &headers[63];
    try expect_invalid(&info);
    headers[63].pNext = null;
    headers[2].pNext = &headers[1];
    try expect_invalid(&info);
}

test "native device untyped outer chain legacy arrays and priorities enforce alignment" {
    try std.testing.expectError(error.Invalid, preflight(null));
    try std.testing.expectError(error.Invalid, preflight(@ptrFromInt(misaligned_address())));
    const priorities = [_]f32{1};
    var queues = [_]c.VkDeviceQueueCreateInfo{basic_queue(&priorities)};
    var info = basic_info(&queues);
    info.pNext = @ptrFromInt(misaligned_address());
    try expect_invalid(&info);
    info.pNext = null;
    malformed_pointer(c.VkDeviceCreateInfo, &info, "pEnabledFeatures");
    try expect_invalid(&info);
    info.pEnabledFeatures = null;
    malformed_pointer(c.VkDeviceCreateInfo, &info, "pQueueCreateInfos");
    try expect_invalid(&info);
    info.pQueueCreateInfos = null;
    try expect_invalid(&info);
    info.pQueueCreateInfos = &queues;
    malformed_pointer(c.VkDeviceQueueCreateInfo, &queues[0], "pQueuePriorities");
    try expect_invalid(&info);
    queues[0].pQueuePriorities = null;
    try expect_invalid(&info);
    queues[0] = basic_queue(&priorities);
    info.enabledExtensionCount = 1;
    malformed_pointer(c.VkDeviceCreateInfo, &info, "ppEnabledExtensionNames");
    try expect_invalid(&info);
    info.ppEnabledExtensionNames = null;
    try expect_invalid(&info);
}

test "native device queue record count unique family total and float boundaries" {
    var priorities = [_]f32{1} ** 16;
    var queues = [_]c.VkDeviceQueueCreateInfo{basic_queue(priorities[0..4])} ** 16;
    for (&queues, 0..) |*queue, index| queue.queueFamilyIndex = @intCast(index);
    var info = basic_info(&queues);
    const full = try preflight(&info);
    try std.testing.expectEqual(@as(u8, 64), full.priority_count);
    try std.testing.expectEqual(@as(u8, 16), full.queue_count);
    queues[15].queueCount = 5;
    try expect_invalid(&info);
    queues[15].queueCount = 4;
    queues[15].queueFamilyIndex = 0;
    try expect_invalid(&info);
    queues[15].queueFamilyIndex = 15;
    info.queueCreateInfoCount = 0;
    try expect_invalid(&info);
    info.queueCreateInfoCount = 17;
    try expect_invalid(&info);
    info.queueCreateInfoCount = 1;
    queues[0].queueCount = 0;
    try expect_invalid(&info);
    queues[0].queueCount = 17;
    try expect_invalid(&info);
    queues[0].queueCount = 16;
    _ = try preflight(&info);
    for ([_]f32{ -0.01, 1.01, std.math.inf(f32), -std.math.inf(f32), std.math.nan(f32) }) |bad| {
        priorities[15] = bad;
        try expect_invalid(&info);
    }
}

test "native device native tags flags queue chain and early layers have exact errors" {
    const priorities = [_]f32{1};
    var queues = [_]c.VkDeviceQueueCreateInfo{basic_queue(&priorities)};
    var info = basic_info(&queues);
    info.sType = c.VK_STRUCTURE_TYPE_APPLICATION_INFO;
    try expect_invalid(&info);
    info.sType = c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.flags = 1;
    try expect_invalid(&info);
    info.flags = 0;
    info.enabledLayerCount = 1;
    malformed_pointer(c.VkDeviceCreateInfo, &info, "pQueueCreateInfos");
    try std.testing.expectError(error.LayerNotPresent, preflight(&info));
    info.enabledLayerCount = 0;
    info.pQueueCreateInfos = &queues;
    queues[0].sType = c.VK_STRUCTURE_TYPE_APPLICATION_INFO;
    try expect_invalid(&info);
    queues[0].sType = c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queues[0].flags = 1;
    try expect_invalid(&info);
    queues[0].flags = 0;
    queues[0].pNext = @ptrFromInt(misaligned_address());
    try expect_invalid(&info);
}

test "native device extension bounded names duplicates and full structural precedence" {
    const priorities = [_]f32{1};
    const queues = [_]c.VkDeviceQueueCreateInfo{basic_queue(&priorities)};
    var info = basic_info(&queues);
    var names = [_][*c]const u8{ "VK_A", "VK_EXT_example" };
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = &names;
    try std.testing.expectError(error.ExtensionNotPresent, preflight(&info));
    names[1] = names[0];
    try expect_invalid(&info);
    names[1] = null;
    try expect_invalid(&info);
    for ([_][*c]const u8{ "VK_", "vk_A", "VK_A-", "VK_\x80" }) |bad| {
        names[1] = bad;
        try expect_invalid(&info);
    }
    var long = [_]u8{'A'} ** 256;
    @memcpy(long[0..3], "VK_");
    names[1] = &long;
    try expect_invalid(&info);
    long[255] = 0;
    try std.testing.expectError(error.ExtensionNotPresent, preflight(&info));
    info.enabledExtensionCount = 33;
    try expect_invalid(&info);
    info.enabledExtensionCount = 1;
    names[0] = "VK_OK_123";
    var node = native_node_t{ .type_tag = Tags[8] };
    node.flags[0] = 2;
    info.pNext = &node;
    try expect_invalid(&info);
    node.flags[0] = 1;
    try std.testing.expectError(error.FeatureNotPresent, preflight(&info));
}

test "supported device owns every admitted true feature and canonical extension ID without retaining native storage" {
    const priorities=[_]f32{1};
    var queues=[_]c.VkDeviceQueueCreateInfo{basic_queue(&priorities)};
    var info=basic_info(&queues);
    const names=[_] [*c]const u8{"VK_KHR_swapchain","VK_EXT_robustness2"};
    info.enabledExtensionCount=2;info.ppEnabledExtensionNames=&names;
    const registry=[_] []const u8{"VK_EXT_robustness2","VK_KHR_swapchain"};
    var policy=support_policy_t{.legacy=[_]u32{1} ** 55,.extension_names=&registry};
    for (Tags,Counts) |tag,extent| {
        var native=native_node_t{.type_tag=tag};
        info.pNext=&native;
        var supported=wire.feature_node_t{.type_tag=tag,.flag_count=extent,.flags=[_]u32{1} ** 55};
        policy.nodes=(@as([*]wire.feature_node_t,@ptrCast(&supported)))[0..1];
        for (0..extent) |index| {
            native.flags[index]=1;
            const request=try preflight_supported(&info,&policy);
            try std.testing.expectEqual(@as(u8,1),request.node_count);
            try std.testing.expectEqual(@as(u8,2),request.extension_count);
            try std.testing.expectEqualSlices(u8,&.{1,0},request.extension_ids[0..2]);
            try std.testing.expectEqual(@as(u32,1),request.nodes[0].flags[index]);
            const old=request;
            native.flags[index]=0;
            try std.testing.expectEqualDeep(old,request);
            if (tag==c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) policy.legacy[index]=0 else supported.flags[index]=0;
            native.flags[index]=1;
            try std.testing.expectError(error.FeatureNotPresent,preflight_supported(&info,&policy));
            native.flags[index]=0;
            if (tag==c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) policy.legacy[index]=1 else supported.flags[index]=1;
        }
    }
    info.pNext=null;
    var legacy=c.VkPhysicalDeviceFeatures{};
    legacy.robustBufferAccess=1;info.pEnabledFeatures=&legacy;
    const request=try preflight_supported(&info,&policy);
    try std.testing.expectEqual(@as(u32,1),request.legacy[0]);
    policy.legacy[0]=0;
    try std.testing.expectError(error.FeatureNotPresent,preflight_supported(&info,&policy));
    policy.legacy[0]=1;policy.extension_names=&.{"VK_KHR_swapchain"};
    try std.testing.expectError(error.ExtensionNotPresent,preflight_supported(&info,&policy));
}
test "supported policy rejects malformed policy and native structure before admission" {
    const priorities=[_]f32{1};
    var queues=[_]c.VkDeviceQueueCreateInfo{basic_queue(&priorities)};
    var info=basic_info(&queues);
    var policy=support_policy_t{};
    try std.testing.expectError(error.Invalid,preflight_supported(null,&policy));
    policy.legacy[0]=2;
    try std.testing.expectError(error.Invalid,preflight_supported(&info,&policy));
    policy.legacy[0]=0;
    var node=wire.feature_node_t{.type_tag=Tags[1],.flag_count=Counts[1]};
    policy.nodes=(@as([*]wire.feature_node_t,@ptrCast(&node)))[0..1];
    node.type_tag=0;try std.testing.expectError(error.Invalid,preflight_supported(&info,&policy));
    node.type_tag=Tags[1];node.flag_count=55;try std.testing.expectError(error.Invalid,preflight_supported(&info,&policy));
    node.flag_count=Counts[1];node.flags[0]=2;try std.testing.expectError(error.Invalid,preflight_supported(&info,&policy));
    node.flags[0]=0;const duplicate=[_]wire.feature_node_t{node,node};policy.nodes=&duplicate;
    try std.testing.expectError(error.Invalid,preflight_supported(&info,&policy));
    const too_many=[_]wire.feature_node_t{node} ** 10;policy.nodes=&too_many;
    try std.testing.expectError(error.Invalid,preflight_supported(&info,&policy));
    policy.nodes=&.{};
    for ([_][]const []const u8{&.{"no"},&.{"VK_test!"},&.{"VK_test","VK_test"}}) |names| {
        policy.extension_names=names;try std.testing.expectError(error.Invalid,preflight_supported(&info,&policy));
    }
    const many_names=[_][]const u8{"VK_test"} ** 33;policy.extension_names=&many_names;
    try std.testing.expectError(error.Invalid,preflight_supported(&info,&policy));
    policy.extension_names=&.{};
    info.flags=1;try std.testing.expectError(error.Invalid,preflight_supported(&info,&policy));
    info.flags=0;info.enabledLayerCount=1;try std.testing.expectError(error.LayerNotPresent,preflight_supported(&info,&policy));
    info.enabledLayerCount=0;
    var modern=native_node_t{.type_tag=Tags[1]};info.pNext=&modern;
    const owned=try preflight_supported(&info,&policy);
    try std.testing.expectEqual(@as(u8,1),owned.node_count);
    modern.flags[0]=1;try std.testing.expectError(error.FeatureNotPresent,preflight_supported(&info,&policy));
    modern.flags[0]=0;modern.type_tag=Tags[0];var legacy=c.VkPhysicalDeviceFeatures{};info.pEnabledFeatures=&legacy;
    try std.testing.expectError(error.Invalid,preflight_supported(&info,&policy));
}
