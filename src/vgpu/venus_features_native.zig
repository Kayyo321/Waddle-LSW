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
/// Maximum distinct promoted Vulkan1.1-1.3 feature structures in one borrowed chain.
pub const MaxPromoted: usize = 30;
/// Copied tags and borrowed known targets. Underlying initialized objects must outlive use;
/// links/headers require external synchronization. No heap ownership or retained global state.
pub const chain_t = struct {
    /// Initialized prefix length, at most MaxNodes after successful collection.
    count: usize = 0,
    /// Owned recognized structure tags in native forward-chain order.
    tags: [MaxNodes]u32 = [_]u32{0} ** MaxNodes,
    /// Borrowed mutable output objects, nonnull in the initialized prefix.
    addresses: [MaxNodes]?*c.VkBaseOutStructure = [_]?*c.VkBaseOutStructure{null} ** MaxNodes,
    /// Initialized promoted prefix length, at most MaxPromoted after successful collection.
    promoted_count: usize = 0,
    /// Owned unique promoted structure tags in native forward-chain order.
    promoted_tags: [MaxPromoted]u32 = [_]u32{0} ** MaxPromoted,
    /// Borrowed mutable promoted output objects, nonnull in the initialized prefix.
    promoted_addresses: [MaxPromoted]?*c.VkBaseOutStructure = [_]?*c.VkBaseOutStructure{null} ** MaxPromoted,
};
/// Owned projected aggregate source for promoted publication; no pointers, immutable sharing safe.
pub const promoted_source_t = struct {
    /// Visible public API version; promoted structures above it remain unwritten.
    api_version: u32 = 0,
    /// Projected Vulkan11/12/13 aggregate Boolean words in native order, extents12/47/15, zero suffix.
    flags: [3][wire.MaxNodeFlags]u32 = [_][wire.MaxNodeFlags]u32{[_]u32{0} ** wire.MaxNodeFlags} ** 3,
};
/// Comptime-only promoted registry record; every promoted Boolean keeps its aggregate member name.
/// native_t is the SDK structure, tag its canonical sType and core its level (0=1.1, 1=1.2, 2=1.3).
pub const promoted_t = struct { native_t: type, tag: u32, core: usize };
const CoreTags = [_]u32{
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
};
const CoreVersions = [_]u32{ c.VK_API_VERSION_1_1, c.VK_API_VERSION_1_2, c.VK_API_VERSION_1_3 };
const CoreCounts = [_]usize{ 12, 47, 15 };
const CoreTypes = .{ c.VkPhysicalDeviceVulkan11Features, c.VkPhysicalDeviceVulkan12Features, c.VkPhysicalDeviceVulkan13Features };
const CoreFirst = .{ "storageBuffer16BitAccess", "samplerMirrorClampToEdge", "robustImageAccess" };
/// Immutable comptime-only registry of the thirty promoted feature structures, grouped by level.
pub const Promoted = [_]promoted_t{
    .{ .native_t = c.VkPhysicalDevice16BitStorageFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, .core = 0 },
    .{ .native_t = c.VkPhysicalDeviceMultiviewFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, .core = 0 },
    .{ .native_t = c.VkPhysicalDeviceVariablePointersFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, .core = 0 },
    .{ .native_t = c.VkPhysicalDeviceProtectedMemoryFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, .core = 0 },
    .{ .native_t = c.VkPhysicalDeviceSamplerYcbcrConversionFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, .core = 0 },
    .{ .native_t = c.VkPhysicalDevice8BitStorageFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceShaderAtomicInt64Features, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceShaderFloat16Int8Features, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceDescriptorIndexingFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceScalarBlockLayoutFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceImagelessFramebufferFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceUniformBufferStandardLayoutFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceTimelineSemaphoreFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceBufferDeviceAddressFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceVulkanMemoryModelFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, .core = 1 },
    .{ .native_t = c.VkPhysicalDeviceImageRobustnessFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceInlineUniformBlockFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDevicePipelineCreationCacheControlFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDevicePrivateDataFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceShaderTerminateInvocationFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceSubgroupSizeControlFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceSynchronization2Features, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceTextureCompressionASTCHDRFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceDynamicRenderingFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceShaderIntegerDotProductFeatures, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, .core = 2 },
    .{ .native_t = c.VkPhysicalDeviceMaintenance4Features, .tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, .core = 2 },
};
/// Immutable promoted tags in registry order, for the ICD cross-module consistency proof.
pub const PromotedTags: [MaxPromoted]u32 = tags: {
    var tags: [MaxPromoted]u32 = undefined;
    for (Promoted, 0..) |entry, index| tags[index] = entry.tag;
    break :tags tags;
};
/// Immutable promoted aggregate levels (0=1.1, 1=1.2, 2=1.3) in registry order.
pub const PromotedCores: [MaxPromoted]u8 = cores: {
    var cores: [MaxPromoted]u8 = undefined;
    for (Promoted, 0..) |entry, index| cores[index] = entry.core;
    break :cores cores;
};
comptime {
    @setEvalBranchQuota(200000);
    if (Promoted.len != MaxPromoted) @compileError("Promoted feature inventory changed");
    var used = [_][wire.MaxNodeFlags]bool{[_]bool{false} ** wire.MaxNodeFlags} ** 3;
    for (Promoted, 0..) |entry, index| {
        if (@alignOf(entry.native_t) != 8 or @offsetOf(entry.native_t, "sType") != 0 or @offsetOf(entry.native_t, "pNext") != 8)
            @compileError("Promoted feature header ABI changed");
        for (Promoted[0..index]) |previous| if (previous.tag == entry.tag) @compileError("Duplicate promoted feature tag");
        for (Tags) |tag| if (tag == entry.tag) @compileError("Promoted tag aliases a canonical node");
        const aggregate_t = CoreTypes[entry.core];
        for (@typeInfo(entry.native_t).Struct.fields[2..]) |field| {
            if (field.type != c.VkBool32 or !@hasField(aggregate_t, field.name)) @compileError("Promoted feature has no aggregate Boolean: " ++ field.name);
            const offset = @offsetOf(aggregate_t, field.name) - @offsetOf(aggregate_t, CoreFirst[entry.core]);
            if (offset % 4 != 0 or offset / 4 >= CoreCounts[entry.core]) @compileError("Promoted feature outside aggregate extent");
            if (used[entry.core][offset / 4]) @compileError("Two promoted structures share one aggregate Boolean");
            used[entry.core][offset / 4] = true;
        }
    }
}
fn promoted_size(tag: u32) ?usize {
    inline for (Promoted) |entry| if (tag == entry.tag) return @sizeOf(entry.native_t);
    return null;
}
fn publish_promoted(comptime entry: promoted_t, output: *entry.native_t, flags: *const [wire.MaxNodeFlags]u32) void {
    const aggregate_t = CoreTypes[entry.core];
    inline for (@typeInfo(entry.native_t).Struct.fields[2..]) |field| {
        const index = comptime (@offsetOf(aggregate_t, field.name) - @offsetOf(aggregate_t, CoreFirst[entry.core])) / 4;
        @field(output.*, field.name) = flags[index];
    }
}
/// Collect native headers. [in] first nullable borrowed accessible initialized chain, caller
/// synchronized until snapshot retirement. Unknown payloads are never accessed. Promoted
/// structures are recorded separately in forward order. Returns an owned bounded snapshot or
/// Invalid for cycle/duplicate/quota/alignment; no fields modified.
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
        if (node_count(node.sType) == null) {
            if (promoted_size(node.sType) == null) continue;
            for (result.promoted_tags[0..result.promoted_count]) |tag| if (tag == node.sType) return error.Invalid;
            // Thirty unique promoted tags bound this index; duplicates are rejected above.
            std.debug.assert(result.promoted_count < MaxPromoted);
            result.promoted_tags[result.promoted_count] = node.sType;
            result.promoted_addresses[result.promoted_count] = node;
            result.promoted_count += 1;
            continue;
        }
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
        if (@sizeOf(chain_t) != 472 or @alignOf(chain_t) != 8 or @sizeOf(node_t) != 196 or
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
/// Publish core55, extension and promoted outputs as one transaction. [in,out] output_address
/// nullable; nonnull addresses must reference an exclusive
/// initialized accessible Features2; [in] chain is collected from exactly output.pNext with
/// immutable headers/links. Borrowed core/nodes/source/snapshot inputs are disjoint from every
/// output; all native objects including unknown records are distinct and non-overlapping. Caller
/// owns storage and synchronization until return. Invalid for every validate_features2 error
/// leaves all outputs untouched. Success writes only named Boolean fields; promoted structures
/// receive their aggregate source words only when source.api_version reaches their promotion
/// version, otherwise they stay unchanged. Outer/chain headers, padding and canaries are preserved.
/// Fixed bounded scalar storage; no heap, locks, transport or pointer retention.
pub fn publish_features2(output_address: ?*anyopaque, chain: *const chain_t, nodes: []const node_t, core: []const u32, source: *const promoted_source_t) !void {
    try validate_features2(output_address, chain, nodes, core, source);
    const output: *c.VkPhysicalDeviceFeatures2 = @ptrCast(@alignCast(output_address.?));
    for (nodes, 0..) |node, index| publish_node(chain.addresses[index].?, node.flags[0..node.flag_count]);
    publish_flags(c.VkPhysicalDeviceFeatures, &output.features, core);
    for (chain.promoted_tags[0..chain.promoted_count], chain.promoted_addresses[0..chain.promoted_count]) |tag, target| {
        inline for (Promoted) |entry| {
            if (tag == entry.tag and source.api_version >= CoreVersions[entry.core])
                publish_promoted(entry, @ptrCast(@alignCast(target.?)), &source.flags[entry.core]);
        }
    }
}

/// Preflight complete Features2 output topology without mutation. [in] output_address nullable,
/// nonnull refers to accessible initialized externally synchronized native storage. [in] chain,
/// nodes, core and source are initialized immutable call-borrowed records disjoint from every
/// output; chain is collected from exactly output.pNext with unchanged headers/links. Invalid for
/// outer null/alignment/tag, core extent/Boolean, known node schema/header/Boolean or pair/outer
/// overlap, a non-Boolean source word or nonzero source suffix, an aggregate node differing from
/// the source, or a promoted target that is null/misaligned/re-tagged/overflowing or overlaps the
/// outer object, a known target or another promoted target. Promoted target checks do not depend
/// on source.api_version. Success authorizes publication only while all same-call inputs remain
/// unchanged. No output byte writes, allocation, transport, locks, global mutation or retained
/// pointers on either result.
pub fn validate_features2(output_address: ?*anyopaque, chain: *const chain_t, nodes: []const node_t, core: []const u32, source: *const promoted_source_t) !void {
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
    try validate_promoted(address, end, chain, nodes, source);
}
fn validate_promoted(address: usize, end: usize, chain: *const chain_t, nodes: []const node_t, source: *const promoted_source_t) !void {
    if (chain.promoted_count > MaxPromoted) return error.Invalid;
    for (&source.flags, CoreCounts) |*flags, count| {
        for (flags[0..count]) |flag| if (flag > 1) return error.Invalid;
        for (flags[count..]) |flag| if (flag != 0) return error.Invalid;
    }
    for (nodes) |node| for (CoreTags, CoreCounts, 0..) |tag, count, core| {
        if (node.type_tag == tag and !std.mem.eql(u32, node.flags[0..count], source.flags[core][0..count])) return error.Invalid;
    };
    for (chain.promoted_tags[0..chain.promoted_count], chain.promoted_addresses[0..chain.promoted_count], 0..) |tag, target, index| {
        const size = promoted_size(tag) orelse return error.Invalid;
        const pointer = target orelse return error.Invalid;
        const target_address = @intFromPtr(pointer);
        if (target_address % @alignOf(c.VkBaseOutStructure) != 0) return error.Invalid;
        // Reject an overflowing extent before reading its header.
        const target_end = std.math.add(usize, target_address, size) catch return error.Invalid;
        if (pointer.sType != tag) return error.Invalid;
        if (address < target_end and target_address < end) return error.Invalid;
        for (nodes, chain.addresses[0..nodes.len]) |node, known| {
            const known_address = @intFromPtr(known.?); // validate_batch proved nonnull and no overflow.
            if (known_address < target_end and target_address < known_address + native_size(node.type_tag)) return error.Invalid;
        }
        for (chain.promoted_tags[0..index], chain.promoted_addresses[0..index]) |previous_tag, previous| {
            const previous_address = @intFromPtr(previous.?); // Checked in an earlier iteration.
            const previous_end = previous_address + promoted_size(previous_tag).?;
            if (previous_address < target_end and target_address < previous_end) return error.Invalid;
        }
    }
}

// Test-only fixtures.
const NoPromoted = promoted_source_t{};
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
    try std.testing.expectEqual(@as(usize, 472), @sizeOf(chain_t));
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
        try @call(.never_inline, validate_features2, .{ &box.output, &chain, &[_]node_t{node}, &core, &NoPromoted });
        try std.testing.expectEqualSlices(u8, &preflight_before, std.mem.asBytes(&box));
        try @call(.never_inline, publish_features2, .{ &box.output, &chain, &[_]node_t{node}, &core, &NoPromoted });
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
        try std.testing.expectError(error.Invalid, @call(.never_inline, validate_features2, .{ &box.output, &chain, &nodes, core[0..count], &NoPromoted }));
        try std.testing.expectError(error.Invalid, @call(.never_inline, publish_features2, .{ &box.output, &chain, &nodes, core[0..count], &NoPromoted }));
        try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    }
    for (0..55) |index| {
        for ([_]u32{ 2, 0xffffffff }) |invalid| {
            core[index] = invalid;
            try std.testing.expectError(error.Invalid, @call(.never_inline, validate_features2, .{ &box.output, &chain, &nodes, core[0..55], &NoPromoted }));
            try std.testing.expectError(error.Invalid, @call(.never_inline, publish_features2, .{ &box.output, &chain, &nodes, core[0..55], &NoPromoted }));
            try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
        }
        core[index] = 1;
    }
    nodes[1].flags[0] = 2;
    try std.testing.expectError(error.Invalid, validate_features2(&box.output, &chain, &nodes, core[0..55], &NoPromoted));
    try std.testing.expectError(error.Invalid, publish_features2(&box.output, &chain, &nodes, core[0..55], &NoPromoted));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    nodes[1] = record(Tags[5]);
    nodes[1].flag_count = 0;
    try std.testing.expectError(error.Invalid, validate_features2(&box.output, &chain, &nodes, core[0..55], &NoPromoted));
    try std.testing.expectError(error.Invalid, publish_features2(&box.output, &chain, &nodes, core[0..55], &NoPromoted));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    nodes[1] = record(Tags[5]);
    nodes[1].type_tag = Tags[4];
    try std.testing.expectError(error.Invalid, validate_features2(&box.output, &chain, &nodes, core[0..55], &NoPromoted));
    try std.testing.expectError(error.Invalid, publish_features2(&box.output, &chain, &nodes, core[0..55], &NoPromoted));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    nodes[1] = record(Tags[5]);
    box.output.sType = 0;
    const invalid_header = std.mem.asBytes(&box).*;
    try std.testing.expectError(error.Invalid, validate_features2(&box.output, &chain, &nodes, core[0..55], &NoPromoted));
    try std.testing.expectError(error.Invalid, publish_features2(&box.output, &chain, &nodes, core[0..55], &NoPromoted));
    try std.testing.expectEqualSlices(u8, &invalid_header, std.mem.asBytes(&box));
    box.output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    const misaligned: *anyopaque = @ptrFromInt(9);
    try std.testing.expectError(error.Invalid, @call(.never_inline, validate_features2, .{ @as(?*anyopaque, null), &chain, &nodes, core[0..55], &NoPromoted }));
    try std.testing.expectError(error.Invalid, @call(.never_inline, publish_features2, .{ @as(?*anyopaque, null), &chain, &nodes, core[0..55], &NoPromoted }));
    try std.testing.expectError(error.Invalid, @call(.never_inline, validate_features2, .{ misaligned, &chain, &nodes, core[0..55], &NoPromoted }));
    try std.testing.expectError(error.Invalid, @call(.never_inline, publish_features2, .{ misaligned, &chain, &nodes, core[0..55], &NoPromoted }));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    const unknown_before = .{ box.unknown, box.middle, box.tail };
    try publish_features2(&box.output, &chain, &nodes, core[0..55], &NoPromoted);
    try std.testing.expectEqualDeep(unknown_before, .{ box.unknown, box.middle, box.tail });
}
test "Features2 core versus chain overlap rejection and empty maximal chains" {
    var output = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
    output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    const core = [_]u32{0} ** 55;
    try publish_features2(&output, &chain_t{}, &.{}, &core, &NoPromoted);
    const overlapping: *c.VkPhysicalDeviceMaintenance5FeaturesKHR = @ptrCast(@alignCast(&output.features));
    overlapping.sType = Tags[4];
    overlapping.pNext = null;
    output.pNext = overlapping;
    const chain = try collect_chain(output.pNext);
    const before = std.mem.asBytes(&output).*;
    try std.testing.expectError(error.Invalid, @call(.never_inline, validate_features2, .{ &output, &chain, &[_]node_t{record(Tags[4])}, &core, &NoPromoted }));
    try std.testing.expectError(error.Invalid, @call(.never_inline, publish_features2, .{ &output, &chain, &[_]node_t{record(Tags[4])}, &core, &NoPromoted }));
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
    try publish_features2(&output, &maximal, &nodes, &core, &NoPromoted);
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
    try @call(.never_inline, validate_features2, .{ &box.output, &chain, &nodes, &core, &NoPromoted });
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
    try @call(.never_inline, publish_features2, .{ &box.output, &chain, &nodes, &core, &NoPromoted });
    try std.testing.expect(!std.mem.eql(u8, &before, std.mem.asBytes(&box)));
}
fn promoted_index(comptime entry: promoted_t, comptime name: []const u8) usize {
    const aggregate_t = CoreTypes[entry.core];
    return (@offsetOf(aggregate_t, name) - @offsetOf(aggregate_t, CoreFirst[entry.core])) / 4;
}
const PromotedApis = [_]u32{ c.VK_API_VERSION_1_0, c.VK_API_VERSION_1_1, c.VK_API_VERSION_1_2, c.VK_API_VERSION_1_3 | 280 };
test "Features2 every promoted Boolean one-hot follows its aggregate source under each API gate" {
    @setEvalBranchQuota(1000000);
    var checked: usize = 0;
    inline for (Promoted) |entry| {
        const box_t = extern struct { before: u64, output: c.VkPhysicalDeviceFeatures2, unknown: c.VkBaseOutStructure, promoted: entry.native_t, after: u64 };
        const fields = @typeInfo(entry.native_t).Struct.fields[2..];
        for (PromotedApis) |api| for (0..fields.len) |selected| {
            var box: box_t = undefined;
            @memset(std.mem.asBytes(&box), 0xa5);
            box.output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            box.output.pNext = &box.unknown;
            box.unknown = .{ .sType = 0x7fffffff, .pNext = @ptrCast(&box.promoted) };
            box.promoted.sType = entry.tag;
            box.promoted.pNext = null;
            const chain = try collect_chain(box.output.pNext);
            try std.testing.expectEqual(@as(usize, 0), chain.count);
            try std.testing.expectEqual(@as(usize, 1), chain.promoted_count);
            var source = promoted_source_t{ .api_version = api };
            inline for (fields, 0..) |field, index| {
                if (index == selected) source.flags[entry.core][promoted_index(entry, field.name)] = 1;
            }
            const core = [_]u32{0} ** CoreFlags;
            var expected = std.mem.asBytes(&box).*;
            @memset(expected[@offsetOf(box_t, "output") + 16 ..][0..@sizeOf(c.VkPhysicalDeviceFeatures)], 0);
            if (api >= CoreVersions[entry.core]) {
                inline for (fields, 0..) |field, index| {
                    const offset = @offsetOf(box_t, "promoted") + @offsetOf(entry.native_t, field.name);
                    std.mem.writeInt(u32, expected[offset..][0..4], @intFromBool(index == selected), @import("builtin").target.cpu.arch.endian());
                }
            }
            const before = std.mem.asBytes(&box).*;
            try @call(.never_inline, validate_features2, .{ &box.output, &chain, &[_]node_t{}, &core, &source });
            try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&box));
            try @call(.never_inline, publish_features2, .{ &box.output, &chain, &[_]node_t{}, &core, &source });
            try std.testing.expectEqualSlices(u8, &expected, std.mem.asBytes(&box));
            checked += 1;
        };
    }
    var fields_total: usize = 0;
    inline for (Promoted) |entry| fields_total += @typeInfo(entry.native_t).Struct.fields.len - 2;
    try std.testing.expectEqual(fields_total * PromotedApis.len, checked);
}
const promoted_storage_t = extern struct { value: [PromotedFeatureBytes]u8 align(8) };
const PromotedFeatureBytes = size: {
    var size: usize = 0;
    for (Promoted) |entry| size = @max(size, @sizeOf(entry.native_t));
    break :size size;
};
const combined_box_t = extern struct {
    output: c.VkPhysicalDeviceFeatures2,
    vulkan11: c.VkPhysicalDeviceVulkan11Features,
    vulkan12: c.VkPhysicalDeviceVulkan12Features,
    vulkan13: c.VkPhysicalDeviceVulkan13Features,
    promoted: [MaxPromoted]promoted_storage_t,
    after: u64,
};
fn combined_init(box: *combined_box_t, with_aggregates: bool) void {
    @memset(std.mem.asBytes(box), 0xa5);
    box.output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    var previous: *c.VkBaseOutStructure = @ptrCast(&box.output);
    if (with_aggregates) {
        inline for (.{ "vulkan11", "vulkan12", "vulkan13" }, CoreTags) |name, tag| {
            const header: *c.VkBaseOutStructure = @ptrCast(&@field(box, name));
            header.sType = tag;
            previous.pNext = header;
            previous = header;
        }
    }
    inline for (Promoted, 0..) |entry, index| {
        const header: *c.VkBaseOutStructure = @ptrCast(&box.promoted[MaxPromoted - 1 - index].value);
        header.sType = entry.tag;
        previous.pNext = header;
        previous = header;
    }
    previous.pNext = null;
}
test "Features2 combined thirty promoted structures and aggregates publish consistent source words" {
    @setEvalBranchQuota(1000000);
    const box = try std.testing.allocator.create(combined_box_t);
    defer std.testing.allocator.destroy(box);
    for (PromotedApis) |api| for ([_]bool{ false, true }) |with_aggregates| {
        combined_init(box, with_aggregates);
        const chain = try collect_chain(box.output.pNext);
        try std.testing.expectEqual(@as(usize, MaxPromoted), chain.promoted_count);
        var source = promoted_source_t{ .api_version = api };
        for (CoreCounts, 0..) |count, core| @memset(source.flags[core][0..count], 1);
        var nodes: [3]node_t = undefined;
        for (CoreTags, 0..) |tag, core| {
            nodes[core] = record(tag);
            @memcpy(nodes[core].flags[0..CoreCounts[core]], source.flags[core][0..CoreCounts[core]]);
        }
        const used = if (with_aggregates) nodes[0..3] else nodes[0..0];
        const before = std.mem.asBytes(box).*;
        try publish_features2(&box.output, &chain, used, &[_]u32{0} ** CoreFlags, &source);
        inline for (Promoted, 0..) |entry, index| {
            const offset = @offsetOf(combined_box_t, "promoted") + (MaxPromoted - 1 - index) * @sizeOf(promoted_storage_t);
            const output: *const entry.native_t = @ptrCast(@alignCast(&box.promoted[MaxPromoted - 1 - index].value));
            if (api >= CoreVersions[entry.core]) {
                inline for (@typeInfo(entry.native_t).Struct.fields[2..]) |field| try std.testing.expectEqual(@as(u32, 1), @field(output.*, field.name));
                try std.testing.expectEqualSlices(u8, before[offset..][0..16], std.mem.asBytes(box)[offset..][0..16]);
                try std.testing.expectEqualSlices(u8, before[offset + @sizeOf(entry.native_t) ..][0 .. PromotedFeatureBytes - @sizeOf(entry.native_t)], std.mem.asBytes(box)[offset + @sizeOf(entry.native_t) ..][0 .. PromotedFeatureBytes - @sizeOf(entry.native_t)]);
            } else {
                try std.testing.expectEqualSlices(u8, before[offset..][0..PromotedFeatureBytes], std.mem.asBytes(box)[offset..][0..PromotedFeatureBytes]);
            }
        }
        if (with_aggregates) {
            inline for (.{ "vulkan11", "vulkan12", "vulkan13" }, 0..) |name, core| {
                inline for (@typeInfo(CoreTypes[core]).Struct.fields[2..]) |field| try std.testing.expectEqual(@as(u32, 1), @field(@field(box, name), field.name));
            }
        }
    };
}
fn promoted_reject(box: *combined_box_t, chain: *const chain_t, nodes: []const node_t, source: *const promoted_source_t) !void {
    const before = std.mem.asBytes(box).*;
    const core = [_]u32{0} ** CoreFlags;
    try std.testing.expectError(error.Invalid, @call(.never_inline, validate_features2, .{ &box.output, chain, nodes, &core, source }));
    try std.testing.expectError(error.Invalid, @call(.never_inline, publish_features2, .{ &box.output, chain, nodes, &core, source }));
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(box));
}
test "Features2 promoted duplicates sources aggregates ranges and aliases reject every byte unchanged" {
    const box = try std.testing.allocator.create(combined_box_t);
    defer std.testing.allocator.destroy(box);
    combined_init(box, true);
    var chain = try collect_chain(box.output.pNext);
    var nodes = [_]node_t{ record(CoreTags[0]), record(CoreTags[1]), record(CoreTags[2]) };
    var source = promoted_source_t{ .api_version = PromotedApis[3] };
    const core = [_]u32{0} ** CoreFlags;
    try validate_features2(&box.output, &chain, &nodes, &core, &source);
    // Source words: non-Boolean, nonzero suffix, and aggregate nodes that disagree.
    for (CoreCounts, 0..) |count, level| {
        source.flags[level][count - 1] = 2;
        try promoted_reject(box, &chain, &nodes, &source);
        source.flags[level][count - 1] = 0;
        if (count < wire.MaxNodeFlags) {
            source.flags[level][count] = 1;
            try promoted_reject(box, &chain, &nodes, &source);
            source.flags[level][count] = 0;
        }
        nodes[level].flags[0] = 1;
        try promoted_reject(box, &chain, &nodes, &source);
        nodes[level].flags[0] = 0;
    }
    // Snapshot ceiling, forged tag, null, misaligned, re-tagged and overflowing promoted targets.
    var forged = chain;
    forged.promoted_count = MaxPromoted + 1;
    try promoted_reject(box, &forged, &nodes, &source);
    forged = chain;
    forged.promoted_tags[3] = 0x7ffffffe;
    try promoted_reject(box, &forged, &nodes, &source);
    forged = chain;
    forged.promoted_addresses[3] = null;
    try promoted_reject(box, &forged, &nodes, &source);
    forged = chain;
    forged.promoted_addresses[3] = malformed_target(@intFromPtr(chain.promoted_addresses[3].?) + 4);
    try promoted_reject(box, &forged, &nodes, &source);
    forged = chain;
    forged.promoted_tags[3] = chain.promoted_tags[4];
    try promoted_reject(box, &forged, &nodes, &source);
    // Overlap with the outer object, a known target and another promoted target.
    for ([_]*c.VkBaseOutStructure{ @ptrCast(@alignCast(&box.output.features)), @ptrCast(@alignCast(&box.vulkan12.storagePushConstant8)), @ptrCast(@alignCast(&box.promoted[1].value[8])) }) |alias| {
        const saved = alias.*;
        alias.sType = chain.promoted_tags[0];
        forged = chain;
        forged.promoted_addresses[0] = alias;
        try promoted_reject(box, &forged, &nodes, &source);
        alias.* = saved;
    }
    // Duplicate promoted tag in the live chain.
    const second: *c.VkBaseOutStructure = @ptrCast(@alignCast(&box.promoted[1].value));
    second.sType = comptime Promoted[0].tag;
    try std.testing.expectError(error.Invalid, collect_chain(box.output.pNext));
    second.sType = comptime Promoted[MaxPromoted - 2].tag;
    // Overflowing promoted extent: an aligned address within the last promoted size of the space,
    // rejected before its header is read.
    forged = chain;
    forged.promoted_addresses[0] = malformed_target(std.math.maxInt(usize) - 7);
    try promoted_reject(box, &forged, &nodes, &source);
}
test "Features2 promoted collector counts toward the walk quota and keeps forward order" {
    var headers: [MaxWalkNodes]promoted_storage_t = undefined;
    for (&headers, 0..) |*storage, index| {
        const header: *c.VkBaseOutStructure = @ptrCast(@alignCast(&storage.value));
        header.sType = if (index < MaxPromoted) PromotedTags[index] else 0x7fffffff;
        header.pNext = if (index + 1 < MaxWalkNodes) @ptrCast(@alignCast(&headers[index + 1].value)) else null;
    }
    const chain = try collect_chain(@ptrCast(&headers[0].value));
    try std.testing.expectEqual(@as(usize, MaxPromoted), chain.promoted_count);
    try std.testing.expectEqualSlices(u32, &PromotedTags, chain.promoted_tags[0..MaxPromoted]);
    const last: *c.VkBaseOutStructure = @ptrCast(@alignCast(&headers[MaxWalkNodes - 1].value));
    last.pNext = @ptrFromInt(8);
    try std.testing.expectError(error.Invalid, collect_chain(@ptrCast(&headers[0].value)));
    for (PromotedCores, promoted_cores_expected()) |actual, expected| try std.testing.expectEqual(expected, actual);
}
fn promoted_cores_expected() [MaxPromoted]u8 {
    return .{ 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
}
