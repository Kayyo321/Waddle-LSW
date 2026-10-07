//! Experimental bounded Vulkan dispatch; full device API/DXVK support is separately gated.
const std = @import("std");
const device_native = @import("venus_device_native.zig");
const device_wire = @import("venus_device_wire.zig");
const features_native = @import("venus_features_native.zig");
const features_wire = @import("venus_features_wire.zig");
const properties_native = @import("venus_properties_native.zig");
const properties_wire = @import("venus_properties_wire.zig");
const extensions_wire = @import("venus_extensions_wire.zig");
const wsi = @import("venus_wsi.zig");
const extra_wire = @import("venus_extra_objects_wire.zig");
const image_view_native = @import("venus_image_view_native.zig");
const template_native = @import("venus_descriptor_template_native.zig");
const pipeline_helpers = @import("venus_pipeline_wire_helpers.zig");
const mixed_wire = @import("venus_sampler_descriptor_wire.zig");
const dynamic_graphics = @import("venus_graphics_dynamic_wire.zig");
const image_transfer = @import("venus_image_transfer_wire.zig");
const image_geometry = @import("venus_image_transfer_native.zig");
const transfer2 = @import("venus_transfer2_native.zig");
const sampler_descriptors = @import("venus_sampler_descriptor_wire.zig");
const shader_wire = @import("venus_shader_wire.zig");
const general_graphics = @import("venus_graphics_general_wire.zig");
const dynamic_rendering = @import("venus_dynamic_rendering_wire.zig");
const modern_sync = @import("venus_modern_sync_wire.zig");
const requirements2_wire = @import("venus_requirements2_wire.zig");
var wsi_state = wsi.state_t{};
const descriptor_wire = @import("venus_descriptor_wire.zig");
const profiles = @import("venus_icd_profiles.zig");
const compute_state = @import("venus_compute_state.zig");
const compute_wire = @import("venus_compute_wire.zig");
var command_registry = compute_state.registry_t{};
var profile_registry = profiles.registry_t{};
// Mutex-owned230400-byte batch staging; no native pointers, scrubbed after every call and abandon.
var descriptor_allocation_snapshots = [_]profiles.descriptor_set_t{.{}} ** 64;
// Mutex-owned460800/98304-byte transactional staging, scrubbed on every return/abandon.
var descriptor_update_snapshots = [_]profiles.descriptor_set_t{.{}} ** 128;
var descriptor_wire_buffers: [64][128]descriptor_wire.buffer_info_t = std.mem.zeroes([64][128]descriptor_wire.buffer_info_t);
var descriptor_wire_images: [64][128]sampler_descriptors.image_info_t = std.mem.zeroes([64][128]sampler_descriptors.image_info_t);
var descriptor_wire_texels: [64][128]u64 = std.mem.zeroes([64][128]u64);
const render_wire = @import("venus_render_wire.zig");
const graphics_wire = @import("venus_graphics_wire.zig");
const graphics_state = @import("venus_graphics_state.zig");
var graphics_recordings = [_]graphics_state.recording_t{.{}} ** 64;
const graphics_pipeline_wire = @import("venus_graphics_pipeline_wire.zig");
const graphics_command_wire = @import("venus_graphics_command_wire.zig");
const builtin = @import("builtin");
const MappingAllocator = if (builtin.is_test) std.testing.allocator else std.heap.c_allocator;
const MaxMappedBytes: u64 = 268435456;
const MappingChunkBytes: usize = 4096;
// Trusted timed frontend requires negotiated TCP v2 resource-read profile.
// Legacy callback, writes, and command replies retain their4KiB ceilings.
const MappingReadChunkBytes: usize = 65536;
var mapping_slots = [_]bool{false} ** 64;
const c = @cImport({
    @cInclude("waddle/venus_icd.h");
    @cInclude("waddle/venus_objects.h");
    @cInclude("waddle/venus_query_wire.h");
});
const MaxInstances: usize = 16;
const MaxDevices: usize = 16;
const physical_feature_cache_t = struct {
    actual_api_version: u32 = 0,
    actual_api_ready: bool = false,
    raw_features_ready: bool = false,
    raw: features_wire.result_t = .{},
};
const FeatureTags = [_]u32{
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
    c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
};
const FeatureCounts = [_]u8{ 12, 47, 15, 1, 1, 2, 3, 1 };
fn boolean_mask(comptime native_t: type, comptime first: []const u8, comptime count: usize, comptime names: anytype) [count]u32 {
    var result = [_]u32{0} ** count;
    const start = @offsetOf(native_t, first);
    inline for (names) |name| {
        if (@sizeOf(@TypeOf(@field(@as(native_t, undefined), name))) != 4) @compileError("Feature field must be VkBool32");
        const offset = @offsetOf(native_t, name);
        if (offset < start or (offset - start) % 4 != 0 or (offset - start) / 4 >= count) @compileError("Feature field outside canonical Boolean extent");
        result[(offset - start) / 4] = 1;
    }
    return result;
}
/// Minimal required core feature policy. Geometry/dual blend/multiviewports/indexed draws
/// use native generalized pipeline/commands and validated image/query paths.
/// These policy bits require actual backend support before becoming public.
const CoreFeatureAllowlist = boolean_mask(c.VkPhysicalDeviceFeatures, "robustBufferAccess", 55, .{
    "depthBiasClamp",            "depthClamp",         "dualSrcBlend",                           "fillModeNonSolid",     "fullDrawIndexUint32",
    "geometryShader", "tessellationShader", "fragmentStoresAndAtomics", "drawIndirectFirstInstance", "imageCubeArray",     "independentBlend",                       "multiDrawIndirect",    "multiViewport",
    "occlusionQueryPrecise",     "robustBufferAccess", "sampleRateShading",                      "shaderClipDistance",   "shaderCullDistance",
    "shaderImageGatherExtended", "shaderInt64",        "shaderSampledImageArrayDynamicIndexing", "textureCompressionBC",
});
/// Implemented canonical node policy in exact FeatureTags order, unused suffix zero.
/// Required shader capabilities execute natively; BDA/timeline/query/maintenance/sync2/dynamic
/// rendering have concrete transport commands. Descriptor updates refresh command and
/// pending ticket references while retaining previous GPU owners until completion.
/// Transform feedback remains entirely disabled and its extension unadvertised.
const NodeFeatureAllowlists = [8][47]u32{
    boolean_mask(c.VkPhysicalDeviceVulkan11Features, "storageBuffer16BitAccess", 47, .{"shaderDrawParameters"}),
    boolean_mask(c.VkPhysicalDeviceVulkan12Features, "samplerMirrorClampToEdge", 47, .{
        "bufferDeviceAddress",                       "descriptorIndexing",              "descriptorBindingSampledImageUpdateAfterBind",
        "descriptorBindingUpdateUnusedWhilePending", "descriptorBindingPartiallyBound", "hostQueryReset",
        "runtimeDescriptorArray",                    "samplerMirrorClampToEdge",        "timelineSemaphore",
        "uniformBufferStandardLayout",               "vulkanMemoryModel",
    }),
    boolean_mask(c.VkPhysicalDeviceVulkan13Features, "robustImageAccess", 47, .{
        "dynamicRendering",                    "maintenance4",     "shaderDemoteToHelperInvocation",
        "shaderZeroInitializeWorkgroupMemory", "synchronization2",
    }),
    boolean_mask(c.VkPhysicalDeviceShaderDrawParametersFeatures, "shaderDrawParameters", 47, .{"shaderDrawParameters"}),
    boolean_mask(c.VkPhysicalDeviceHostQueryResetFeatures, "hostQueryReset", 47, .{"hostQueryReset"}),
    [_]u32{0} ** 47,
    boolean_mask(c.VkPhysicalDeviceRobustness2FeaturesEXT, "robustBufferAccess2", 47, .{ "robustBufferAccess2", "nullDescriptor" }),
    boolean_mask(c.VkPhysicalDeviceMaintenance5FeaturesKHR, "maintenance5", 47, .{"maintenance5"}),
};
// Each raw extension array is owned by one physical namespace until acknowledged parent retirement.
const extension_cache_t = struct {
    handle: u64 = 0,
    records: ?[]extensions_wire.extension_t = null,
    ready: bool = false,
};
var extension_caches = [_]extension_cache_t{.{}} ** (MaxInstances * MaxDevices);
var extension_backend_result: i32 = c.VK_SUCCESS;
fn retire_extension_cache(handle: u64) void {
    for (&extension_caches) |*entry| if (entry.handle == handle or handle == 0) {
        if (entry.records) |records| MappingAllocator.free(records);
        entry.* = .{};
    };
}
const InstanceProperties2: u32 = 1;
const InstanceSurface: u32 = 2;
const InstanceWin32Surface: u32 = 4;
const instance_advertisement_t = struct { handle: u64 = 0, api: u32 = c.VK_API_VERSION_1_0, mask: u32 = 0 };
var instance_advertisements = [_]instance_advertisement_t{.{}} ** MaxInstances;
const InstanceExtensionNames = [_][]const u8{ "VK_KHR_get_physical_device_properties2", "VK_KHR_surface", "VK_KHR_win32_surface" };
const InstanceExtensionVersions = [_]u32{ c.VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_SPEC_VERSION, c.VK_KHR_SURFACE_SPEC_VERSION, 6 };
fn instance_extension_supported(index: usize) bool {
    if (!negotiated_capabilities_ready) return false;
    return index == 0 or builtin.os.tag == .windows or builtin.is_test;
}
fn admit_instance_extensions(info: *const c.VkInstanceCreateInfo) !u32 {
    if (info.enabledExtensionCount > InstanceExtensionNames.len or
        (info.enabledExtensionCount != 0 and info.ppEnabledExtensionNames == null)) return error.Invalid;
    if (info.enabledExtensionCount != 0 and @intFromPtr(info.ppEnabledExtensionNames) % @alignOf([*c]const u8) != 0) return error.Invalid;
    var mask: u32 = 0;
    if (info.enabledExtensionCount != 0) for (info.ppEnabledExtensionNames[0..info.enabledExtensionCount]) |pointer| {
        const name = bounded_name(pointer) orelse return error.Invalid;
        var found = false;
        for (InstanceExtensionNames, 0..) |known, index| {
            if (!std.mem.eql(u8, name, known)) continue;
            if (!instance_extension_supported(index)) return error.Extension;
            const bit = @as(u32, 1) << @as(u5, @intCast(index));
            if (mask & bit != 0) return error.Invalid;
            mask |= bit;
            found = true;
            break;
        }
        if (!found) return error.Extension;
    };
    if (mask & InstanceWin32Surface != 0 and mask & InstanceSurface == 0) return error.Extension;
    return mask;
}
fn instance_proc_allowed(handle: u64, name: []const u8) bool {
    var enabled = instance_advertisement_t{};
    for (instance_advertisements) |entry| if (entry.handle == handle) { enabled = entry; break; };
    if (std.mem.eql(u8, name, "vkGetPhysicalDeviceFeatures2KHR") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceProperties2KHR") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceFormatProperties2KHR") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceImageFormatProperties2KHR") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceQueueFamilyProperties2KHR") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceMemoryProperties2KHR") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR"))
        return enabled.mask & InstanceProperties2 != 0;
    if (std.mem.eql(u8, name, "vkGetPhysicalDeviceFeatures2") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceProperties2") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceFormatProperties2") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceImageFormatProperties2") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceQueueFamilyProperties2") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceMemoryProperties2") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceSparseImageFormatProperties2"))
        return enabled.api >= c.VK_API_VERSION_1_1;
    if (std.mem.eql(u8, name, "vkCreateWin32SurfaceKHR") or std.mem.eql(u8, name, "vkGetPhysicalDeviceWin32PresentationSupportKHR"))
        return enabled.mask & InstanceWin32Surface != 0;
    if (std.mem.eql(u8, name, "vkDestroySurfaceKHR") or std.mem.eql(u8, name, "vkGetPhysicalDeviceSurfaceSupportKHR") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") or std.mem.eql(u8, name, "vkGetPhysicalDeviceSurfaceFormatsKHR") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceSurfacePresentModesKHR")) return enabled.mask & InstanceSurface != 0;
    return true;
}
const instance_cache_t = struct {
    handle: u64 = 0,
    ready: bool = false,
    count: u32 = 0,
    physical: [MaxDevices]u64 = [_]u64{0} ** MaxDevices,
    feature_caches: [MaxDevices]physical_feature_cache_t = [_]physical_feature_cache_t{.{}} ** MaxDevices,
};
// Immutable mutex-owned optional enablement, live only after exact device-create ACK.
// Canonical owned values; no caller pointer/raw support state. Whole-cache retirement scrubs it.
const device_enabled_state_t = struct {
    features: features_wire.result_t = .{},
    extension_mask: u32 = 0,
};
const device_cache_t = struct {
    enabled_state: device_enabled_state_t = .{},
    handle: u64 = 0,
    graphics_queue_ready: bool = false,
    graphics_queue_count: u32 = 0,
    graphics_queue_flags: [64]u32 = [_]u32{0} ** 64,
    descriptor_limits_ready: bool = false,
    descriptor_alignments: [2]u64 = [_]u64{0} ** 2,
    descriptor_ranges: [2]u32 = [_]u32{0} ** 2,
    compute_group_limits: [3]u32 = [_]u32{0} ** 3,
    // Validated actual host push ceiling; zero until the complete raw properties cache is ready.
    max_push_bytes: u32 = 0,
    family_count: usize = 0,
    families: [16]u32 = [_]u32{0} ** 16,
    counts: [16]u32 = [_]u32{0} ** 16,
    queues: [64]u64 = [_]u64{0} ** 64,
    rings: [64]u32 = [_]u32{0} ** 64,
    ready: [64]bool = [_]bool{false} ** 64,
};
comptime {
    if (@sizeOf(device_enabled_state_t) != 1796 or @alignOf(device_enabled_state_t) != 4 or
        @sizeOf(device_cache_t) != 3080 or @alignOf(device_cache_t) != 8 or
        @offsetOf(device_cache_t, "enabled_state") != 544 or @sizeOf(@TypeOf(device_caches)) != 49280)
        @compileError("Immutable device enablement cache ABI changed");
}
const command_state_t = enum { Initial, Recording, Executable, Invalid, Pending };
const mapping_span_t = struct { start: u64 = 0, end: u64 = 0 };
const resource_state_t = struct {
    id: u64 = 0,
    profile_index: u8 = 0,
    pipeline_bind_point: u32 = 0,
    command_profile_index: u8 = 0,
    descriptor_uses: [8]u64 = [_]u64{0} ** 8,
    index_buffer: u64 = 0,
    index_offset: u64 = 0,
    index_size: u64 = 0,
    index_type: u32 = 0,
    descriptor_max_sets: u32 = 0,
    descriptor_live_sets: u32 = 0,
    descriptor_capacity: [11]u32 = [_]u32{0} ** 11,
    descriptor_used: [11]u32 = [_]u32{0} ** 11,
    inflight_count: u32 = 0,
    idle_refs: u32 = 0,
    allocation_size: u64 = 0,
    allocation_flags: u32 = 0,
    type_index: u32 = 0,
    mapping_resource: u32 = 0,
    mapped_bytes: ?[]align(4096) u8 = null,
    mapped_baseline: ?[]u8 = null,
    mapped_offset: u64 = 0,
    mapped_size: u64 = 0,
    gpu_spans: [16]mapping_span_t = [_]mapping_span_t{.{}} ** 16,
    gpu_span_count: usize = 0,
    address_exposed: bool = false,
    bound_memory: u64 = 0,
    memory_offset: u64 = 0,
    buffer_size: u64 = 0,
    buffer_usage: u32 = 0,
    render_format: u32 = 0,
    render_initial_layout: u32 = 0,
    render_final_layout: u32 = 0,
    framebuffer_view: u64 = 0,
    framebuffer_extent: [2]u32 = .{ 0, 0 },
    image_extent: [3]u32 = .{ 0, 0, 0 },
    image_samples: u32 = 0,
    view_type: u32 = 0,
    view_range: c.VkImageSubresourceRange = std.mem.zeroes(c.VkImageSubresourceRange),
    view_components: c.VkComponentMapping = std.mem.zeroes(c.VkComponentMapping),
    image_levels: u32 = 0,
    image_layers: u32 = 0,
    image_format: u32 = 0,
    image_type: u32 = 0,
    image_usage: u32 = 0,
    image_tiling: u32 = 0,
    image_view_metadata: image_view_native.image_t = .{},
    view_image: u64 = 0,
    buffer_references: [8]u64 = [_]u64{0} ** 8,
    queue_family: u32 = 0,
    pool_family: u32 = 0,
    pool_flags: u32 = 0,
    command_state: command_state_t = .Initial,
    command_level: u32 = 0,
    command_flags: u32 = 0,
    requirements: c.VkMemoryRequirements = std.mem.zeroes(c.VkMemoryRequirements),
};
var resource_states = [_]resource_state_t{.{}} ** 512;
const submission_ticket_t = struct {
    queue: u64 = 0,
    sequence: u64 = 0,
    fence: u64 = 0,
    references: [8]u64 = [_]u64{0} ** 8,
};
var submission_tickets = [_]submission_ticket_t{.{}} ** 128;
var submission_sequence: u64 = 0;
fn include_reference(ticket: *submission_ticket_t, record: *const c.venus_object_t) bool {
    const index = resource_index(record);
    const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
    const repeated = ticket.references[index / 64] & bit != 0;
    ticket.references[index / 64] |= bit;
    return repeated;
}
fn retire_ticket(ticket: *submission_ticket_t) void {
    for (&resource_states, 0..) |*state, index| {
        const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
        if (ticket.references[index / 64] & bit == 0) continue;
        std.debug.assert(slots[index].id != 0 and state.inflight_count != 0);
        state.inflight_count -= 1;
        if (slots[index].kind == c.VK_OBJECT_TYPE_COMMAND_BUFFER and state.inflight_count == 0)
            state.command_state = if (state.command_flags & 1 != 0) .Invalid else .Executable;
    }
    ticket.* = .{};
}
fn retire_queue(handle: u64) void {
    for (&submission_tickets) |*ticket| if (ticket.queue == handle) retire_ticket(ticket);
}
fn retire_fence(handle: u64) void {
    var queue: u64 = 0;
    var sequence: u64 = 0;
    for (submission_tickets) |ticket| if (ticket.queue != 0 and ticket.fence == handle) {
        queue = ticket.queue;
        sequence = ticket.sequence;
        break;
    };
    if (queue == 0) return;
    for (&submission_tickets) |*ticket|
        if (ticket.queue == queue and ticket.sequence <= sequence) retire_ticket(ticket);
}
fn resource_index(record: [*c]const c.venus_object_t) usize {
    for (slots, 0..) |slot, index| if (slot.id == record.*.id) return index;
    unreachable;
}
fn resource_state(record: [*c]const c.venus_object_t) *resource_state_t {
    return &resource_states[resource_index(record)];
}
const QueueTimelineTag: u32 = 1000384005;
var ring_slots = [_]bool{false} ** 64;
var device_caches = [_]device_cache_t{.{}} ** 16;
var mutex = std.Thread.Mutex{};
// Internal synchronous composition may nest Vulkan operations on one thread. The actual
// process mutex stays held until the outermost operation retires; callbacks cannot reenter.
threadlocal var lock_depth: usize = 0;
fn lock_icd() void {
    if (lock_depth == 0) mutex.lock();
    lock_depth += 1;
}
fn unlock_icd() void {
    std.debug.assert(lock_depth != 0);
    lock_depth -= 1;
    if (lock_depth == 0) mutex.unlock();
}
var namespace_id: u32 = 1;
var command = std.mem.zeroes(c.venus_command_t);
var objects = std.mem.zeroes(c.venus_objects_t);
var slots: [512]c.venus_object_t = undefined;
var caches = [_]instance_cache_t{.{}} ** MaxInstances;
const MaxUpdateBytes: usize = 65536;
const MaxUpdateWireBytes: usize = @max(48 + MaxUpdateBytes, shader_wire.MaxPacketBytes);
const CommandPrefixBytes: usize = 36;
var update_encoded: [MaxUpdateWireBytes]u8 = undefined;
var tx: [CommandPrefixBytes + MaxUpdateWireBytes]u8 = undefined;
var rx: [extensions_wire.MaxReplyBytes]u8 = undefined;
const TimedReplyBytes: usize = 524288;
const TransactionMs: u64 = 5000;
var timed_exchange: c.venus_icd_exchange_until_t = null;
var timed_clock: c.venus_icd_clock_t = null;
var transaction_deadline: u64 = 0;
var reply_profile_ready: bool = false;
var lost: c_int = c.RingOk;
var negotiated_capabilities = std.mem.zeroes(c.venus_capabilities_t);
var negotiated_capabilities_ready: bool = false;
const exchange_t = *const fn (
    ?*anyopaque,
    [*c]const c.venus_request_t,
    ?*const anyopaque,
    usize,
    [*c]c.venus_request_t,
    ?*anyopaque,
    usize,
) callconv(.C) c_int;
fn clear() void {
    wsi_state = .{};
    retire_extension_cache(0);
    for (&resource_states) |*state| release_shadow(state);
    mapping_slots = [_]bool{false} ** 64;
    c.venus_command_free(&command);
    c.venus_objects_free(&objects);
    caches = [_]instance_cache_t{.{}} ** MaxInstances;
    instance_advertisements = [_]instance_advertisement_t{.{}} ** MaxInstances;
    device_caches = [_]device_cache_t{.{}} ** 16;
    template_owners = [_]template_owner_t{.{}} ** 32;
    ring_slots = [_]bool{false} ** 64;
    gpu_fences = [_]u64{0} ** 64;
    profile_registry = .{};
    command_registry = .{};
    graphics_recordings = [_]graphics_state.recording_t{.{}} ** 64;
    @memset(&descriptor_allocation_snapshots, .{});
    @memset(&descriptor_update_snapshots, .{});
    @memset(std.mem.asBytes(&descriptor_wire_buffers), 0);
    resource_states = [_]resource_state_t{.{}} ** 512;
    submission_tickets = [_]submission_ticket_t{.{}} ** 128;
    submission_sequence = 0;
    @memset(&update_encoded, 0);
    lost = c.RingOk;
    negotiated_capabilities = std.mem.zeroes(c.venus_capabilities_t);
    negotiated_capabilities_ready = false;
    timed_exchange = null;
    timed_clock = null;
    transaction_deadline = 0;
    reply_profile_ready = false;
}
/// Borrow one exclusive negotiated backend; public header defines ownership/deadlines/threads.
export fn venus_icd_bind(exchange: ?exchange_t, context: ?*anyopaque) c_int {
    lock_icd();
    defer unlock_icd();
    const status = bind_locked(exchange, context);
    if (status != c.RingOk) return status;
    negotiated_capabilities = std.mem.zeroes(c.venus_capabilities_t);
    negotiated_capabilities_ready = false;
    return c.RingOk;
}
/// Copy the actual negotiated profile; public header defines borrowed owners and failure preservation.
export fn venus_icd_bind_capabilities(exchange: ?exchange_t, context: ?*anyopaque, capabilities: [*c]const c.venus_capabilities_t) c_int {
    lock_icd();
    defer unlock_icd();
    if (capabilities == null or c.venus_capabilities_compatible(capabilities) != c.RingOk) return c.RingInvalid;
    const status = bind_locked(exchange, context);
    if (status != c.RingOk) return status;
    negotiated_capabilities = capabilities.*;
    negotiated_capabilities_ready = true;
    return c.RingOk;
}
// Caller holds mutex; validated fixed buffers make object/command initialization infallible.
fn bind_locked(exchange: ?exchange_t, context: ?*anyopaque) c_int {
    if (exchange == null or context == null or command.exchange != null) return c.RingInvalid;
    if (namespace_id == std.math.maxInt(u32)) return c.RingLimit;
    const status = c.venus_objects_init(&objects, &slots, slots.len, namespace_id, context);
    std.debug.assert(status == c.RingOk);
    wsi_state.next_id = (@as(u64, namespace_id) << 32) | 1;
    namespace_id += 1;
    const initialized = c.venus_command_init(&command, exchange, context, &tx, tx.len, &rx, 4096);
    std.debug.assert(initialized == c.RingOk);
    return c.RingOk;
}
/// Bind an absolute-deadline frontend after verifying the trusted actual reply allocation.
/// Header defines borrowed callback lifetime and caller-owned receiver retirement on failure.
export fn venus_icd_bind_timed(exchange_until: c.venus_icd_exchange_until_t, clock_ms: c.venus_icd_clock_t,
    context: ?*anyopaque, capabilities: [*c]const c.venus_capabilities_t, reply_bytes: u32) c_int {
    lock_icd();
    defer unlock_icd();
    if (exchange_until == null or clock_ms == null or context == null or capabilities == null or
        command.exchange != null or reply_bytes != TimedReplyBytes or
        c.venus_capabilities_compatible(capabilities) != c.RingOk) return c.RingInvalid;
    if (namespace_id == std.math.maxInt(u32)) return c.RingLimit;
    const now = clock_ms.?(context);
    if (now == 0) return c.RingClosed;
    const deadline = std.math.add(u64, now, TransactionMs) catch return c.RingLimit;
    var byte: u8 = 0;
    for ([_]usize{ TimedReplyBytes - 1, TimedReplyBytes }, 0..) |offset, index| {
        const before = clock_ms.?(context);
        if (before == 0) return c.RingClosed;
        if (before >= deadline) return c.RingTimeout;
        var offered = std.mem.zeroes(c.venus_request_t);
        offered.kind = c.RequestReply;
        offered.argument_zero = offset;
        offered.argument_one = 1;
        var response = std.mem.zeroes(c.venus_request_t);
        const status = exchange_until.?(context, &offered, null, 0, &response, &byte, 1, deadline);
        const after = clock_ms.?(context);
        if (after == 0) return c.RingClosed;
        if (after < before) return c.RingCorrupt;
        if (after >= deadline) return c.RingTimeout;
        if (index == 0) {
            if (status != c.RingOk) return status;
            if (response.kind != c.RequestReply or response.direction != 1 or response.status != c.RequestSuccess or
                response.payload_bytes != 1 or response.argument_zero != 0 or response.argument_one != 0 or
                response.resource_id != 0 or response.flags != 0) return c.RingCorrupt;
        } else if (status != c.RingInvalid or response.kind != c.RequestReply or response.direction != 1 or
            response.status != c.RequestInvalid or response.payload_bytes != 0 or response.argument_zero != 0 or
            response.argument_one != 0 or response.resource_id != 0 or response.flags != 0) return c.RingCorrupt;
    }
    const status = bind_locked(timed_adapter, context);
    if (status != c.RingOk) return status;
    timed_exchange = exchange_until;
    timed_clock = clock_ms;
    reply_profile_ready = true;
    negotiated_capabilities = capabilities.*;
    negotiated_capabilities_ready = true;
    return c.RingOk;
}
// Command calls retain one absolute deadline, including callbacks and every bounded read.
fn timed_adapter(context: ?*anyopaque, offered: [*c]const c.venus_request_t, input: ?*const anyopaque,
    length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
    const now = timed_clock.?(context);
    if (now == 0) return c.RingClosed;
    const deadline = if (transaction_deadline != 0) transaction_deadline else
        std.math.add(u64, now, TransactionMs) catch return c.RingLimit;
    if (now >= deadline) return c.RingTimeout;
    const status = timed_exchange.?(context, offered, input, length, response, output, capacity, deadline);
    const after = timed_clock.?(context);
    if (after == 0) return c.RingClosed;
    if (after < now) return c.RingCorrupt;
    if (after >= deadline) return c.RingTimeout;
    return status;
}
/// Release an empty binding; header specifies no frontend/resource teardown.
export fn venus_icd_unbind() c_int {
    lock_icd();
    defer unlock_icd();
    if (objects.live_count != 0 or command.state == c.CommandSubmitted or
        command.state == c.CommandReading or command.state == c.CommandReady) return c.RingAgain;
    clear();
    return c.RingOk;
}
/// Reset only after the caller retires the old receiver; header defines cancellation boundary.
export fn venus_icd_abandon() void {
    lock_icd();
    defer unlock_icd();
    clear();
}
/// External loader negotiation; accepts2..5, header defines preserved failure output.
export fn venus_icd_negotiate_loader(version: ?*u32) c_int {
    const value = version orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (value.* < 2) return c.VK_ERROR_INITIALIZATION_FAILED;
    value.* = @min(value.*, 5);
    return c.VK_SUCCESS;
}
fn object(handle: u64, kind: u32) ?*c.venus_object_t {
    var found: [*c]c.venus_object_t = null;
    if (c.venus_objects_lookup(&objects, handle, kind, 1, &found) != c.RingOk) return null;
    return @ptrCast(found);
}
fn cache(handle: u64) ?*instance_cache_t {
    if (object(handle, c.VK_OBJECT_TYPE_INSTANCE) == null) return null;
    for (&caches) |*entry| if (entry.handle == handle) return entry;
    return null;
}
fn failure(status: c_int) c_int {
    lost = status;
    return c.VK_ERROR_DEVICE_LOST;
}
fn transact(bytes: []const u8) ?[]const u8 {
    return transact_sized(bytes, 4096);
}
fn transact_sized(bytes: []const u8, reply_bytes: usize) ?[]const u8 {
    if (command.exchange == null or lost != c.RingOk) return null;
    if (reply_bytes > 4096 and !reply_profile_ready) return null;
    if (reply_bytes < 4 or reply_bytes > rx.len or reply_bytes % 4 != 0) return null;
    command.rx_bytes = reply_bytes;
    if (timed_clock) |clock| {
        const now = clock(command.context);
        if (now == 0) { _ = failure(c.RingClosed); return null; }
        transaction_deadline = std.math.add(u64, now, TransactionMs) catch { _ = failure(c.RingLimit); return null; };
    }
    defer transaction_deadline = 0;
    const start = c.venus_command_start(&command, bytes.ptr, bytes.len);
    if (start != c.RingOk) {
        _ = failure(start);
        return null;
    }
    for (0..@as(usize, if (timed_clock != null) 5000 else 1000)) |_| {
        if (timed_clock) |clock| {
            const now = clock(command.context);
            if (now == 0 or now >= transaction_deadline) { _ = failure(if (now == 0) c.RingClosed else c.RingTimeout); return null; }
        }
        const status = c.venus_command_poll(&command);
        if (status == c.RingOk) {
            var view: ?*const anyopaque = null;
            var length: usize = 0;
            const taken = c.venus_command_take(&command, &view, &length);
            std.debug.assert(taken == c.RingOk);
            return @as([*]const u8, @ptrCast(view.?))[0..length];
        }
        if (status != c.RingAgain) {
            _ = failure(status);
            return null;
        }
        std.time.sleep(std.time.ns_per_ms);
    }
    _ = failure(c.RingTimeout);
    return null;
}
/// Core instance creation; native inputs borrowed for call, output handle NULL on failure.
/// Mutex serialized; no allocations; reserved host identity published only after exact reply.
fn create_instance(
    info: [*c]const c.VkInstanceCreateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkInstance,
) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (info == null or command.exchange == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (info.*.pApplicationInfo != null) {
        const version = info.*.pApplicationInfo.*.apiVersion;
        if (version != 0 and (version >> 29 != 0 or
            ((version >> 22) & 0x7f) != 1 or version > (if (reply_profile_ready) ImplementedApiVersion | 0xfff else c.VK_API_VERSION_1_0 | 0xfff)))
            return c.VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    const extension_mask = admit_instance_extensions(@ptrCast(info)) catch |err| return
        if (err == error.Extension) c.VK_ERROR_EXTENSION_NOT_PRESENT else c.VK_ERROR_INITIALIZATION_FAILED;
    var available: ?*instance_cache_t = null;
    for (&caches) |*entry| if (entry.handle == 0) {
        available = entry;
        break;
    };
    const entry = available orelse return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var instance: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_INSTANCE, 0, 1, &instance) != c.RingOk)
        return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    // The loader supplies private link/callback records in the instance chain.
    // Strip only its known ABI tag; application extension chains remain unsupported.
    var native_info = info.*;
    var next = native_info.pNext;
    var links: usize = 0;
    while (next != null) {
        const link: *const c.VkBaseInStructure = @ptrCast(@alignCast(next.?));
        if (links == 32 or link.sType != c.VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            _ = c.venus_objects_release(&objects, instance.*.handle, c.VK_OBJECT_TYPE_INSTANCE, 1);
            return c.VK_ERROR_INITIALIZATION_FAILED;
        }
        links += 1;
        next = link.pNext;
    }
    native_info.pNext = null;
    // Pinned receiver rejects all application instance names and creates an actual >=1.1
    // host instance (vkr_instance.c). Query/Win32 names describe guest behavior only.
    native_info.enabledExtensionCount = 0;
    native_info.ppEnabledExtensionNames = null;
    var encoded: [4096]u8 = undefined;
    var written: usize = 0;
    const status = c.venus_instance_wire_create(
        &native_info,
        instance.*.id,
        &encoded,
        encoded.len,
        &written,
    );
    if (status != c.RingOk) {
        _ = c.venus_objects_release(&objects, instance.*.handle, c.VK_OBJECT_TYPE_INSTANCE, 1);
        return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    const reply = transact(encoded[0..written]) orelse return c.VK_ERROR_DEVICE_LOST;
    var result: c.VkResult = c.VK_ERROR_UNKNOWN;
    if (c.venus_instance_wire_create_reply(
        &result,
        reply.ptr,
        reply.len,
        instance.*.id,
    ) != c.RingOk)
        return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, instance.*.handle, c.VK_OBJECT_TYPE_INSTANCE, 1);
        return result;
    }
    entry.* = .{ .handle = instance.*.handle };
    for (&instance_advertisements) |*advertisement| if (advertisement.handle == 0) {
        advertisement.* = .{ .handle = instance.*.handle, .mask = extension_mask,
            .api = if (info.*.pApplicationInfo != null and info.*.pApplicationInfo.*.apiVersion != 0)
                info.*.pApplicationInfo.*.apiVersion else c.VK_API_VERSION_1_0 };
        break;
    };
    output.* = @ptrFromInt(instance.*.handle);
    return c.VK_SUCCESS;
}
/// Host instance destruction before local child/root retirement; no allocations.
/// Invalid handles ignored, failed transport poisons binding until caller abandonment.
fn destroy_instance(
    instance: c.VkInstance,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    const handle = if (instance) |value| @intFromPtr(value) else return;
    const entry = cache(handle) orelse return;
    const record = object(handle, c.VK_OBJECT_TYPE_INSTANCE).?;
    // Refuse invalid parent-before-child teardown before any host submission.
    for (slots) |child| {
        if (child.kind == c.VK_OBJECT_TYPE_DEVICE) {
            for (entry.physical[0..entry.count]) |physical| {
                if (object(physical, c.VK_OBJECT_TYPE_PHYSICAL_DEVICE).?.id == child.parent_id)
                    return;
            }
        }
    }
    var encoded: [24]u8 = undefined;
    var written: usize = 0;
    std.debug.assert(
        c.venus_instance_wire_destroy(record.id, &encoded, encoded.len, &written) == c.RingOk,
    );
    _ = transact(encoded[0..written]) orelse return;
    for (entry.physical[0..entry.count]) |physical| {
        retire_extension_cache(physical);
        if (c.venus_objects_release(
            &objects,
            physical,
            c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,
            1,
        ) != c.RingOk) {
            _ = failure(c.RingCorrupt);
            return;
        }
    }
    if (c.venus_objects_release(&objects, handle, c.VK_OBJECT_TYPE_INSTANCE, 1) != c.RingOk) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for (&instance_advertisements) |*advertisement| if (advertisement.handle == handle) { advertisement.* = .{}; break; };
    entry.* = .{};
}
fn discover(entry: *instance_cache_t) c_int {
    if (entry.ready) return c.VK_SUCCESS;
    const instance = object(entry.handle, c.VK_OBJECT_TYPE_INSTANCE).?;
    var encoded: [164]u8 = undefined;
    var written: usize = 0;
    std.debug.assert(
        c.venus_query_wire_enumerate(
            instance.id,
            null,
            0,
            &encoded,
            encoded.len,
            &written,
        ) == c.RingOk,
    );
    var reply = transact(encoded[0..written]) orelse return c.VK_ERROR_DEVICE_LOST;
    var result: c.VkResult = c.VK_ERROR_UNKNOWN;
    var count: u32 = 0;
    if (c.venus_query_wire_enumerate_reply(
        &result,
        &count,
        null,
        0,
        reply.ptr,
        reply.len,
    ) != c.RingOk)
        return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) return result;
    if (count == 0) {
        entry.ready = true;
        return c.VK_SUCCESS;
    }
    var ids: [MaxDevices]u64 = undefined;
    for (0..count) |index| {
        var physical: [*c]c.venus_object_t = null;
        if (c.venus_objects_reserve(
            &objects,
            c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,
            instance.id,
            1,
            &physical,
        ) != c.RingOk) return failure(c.RingLimit);
        ids[index] = physical.*.id;
        entry.physical[index] = physical.*.handle;
        entry.count += 1;
    }
    std.debug.assert(
        c.venus_query_wire_enumerate(
            instance.id,
            &ids,
            count,
            &encoded,
            encoded.len,
            &written,
        ) == c.RingOk,
    );
    reply = transact(encoded[0..written]) orelse return c.VK_ERROR_DEVICE_LOST;
    var returned: u32 = 0;
    if (c.venus_query_wire_enumerate_reply(
        &result,
        &returned,
        &ids,
        count,
        reply.ptr,
        reply.len,
    ) != c.RingOk)
        return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS or returned != count) return failure(c.RingCorrupt);
    entry.ready = true;
    return c.VK_SUCCESS;
}
/// Enumerate stable borrowed physical handles; count-only/fill capacity follows Vulkan ABI.
/// No allocation; exact host IDs validated before publication, mutex serialized.
fn enumerate_physical(
    instance: c.VkInstance,
    count: [*c]u32,
    output: [*c]c.VkPhysicalDevice,
) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (instance == null or count == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const entry = cache(@intFromPtr(instance.?)) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const result = discover(entry);
    if (result != c.VK_SUCCESS) return result;
    if (output == null) {
        count.* = entry.count;
        return c.VK_SUCCESS;
    }
    const copied = @min(count.*, entry.count);
    for (0..copied) |index| output[index] = @ptrFromInt(entry.physical[index]);
    count.* = copied;
    return if (copied < entry.count) c.VK_INCOMPLETE else c.VK_SUCCESS;
}
fn query(physical: c.VkPhysicalDevice, command_id: u32) ?[]const u8 {
    if (physical == null) return null;
    const record = object(
        @intFromPtr(physical.?),
        c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,
    ) orelse return null;
    var encoded: [40]u8 = undefined;
    var written: usize = 0;
    std.debug.assert(
        c.venus_query_wire_fixed(
            command_id,
            record.id,
            &encoded,
            encoded.len,
            &written,
        ) == c.RingOk,
    );
    return transact(encoded[0..written]);
}
const reader_t = struct {
    bytes: []const u8,
    used: usize = 0,
    fn scalar(self: *reader_t, comptime value_t: type) !value_t {
        const size = @sizeOf(value_t);
        if (size > self.bytes.len - self.used) return error.Bounds;
        const word = std.mem.readInt(value_t, self.bytes[self.used..][0..size], .little);
        self.used += size;
        return word;
    }
    fn value(self: *reader_t, comptime value_t: type) !value_t {
        switch (@typeInfo(value_t)) {
            .Int => return self.scalar(value_t),
            .Struct => |info| {
                var result = std.mem.zeroes(value_t);
                inline for (info.fields) |field| {
                    @field(result, field.name) = try self.value(field.type);
                }
                return result;
            },
            else => @compileError("Only fixed integer physical query outputs supported"),
        }
    }
};
fn physical_request(
    physical: c.VkPhysicalDevice,
    command_id: u32,
    args: []const u32,
    capacity: ?u32,
) ?[]const u8 {
    if (physical == null) return null;
    const record = object(
        @intFromPtr(physical.?),
        c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,
    ) orelse return null;
    var encoded: [64]u8 = undefined;
    std.mem.writeInt(u32, encoded[0..4], command_id, .little);
    std.mem.writeInt(u32, encoded[4..8], 1, .little);
    std.mem.writeInt(u64, encoded[8..16], record.id, .little);
    var used: usize = 16;
    for (args) |arg| {
        std.mem.writeInt(u32, encoded[used..][0..4], arg, .little);
        used += 4;
    }
    std.mem.writeInt(u64, encoded[used..][0..8], 1, .little);
    used += 8;
    if (capacity) |count| {
        std.mem.writeInt(u32, encoded[used..][0..4], count, .little);
        used += 4;
        std.mem.writeInt(u64, encoded[used..][0..8], count, .little);
        used += 8;
    }
    return transact(encoded[0..used]);
}
fn fixed_value(comptime value_t: type, reader: *reader_t, command_id: u32) !value_t {
    if (try reader.scalar(u32) != command_id or try reader.scalar(u64) != 1) return error.Value;
    return reader.value(value_t);
}
/// Query host format flags; borrowed native output preserved on malformed reply, mutex serialized.
fn format_properties(
    physical: c.VkPhysicalDevice,
    format: c.VkFormat,
    output: [*c]c.VkFormatProperties,
) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (output == null) return;
    const reply = physical_request(physical, 4, &.{format}, null) orelse return;
    var reader = reader_t{ .bytes = reply };
    const value = fixed_value(c.VkFormatProperties, &reader, 4) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    output.* = value;
}
fn image_value(reader: *reader_t, output: *c.VkImageFormatProperties) !i32 {
    if (try reader.scalar(u32) != 5) return error.Value;
    const result = try reader.scalar(i32);
    if (result > 0 or try reader.scalar(u64) != 1) return error.Value;
    const value = try reader.value(c.VkImageFormatProperties);
    if (result == 0) output.* = value;
    return result;
}
/// Query host image limits; returns native Vulkan error or sticky DeviceLost, no allocations.
fn image_properties(
    physical: c.VkPhysicalDevice,
    format: c.VkFormat,
    kind: c.VkImageType,
    tiling: c.VkImageTiling,
    usage: c.VkImageUsageFlags,
    flags: c.VkImageCreateFlags,
    output: [*c]c.VkImageFormatProperties,
) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const reply = physical_request(
        physical,
        5,
        &.{ format, kind, tiling, usage, flags },
        null,
    ) orelse return c.VK_ERROR_DEVICE_LOST;
    var reader = reader_t{ .bytes = reply };
    return image_value(&reader, @ptrCast(output)) catch return failure(c.RingCorrupt);
}
fn array_values(
    comptime value_t: type,
    reader: *reader_t,
    command_id: u32,
    capacity: u32,
    fill: bool,
    count: *u32,
    output: [*c]value_t,
) !void {
    if (try reader.scalar(u32) != command_id or try reader.scalar(u64) != 1) return error.Value;
    const returned = try reader.scalar(u32);
    const array_count = try reader.scalar(u64);
    if (returned > 64 or array_count != (if (fill) returned else @as(u32, 0)) or
        (fill and returned > capacity)) return error.Value;
    var values: [64]value_t = undefined;
    if (fill) {
        for (values[0..returned]) |*value| value.* = try reader.value(value_t);
    }
    if (fill) @memcpy(output[0..returned], values[0..returned]);
    count.* = returned;
}
/// Query actual host queue families into bounded caller capacity; malformed reply preserves output.
fn queue_properties(
    physical: c.VkPhysicalDevice,
    count: [*c]u32,
    output: [*c]c.VkQueueFamilyProperties,
) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (count == null) return;
    const fill = output != null;
    if (fill and count.* == 0) return;
    const capacity = if (fill) @min(count.*, 64) else @as(u32, 0);
    const reply = physical_request(physical, 7, &.{}, capacity) orelse return;
    var reader = reader_t{ .bytes = reply };
    array_values(
        c.VkQueueFamilyProperties,
        &reader,
        7,
        capacity,
        fill,
        @ptrCast(count),
        output,
    ) catch {
        _ = failure(c.RingCorrupt);
    };
}
/// Query host sparse image layout, at most64 entries; borrowed output, no allocation.
fn sparse_properties(
    physical: c.VkPhysicalDevice,
    format: c.VkFormat,
    kind: c.VkImageType,
    samples: c.VkSampleCountFlagBits,
    usage: c.VkImageUsageFlags,
    tiling: c.VkImageTiling,
    count: [*c]u32,
    output: [*c]c.VkSparseImageFormatProperties,
) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (count == null) return;
    const fill = output != null;
    if (fill and count.* == 0) return;
    const capacity = if (fill) @min(count.*, 64) else @as(u32, 0);
    const reply = physical_request(
        physical,
        33,
        &.{ format, kind, samples, usage, tiling },
        capacity,
    ) orelse return;
    var reader = reader_t{ .bytes = reply };
    array_values(
        c.VkSparseImageFormatProperties,
        &reader,
        33,
        capacity,
        fill,
        @ptrCast(count),
        output,
    ) catch {
        _ = failure(c.RingCorrupt);
    };
}
const writer_t = struct {
    bytes: [8192]u8 = undefined,
    used: usize = 0,
    fn put(self: *writer_t, comptime word_t: type, word: word_t) void {
        std.debug.assert(@sizeOf(word_t) <= self.bytes.len - self.used);
        std.mem.writeInt(word_t, self.bytes[self.used..][0..@sizeOf(word_t)], word, .little);
        self.used += @sizeOf(word_t);
    }
    fn header(self: *writer_t, command_id: u32, id: u64) void {
        self.put(u32, command_id);
        self.put(u32, 1);
        self.put(u64, id);
    }
};
fn device_cache(handle: u64) ?*device_cache_t {
    if (object(handle, c.VK_OBJECT_TYPE_DEVICE) == null) return null;
    for (&device_caches) |*entry| if (entry.handle == handle) return entry;
    return null;
}
// Implemented policy only; every public feature and forwarded extension intersects real host data.
const DeviceExtensionNames = [_][]const u8{
    "VK_KHR_swapchain", "VK_EXT_robustness2", "VK_KHR_maintenance5", "VK_KHR_pipeline_library",
};
const DeviceExtensionBits = [_]u32{ 0, 287, 471, 291 };
const ImplementedApiVersion: u32 = c.VK_API_VERSION_1_3;
fn raw_extension_version(raw: *const extension_cache_t, name: []const u8) ?u32 {
    for (raw.records orelse &.{}) |record| {
        const length = std.mem.indexOfScalar(u8, &record.name, 0) orelse unreachable;
        if (std.mem.eql(u8, record.name[0..length], name)) return record.version;
    }
    return null;
}
fn supported_device_extensions(physical: c.VkPhysicalDevice, names: *[4][]const u8, versions: *[4]u32) !usize {
    var count: usize = 0;
    // Guest swapchain is an implemented Win32 presentation facade, not a host name.
    if (builtin.os.tag == .windows) {
        names[count] = DeviceExtensionNames[0]; versions[count] = 70; count += 1;
    }
    if (!reply_profile_ready) return count;
    const raw = try ensure_raw_extensions(physical);
    for (DeviceExtensionNames[1..], DeviceExtensionBits[1..]) |name, bit| {
        if (c.venus_capabilities_extension(&negotiated_capabilities, bit) == 0) continue;
        const version = raw_extension_version(raw, name) orelse continue;
        names[count] = name; versions[count] = version; count += 1;
    }
    return count;
}
fn feature_node_extension_supported(tag: u32, names: []const []const u8) bool {
    const required: []const u8 = switch (tag) {
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT => "VK_EXT_robustness2",
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR => "VK_KHR_maintenance5",
        else => return true,
    };
    for (names) |name| if (std.mem.eql(u8, name, required)) return true;
    return false;
}
fn project_device_feature_nodes(raw: *const features_wire.result_t, names: []const []const u8) [8]device_wire.feature_node_t {
    var nodes = [_]device_wire.feature_node_t{.{}} ** 8;
    for (FeatureTags, FeatureCounts, 0..) |tag, count, index| {
        nodes[index].type_tag = tag; nodes[index].flag_count = count;
        if (!feature_node_extension_supported(tag, names)) continue;
        for (raw.nodes[0..raw.count]) |source| {
            if (source.type_tag != tag) continue;
            for (source.flags[0..count], NodeFeatureAllowlists[index][0..count], 0..) |flag, mask, field|
                nodes[index].flags[field] = flag & mask;
            break;
        }
    }
    return nodes;
}
fn enabled_device_request(request: *const device_native.owned_request_t, names: []const []const u8) device_enabled_state_t {
    var result = disabled_device_state();
    result.features.core = request.legacy;
    for (request.nodes[0..request.node_count]) |node| {
        if (node.type_tag == c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            @memcpy(&result.features.core, node.flags[0..55]); continue;
        }
        const index = feature_index(node.type_tag);
        @memcpy(result.features.nodes[index].flags[0..node.flag_count], node.flags[0..node.flag_count]);
    }
    // Promoted names refer to the same requested feature; do not enable additional features.
    const shader_index = (@offsetOf(c.VkPhysicalDeviceVulkan11Features, "shaderDrawParameters") - @offsetOf(c.VkPhysicalDeviceVulkan11Features, "storageBuffer16BitAccess")) / 4;
    const query_index = (@offsetOf(c.VkPhysicalDeviceVulkan12Features, "hostQueryReset") - @offsetOf(c.VkPhysicalDeviceVulkan12Features, "samplerMirrorClampToEdge")) / 4;
    const shader = result.features.nodes[0].flags[shader_index] | result.features.nodes[3].flags[0];
    result.features.nodes[0].flags[shader_index] = shader; result.features.nodes[3].flags[0] = shader;
    const query_reset = result.features.nodes[1].flags[query_index] | result.features.nodes[4].flags[0];
    result.features.nodes[1].flags[query_index] = query_reset; result.features.nodes[4].flags[0] = query_reset;
    for (request.extension_ids[0..request.extension_count]) |id| {
        std.debug.assert(id < names.len);
        for (DeviceExtensionNames, 0..) |known, index| if (std.mem.eql(u8, known, names[id])) {
            result.extension_mask |= @as(u32, 1) << @as(u5, @intCast(index)); break;
        };
    }
    return result;
}
fn encode_device_supported(request: *const device_native.owned_request_t, physical_id: u64, id: u64, names: []const []const u8) !render_wire.writer_t {
    var queues: [16]device_wire.queue_t = undefined;
    var offset: usize = 0;
    for (0..request.queue_count) |index| {
        const count = request.counts[index];
        queues[index] = .{ .family_index = request.families[index], .priorities = request.priorities[offset..][0..count] };
        offset += count;
    }
    var host_names: [32][]const u8 = undefined;
    var host_count: usize = 0;
    for (request.extension_ids[0..request.extension_count]) |extension_id| {
        if (extension_id >= names.len) return error.Invalid;
        const name = names[extension_id];
        if (std.mem.eql(u8, name, "VK_KHR_swapchain")) continue;
        host_names[host_count] = name; host_count += 1;
    }
    return device_wire.create_device(physical_id, id, queues[0..request.queue_count], host_names[0..host_count], if (request.legacy_present) &request.legacy else null, request.nodes[0..request.node_count]);
}

fn encode_device(request: *const device_native.owned_request_t, physical_id: u64, id: u64) !render_wire.writer_t {
    var queues: [16]device_wire.queue_t = undefined;
    var offset: usize = 0;
    for (0..request.queue_count) |index| {
        const count = request.counts[index];
        queues[index] = .{ .family_index = request.families[index], .priorities = request.priorities[offset..][0..count] };
        offset += count;
    }
    // Native preflight established the entire owned shape/<=1096-byte capacity before reservation.
    return device_wire.create_device(physical_id, id, queues[0..request.queue_count], &.{}, if (request.legacy_present) &request.legacy else null, request.nodes[0..request.node_count]);
}
fn disabled_device_state() device_enabled_state_t {
    var result = device_enabled_state_t{};
    result.features.count = features_wire.MaxNodes;
    for (FeatureTags, FeatureCounts, 0..) |tag, count, index| {
        result.features.nodes[index].type_tag = tag;
        result.features.nodes[index].flag_count = count;
    }
    return result;
}
fn identity_reply(bytes: []const u8, command_id: u32, id: u64, has_result: bool) !i32 {
    var reader = reader_t{ .bytes = bytes };
    if (try reader.scalar(u32) != command_id) return error.Value;
    const result = if (has_result) try reader.scalar(i32) else 0;
    if (try reader.scalar(u64) != 1) return error.Value;
    const returned_id = try reader.scalar(u64);
    if (returned_id != id and !(result < 0 and returned_id == 0)) return error.Value;
    return result;
}
/// Borrowed native input, allocation-free serialized reservation/publication; NULL output on error.
/// Full bounded native preflight precedes reservations. True features/names require
/// implemented policy and actual host/protocol support; unknown headers omit without payload access.
/// @param[in] physical Nonnull live private physical handle, validated without dereference.
/// @param[in] info_address Nullable untyped accessible immutable disjoint native structs/arrays,
/// bounded names and features for this synchronous call; no retained input pointer.
/// @param[in] allocator Nullable borrowed callbacks; no allocations performed or retained.
/// @param[out] output_address Nullable untyped borrowed VkDevice storage, disjoint from inputs;
/// alignment checked before writing NULL. Invalid/misaligned output is never dereferenced.
/// @return Native host result, initialization/layer/feature/extension/host-memory errors or loss.
/// Mutex serialized; snapshot borrows end before reservation/encoding, no caller pointer retained.
fn create_device(
    physical: c.VkPhysicalDevice,
    info_address: ?*const anyopaque,
    allocator: [*c]const c.VkAllocationCallbacks,
    output_address: ?*anyopaque,
) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    const output_raw = output_address orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (@intFromPtr(output_raw) % @alignOf(c.VkDevice) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const output: *c.VkDevice = @ptrCast(@alignCast(output_raw));
    output.* = null;
    if (physical == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(physical.?),
        c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    // Preserve the no-query fast path for legacy/all-false requests. Structural
    // errors are rejected before host queries; true/named requests intersect caches.
    const simple: ?device_native.owned_request_t = device_native.preflight(info_address) catch |err| switch (err) {
        error.FeatureNotPresent, error.ExtensionNotPresent => null,
        error.LayerNotPresent => return c.VK_ERROR_LAYER_NOT_PRESENT,
        else => return c.VK_ERROR_INITIALIZATION_FAILED,
    };
    if (simple == null and !reply_profile_ready) {
        _ = device_native.preflight(info_address) catch |err| return switch (err) {
            error.FeatureNotPresent => c.VK_ERROR_FEATURE_NOT_PRESENT,
            error.ExtensionNotPresent => c.VK_ERROR_EXTENSION_NOT_PRESENT,
            else => c.VK_ERROR_INITIALIZATION_FAILED,
        };
        unreachable;
    }
    var supported_names: [4][]const u8 = undefined;
    var supported_versions: [4]u32 = undefined;
    var supported_count: usize = 0;
    const request = simple orelse blk: {
        const raw = ensure_raw_features(physical) orelse return if (lost != c.RingOk) c.VK_ERROR_DEVICE_LOST else c.VK_ERROR_INITIALIZATION_FAILED;
        supported_count = supported_device_extensions(physical, &supported_names, &supported_versions) catch |err| return switch (err) {
            error.OutOfMemory => c.VK_ERROR_OUT_OF_HOST_MEMORY,
            error.Backend => extension_backend_result,
            error.Lost => c.VK_ERROR_DEVICE_LOST,
            else => c.VK_ERROR_INITIALIZATION_FAILED,
        };
        const supported_nodes = project_device_feature_nodes(raw, supported_names[0..supported_count]);
        const support_policy = device_native.support_policy_t{ .legacy = project_core(&raw.core), .nodes = &supported_nodes, .extension_names = supported_names[0..supported_count] };
        break :blk device_native.preflight_supported(info_address, &support_policy) catch |err| return switch (err) {
            error.LayerNotPresent => c.VK_ERROR_LAYER_NOT_PRESENT,
            error.FeatureNotPresent => c.VK_ERROR_FEATURE_NOT_PRESENT,
            error.ExtensionNotPresent => c.VK_ERROR_EXTENSION_NOT_PRESENT,
            else => c.VK_ERROR_INITIALIZATION_FAILED,
        };
    };
    // Extension feature structures require the extension to be enabled in this
    // request, not merely present on the physical device.
    for (request.nodes[0..request.node_count]) |node| {
        const required: ?[]const u8 = switch (node.type_tag) {
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT => DeviceExtensionNames[1],
            c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR => DeviceExtensionNames[2],
            else => null,
        };
        const name = required orelse continue;
        var enabled = false;
        for (request.extension_ids[0..request.extension_count]) |id| if (std.mem.eql(u8, supported_names[id], name)) { enabled = true; break; };
        if (!enabled) for (node.flags[0..node.flag_count]) |flag| if (flag != 0) return c.VK_ERROR_FEATURE_NOT_PRESENT;
    }
    var available: ?*device_cache_t = null;
    for (&device_caches) |*entry| if (entry.handle == 0) {
        available = entry;
        break;
    };
    const entry = available orelse return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE, parent.id, 1, &record) !=
        c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    const encoded = encode_device_supported(&request, parent.id, record.*.id, supported_names[0..supported_count]) catch {
        // Internal owned-profile invariant failure: no peer/queue/ring ownership yet.
        // Release exactly this device; monotonic namespace identity is never rewound.
        const release_status = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_DEVICE, 1);
        std.debug.assert(release_status == c.RingOk);
        return c.VK_ERROR_INITIALIZATION_FAILED;
    };
    var staged = device_cache_t{ .handle = record.*.handle, .family_count = request.queue_count };
    var queue_index: usize = 0;
    for (0..staged.family_count) |family_index| {
        staged.families[family_index] = request.families[family_index];
        staged.counts[family_index] = request.counts[family_index];
        for (0..request.counts[family_index]) |_| {
            var ring: ?u32 = null;
            for (ring_slots[1..], 1..) |occupied, ring_index| if (!occupied) {
                ring = @intCast(ring_index);
                break;
            };
            if (ring == null) {
                release_device_reservation(&staged, record);
                return c.VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            var queue: [*c]c.venus_object_t = null;
            if (c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_QUEUE, record.*.id, 1, &queue) != c.RingOk) {
                release_device_reservation(&staged, record);
                return c.VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            ring_slots[ring.?] = true;
            staged.queues[queue_index] = queue.*.handle;
            staged.rings[queue_index] = ring.?;
            queue_index += 1;
        }
    }
    const reply = transact(encoded.bytes[0..encoded.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, 11, record.*.id, true) catch
        return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0) return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        release_device_reservation(&staged, record);
        return result;
    }
    staged.enabled_state = enabled_device_request(&request, supported_names[0..supported_count]);
    entry.* = staged;
    resource_state(record).* = .{ .id = record.*.id };
    output.* = @ptrFromInt(entry.handle);
    return c.VK_SUCCESS;
}
fn release_device_reservation(entry: *const device_cache_t, device: [*c]c.venus_object_t) void {
    for (entry.queues, 0..) |handle, index| if (handle != 0) {
        std.debug.assert(c.venus_objects_release(&objects, handle, c.VK_OBJECT_TYPE_QUEUE, 1) == c.RingOk);
        ring_slots[entry.rings[index]] = false;
    };
    std.debug.assert(c.venus_objects_release(&objects, device.*.handle, c.VK_OBJECT_TYPE_DEVICE, 1) == c.RingOk);
}
/// Borrowed output cleared for invalid/lost calls; stable queue identity lasts until device retire.
/// @param[in] device Nullable validated private device handle, not dereferenced.
/// @param[in] family Queue family requested during device creation.
/// @param[in] index Zero-based index below that family's requested count.
/// @param[out] output Nullable borrowed handle storage; NULL on invalid/lost calls.
/// CreateDevice already reserves all valid requested private queue/ring capacity.
/// Mutex serialized, no allocations; reply identity must match a private reservation.
fn get_device_queue(
    device: c.VkDevice,
    family: u32,
    index: u32,
    output: [*c]c.VkQueue,
) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (output == null) return;
    output.* = null;
    if (device == null or lost != c.RingOk) return;
    const entry = device_cache(@intFromPtr(device.?)) orelse return;
    var offset: usize = 0;
    var position: ?usize = null;
    for (entry.families[0..entry.family_count], 0..) |number, family_index| {
        if (family == number and index < entry.counts[family_index]) position = offset + index;
        offset += entry.counts[family_index];
    }
    const queue_index = position orelse return;
    if (entry.ready[queue_index]) {
        output.* = @ptrFromInt(entry.queues[queue_index]);
        return;
    }
    const ring_index = entry.rings[queue_index];
    const parent = object(entry.handle, c.VK_OBJECT_TYPE_DEVICE).?;
    const queue = object(entry.queues[queue_index], c.VK_OBJECT_TYPE_QUEUE).?;
    var writer = writer_t{};
    writer.header(155, parent.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2);
    writer.put(u64, 1);
    writer.put(u32, QueueTimelineTag);
    writer.put(u64, 0);
    writer.put(u32, ring_index);
    writer.put(u32, 0);
    writer.put(u32, family);
    writer.put(u32, index);
    writer.put(u64, 1);
    writer.put(u64, queue.*.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    _ = identity_reply(reply, 155, queue.*.id, false) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    resource_state(queue).* = .{ .id = queue.*.id, .queue_family = family };
    entry.ready[queue_index] = true;
    output.* = @ptrFromInt(queue.*.handle);
}
/// Retire host device then private queues/device; loss retains reservations.
/// @param[in] device Nullable private handle, foreign/retired handles ignored without dereference.
/// @param[in] allocator Nullable borrowed callback input; no callbacks or allocations performed.
/// Allocation-free/mutex serialized; caller must first destroy any future nonqueue children.
fn destroy_device(
    device: c.VkDevice,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (device == null) return;
    const entry = device_cache(@intFromPtr(device.?)) orelse return;
    const record = object(entry.handle, c.VK_OBJECT_TYPE_DEVICE).?;
    if (resource_state(record).idle_refs != 0) return;
    for (entry.queues) |handle| if (handle != 0) {
        if (resource_state(object(handle, c.VK_OBJECT_TYPE_QUEUE).?).idle_refs != 0) return;
    };
    for (submission_tickets) |ticket| {
        if (ticket.queue != 0 and object(ticket.queue, c.VK_OBJECT_TYPE_QUEUE).?.parent_id == record.id)
            return;
    }
    const callback_context = wsi_callback_context_t{ .device = entry.handle };
    const backend = wsi_backend(&callback_context);
    wsi.destroy_device(&wsi_state, &backend, entry.handle);
    retire_device_templates(record.id);
    for (slots) |child| {
        if (child.id != 0 and child.parent_id == record.id and child.kind != c.VK_OBJECT_TYPE_QUEUE)
            return;
    }
    var writer = writer_t{};
    writer.header(12, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const reply_command = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (reply_command != 12) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for (entry.queues) |handle| if (handle != 0) {
        resource_state(object(handle, c.VK_OBJECT_TYPE_QUEUE).?).* = .{};
        std.debug.assert(c.venus_objects_release(&objects, handle, c.VK_OBJECT_TYPE_QUEUE, 1) ==
            c.RingOk);
    };
    resource_state(record).* = .{};
    std.debug.assert(c.venus_objects_release(&objects, entry.handle, c.VK_OBJECT_TYPE_DEVICE, 1) ==
        c.RingOk);
    for (entry.rings) |ring| if (ring != 0) {
        ring_slots[ring] = false;
    };
    entry.* = .{};
}
// Actual host count/fill query; heap ownership publishes only after complete duplicate/name validation.
fn ensure_raw_extensions(physical: c.VkPhysicalDevice) !*const extension_cache_t {
    if (!reply_profile_ready) return error.Unavailable;
    const handle = if (physical) |value| @intFromPtr(value) else return error.Invalid;
    const record = object(handle, c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) orelse return error.Invalid;
    var target: ?*extension_cache_t = null;
    for (&extension_caches) |*entry| {
        if (entry.handle == handle and entry.ready) return entry;
        if (target == null and entry.handle == 0) target = entry;
    }
    const entry = target orelse return error.OutOfMemory;
    const count_request = try extensions_wire.encode_count(record.id);
    const count_reply = transact(count_request.bytes[0..count_request.used]) orelse return error.Lost;
    const count_status = extensions_wire.decode_status(count_reply, null) catch {
        _ = failure(c.RingCorrupt); return error.Lost;
    };
    if (count_status < 0) { extension_backend_result = count_status; return error.Backend; }
    const count = extensions_wire.decode_count(count_reply) catch {
        _ = failure(c.RingCorrupt); return error.Lost;
    };
    if (count == 0) {
        entry.* = .{ .handle = handle, .ready = true };
        return entry;
    }
    const records = try MappingAllocator.alloc(extensions_wire.extension_t, count);
    errdefer MappingAllocator.free(records);
    const fill_request = try extensions_wire.encode_fill(record.id, count);
    const fill_reply = transact_sized(fill_request.bytes[0..fill_request.used], 28 + @as(usize, count) * 268) orelse return error.Lost;
    const fill_status = extensions_wire.decode_status(fill_reply, count) catch {
        _ = failure(c.RingCorrupt); return error.Lost;
    };
    if (fill_status < 0) { extension_backend_result = fill_status; return error.Backend; }
    const filled = extensions_wire.decode_fill(fill_reply, records) catch {
        _ = failure(c.RingCorrupt); return error.Lost;
    };
    if (filled.incomplete or filled.count != count) return error.Unavailable;
    entry.* = .{ .handle = handle, .records = records, .ready = true };
    return entry;
}
/// Enumerate the implemented intersection; raw backend names never grant unsupported behavior.
/// Borrowed caller storage/count, mutex serialized. Raw heap cache belongs to physical namespace
/// and is freed on acknowledged parent destruction or receiver-retired abandonment.
/// Enumerate only implemented guest WSI and actual host/protocol extension intersection.
/// [in] physical live handle, nullable layer unsupported; [in,out] count/caller array
/// borrowed for call. No pointer escapes. Raw cache owner is physical namespace.
fn device_extensions(physical: c.VkPhysicalDevice, layer: [*c]const u8,
    count: [*c]u32, output: [*c]c.VkExtensionProperties) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (physical == null or object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) == null or count == null)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    if (layer != null) return c.VK_ERROR_LAYER_NOT_PRESENT;
    var names: [4][]const u8 = undefined;
    var versions: [4]u32 = undefined;
    const total = supported_device_extensions(physical, &names, &versions) catch |err| return switch (err) {
        error.OutOfMemory => c.VK_ERROR_OUT_OF_HOST_MEMORY,
        error.Backend => extension_backend_result,
        error.Lost => c.VK_ERROR_DEVICE_LOST,
        else => c.VK_ERROR_INITIALIZATION_FAILED,
    };
    if (output == null) { count.* = @intCast(total); return c.VK_SUCCESS; }
    const copied = @min(count.*, total);
    for (0..copied) |index| {
        output[index] = std.mem.zeroes(c.VkExtensionProperties);
        @memcpy(output[index].extensionName[0..names[index].len], names[index]);
        output[index].specVersion = versions[index];
    }
    count.* = @intCast(copied);
    return if (copied < total) c.VK_INCOMPLETE else c.VK_SUCCESS;
}

// Caller holds mutex; live physical namespace selects its parent-owned scalar cache only.
fn physical_features_cache(physical: c.VkPhysicalDevice) ?*physical_feature_cache_t {
    if (physical == null or object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) == null) return null;
    for (&caches) |*entry| {
        for (entry.physical[0..entry.count], 0..) |handle, index| {
            if (handle == @intFromPtr(physical.?)) return &entry.feature_caches[index];
        }
    }
    return null;
}
// Decode before cache publication; subsequent fully decoded API values must preserve identity.
fn raw_properties(physical: c.VkPhysicalDevice) ?c.VkPhysicalDeviceProperties {
    const entry = physical_features_cache(physical) orelse return null;
    const reply = query(physical, 6) orelse return null;
    var staged: c.VkPhysicalDeviceProperties = undefined;
    if (c.venus_values_properties_decode(&staged, reply.ptr, reply.len) != c.RingOk or
        staged.apiVersion >> 29 != 0 or ((staged.apiVersion >> 22) & 0x7f) != 1 or
        (entry.actual_api_ready and entry.actual_api_version != staged.apiVersion))
    {
        _ = failure(c.RingCorrupt);
        return null;
    }
    entry.actual_api_version = staged.apiVersion;
    entry.actual_api_ready = true;
    return staged;
}
// Fixed owned Boolean copy in native declaration order, never native padding or a byte cast.
fn native_core_flags(value: *const c.VkPhysicalDeviceFeatures) [features_wire.CoreFlags]u32 {
    var flags: [features_wire.CoreFlags]u32 = undefined;
    inline for (@typeInfo(c.VkPhysicalDeviceFeatures).Struct.fields, 0..) |field, index|
        flags[index] = @field(value.*, field.name);
    return flags;
}
// Caller holds mutex; immutable complete raw record is published only after full validation.
fn ensure_raw_features(physical: c.VkPhysicalDevice) ?*const features_wire.result_t {
    if (!negotiated_capabilities_ready or lost != c.RingOk) return null;
    const entry = physical_features_cache(physical) orelse return null;
    if (entry.raw_features_ready) return &entry.raw;
    if (!entry.actual_api_ready) _ = raw_properties(physical) orelse return null;
    var staged = features_wire.result_t{};
    if (entry.actual_api_version < c.VK_API_VERSION_1_1) {
        const reply = query(physical, 3) orelse return null;
        var core: c.VkPhysicalDeviceFeatures = undefined;
        if (c.venus_values_features_decode(&core, reply.ptr, reply.len) != c.RingOk) {
            _ = failure(c.RingCorrupt);
            return null;
        }
        staged.core = native_core_flags(&core);
    } else {
        var tags: [features_wire.MaxNodes]u32 = undefined;
        var count: usize = 0;
        for (FeatureTags, 0..) |tag, index| {
            const supported = switch (index) {
                0, 1, 4 => entry.actual_api_version >= c.VK_API_VERSION_1_2,
                2 => entry.actual_api_version >= c.VK_API_VERSION_1_3,
                3 => true,
                5 => c.venus_capabilities_extension(&negotiated_capabilities, 29) != 0,
                6 => c.venus_capabilities_extension(&negotiated_capabilities, 287) != 0,
                7 => c.venus_capabilities_extension(&negotiated_capabilities, 471) != 0,
                else => unreachable,
            };
            if (supported) {
                tags[count] = tag;
                count += 1;
            }
        }
        const record = object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE).?;
        const writer = features_wire.query(record.id, tags[0..count]) catch unreachable;
        const reply = transact(writer.bytes[0..writer.used]) orelse return null;
        staged = features_wire.decode(reply, tags[0..count]) catch {
            _ = failure(c.RingCorrupt);
            return null;
        };
    }
    entry.raw = staged;
    entry.raw_features_ready = true;
    return &entry.raw;
}
fn feature_index(tag: u32) usize {
    for (FeatureTags, 0..) |known, index| if (known == tag) return index;
    unreachable;
}
fn zero_feature_nodes(chain: *const features_native.chain_t) [features_wire.MaxNodes]features_wire.node_t {
    var nodes = [_]features_wire.node_t{.{}} ** features_wire.MaxNodes;
    for (chain.tags[0..chain.count], 0..) |tag, index|
        nodes[index] = .{ .type_tag = tag, .flag_count = FeatureCounts[feature_index(tag)] };
    return nodes;
}
fn project_core(flags: *const [features_wire.CoreFlags]u32) [features_wire.CoreFlags]u32 {
    if (!reply_profile_ready) return [_]u32{0} ** features_wire.CoreFlags;
    var projected: [features_wire.CoreFlags]u32 = undefined;
    for (&projected, flags, CoreFeatureAllowlist) |*output, raw, mask| output.* = raw & mask;
    return projected;
}
/// Query complete raw host features and publish only proven guest flags as one transaction.
/// [in] physical nullable live namespace handle; [in,out] output_address nullable initialized
/// accessible exclusive Features2 storage. Headers/links immutable for this call; unknown
/// payloads preserved. Invalid native input issues no command and leaves all bytes unchanged.
/// Corrupt replies preserve output and poison binding. Mutex serialized, no heap/pointer retention.
fn features2(physical: c.VkPhysicalDevice, output_address: ?*anyopaque) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (!negotiated_capabilities_ready or physical_features_cache(physical) == null) return;
    const address = output_address orelse return;
    if (@intFromPtr(address) % @alignOf(c.VkPhysicalDeviceFeatures2) != 0) return;
    const output: *c.VkPhysicalDeviceFeatures2 = @ptrCast(@alignCast(address));
    if (output.sType != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) return;
    const chain = features_native.collect_chain(output.pNext) catch return;
    var nodes = zero_feature_nodes(&chain);
    const zero_core = [_]u32{0} ** features_wire.CoreFlags;
    features_native.validate_features2(output_address, &chain, nodes[0..chain.count], &zero_core) catch return;
    const raw = ensure_raw_features(physical) orelse return;
    const core = project_core(&raw.core);
    var supported_names: [4][]const u8 = undefined;
    var supported_versions: [4]u32 = undefined;
    const supported_count = if (reply_profile_ready) supported_device_extensions(physical, &supported_names, &supported_versions) catch return else 0;
    for (nodes[0..chain.count]) |*node| {
        if (!feature_node_extension_supported(node.type_tag, supported_names[0..supported_count])) continue;
        const index = feature_index(node.type_tag);
        for (raw.nodes[0..raw.count]) |source| {
            if (source.type_tag != node.type_tag) continue;
            for (node.flags[0..node.flag_count], source.flags[0..source.flag_count], NodeFeatureAllowlists[index][0..node.flag_count]) |*target, flag, mask|
                target.* = if (reply_profile_ready) flag & mask else 0;
            break;
        }
    }
    features_native.publish_features2(output_address, &chain, nodes[0..chain.count], &core) catch return;
}
/// Query modern typed physical properties; caller owns initialized exclusive root/chain storage.
/// Invalid topology or malformed replies preserve all output bytes. No retained caller pointers;
/// the process mutex serializes transport and publication. Unknown payloads remain untouched.
fn properties2(physical: c.VkPhysicalDevice, output_address: ?*anyopaque) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (!negotiated_capabilities_ready or physical_features_cache(physical) == null) return;
    const address = output_address orelse return;
    if (@intFromPtr(address) % @alignOf(c.VkPhysicalDeviceProperties2) != 0) return;
    const output: *c.VkPhysicalDeviceProperties2 = @ptrCast(@alignCast(address));
    if (output.sType != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2) return;
    const chain = properties_native.collect_chain(output.pNext) catch return;
    const record = object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE).?;
    const encoded = properties_wire.query(record.id, chain.tags[0..chain.count]) catch return;
    const reply = transact(encoded.bytes[0..encoded.used]) orelse return;
    var staged = properties_wire.decode(reply, chain.tags[0..chain.count]) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    const entry = physical_features_cache(physical).?;
    if (staged.properties.apiVersion >> 29 != 0 or
        ((staged.properties.apiVersion >> 22) & 0x7f) != 1 or
        (entry.actual_api_ready and entry.actual_api_version != staged.properties.apiVersion)) {
        _ = failure(c.RingCorrupt);
        return;
    }
    const actual_api = staged.properties.apiVersion;
    project_properties(@ptrCast(&staged.properties));
    properties_native.publish_properties2(output_address, &chain, &staged) catch return;
    entry.actual_api_version = actual_api;
    entry.actual_api_ready = true;
}
// All physical property entrypoints use the same guest implementation limits.
fn project_properties(staged: *c.VkPhysicalDeviceProperties) void {
    staged.limits.maxBoundDescriptorSets = @min(staged.limits.maxBoundDescriptorSets, profiles.MaxSets);
    staged.limits.maxVertexInputBindings = @min(staged.limits.maxVertexInputBindings, 32);
    staged.limits.maxVertexInputAttributes = @min(staged.limits.maxVertexInputAttributes, 32);
    staged.limits.maxViewports = @min(staged.limits.maxViewports, 16);
    staged.limits.maxColorAttachments = @min(staged.limits.maxColorAttachments, 8);
    staged.limits.maxPushConstantsSize = @min(staged.limits.maxPushConstantsSize, profiles.MaxPushBytes);
    staged.apiVersion = if (reply_profile_ready) @min(staged.apiVersion, ImplementedApiVersion) else c.VK_API_VERSION_1_0;
    staged.limits.nonCoherentAtomSize = 1;
    staged.limits.minMemoryMapAlignment = 4096;
}
/// Query actual host properties into caller storage only after bounded reply validation.
/// Borrowed output, no allocations; errors preserve output and poison transport binding.
fn properties(physical: c.VkPhysicalDevice, output: [*c]c.VkPhysicalDeviceProperties) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (output == null) return;
    var staged = raw_properties(physical) orelse return;
    project_properties(&staged);
    output.* = staged;
}
/// Query raw host core features privately; publish the same implementation intersection as Features2.
/// [in] physical nullable namespace handle; [out] output nullable borrowed initialized exclusive
/// native core storage. Errors preserve output; malformed replies poison binding. Mutex serialized,
/// no allocations or retained caller pointers; legacy mode uses command3 and never command147.
fn features(physical: c.VkPhysicalDevice, output: [*c]c.VkPhysicalDeviceFeatures) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (output == null) return;
    var raw: [features_wire.CoreFlags]u32 = undefined;
    if (negotiated_capabilities_ready) {
        const snapshot = ensure_raw_features(physical) orelse return;
        raw = snapshot.core;
    } else {
        const reply = query(physical, 3) orelse return;
        var staged: c.VkPhysicalDeviceFeatures = undefined;
        if (c.venus_values_features_decode(&staged, reply.ptr, reply.len) != c.RingOk) {
            _ = failure(c.RingCorrupt);
            return;
        }
        raw = native_core_flags(&staged);
    }
    const core = project_core(&raw);
    inline for (@typeInfo(c.VkPhysicalDeviceFeatures).Struct.fields, 0..) |field, index|
        @field(output.*, field.name) = core[index];
}
comptime {
    if (@sizeOf(features_wire.result_t) != 1792 or @alignOf(features_wire.result_t) != 4 or
        @sizeOf(physical_feature_cache_t) != 1800 or @alignOf(physical_feature_cache_t) != 4 or
        @offsetOf(physical_feature_cache_t, "raw") != 4 or
        @offsetOf(physical_feature_cache_t, "actual_api_ready") != 1796 or
        @offsetOf(physical_feature_cache_t, "raw_features_ready") != 1797 or
        @sizeOf(instance_cache_t) != 28944 or @alignOf(instance_cache_t) != 8 or
        @offsetOf(instance_cache_t, "feature_caches") != 140)
        @compileError("Features2 raw cache ownership ABI changed");
}
/// Query actual host memory layout; same serialized/preserved-output contract as properties.
fn memory(
    physical: c.VkPhysicalDevice,
    output: [*c]c.VkPhysicalDeviceMemoryProperties,
) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (output == null) return;
    const reply = query(physical, 8) orelse return;
    if (c.venus_values_memory_decode(output, reply.ptr, reply.len) != c.RingOk) {
        _ = failure(c.RingCorrupt);
        return;
    }
    // Actual coherent host types use implicit dirty publication and completed-GPU merging.
    // Noncoherent host allocations have no supported map/export path.
    for (output.*.memoryTypes[0..output.*.memoryTypeCount]) |*memory_type| {
        if (memory_type.propertyFlags & c.VK_MEMORY_PROPERTY_HOST_COHERENT_BIT == 0)
            memory_type.propertyFlags &= ~@as(u32, c.VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
}
/// Enumerate implemented guest instance names; borrowed count/capacity storage, mutex serialized.
/// Guest query and Win32 names are never forwarded as unsupported Linux host names.
fn enumerate_instance_extensions(layer: [*c]const u8, count: [*c]u32, output: [*c]c.VkExtensionProperties) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (count == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (layer != null) return c.VK_ERROR_LAYER_NOT_PRESENT;
    var names: [3]c.VkExtensionProperties = undefined;
    var total: u32 = 0;
    for (InstanceExtensionNames, InstanceExtensionVersions, 0..) |name, version, index| {
        if (!instance_extension_supported(index)) continue;
        names[total] = std.mem.zeroes(c.VkExtensionProperties);
        @memcpy(names[total].extensionName[0..name.len], name);
        names[total].specVersion = version;
        total += 1;
    }
    if (output == null) { count.* = total; return c.VK_SUCCESS; }
    const copied = @min(count.*, total);
    @memcpy(output[0..copied], names[0..copied]);
    count.* = copied;
    return if (copied < total) c.VK_INCOMPLETE else c.VK_SUCCESS;
}
/// Enumerate empty supported instance extensions; borrowed output, no allocation or transport.
fn enumerate_extensions(
    layer: [*c]const u8,
    count: [*c]u32,
    output: [*c]c.VkExtensionProperties,
) callconv(.C) c_int {
    _ = output;
    if (count == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (layer != null) return c.VK_ERROR_LAYER_NOT_PRESENT;
    count.* = 0;
    return c.VK_SUCCESS;
}
/// Report the implemented API ceiling; no ownership or transport, thread-safe.
fn enumerate_version(version: [*c]u32) callconv(.C) c_int {
    if (version == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    lock_icd(); defer unlock_icd();
    version.* = if (reply_profile_ready) ImplementedApiVersion else c.VK_API_VERSION_1_0;
    return c.VK_SUCCESS;
}
fn child_object(handle: u64, kind: u32, parent_id: u64) ?*c.venus_object_t {
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_lookup(&objects, handle, kind, 0, &record) != c.RingOk or
        record.*.parent_id != parent_id) return null;
    return @ptrCast(record);
}
fn result_reply(bytes: []const u8, command_id: u32, pending: i32) c_int {
    var reader = reader_t{ .bytes = bytes };
    const received = reader.scalar(u32) catch return failure(c.RingCorrupt);
    if (received != command_id) return failure(c.RingCorrupt);
    const result = reader.scalar(i32) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0 and result != pending) return failure(c.RingCorrupt);
    return result;
}
/// Create one device-owned nondispatchable fence without allocation.
/// @param[in] device Nonnull live private device, validated without native dereference.
/// @param[in] info Nonnull borrowed canonical flags0/1/no-extension native input.
/// @param[in] allocator Nullable borrowed callbacks, not retained or invoked.
/// @param[out] output Nonnull native handle storage, NULL on any failure.
/// @return Host result, explicit local validation/memory errors or sticky device loss.
/// @note Mutex serialized; publish only a validated host reservation. Caller owns the fence.
fn create_fence(
    device: c.VkDevice,
    info: [*c]const c.VkFenceCreateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkFence,
) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or info.*.sType != c.VK_STRUCTURE_TYPE_FENCE_CREATE_INFO or
        info.*.pNext != null or info.*.flags > 1) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_FENCE, parent.id, 0, &record) !=
        c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var writer = writer_t{};
    writer.header(35, parent.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    writer.put(u64, 0);
    writer.put(u32, info.*.flags);
    writer.put(u64, 0);
    writer.put(u64, 1);
    writer.put(u64, record.*.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, 35, record.*.id, true) catch
        return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0) return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_FENCE, 0);
        return result;
    }
    resource_state(record).* = .{ .id = record.*.id };
    output.* = @ptrFromInt(record.*.handle);
    return c.VK_SUCCESS;
}
/// Destroy a quiescent device-owned fence; invalid/foreign handles ignored.
/// @param[in] device Nonnull live private parent; no native handle dereference.
/// @param[in] fence Nullable private token, borrowed until successful host destruction.
/// @param[in] allocator Nullable unused borrowed callbacks; no allocation or retained pointer.
/// @note Mutex serialized; caller retires GPU references first. Loss retains ownership.
fn destroy_fence(
    device: c.VkDevice,
    fence: c.VkFence,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (device == null or fence == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(
        @intFromPtr(fence.?),
        c.VK_OBJECT_TYPE_FENCE,
        parent.id,
    ) orelse return;
    if (resource_state(record).inflight_count != 0) return;
    var writer = writer_t{};
    writer.header(36, parent.id);
    writer.put(u64, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 36) {
        _ = failure(c.RingCorrupt);
        return;
    }
    resource_state(record).* = .{};
    std.debug.assert(c.venus_objects_release(&objects, record.handle, c.VK_OBJECT_TYPE_FENCE, 0) ==
        c.RingOk);
}
/// Create one device-owned nondispatchable semaphore without allocation.
/// @param[in] device Nonnull live private device, validated without native dereference.
/// @param[in] info Nonnull borrowed canonical flags0/no-extension native input.
/// @param[in] allocator Nullable borrowed callbacks, not retained or invoked.
/// @param[out] output Nonnull native handle storage, NULL on any failure.
/// @return Host result, explicit local validation/memory errors or sticky device loss.
/// @note Mutex serialized; publish only a validated host reservation. Caller owns the semaphore.
fn create_semaphore(device: c.VkDevice, info: [*c]const c.VkSemaphoreCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkSemaphore) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or info.*.sType != c.VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO or info.*.flags != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    var kind: u32 = 0;
    var initial: u64 = 0;
    if (info.*.pNext) |pointer| {
        if (@intFromPtr(pointer) % @alignOf(c.VkSemaphoreTypeCreateInfo) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
        const node: *const c.VkSemaphoreTypeCreateInfo = @ptrCast(@alignCast(pointer));
        if (node.sType != c.VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO or node.pNext != null or node.semaphoreType > 1 or (node.semaphoreType == 0 and node.initialValue != 0)) return c.VK_ERROR_INITIALIZATION_FAILED;
        kind = node.semaphoreType;
        initial = node.initialValue;
    }
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (kind != 0 and !timeline_enabled(parent)) return c.VK_ERROR_FEATURE_NOT_PRESENT;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_SEMAPHORE, parent.id, 0, &record) != c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var writer: writer_t = undefined;
    if (info.*.pNext == null) {
        writer = .{};
        writer.header(40, parent.id); writer.put(u64, 1); writer.put(u32, c.VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        writer.put(u64, 0); writer.put(u32, 0); writer.put(u64, 0); writer.put(u64, 1); writer.put(u64, record.*.id);
    } else {
        const encoded = modern_sync.create_semaphore(parent.id, record.*.id, kind, initial) catch {
            _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_SEMAPHORE, 0);
            return c.VK_ERROR_INITIALIZATION_FAILED;
        };
        writer = .{};
        @memcpy(writer.bytes[0..encoded.used], encoded.bytes[0..encoded.used]); writer.used = encoded.used;
    }
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, 40, record.*.id, true) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0) return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_SEMAPHORE, 0);
        return result;
    }
    resource_state(record).* = .{ .id = record.*.id, .buffer_usage = kind, .allocation_size = initial };
    output.* = @ptrFromInt(record.*.handle);
    return c.VK_SUCCESS;
}
/// Destroy a quiescent device-owned semaphore; invalid/foreign handles ignored.
/// @param[in] device Nonnull live private parent; no native handle dereference.
/// @param[in] semaphore Nullable private token, borrowed until successful host destruction.
/// @param[in] allocator Nullable unused borrowed callbacks; no allocation or retained pointer.
/// @note Mutex serialized; caller retires GPU references first. Loss retains ownership.
fn destroy_semaphore(
    device: c.VkDevice,
    semaphore: c.VkSemaphore,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (device == null or semaphore == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(
        @intFromPtr(semaphore.?),
        c.VK_OBJECT_TYPE_SEMAPHORE,
        parent.id,
    ) orelse return;
    if (resource_state(record).inflight_count != 0 or resource_state(record).idle_refs != 0) return;
    var writer = writer_t{};
    writer.header(41, parent.id);
    writer.put(u64, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 41) {
        _ = failure(c.RingCorrupt);
        return;
    }
    resource_state(record).* = .{};
    std.debug.assert(c.venus_objects_release(&objects, record.handle, c.VK_OBJECT_TYPE_SEMAPHORE, 0) ==
        c.RingOk);
}
/// Create a device-owned core buffer after validating bounded native input.
/// @param[in] device Nonnull live private device; no caller handle dereference.
/// @param[in] info Nonnull accessible canonical info and optional2..16 family array.
/// @param[in] allocator Nullable unused callbacks, borrowed only for call.
/// @param[out] output Nonnull borrowed handle storage, NULL on failure.
/// @return Host result, initialization error, registry exhaustion or sticky device loss.
/// @note Allocation-free and mutex serialized; record owned until host destruction.
fn create_buffer(
    device: c.VkDevice,
    info: [*c]const c.VkBufferCreateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkBuffer,
) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or
        info.*.sType != c.VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO or
        info.*.flags != 0 or info.*.size == 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    var usage: u64 = info.*.usage;
    if (info.*.pNext) |pointer| {
        if (@intFromPtr(pointer) % @alignOf(c.VkBufferUsageFlags2CreateInfoKHR) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
        const node: *const c.VkBufferUsageFlags2CreateInfoKHR = @ptrCast(@alignCast(pointer));
        if (node.sType != c.VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR or node.pNext != null) return c.VK_ERROR_INITIALIZATION_FAILED;
        usage = node.usage;
    }
    if (usage == 0 or usage & ~@as(u64, 0x1ff | c.VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (usage & c.VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT != 0 and !device_address_enabled(parent)) return c.VK_ERROR_FEATURE_NOT_PRESENT;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var family_count: u32 = 0;
    if (info.*.sharingMode == c.VK_SHARING_MODE_CONCURRENT) {
        family_count = info.*.queueFamilyIndexCount;
        if (family_count < 2 or family_count > 16 or info.*.pQueueFamilyIndices == null)
            return c.VK_ERROR_INITIALIZATION_FAILED;
        const entry = device_cache(parent.handle).?;
        for (info.*.pQueueFamilyIndices[0..family_count], 0..) |family, index| {
            if (std.mem.indexOfScalar(
                u32,
                entry.families[0..entry.family_count],
                family,
            ) == null or std.mem.indexOfScalar(
                u32,
                info.*.pQueueFamilyIndices[0..index],
                family,
            ) != null) return c.VK_ERROR_INITIALIZATION_FAILED;
        }
    } else if (info.*.sharingMode != c.VK_SHARING_MODE_EXCLUSIVE)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_BUFFER,
        parent.id,
        0,
        &record,
    ) != c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var writer = writer_t{};
    writer.header(50, parent.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    writer.put(u64, 0);
    writer.put(u32, 0);
    writer.put(u64, info.*.size);
    writer.put(u32, @intCast(usage));
    writer.put(u32, info.*.sharingMode);
    writer.put(u32, family_count);
    writer.put(u64, family_count);
    if (family_count != 0) for (info.*.pQueueFamilyIndices[0..family_count]) |family| {
        writer.put(u32, family);
    };
    writer.put(u64, 0);
    writer.put(u64, 1);
    writer.put(u64, record.*.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, 50, record.*.id, true) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0) return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_BUFFER, 0);
        return result;
    }
    resource_state(record).* = .{
        .id = record.*.id,
        .buffer_size = info.*.size,
        .buffer_usage = @intCast(usage),
    };
    output.* = @ptrFromInt(record.*.handle);
    return c.VK_SUCCESS;
}
/// Destroy a quiescent buffer, ignoring NULL/stale/foreign handles.
/// @param[in] device Nullable private device parent, borrowed for call.
/// @param[in] buffer Nullable device-owned token, consumed only after host destruction.
/// @param[in] allocator Nullable unused callbacks, no pointer retained.
/// @note Allocation-free and mutex serialized; caller retires GPU uses first.
/// Host loss retains uncertain ownership until receiver retirement and abandonment.
fn destroy_buffer(
    device: c.VkDevice,
    buffer: c.VkBuffer,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (device == null or buffer == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_BUFFER,
        parent.id,
    ) orelse return;
    if (resource_state(record).inflight_count != 0) return;
    for (&slots) |*child| if (child.id != 0 and child.kind == c.VK_OBJECT_TYPE_BUFFER_VIEW and
        child.parent_id == parent.id and resource_state(child).view_image == record.handle) return;
    const index = resource_index(record);
    const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
    for (resource_states) |state| if (state.buffer_references[index / 64] & bit != 0 and
        state.command_state == .Pending) return;
    var writer = writer_t{};
    writer.header(51, parent.id);
    writer.put(u64, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 51) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for (&resource_states) |*state| if (state.buffer_references[index / 64] & bit != 0) {
        state.command_state = .Invalid;
        state.buffer_references = [_]u64{0} ** 8;
    };
    resource_state(record).* = .{};
    std.debug.assert(
        c.venus_objects_release(&objects, record.handle, c.VK_OBJECT_TYPE_BUFFER, 0) == c.RingOk,
    );
}
/// Query actual host memory requirements through a validated private buffer parent.
/// @param[in] device Nullable private parent handle, borrowed for call.
/// @param[in] buffer Nullable private device-owned token, no pointer dereference.
/// @param[out] output Nullable borrowed storage, preserved on all errors.
/// @note Allocation-free and mutex serialized; invalid handles ignored.
/// Peer/transport errors poison binding; staged size/alignment/type bits validated.
fn buffer_requirements(
    device: c.VkDevice,
    buffer: c.VkBuffer,
    output: [*c]c.VkMemoryRequirements,
) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (device == null or buffer == null or output == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_BUFFER,
        parent.id,
    ) orelse return;
    const value = query_buffer_requirements(
        parent.id,
        record.id,
        resource_state(record).buffer_size,
    ) orelse return;
    resource_state(record).requirements = value;
    output.* = value;
}
fn query_buffer_requirements(
    device_id: u64,
    buffer_id: u64,
    minimum_size: u64,
) ?c.VkMemoryRequirements {
    var writer = writer_t{};
    writer.header(30, device_id);
    writer.put(u64, buffer_id);
    writer.put(u64, 1);
    const reply = transact(writer.bytes[0..writer.used]) orelse return null;
    var reader = reader_t{ .bytes = reply };
    const value = fixed_value(c.VkMemoryRequirements, &reader, 30) catch {
        _ = failure(c.RingCorrupt);
        return null;
    };
    if (value.size < minimum_size or value.size == 0 or value.alignment == 0 or
        value.alignment & (value.alignment - 1) != 0 or value.memoryTypeBits == 0)
    {
        _ = failure(c.RingCorrupt);
        return null;
    }
    return value;
}
/// Publish a reserved resource only after an exact successful host identity reply.
/// Caller holds mutex; packet is owned scratch, final eight bytes are reserved output identity.
fn create_render_resource(parent: *c.venus_object_t, kind: u32, writer: anytype, output: *u64) c_int {
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(&objects, kind, parent.id, 0, &record) != c.RingOk)
        return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    std.mem.writeInt(u64, writer.bytes[writer.used - 8 ..][0..8], record.*.id, .little);
    const command_id = std.mem.readInt(u32, writer.bytes[0..4], .little);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, command_id, record.*.id, true) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0) return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, record.*.handle, kind, 0);
        return result;
    }
    resource_state(record).* = .{ .id = record.*.id };
    output.* = record.*.handle;
    return c.VK_SUCCESS;
}
/// Create canonical single-color pass. [in] device/info borrowed nonnull; allocator nullable unused.
/// [out] output nonnull, NULL on error; success transfers guest identity until destruction.
/// Returns native result or local invalid/OOM/loss; mutex serialized and allocation-free.
fn create_render_pass(device: c.VkDevice, info: [*c]const c.VkRenderPassCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkRenderPass) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var staged_info = info.*;
    var attachment: c.VkAttachmentDescription = undefined;
    if (staged_info.attachmentCount == 1 and staged_info.pAttachments != null) {
        attachment = staged_info.pAttachments[0];
        if (attachment.initialLayout == c.VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) attachment.initialLayout = c.VK_IMAGE_LAYOUT_GENERAL;
        if (attachment.finalLayout == c.VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) attachment.finalLayout = c.VK_IMAGE_LAYOUT_GENERAL;
        staged_info.pAttachments = &attachment;
    }
    var writer = graphics_wire.create_render_pass(@ptrCast(&staged_info), parent.id, 1) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_RENDER_PASS, &writer, &handle);
    if (result != c.VK_SUCCESS) return result;
    const state = resource_state(child_object(handle, c.VK_OBJECT_TYPE_RENDER_PASS, parent.id).?);
    state.render_format = staged_info.pAttachments[0].format;
    state.render_initial_layout = staged_info.pAttachments[0].initialLayout;
    state.render_final_layout = staged_info.pAttachments[0].finalLayout;
    output.* = @ptrFromInt(handle);
    return c.VK_SUCCESS;
}
/// Destroy quiescent pass. [in] nullable borrowed device/pass/callback tokens.
/// Void; invalid or pending ignored, uncertain native destruction retains identity.
/// Copied pipeline/framebuffer compatibility survives retirement; mutex serialized.
fn destroy_render_pass(device: c.VkDevice, pass: c.VkRenderPass, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (pass) |value| @intFromPtr(value) else 0, c.VK_OBJECT_TYPE_RENDER_PASS, 83);
}

fn framebuffer_attachment(view: *const c.venus_object_t, width: u32, height: u32, format: u32) bool {
    const state = resource_state(view);
    const image = child_object(state.view_image, c.VK_OBJECT_TYPE_IMAGE, view.parent_id) orelse return false;
    const image_state = resource_state(image);
    if (image_state.bound_memory == 0 or image_state.image_type != c.VK_IMAGE_TYPE_2D or
        image_state.image_samples != 1 or state.image_format != format or
        state.image_usage & c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT == 0 or
        (state.view_type != c.VK_IMAGE_VIEW_TYPE_2D and state.view_type != c.VK_IMAGE_VIEW_TYPE_2D_ARRAY)) return false;
    const range = state.view_range;
    if (range.aspectMask != c.VK_IMAGE_ASPECT_COLOR_BIT or !image_range_valid(image_state, range)) return false;
    const levels = if (range.levelCount == std.math.maxInt(u32)) image_state.image_levels - range.baseMipLevel else range.levelCount;
    const layers = if (range.layerCount == std.math.maxInt(u32)) image_state.image_layers - range.baseArrayLayer else range.layerCount;
    if (levels != 1 or layers != 1) return false;
    const components = state.view_components;
    for ([_]u32{ components.r, components.g, components.b, components.a }, 0..) |component, index|
        if (component != c.VK_COMPONENT_SWIZZLE_IDENTITY and component != index + 3) return false;
    const shift: u5 = @intCast(range.baseMipLevel);
    return width <= @max(@as(u32, 1), image_state.image_extent[0] >> shift) and
        height <= @max(@as(u32, 1), image_state.image_extent[1] >> shift);
}
/// Create one-color framebuffer. [in] nonnull device/info/one-view array borrowed; allocator nullable unused.
/// [out] nonnull output NULL on failure, owned guest token on success. Returns host/local invalid/OOM/loss.
/// Resolves bound live image, exact mip/layer, identity components and actual dimensions before reservation.
/// Copies scalar compatibility/view identity; no array retention or heap allocation; mutex serialized.
fn create_framebuffer(device: c.VkDevice, info: [*c]const c.VkFramebufferCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkFramebuffer) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (info.*.sType != c.VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO or info.*.pNext != null or
        info.*.flags != 0 or info.*.renderPass == null or info.*.attachmentCount != 1 or
        info.*.pAttachments == null or info.*.pAttachments[0] == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const pass = child_object(@intFromPtr(info.*.renderPass.?), c.VK_OBJECT_TYPE_RENDER_PASS, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const view = child_object(@intFromPtr(info.*.pAttachments[0].?), c.VK_OBJECT_TYPE_IMAGE_VIEW, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const format = resource_state(pass).render_format;
    if (!framebuffer_attachment(view, info.*.width, info.*.height, format)) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var writer = graphics_wire.create_framebuffer(@ptrCast(info), parent.id, pass.id, view.id, 1) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_FRAMEBUFFER, &writer, &handle);
    if (result != c.VK_SUCCESS) return result;
    const state = resource_state(child_object(handle, c.VK_OBJECT_TYPE_FRAMEBUFFER, parent.id).?);
    state.render_format = format;
    state.framebuffer_view = view.handle;
    state.framebuffer_extent = .{ info.*.width, info.*.height };
    output.* = @ptrFromInt(handle);
    return c.VK_SUCCESS;
}
/// Destroy quiescent framebuffer. [in] nullable borrowed device/framebuffer/callback tokens.
/// Void; invalid/pending ignored. Exact native acknowledgment retires owned token; mutex serialized.
fn destroy_framebuffer(device: c.VkDevice, framebuffer: c.VkFramebuffer, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (framebuffer) |value| @intFromPtr(value) else 0, c.VK_OBJECT_TYPE_FRAMEBUFFER, 81);
}

/// Create a core device-owned image. [in] device/info borrowed nonnull, allocator nullable unused.
/// [out] output nonnull handle storage, NULL on error. Returns host result/local invalid/OOM/loss.
/// Mutex serialized, allocation-free; owns identity until exact host destruction or retired abandon.
fn create_image(device: c.VkDevice, info: [*c]const c.VkImageCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkImage) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const metadata = image_view_native.snapshot(@ptrCast(info)) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = render_wire.create_image(@ptrCast(info), parent.id, 1) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    const dimension = @max(info.*.extent.width, @max(info.*.extent.height, info.*.extent.depth));
    if (info.*.mipLevels > 32 - @clz(dimension) or
        (info.*.samples != 1 and (info.*.imageType != c.VK_IMAGE_TYPE_2D or
        info.*.mipLevels != 1 or info.*.tiling != c.VK_IMAGE_TILING_OPTIMAL))) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (info.*.sharingMode == c.VK_SHARING_MODE_CONCURRENT) {
        const entry = device_cache(parent.handle).?;
        for (info.*.pQueueFamilyIndices[0..info.*.queueFamilyIndexCount]) |family|
            if (std.mem.indexOfScalar(u32, entry.families[0..entry.family_count], family) == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_IMAGE, &writer, &handle);
    if (result != c.VK_SUCCESS) return result;
    const state = resource_state(child_object(handle, c.VK_OBJECT_TYPE_IMAGE, parent.id).?);
    state.image_extent = .{ info.*.extent.width, info.*.extent.height, info.*.extent.depth };
    state.image_samples = info.*.samples;
    state.image_levels = info.*.mipLevels;
    state.image_layers = info.*.arrayLayers;
    state.image_format = info.*.format;
    state.image_type = info.*.imageType;
    state.image_usage = info.*.usage;
    state.image_tiling = info.*.tiling;
    state.image_view_metadata = metadata;
    output.* = @ptrFromInt(handle);
    return c.VK_SUCCESS;
}
fn image_aspects(format: u32) u32 {
    return switch (format) {
        124, 125, 126 => c.VK_IMAGE_ASPECT_DEPTH_BIT,
        127 => c.VK_IMAGE_ASPECT_STENCIL_BIT,
        128, 129, 130 => c.VK_IMAGE_ASPECT_DEPTH_BIT | c.VK_IMAGE_ASPECT_STENCIL_BIT,
        else => c.VK_IMAGE_ASPECT_COLOR_BIT,
    };
}
fn image_range_valid(state: *const resource_state_t, range: c.VkImageSubresourceRange) bool {
    if (range.aspectMask == 0 or range.aspectMask & ~image_aspects(state.image_format) != 0 or
        range.baseMipLevel >= state.image_levels or range.baseArrayLayer >= state.image_layers or
        range.levelCount == 0 or range.layerCount == 0) return false;
    return (range.levelCount == std.math.maxInt(u32) or range.levelCount <= state.image_levels - range.baseMipLevel) and
        (range.layerCount == std.math.maxInt(u32) or range.layerCount <= state.image_layers - range.baseArrayLayer);
}
/// Create a view retaining its image. [in] device/info nonnull borrowed, allocator nullable unused.
/// [out] output nonnull storage, NULL on failure. Returns host result/local invalid/OOM/loss.
/// Mutex serialized, allocation-free; validates owned mutable-format compatibility and cube/subresource ranges.
fn create_image_view(device: c.VkDevice, info: [*c]const c.VkImageViewCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkImageView) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or info.*.image == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const image = child_object(@intFromPtr(info.*.image.?), c.VK_OBJECT_TYPE_IMAGE, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const state = resource_state(image);
    if (state.bound_memory == 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const enabled = device_cache(parent.handle).?.enabled_state.features.core;
    const range = image_view_native.validate(&state.image_view_metadata, @ptrCast(info), enabled[@offsetOf(c.VkPhysicalDeviceFeatures, "imageCubeArray") / 4] != 0) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    var normalized = info.*;
    normalized.subresourceRange = @bitCast(range);
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var writer = render_wire.create_image_view(@ptrCast(&normalized), parent.id, image.id, 1) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_IMAGE_VIEW, &writer, &handle);
    if (result != c.VK_SUCCESS) return result;
    const view_state = resource_state(child_object(handle, c.VK_OBJECT_TYPE_IMAGE_VIEW, parent.id).?);
    view_state.view_image = image.handle;
    view_state.view_type = info.*.viewType;
    view_state.view_range = normalized.subresourceRange;
    view_state.image_format = info.*.format;
    view_state.image_usage = (image_view_native.view_usage(@ptrCast(info)) catch unreachable) orelse state.image_usage;
    view_state.view_components = info.*.components;
    output.* = @ptrFromInt(handle);
    return c.VK_SUCCESS;
}
/// Destroy quiescent rendering resource after exact acknowledgment. Caller holds mutex.
/// Recorded references invalidate only after successful destruction; pending references retain ownership.
fn destroy_render_resource(device: c.VkDevice, handle: u64, kind: u32, command_id: u32) void {
    if (device == null or handle == 0) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(handle, kind, parent.id) orelse return;
    const index = resource_index(record);
    const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
    if (resource_state(record).inflight_count != 0) return;
    for (resource_states) |state| {
        if (kind == c.VK_OBJECT_TYPE_IMAGE and state.view_image == handle) return;
        if (state.command_state == .Pending and state.buffer_references[index / 64] & bit != 0) return;
    }
    var writer = writer_t{};
    writer.header(command_id, parent.id);
    writer.put(u64, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    if (reply.len < 4 or std.mem.readInt(u32, reply[0..4], .little) != command_id) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for (&resource_states) |*state| if (state.buffer_references[index / 64] & bit != 0) {
        state.command_state = .Invalid;
        state.buffer_references = [_]u64{0} ** 8;
    };
    if (kind == c.VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT) std.debug.assert(profiles.release_slot(&profile_registry.descriptor_layouts, resource_state(record).profile_index));
    if (kind == c.VK_OBJECT_TYPE_PIPELINE_LAYOUT) std.debug.assert(profiles.release_slot(&profile_registry.pipeline_layouts, resource_state(record).profile_index));
    if (kind == c.VK_OBJECT_TYPE_PIPELINE) std.debug.assert(profiles.release_slot(&profile_registry.pipelines, resource_state(record).profile_index));
    resource_state(record).* = .{};
    std.debug.assert(c.venus_objects_release(&objects, handle, kind, 0) == c.RingOk);
}
/// Destroy image. [in] device/image nullable borrowed tokens; allocator nullable unused.
/// Void; invalid/pending/live-view resources ignored. Mutex serialized; host loss retains ownership.
fn destroy_image(device: c.VkDevice, image: c.VkImage, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (image) |value| @intFromPtr(value) else 0, c.VK_OBJECT_TYPE_IMAGE, 55);
}
/// Destroy view and release image retention. [in] nullable tokens/callbacks borrowed for call.
/// Void; invalid/pending ignored. Mutex serialized; exact host acknowledgment precedes retirement.
fn destroy_image_view(device: c.VkDevice, view: c.VkImageView, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (view) |value| @intFromPtr(value) else 0, c.VK_OBJECT_TYPE_IMAGE_VIEW, 58);
}
/// Create one core compute pipeline from same-device private shader/layout definitions.
/// [in] device nonnull borrowed; cache must be NULL; count1, infos nonnull borrowed for call.
/// [in] allocator nullable unused; [out] output nonnull count1 writable token storage.
/// Validated count1 failures clear output; unsupported counts leave output untouched.
/// Returns native result/local invalid/OOM/sticky loss; mutex serialized, allocation-free.
/// The successful pipeline owns a copied layout profile; shader/layout tokens may then retire.
fn create_compute_pipelines(device: c.VkDevice, pipeline_cache: c.VkPipelineCache, count: u32, infos: [*c]const c.VkComputePipelineCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkPipeline) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    // Unsupported counts are rejected before accessing caller arrays or output extents.
    if (count != 1) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or infos == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const info = &infos[0];
    if (info.layout == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const cache_id = if (pipeline_cache) |handle| (child_object(@intFromPtr(handle), c.VK_OBJECT_TYPE_PIPELINE_CACHE, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED).id else 0;
    var stages: pipeline_stage_inputs_t = .{};
    defer release_pipeline_stages(device, &stages);
    const stage_result = resolve_pipeline_stages(device, parent.id, @ptrCast(&info.stage), 1, &stages);
    if (stage_result != c.VK_SUCCESS) return stage_result;
    var normalized = info.*;
    normalized.stage = stages.stages[0];
    const layout = child_object(@intFromPtr(info.layout.?), c.VK_OBJECT_TYPE_PIPELINE_LAYOUT, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var writer = render_wire.create_compute_pipeline_cached(@ptrCast(&normalized), parent.id, cache_id, stages.ids[0], layout.id, 1) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    const profile = profiles.get_profile(&profile_registry.pipeline_layouts, resource_state(layout).profile_index).?.*;
    const index = profiles.reserve_slot(&profile_registry.pipelines, profile) catch return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_PIPELINE, &writer, &handle);
    if (result != c.VK_SUCCESS) {
        if (lost == c.RingOk) std.debug.assert(profiles.release_slot(&profile_registry.pipelines, index));
        return result;
    }
    const state = resource_state(child_object(handle, c.VK_OBJECT_TYPE_PIPELINE, parent.id).?);
    state.profile_index = index;
    state.pipeline_bind_point = 1;
    output.* = @ptrFromInt(handle);
    return c.VK_SUCCESS;
}
/// Create one canonical vertexless graphics pipeline. [in] device/info borrowed nonnull;
/// count must1 and cache null; allocator nullable unused. [out] output nonnull NULL on failure.
/// Success owns guest identity and copied empty layout/pass definitions until retirement.
/// Returns native/local invalid/OOM/loss; fixed shared64-pipeline quota, mutex serialized.

/// Destroy a quiescent pipeline. [in] nullable private tokens/callbacks borrowed for call.
/// Void; pending resources remain owned. Exact host acknowledgment retires metadata and identity.
/// Mutex serialized, no allocation; destroying recorded pipeline invalidates affected commands.
fn destroy_pipeline(device: c.VkDevice, pipeline: c.VkPipeline, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (pipeline) |value| @intFromPtr(value) else 0, c.VK_OBJECT_TYPE_PIPELINE, 67);
}

/// Query actual image requirements. [in] nullable private tokens borrowed; [out] nullable storage.
/// Void; output preserved on failure. Mutex serialized, allocation-free; malformed host reply poisons binding.
fn image_requirements(device: c.VkDevice, image: c.VkImage, output: [*c]c.VkMemoryRequirements) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (device == null or image == null or output == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(@intFromPtr(image.?), c.VK_OBJECT_TYPE_IMAGE, parent.id) orelse return;
    const value = query_image_requirements(parent.id, record.id) orelse return;
    resource_state(record).requirements = value;
    output.* = value;
}
fn query_image_requirements(device_id: u64, image_id: u64) ?c.VkMemoryRequirements {
    var writer = writer_t{};
    writer.header(31, device_id);
    writer.put(u64, image_id);
    writer.put(u64, 1);
    const reply = transact(writer.bytes[0..writer.used]) orelse return null;
    var reader = reader_t{ .bytes = reply };
    const value = fixed_value(c.VkMemoryRequirements, &reader, 31) catch {
        _ = failure(c.RingCorrupt);
        return null;
    };
    if (value.size == 0 or value.alignment == 0 or value.alignment & (value.alignment - 1) != 0 or value.memoryTypeBits == 0) {
        _ = failure(c.RingCorrupt);
        return null;
    }
    return value;
}
/// Bind image memory. [in] nonnull same-device image/memory tokens borrowed; offset checked/aligned.
/// Returns host result/local invalid/loss; mutex serialized, allocation-free, relationship published on success.
fn bind_image_memory(device: c.VkDevice, image: c.VkImage, memory_handle: c.VkDeviceMemory, offset: u64) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (device == null or image == null or memory_handle == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = child_object(@intFromPtr(image.?), c.VK_OBJECT_TYPE_IMAGE, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const memory_record = child_object(@intFromPtr(memory_handle.?), c.VK_OBJECT_TYPE_DEVICE_MEMORY, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const state = resource_state(record);
    const allocation = resource_state(memory_record);
    if (state.bound_memory != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (state.requirements.size == 0) state.requirements = query_image_requirements(parent.id, record.id) orelse return c.VK_ERROR_DEVICE_LOST;
    const requirements = state.requirements;
    if (requirements.memoryTypeBits & (@as(u32, 1) << @as(u5, @intCast(allocation.type_index))) == 0 or
        offset % requirements.alignment != 0 or offset > allocation.allocation_size or requirements.size > allocation.allocation_size - offset)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = writer_t{};
    writer.header(29, parent.id);
    writer.put(u64, record.id);
    writer.put(u64, memory_record.id);
    writer.put(u64, offset);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 29, 0);
    if (result == c.VK_SUCCESS) {
        state.bound_memory = memory_record.handle;
        state.memory_offset = offset;
    }
    return result;
}
/// Create bounded core shader module. [in] device/info nonnull borrowed, callbacks nullable unused.
/// [out] output nonnull, NULL on failure. Returns host/local invalid/limit/OOM/loss.
/// Mutex serialized, allocation-free; host owns semantic SPIR-V validation and execution.
fn create_shader_module(device: c.VkDevice, info: [*c]const c.VkShaderModuleCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkShaderModule) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var writer = shader_wire.create_shader(@ptrCast(info), parent.id, 1) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_SHADER_MODULE, &writer, &handle);
    if (result == c.VK_SUCCESS) output.* = @ptrFromInt(handle);
    return result;
}
/// Destroy shader token. [in] nullable device/module/callbacks borrowed for call.
/// Void; invalid/inflight ignored. Mutex serialized, exact acknowledgment retires ownership.
fn destroy_shader_module(device: c.VkDevice, shader: c.VkShaderModule, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (shader) |value| @intFromPtr(value) else 0, c.VK_OBJECT_TYPE_SHADER_MODULE, 60);
}

/// Create copied descriptor-layout metadata. [in] nullable device/info/callbacks borrowed.
/// [out] output nonnull token storage, NULL on error. Returns native/local invalid/OOM/loss.
/// Mutex serialized; fixed slot owns snapshot until exact native retirement or receiver abandon.

/// Destroy descriptor layout token; copied dependent definitions impose no retention.
/// [in] nullable borrowed tokens/callbacks. Void; invalid/pending ignored, no allocations.
/// Mutex serialized; fixed snapshot released only after exact native acknowledgment.
fn destroy_descriptor_layout(device: c.VkDevice, layout: c.VkDescriptorSetLayout, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (layout) |value| @intFromPtr(value) else 0, c.VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, 73);
}
/// Create a pipeline layout with copied structural set/push definitions.
/// [in] nullable borrowed native inputs, original layout tokens translated and not retained.
/// [out] output nonnull, NULL on failure. Returns native/local invalid/OOM/loss.
/// Mutex serialized; fixed profile ownership ends after native retirement or receiver abandon.
fn create_pipeline_layout(device: c.VkDevice, info: [*c]const c.VkPipelineLayoutCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkPipelineLayout) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (info.*.sType != c.VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO or info.*.pNext != null or info.*.flags != 0 or
        info.*.setLayoutCount > profiles.MaxSets or (info.*.setLayoutCount != 0 and info.*.pSetLayouts == null) or
        info.*.pushConstantRangeCount > profiles.MaxPushRanges or
        (info.*.pushConstantRangeCount != 0 and info.*.pPushConstantRanges == null)) return c.VK_ERROR_INITIALIZATION_FAILED;
    var ids: [profiles.MaxSets]u64 = undefined;
    var layouts: [profiles.MaxSets]profiles.descriptor_layout_t = undefined;
    if (info.*.setLayoutCount != 0) for (info.*.pSetLayouts[0..info.*.setLayoutCount], 0..) |layout, index| {
        if (layout == null) return c.VK_ERROR_INITIALIZATION_FAILED;
        const record = child_object(@intFromPtr(layout.?), c.VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
        ids[index] = record.id;
        layouts[index] = (profiles.get_profile(&profile_registry.descriptor_layouts, resource_state(record).profile_index) orelse unreachable).*;
    };
    var writer = render_wire.create_pipeline_layout(@ptrCast(info), ids[0..info.*.setLayoutCount], parent.id, 1) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    if (info.*.pushConstantRangeCount != 0) {
        if (!ensure_descriptor_limits(parent)) return if (lost != c.RingOk) c.VK_ERROR_DEVICE_LOST else c.VK_ERROR_INITIALIZATION_FAILED;
        const maximum = @min(device_cache(parent.handle).?.max_push_bytes, profiles.MaxPushBytes);
        for (info.*.pPushConstantRanges[0..info.*.pushConstantRangeCount]) |range| {
            if (range.offset >= maximum or range.size > maximum - range.offset) return c.VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    var ranges: [profiles.MaxPushRanges]profiles.push_range_t = undefined;
    if (info.*.pushConstantRangeCount != 0) for (info.*.pPushConstantRanges[0..info.*.pushConstantRangeCount], 0..) |range, index| {
        ranges[index] = .{ .stage_flags = range.stageFlags, .offset = range.offset, .size = range.size };
    };
    const profile = profiles.normalize_pipeline(layouts[0..info.*.setLayoutCount], ranges[0..info.*.pushConstantRangeCount]) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    const index = profiles.reserve_slot(&profile_registry.pipeline_layouts, profile) catch return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_PIPELINE_LAYOUT, &writer, &handle);
    if (result != c.VK_SUCCESS) {
        if (lost == c.RingOk) std.debug.assert(profiles.release_slot(&profile_registry.pipeline_layouts, index));
        return result;
    }
    resource_state(child_object(handle, c.VK_OBJECT_TYPE_PIPELINE_LAYOUT, parent.id).?).profile_index = index;
    output.* = @ptrFromInt(handle);
    return c.VK_SUCCESS;
}
/// Destroy copied pipeline layout owner. [in] nullable borrowed tokens/callbacks.
/// Void; invalid/pending ignored. Mutex serialized; acknowledgment precedes slot scrubbing.
fn destroy_pipeline_layout(device: c.VkDevice, layout: c.VkPipelineLayout, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (layout) |value| @intFromPtr(value) else 0, c.VK_OBJECT_TYPE_PIPELINE_LAYOUT, 69);
}

/// Create fixed-quota pool metadata. [in] nullable borrowed device/info/callbacks.
/// [out] output nonnull, NULL on failure. Returns native/local invalid/OOM/loss.
/// Mutex serialized; owns native identity and quotas until destruction or retired abandon.

/// Reset pool and all sets only after exact native success. [in] nullable borrowed private tokens.
/// flags must0. Returns native/local invalid/loss; pending sets prohibit reset.
/// Mutex serialized, allocation-free; successful retirement scrubs profiles and refunds all quotas.
fn reset_descriptor_pool(device: c.VkDevice, pool_handle: c.VkDescriptorPool, flags: u32) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (device == null or pool_handle == null or flags != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const pool = child_object(@intFromPtr(pool_handle.?), c.VK_OBJECT_TYPE_DESCRIPTOR_POOL, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (!descriptor_pool_idle(pool)) return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = writer_t{};
    writer.header(76, parent.id);
    writer.put(u64, pool.id);
    writer.put(u32, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 76, 0);
    if (result == c.VK_SUCCESS) retire_pool_sets(pool);
    return result;
}
/// Destroy quiescent pool and implicit child sets. [in] nullable borrowed tokens/callbacks.
/// Void; invalid/pending ignored. Mutex serialized; prefix acknowledgment precedes every release.
fn destroy_descriptor_pool(device: c.VkDevice, pool_handle: c.VkDescriptorPool, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (device == null or pool_handle == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const pool = child_object(@intFromPtr(pool_handle.?), c.VK_OBJECT_TYPE_DESCRIPTOR_POOL, parent.id) orelse return;
    if (!descriptor_pool_idle(pool)) return;
    var writer = writer_t{};
    writer.header(75, parent.id);
    writer.put(u64, pool.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    if (reply.len < 4 or std.mem.readInt(u32, reply[0..4], .little) != 75) {
        _ = failure(c.RingCorrupt);
        return;
    }
    retire_pool_sets(pool);
    resource_state(pool).* = .{};
    std.debug.assert(c.venus_objects_release(&objects, pool.handle, c.VK_OBJECT_TYPE_DESCRIPTOR_POOL, 0) == c.RingOk);
}
fn descriptor_sets_reply(bytes: []const u8, ids: []const u64) !c_int {
    var reader = reader_t{ .bytes = bytes };
    if (try reader.scalar(u32) != 77) return error.Value;
    const result = try reader.scalar(i32);
    if (result > 0 or try reader.scalar(u64) != ids.len) return error.Value;
    for (ids) |id| {
        const received = try reader.scalar(u64);
        if (received != id and (result == 0 or received != 0)) return error.Value;
    }
    return result;
}
fn rollback_descriptor_sets(records: []const *c.venus_object_t) void {
    for (records) |record| {
        const state = resource_state(record);
        std.debug.assert(profiles.release_slot(&profile_registry.sets, state.profile_index));
        state.* = .{};
        std.debug.assert(c.venus_objects_release(&objects, record.handle, c.VK_OBJECT_TYPE_DESCRIPTOR_SET, 0) == c.RingOk);
    }
}
/// Allocate a transactional batch of pool-owned sets with copied layout definitions.
/// [in] nonnull borrowed device/info; native accessible arrays1..64, no chain.
/// [out] output nonnull handles[count], cleared for bounded failures; count>64 preserves storage.
/// Returns native/local invalid/OOM/loss. Mutex serialized, allocation-free; uncertain IDs retained until abandon.

/// Free a whole validated pool-owned batch after native success. [in] borrowed nullable tokens/array.
/// count0 is a no-op;1..64 requires accessible nonnull immutable sets and FREE_SET pool flag.
/// Returns native/local invalid/loss. Mutex serialized; duplicates/foreign/pending reject before native work.
fn free_descriptor_sets(device: c.VkDevice, pool_handle: c.VkDescriptorPool, count: u32, handles: [*c]const c.VkDescriptorSet) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (count == 0) return c.VK_SUCCESS;
    if (count > 64 or device == null or pool_handle == null or handles == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const pool = child_object(@intFromPtr(pool_handle.?), c.VK_OBJECT_TYPE_DESCRIPTOR_POOL, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (resource_state(pool).pool_flags & 1 == 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    var records: [64]*c.venus_object_t = undefined;
    var writer = writer_t{};
    writer.header(78, parent.id);
    writer.put(u64, pool.id);
    writer.put(u32, count);
    writer.put(u64, count);
    for (handles[0..count], 0..) |handle, index| {
        const record = descriptor_set_for(handle, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
        if (record.parent_id != pool.id or !descriptor_set_idle(record)) return c.VK_ERROR_INITIALIZATION_FAILED;
        for (records[0..index]) |previous| if (previous.id == record.id) return c.VK_ERROR_INITIALIZATION_FAILED;
        records[index] = record;
        writer.put(u64, record.id);
    }
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 78, 0);
    if (result == c.VK_SUCCESS) for (records[0..count]) |record| retire_descriptor_set(record, pool);
    return result;
}

// Called with the global mutex; cache only a fully validated host properties reply.
fn ensure_descriptor_limits(parent: *const c.venus_object_t) bool {
    const entry = device_cache(parent.handle) orelse return false;
    if (entry.descriptor_limits_ready) return true;
    var physical: c.VkPhysicalDevice = null;
    for (slots) |slot| if (slot.id == parent.parent_id and slot.kind == c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) {
        physical = @ptrFromInt(slot.handle);
        break;
    };
    const value = raw_properties(physical) orelse return false;
    const alignments = [_]u64{ value.limits.minUniformBufferOffsetAlignment, value.limits.minStorageBufferOffsetAlignment };
    const ranges = [_]u32{ value.limits.maxUniformBufferRange, value.limits.maxStorageBufferRange };
    for (alignments, ranges) |alignment, range| if (alignment == 0 or alignment & (alignment - 1) != 0 or range == 0) {
        _ = failure(c.RingCorrupt);
        return false;
    };
    for (value.limits.maxComputeWorkGroupCount) |limit| if (limit == 0) {
        _ = failure(c.RingCorrupt);
        return false;
    };
    if (value.limits.maxPushConstantsSize < 128) {
        _ = failure(c.RingCorrupt);
        return false;
    }
    entry.max_push_bytes = value.limits.maxPushConstantsSize;
    entry.descriptor_alignments = alignments;
    entry.descriptor_ranges = ranges;
    entry.compute_group_limits = value.limits.maxComputeWorkGroupCount;
    entry.descriptor_limits_ready = true;
    return true;
}
// Caller validates/caches host limits first; checks copied token against its current resource owner.
fn descriptor_buffer_valid(parent: *const c.venus_object_t, descriptor: *const profiles.descriptor_t) bool {
    if (descriptor.descriptor_type < 6 or descriptor.descriptor_type > 9 or descriptor.buffer == 0) return false;
    const record = child_object(descriptor.buffer, c.VK_OBJECT_TYPE_BUFFER, parent.id) orelse return false;
    const state = resource_state(record);
    const uniform = descriptor.descriptor_type == 6 or descriptor.descriptor_type == 8;
    const kind: usize = if (uniform) 0 else 1;
    const usage: u32 = if (uniform) c.VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT else c.VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (state.bound_memory == 0 or state.buffer_usage & usage == 0 or descriptor.offset >= state.buffer_size or descriptor.range == 0) return false;
    const extent = if (descriptor.range == std.math.maxInt(u64)) state.buffer_size - descriptor.offset else descriptor.range;
    const entry = device_cache(parent.handle).?;
    std.debug.assert(entry.descriptor_limits_ready);
    return descriptor.offset % entry.descriptor_alignments[kind] == 0 and extent <= state.buffer_size - descriptor.offset and extent <= entry.descriptor_ranges[kind];
}
fn descriptor_span(profile: *profiles.descriptor_set_t, binding: u32, first: u32, count: u32) ?[]profiles.descriptor_t {
    if (count == 0 or count > 64) return null;
    for (profile.descriptors[0..profile.descriptor_count], 0..) |descriptor, index| if (descriptor.binding == binding and descriptor.array_element == first) {
        if (count > profile.descriptor_count - index) return null;
        const span = profile.descriptors[index..][0..count];
        for (span, 0..) |element, offset| if (element.binding != binding or element.array_element != @as(u64, first) + offset) return null;
        return span;
    };
    return null;
}
/// Transactionally update buffer descriptors/copies. [in] nullable device, bounded borrowed immutable arrays.
/// counts0 permit null; maximum64 operations,64 elements/write. Void; malformed/unsupported calls ignored.
/// Mutex serialized; fixed scrubbed staging owns no input pointers, metadata publishes after native prefix ack.
/// Writes validate actual limits, resource usage/bounds/parent. Pending destinations reject; copy sources may be pending/undefined.
/// Core destination updates invalidate referencing recording/executable commands after acknowledgment.
/// Resolve one bounded owned descriptor element; sparse growth stays in staging.
fn descriptor_element(profile: *profiles.descriptor_set_t, binding: u32, element: u32) ?*profiles.descriptor_t {
    if (profile.sparse) return profiles.sparse_element(profile,binding,element) catch null;
    for (profile.descriptors[0..profile.descriptor_count]) |*value| if (value.binding == binding and value.array_element == element) return value;
    return null;
}
fn null_descriptor_enabled(parent: *const c.venus_object_t) bool { return device_feature(parent,c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,2); }
/// Validate mixed named identities without retaining or publishing resource state.
fn descriptor_resource_valid(parent: *const c.venus_object_t, value: *const profiles.descriptor_t) bool {
    if (value.descriptor_type >= 6 and value.descriptor_type <= 9) return if (value.buffer == 0) null_descriptor_enabled(parent) else descriptor_buffer_valid(parent,value);
    if (value.descriptor_type == 4 or value.descriptor_type == 5) {
        if (value.texel_view == 0) return null_descriptor_enabled(parent);
        const view = child_object(value.texel_view,c.VK_OBJECT_TYPE_BUFFER_VIEW,parent.id) orelse return false;
        const buffer = child_object(resource_state(view).view_image,c.VK_OBJECT_TYPE_BUFFER,parent.id) orelse return false;
        return resource_state(buffer).bound_memory != 0;
    }
    if (value.descriptor_type > 10) return false;
    if (value.descriptor_type <= 1 and value.sampler != 0 and child_object(value.sampler,c.VK_OBJECT_TYPE_SAMPLER,parent.id) == null) return false;
    if (value.descriptor_type == 0) return value.sampler != 0;
    if (value.descriptor_type == 1 and value.sampler == 0) return false;
    if (value.image_view == 0) return value.descriptor_type != 10 and null_descriptor_enabled(parent);
    const view = child_object(value.image_view,c.VK_OBJECT_TYPE_IMAGE_VIEW,parent.id) orelse return false;
    const image = child_object(resource_state(view).view_image,c.VK_OBJECT_TYPE_IMAGE,parent.id) orelse return false;
    const usage: u32 = switch (value.descriptor_type) { 1,2 => c.VK_IMAGE_USAGE_SAMPLED_BIT, 3=>c.VK_IMAGE_USAGE_STORAGE_BIT, 10=>c.VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, else=>return false };
    return resource_state(image).bound_memory != 0 and resource_state(view).image_usage & usage != 0 and
        (value.image_layout == c.VK_IMAGE_LAYOUT_GENERAL or (value.descriptor_type != 3 and (value.image_layout == c.VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL or value.image_layout == c.VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)));
}
/// Transactional mixed image/texel/buffer descriptor writes and copies. Caller
/// arrays borrowed until return; fixed scrubbed staging owns copied values only.
/// Native ACK publishes metadata. Invalid/local quota failures preserve all sets.
fn update_descriptor_sets(device: c.VkDevice, write_count: u32, writes: [*c]const c.VkWriteDescriptorSet, copy_count: u32, copies: [*c]const c.VkCopyDescriptorSet) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    defer @memset(&descriptor_update_snapshots,.{});
    defer @memset(std.mem.asBytes(&descriptor_wire_buffers),0);
    defer @memset(std.mem.asBytes(&descriptor_wire_images),0);
    defer @memset(std.mem.asBytes(&descriptor_wire_texels),0);
    if (device == null or lost != c.RingOk or write_count > 64 or copy_count > 64 or (write_count != 0 and writes == null) or (copy_count != 0 and copies == null) or (write_count == 0 and copy_count == 0)) return;
    const parent = object(@intFromPtr(device.?),c.VK_OBJECT_TYPE_DEVICE) orelse return;
    if (!ensure_descriptor_limits(parent)) return;
    for (profile_registry.sets,&descriptor_update_snapshots) |entry,*snapshot| snapshot.* = entry.profile;
    var touched = [_]bool{false} ** 128;
    var ordinary = [_]bool{false} ** 128;
    var encoded_writes: [64]sampler_descriptors.write_t = undefined;
    var encoded_copies: [64]descriptor_wire.copy_t = undefined;
    if (write_count != 0) for (writes[0..write_count],0..) |write,index| {
        if (write.sType != c.VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET or write.pNext != null or write.descriptorType > 10 or write.descriptorCount == 0 or write.descriptorCount > 128 or write.dstArrayElement > std.math.maxInt(u32)-write.descriptorCount) return;
        const record = descriptor_set_for(write.dstSet,parent.id) orelse return;
        const slot = resource_state(record).profile_index-1;
        const snapshot = &descriptor_update_snapshots[slot];
        const flags = descriptor_binding_flags(snapshot,write.dstBinding) orelse return;
        const mutable = flags & (c.VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | c.VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT) != 0;
        if (!descriptor_set_idle(record) and !mutable) return;
        ordinary[slot] = ordinary[slot] or !mutable;
        var encoded = sampler_descriptors.write_t{ .set=record.id,.binding=write.dstBinding,.element=write.dstArrayElement,.descriptor_type=write.descriptorType };
        const image_family = write.descriptorType <= 3 or write.descriptorType == 10;
        const texel_family = write.descriptorType == 4 or write.descriptorType == 5;
        if ((image_family and write.pImageInfo == null) or (texel_family and write.pTexelBufferView == null) or (!image_family and !texel_family and write.pBufferInfo == null)) return;
        for (0..write.descriptorCount) |element| {
            const target = descriptor_element(snapshot,write.dstBinding,write.dstArrayElement+@as(u32,@intCast(element))) orelse return;
            if (target.descriptor_type != write.descriptorType) return;
            var value = profiles.descriptor_t{ .binding=target.binding,.array_element=target.array_element,.descriptor_type=target.descriptor_type };
            if (image_family) {
                const input = write.pImageInfo[element];
                value.sampler=if(write.descriptorType<=1 and input.sampler!=null) @intFromPtr(input.sampler.?) else 0;
                value.image_view=if(write.descriptorType!=0 and input.imageView!=null) @intFromPtr(input.imageView.?) else 0;
                value.image_layout=if(write.descriptorType!=0) input.imageLayout else 0;
                for (snapshot.layout.bindings[0..snapshot.layout.binding_count]) |binding| if (binding.binding == write.dstBinding and binding.immutable_count != 0) {
                    if (value.array_element >= binding.immutable_count) return;
                    value.sampler=snapshot.layout.immutable_samplers[@as(usize,binding.immutable_offset)+value.array_element];
                };
                if (!descriptor_resource_valid(parent,&value)) return;
                descriptor_wire_images[index][element]=.{ .sampler=if(value.sampler!=0) (child_object(value.sampler,c.VK_OBJECT_TYPE_SAMPLER,parent.id) orelse return).id else 0,.view=if(value.image_view!=0) (child_object(value.image_view,c.VK_OBJECT_TYPE_IMAGE_VIEW,parent.id) orelse return).id else 0,.layout=value.image_layout };
            } else if (texel_family) {
                value.texel_view=if(write.pTexelBufferView[element])|handle| @intFromPtr(handle) else 0;
                if (!descriptor_resource_valid(parent,&value)) return;
                descriptor_wire_texels[index][element]=if(value.texel_view!=0) (child_object(value.texel_view,c.VK_OBJECT_TYPE_BUFFER_VIEW,parent.id) orelse return).id else 0;
            } else {
                const input=write.pBufferInfo[element]; value.buffer=if(input.buffer)|handle| @intFromPtr(handle) else 0; value.offset=input.offset; value.range=input.range;
                if (!descriptor_resource_valid(parent,&value)) return;
                descriptor_wire_buffers[index][element]=.{ .buffer_id=if(value.buffer!=0) child_object(value.buffer,c.VK_OBJECT_TYPE_BUFFER,parent.id).?.id else 0,.offset=value.offset,.range=value.range };
            }
            target.*=value;
        }
        if(image_family) encoded.images=descriptor_wire_images[index][0..write.descriptorCount] else if(texel_family) encoded.texels=descriptor_wire_texels[index][0..write.descriptorCount] else encoded.buffers=descriptor_wire_buffers[index][0..write.descriptorCount];
        encoded_writes[index]=encoded; touched[slot]=true;
    };
    if(copy_count!=0) for(copies[0..copy_count],0..) |copy,index| {
        if(copy.sType!=c.VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET or copy.pNext!=null or copy.descriptorCount==0 or copy.descriptorCount>128 or copy.srcArrayElement>std.math.maxInt(u32)-copy.descriptorCount or copy.dstArrayElement>std.math.maxInt(u32)-copy.descriptorCount) return;
        const source=descriptor_set_for(copy.srcSet,parent.id) orelse return; const destination=descriptor_set_for(copy.dstSet,parent.id) orelse return;
        const destination_profile = &descriptor_update_snapshots[resource_state(destination).profile_index-1];
        const flags = descriptor_binding_flags(destination_profile,copy.dstBinding) orelse return;
        const mutable = flags & (c.VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | c.VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT) != 0;
        if(!descriptor_set_idle(destination) and !mutable) return;
        ordinary[resource_state(destination).profile_index-1] = ordinary[resource_state(destination).profile_index-1] or !mutable;
        if(source.id==destination.id and copy.srcBinding==copy.dstBinding and @as(u64,copy.srcArrayElement)<@as(u64,copy.dstArrayElement)+copy.descriptorCount and @as(u64,copy.dstArrayElement)<@as(u64,copy.srcArrayElement)+copy.descriptorCount) return;
        const src=&descriptor_update_snapshots[resource_state(source).profile_index-1]; const dst=&descriptor_update_snapshots[resource_state(destination).profile_index-1];
        for(0..copy.descriptorCount) |element| {
            const value=(descriptor_element(src,copy.srcBinding,copy.srcArrayElement+@as(u32,@intCast(element))) orelse return).*;
            const target=descriptor_element(dst,copy.dstBinding,copy.dstArrayElement+@as(u32,@intCast(element))) orelse return;
            if(target.descriptor_type!=value.descriptor_type) return;
            const binding=target.binding; const array_element=target.array_element;
            target.*=if (descriptor_resource_valid(parent,&value)) value else .{ .descriptor_type = value.descriptor_type };
            target.binding=binding; target.array_element=array_element;
        }
        touched[resource_state(destination).profile_index-1]=true;
        encoded_copies[index]=.{.source_set=source.id,.source_binding=copy.srcBinding,.source_element=copy.srcArrayElement,.destination_set=destination.id,.destination_binding=copy.dstBinding,.destination_element=copy.dstArrayElement,.count=copy.descriptorCount};
    };
    const writer=sampler_descriptors.update_sets(parent.id,encoded_writes[0..write_count],encoded_copies[0..copy_count]) catch return;
    const reply=transact(writer.bytes[0..writer.used]) orelse return;
    if(reply.len<4 or std.mem.readInt(u32,reply[0..4],.little)!=79) { _=failure(c.RingCorrupt); return; }
    for (touched, 0..) |modified, index| if (modified) {
        profile_registry.sets[index].profile = descriptor_update_snapshots[index];
        if (!ordinary[index]) {
            for (&slots) |*slot| if (slot.id != 0 and slot.kind == c.VK_OBJECT_TYPE_DESCRIPTOR_SET and resource_state(slot).profile_index == index + 1) {
                retain_pending_descriptor_update(parent,slot,&profile_registry.sets[index].profile);
            };
            continue;
        }
        for (slots, 0..) |slot, resource_slot| if (slot.id != 0 and slot.kind == c.VK_OBJECT_TYPE_DESCRIPTOR_SET and resource_states[resource_slot].profile_index == index + 1) {
            const bit = @as(u64, 1) << @as(u6, @intCast(resource_slot % 64));
            for (&resource_states) |*recording| if ((recording.command_state == .Recording or recording.command_state == .Executable) and recording.buffer_references[resource_slot / 64] & bit != 0) {
                recording.command_state = .Invalid;
                recording.buffer_references = [_]u64{0} ** 8;
            };
        };
    };
}

/// Allocate private device memory with exact host identity validation.
/// @param[in] device Nonnull private live parent, borrowed for call.
/// @param[in] info Nonnull canonical allocation info, borrowed; no pNext supported.
/// @param[in] allocator Nullable unused callbacks, no pointer retained.
/// @param[out] output Nonnull borrowed token storage; NULL on failure.
/// @return Host result, local initialization/exhaustion or sticky device loss.
/// @note Mutex serialized, no local allocation; owns host memory until validated free.
fn allocate_memory(
    device: c.VkDevice,
    info: [*c]const c.VkMemoryAllocateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkDeviceMemory,
) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or
        info.*.sType != c.VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO or
        info.*.allocationSize == 0 or info.*.memoryTypeIndex >= 32) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    var flags: ?extra_wire.allocation_flags_t = null;
    var dedicated: ?extra_wire.dedicated_t = null;
    var next = info.*.pNext;
    var seen: [64]usize = undefined;
    var node_count: usize = 0;
    while (next) |pointer| {
        const address = @intFromPtr(pointer);
        if (node_count == seen.len or address % @alignOf(c.VkBaseInStructure) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
        for (seen[0..node_count]) |prior| if (prior == address) return c.VK_ERROR_INITIALIZATION_FAILED;
        seen[node_count] = address; node_count += 1;
        const header: *const c.VkBaseInStructure = @ptrCast(@alignCast(pointer));
        switch (header.sType) {
            c.VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO => {
                if (flags != null or address % @alignOf(c.VkMemoryAllocateFlagsInfo) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
                const value: *const c.VkMemoryAllocateFlagsInfo = @ptrCast(@alignCast(pointer));
                flags = .{ .flags = value.flags, .device_mask = value.deviceMask };
            },
            c.VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO => {
                if (dedicated != null or address % @alignOf(c.VkMemoryDedicatedAllocateInfo) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
                const value: *const c.VkMemoryDedicatedAllocateInfo = @ptrCast(@alignCast(pointer));
                var ids = extra_wire.dedicated_t{ .image = 0, .buffer = 0 };
                if (value.image) |image| ids.image = (child_object(@intFromPtr(image), c.VK_OBJECT_TYPE_IMAGE, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED).id;
                if (value.buffer) |buffer| ids.buffer = (child_object(@intFromPtr(buffer), c.VK_OBJECT_TYPE_BUFFER, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED).id;
                dedicated = ids;
            },
            else => return c.VK_ERROR_INITIALIZATION_FAILED,
        }
        next = @ptrCast(header.pNext);
    }
    if (flags) |value| { if (value.flags & c.VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT != 0 and !device_address_enabled(parent)) return c.VK_ERROR_FEATURE_NOT_PRESENT; }
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_DEVICE_MEMORY,
        parent.id,
        0,
        &record,
    ) != c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    const writer = extra_wire.allocate_memory(parent.id, record.*.id, info.*.allocationSize, info.*.memoryTypeIndex, flags, dedicated) catch {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_DEVICE_MEMORY, 0);
        return c.VK_ERROR_INITIALIZATION_FAILED;
    };
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, 21, record.*.id, true) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0) return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_DEVICE_MEMORY, 0);
        return result;
    }
    resource_state(record).* = .{
        .id = record.*.id,
        .allocation_size = info.*.allocationSize,
        .allocation_flags = if (flags) |value| value.flags else 0,
        .type_index = info.*.memoryTypeIndex,
    };
    output.* = @ptrFromInt(record.*.handle);
    return c.VK_SUCCESS;
}
/// Free memory after all private bound buffers and GPU uses are retired.
/// @param[in] device Nullable private parent, borrowed; invalid handle ignored.
/// @param[in] memory_handle Nullable private token; consumed only after exact host free.
/// @param[in] allocator Nullable unused callbacks; no pointer retained.
/// @note Mutex serialized, allocation-free. Loss retains ownership until abandonment.
fn free_memory(
    device: c.VkDevice,
    memory_handle: c.VkDeviceMemory,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (device == null or memory_handle == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(
        @intFromPtr(memory_handle.?),
        c.VK_OBJECT_TYPE_DEVICE_MEMORY,
        parent.id,
    ) orelse return;
    for (resource_states) |state| if (state.bound_memory == record.handle) return;
    const state = resource_state(record);
    if (state.mapping_resource != 0) {
        var request = std.mem.zeroes(c.venus_request_t);
        request.kind = c.RequestFree;
        request.resource_id = state.mapping_resource;
        if (mapping_exchange(&request, null, 0, null, 0) != c.RingOk) return;
        mapping_slots[state.mapping_resource - 2] = false;
        state.mapping_resource = 0;
    }
    release_shadow(state);
    var writer = writer_t{};
    writer.header(22, parent.id);
    writer.put(u64, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 22) {
        _ = failure(c.RingCorrupt);
        return;
    }
    resource_state(record).* = .{};
    std.debug.assert(
        c.venus_objects_release(
            &objects,
            record.handle,
            c.VK_OBJECT_TYPE_DEVICE_MEMORY,
            0,
        ) == c.RingOk,
    );
}
fn release_shadow(state: *resource_state_t) void {
    if (state.mapped_bytes) |bytes| MappingAllocator.free(bytes);
    if (state.mapped_baseline) |baseline| MappingAllocator.free(baseline);
    state.mapped_baseline = null;
    state.mapped_bytes = null;
    state.mapped_offset = 0;
    state.mapped_size = 0;
}
fn mapping_exchange(request: *const c.venus_request_t, input: ?*const anyopaque, length: usize, output: ?*anyopaque, capacity: usize) c_int {
    if (lost != c.RingOk or command.exchange == null) return c.RingClosed;
    var response = std.mem.zeroes(c.venus_request_t);
    const status = command.exchange.?(command.context, request, input, length, &response, output, capacity);
    if (status != c.RingOk) {
        if (status != c.RingInvalid and status != c.RingLimit and status != c.RingAgain)
            _ = failure(status);
        return status;
    }
    if (response.kind != request.kind or response.direction != 1 or response.status != 0 or
        response.resource_id != 0 or response.flags != 0 or response.argument_zero != 0 or
        response.argument_one != 0 or response.payload_bytes != capacity)
    {
        _ = failure(c.RingCorrupt);
        return c.RingCorrupt;
    }
    return c.RingOk;
}
// Baseline owns the last synchronized mapped bytes. CPU changes are published
// before host signaling/submission; completed GPU bytes merge without replacing
// CPU changes made since that baseline. Caller synchronizes conflicting accesses.
fn merge_mapping(bytes: []u8, baseline: []u8, incoming: []const u8) void {
    for (bytes, baseline, incoming) |*current, *previous, value| {
        if (current.* == previous.*) current.* = value;
        previous.* = value;
    }
}
/// Retain conservative potentially GPU-written extents in the allocation owner.
/// No resource lifetime is extended; intervals survive buffer/image destruction.
/// Sorted union is bounded16; excess fragmentation collapses to the full allocation.
/// Caller holds the ICD mutex. Missing or invalid geometry also falls back to full.
fn retain_gpu_span(state: *resource_state_t, first: u64, count: u64) void {
    if (state.allocation_size == 0) return;
    if (count == 0 or first > state.allocation_size or count > state.allocation_size - first) {
        state.gpu_spans[0] = .{ .end = state.allocation_size };
        state.gpu_span_count = 1;
        return;
    }
    var span = mapping_span_t{ .start = first, .end = first + count };
    var index: usize = 0;
    while (index < state.gpu_span_count) {
        const prior = state.gpu_spans[index];
        if (prior.end < span.start or prior.start > span.end) { index += 1; continue; }
        span.start = @min(span.start, prior.start);
        span.end = @max(span.end, prior.end);
        state.gpu_span_count -= 1;
        for (index..state.gpu_span_count) |next| state.gpu_spans[next] = state.gpu_spans[next + 1];
    }
    if (state.gpu_span_count == state.gpu_spans.len) {
        state.gpu_spans[0] = .{ .end = state.allocation_size };
        state.gpu_span_count = 1;
        return;
    }
    index = 0;
    while (index < state.gpu_span_count and state.gpu_spans[index].start < span.start) index += 1;
    var last = state.gpu_span_count;
    while (last > index) : (last -= 1) state.gpu_spans[last] = state.gpu_spans[last - 1];
    state.gpu_spans[index] = span;
    state.gpu_span_count += 1;
}
/// Snapshot all submitted bound resource ranges, conservatively treating reads
/// as writes. Address-exposed buffers are included even without native handles
/// in the submitted command references. No timeline result clears this ledger.
fn retain_submission_mapping_spans(parent_id: u64, references: [8]u64) void {
    for (&slots, &resource_states, 0..) |*record, *state, index| {
        if (record.id == 0 or record.parent_id != parent_id or
            (record.kind != c.VK_OBJECT_TYPE_BUFFER and record.kind != c.VK_OBJECT_TYPE_IMAGE)) continue;
        const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
        if (references[index / 64] & bit == 0 and !state.address_exposed) continue;
        const allocation = child_object(state.bound_memory, c.VK_OBJECT_TYPE_DEVICE_MEMORY, parent_id) orelse continue;
        retain_gpu_span(resource_state(allocation), state.memory_offset,
            if (record.kind == c.VK_OBJECT_TYPE_BUFFER) state.buffer_size else state.requirements.size);
    }
}

fn synchronize_mapping(state: *resource_state_t, writing: bool) c_int {
    const bytes = state.mapped_bytes orelse return c.RingOk;
    const baseline = state.mapped_baseline orelse return c.RingCorrupt;
    const start: usize = @intCast(state.mapped_offset);
    if (!writing) {
        var incoming: [MappingReadChunkBytes]u8 = undefined;
        const mapped_end = state.mapped_offset + state.mapped_size;
        for (state.gpu_spans[0..state.gpu_span_count]) |span| {
            var absolute = @max(span.start, state.mapped_offset);
            const end = @min(span.end, mapped_end);
            while (absolute < end) {
                const count: usize = @intCast(@min(if (reply_profile_ready) MappingReadChunkBytes else MappingChunkBytes, end - absolute));
                var request = std.mem.zeroes(c.venus_request_t);
                request.kind = c.RequestRead;
                request.resource_id = state.mapping_resource;
                request.argument_zero = absolute;
                request.argument_one = count;
                const status = mapping_exchange(&request, null, 0, &incoming, count);
                if (status != c.RingOk) return status;
                const offset: usize = @intCast(absolute - state.mapped_offset);
                merge_mapping(bytes[start + offset ..][0..count], baseline[offset..][0..count], incoming[0..count]);
                absolute += count;
            }
        }
        return c.RingOk;
    }
    var cursor: usize = 0;
    while (cursor < baseline.len) {
        var packet: [MappingChunkBytes]u8 = undefined;
        var used: usize = 4;
        var ranges: u32 = 0;
        while (cursor < baseline.len) {
            while (cursor < baseline.len and bytes[start + cursor] == baseline[cursor]) cursor += 1;
            if (cursor == baseline.len or packet.len - used < 13) break;
            const first = cursor;
            const maximum = packet.len - used - 12;
            while (cursor < baseline.len and cursor - first < maximum and bytes[start + cursor] != baseline[cursor]) cursor += 1;
            const count = cursor - first;
            std.mem.writeInt(u64, packet[used..][0..8], start + first, .little);
            std.mem.writeInt(u32, packet[used + 8 ..][0..4], @intCast(count), .little);
            @memcpy(packet[used + 12 ..][0..count], bytes[start + first ..][0..count]);
            used += 12 + count;
            ranges += 1;
        }
        if (ranges == 0) continue;
        std.mem.writeInt(u32, packet[0..4], ranges, .little);
        var request = std.mem.zeroes(c.venus_request_t);
        request.kind = c.RequestWrite;
        request.flags = 2;
        request.resource_id = state.mapping_resource;
        request.argument_one = used;
        request.payload_bytes = @intCast(used);
        const status = mapping_exchange(&request, &packet, used, null, 0);
        if (status != c.RingOk) return status;
        var position: usize = 4;
        for (0..ranges) |_| {
            const offset: usize = @intCast(std.mem.readInt(u64, packet[position..][0..8], .little));
            const count: usize = std.mem.readInt(u32, packet[position + 8 ..][0..4], .little);
            @memcpy(baseline[offset - start ..][0..count], packet[position + 12 ..][0..count]);
            position += 12 + count;
        }
    }
    return c.RingOk;
}
fn synchronize_device_mappings(parent_id: u64, writing: bool) c_int {
    for (&slots, &resource_states) |*record, *state| {
        if (record.kind != c.VK_OBJECT_TYPE_DEVICE_MEMORY or record.parent_id != parent_id or state.mapped_bytes == null) continue;
        const status = synchronize_mapping(state, writing);
        if (status != c.RingOk) return failure(status);
    }
    return c.VK_SUCCESS;
}

fn copy_mapping(state: *resource_state_t, offset: u64, size: u64, writing: bool) c_int {
    const bytes = state.mapped_bytes.?;
    var cursor: u64 = offset;
    var remaining = size;
    while (remaining != 0) {
        const count: usize = @intCast(@min(remaining, if (!writing and reply_profile_ready) MappingReadChunkBytes else MappingChunkBytes));
        var request = std.mem.zeroes(c.venus_request_t);
        request.kind = if (writing) c.RequestWrite else c.RequestRead;
        request.resource_id = state.mapping_resource;
        request.argument_zero = cursor;
        request.argument_one = count;
        request.payload_bytes = if (writing) @intCast(count) else 0;
        const pointer = bytes[@intCast(cursor)..].ptr;
        const status = mapping_exchange(&request, if (writing) pointer else null, if (writing) count else 0, if (writing) null else pointer, if (writing) 0 else count);
        if (status != c.RingOk) return status;
        if (state.mapped_baseline) |baseline| {
            const base: usize = @intCast(cursor - state.mapped_offset);
            @memcpy(baseline[base..][0..count], bytes[@intCast(cursor)..][0..count]);
        }
        cursor += count;
        remaining -= count;
    }
    return c.RingOk;
}
/// Map a bounded coherent shadow of actual exported Vulkan allocation storage.
/// @param[in] device Nonnull borrowed private parent; memory must belong to it.
/// @param[in] memory_handle Nonnull owned allocation token, retained until free.
/// @param[in] offset Byte offset strictly inside allocation; flags must be zero.
/// @param[in] size Nonzero range or VK_WHOLE_SIZE; bounded by allocation.
/// @param[out] output Nonnull writable pointer storage, NULL on all failures.
/// @return SUCCESS, MEMORY_MAP_FAILED for invalid bounds/export/quota, host OOM, or device loss.
/// @note Mutex serialized. ICD owns shadow until unmap/free/abandon; caller externally synchronizes GPU access.
fn map_memory(device: c.VkDevice, memory_handle: c.VkDeviceMemory, offset: u64, size: u64, flags: u32, output: [*c]?*anyopaque) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_MEMORY_MAP_FAILED;
    output.* = null;
    if (device == null or memory_handle == null or flags != 0) return c.VK_ERROR_MEMORY_MAP_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_MEMORY_MAP_FAILED;
    const record = child_object(@intFromPtr(memory_handle.?), c.VK_OBJECT_TYPE_DEVICE_MEMORY, parent.id) orelse return c.VK_ERROR_MEMORY_MAP_FAILED;
    const state = resource_state(record);
    if (state.mapped_bytes != null or offset >= state.allocation_size or size == 0 or state.allocation_size > MaxMappedBytes)
        return c.VK_ERROR_MEMORY_MAP_FAILED;
    const count = if (size == std.math.maxInt(u64)) state.allocation_size - offset else size;
    if (count > state.allocation_size - offset) return c.VK_ERROR_MEMORY_MAP_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var physical: ?*c.venus_object_t = null;
    for (&slots) |*slot| if (slot.id == parent.parent_id and slot.kind == c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) {
        physical = slot;
        break;
    };
    const reply = query(@ptrFromInt(physical.?.handle), 8) orelse return c.VK_ERROR_DEVICE_LOST;
    var properties_value: c.VkPhysicalDeviceMemoryProperties = undefined;
    if (c.venus_values_memory_decode(&properties_value, reply.ptr, reply.len) != c.RingOk)
        return failure(c.RingCorrupt);
    if (state.type_index >= properties_value.memoryTypeCount or
        properties_value.memoryTypes[state.type_index].propertyFlags &
        (c.VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | c.VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) !=
        (c.VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | c.VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        return c.VK_ERROR_MEMORY_MAP_FAILED;
    const bytes = MappingAllocator.alignedAlloc(u8, 4096, @intCast(state.allocation_size)) catch return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    state.mapped_bytes = bytes;
    if (state.mapping_resource == 0) {
        var slot_index: ?usize = null;
        for (mapping_slots, 0..) |occupied, index| if (!occupied) {
            slot_index = index;
            break;
        };
        if (slot_index == null) {
            release_shadow(state);
            return c.VK_ERROR_MEMORY_MAP_FAILED;
        }
        var request = std.mem.zeroes(c.venus_request_t);
        request.kind = c.RequestCreate;
        request.resource_id = @intCast(slot_index.? + 2);
        request.flags = 1;
        request.argument_zero = record.id;
        request.argument_one = (state.allocation_size + 4095) & ~@as(u64, 4095);
        const status = mapping_exchange(&request, null, 0, null, 0);
        if (status != c.RingOk) {
            release_shadow(state);
            return if (lost != c.RingOk) c.VK_ERROR_DEVICE_LOST else c.VK_ERROR_MEMORY_MAP_FAILED;
        }
        mapping_slots[slot_index.?] = true;
        state.mapping_resource = request.resource_id;
    }
    const status = copy_mapping(state, offset, count, false);
    if (status != c.RingOk) {
        release_shadow(state);
        return if (lost != c.RingOk) c.VK_ERROR_DEVICE_LOST else c.VK_ERROR_MEMORY_MAP_FAILED;
    }
    const baseline = MappingAllocator.alloc(u8, @intCast(count)) catch {
        release_shadow(state);
        return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    };
    @memcpy(baseline, bytes[@intCast(offset)..][0..@intCast(count)]);
    state.mapped_baseline = baseline;
    state.mapped_offset = offset;
    state.mapped_size = count;
    output.* = bytes[@intCast(offset)..].ptr;
    return c.VK_SUCCESS;
}
/// Release only the ICD-owned shadow; export survives remap until memory free.
/// @param[in] device Nullable borrowed parent, invalid/stale handles ignored.
/// @param[in] memory_handle Nullable token; CPU dirty bytes publish before shadow retirement.
/// @note Mutex serialized, allocation-free serializer; pointer expires upon return.
fn unmap_memory(device: c.VkDevice, memory_handle: c.VkDeviceMemory) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (device == null or memory_handle == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(@intFromPtr(memory_handle.?), c.VK_OBJECT_TYPE_DEVICE_MEMORY, parent.id) orelse return;
    if (synchronize_mapping(resource_state(record), true) != c.RingOk) {
        _ = failure(c.RingClosed);
    }
    release_shadow(resource_state(record));
}
fn mapped_ranges(device: c.VkDevice, count: u32, ranges: [*c]const c.VkMappedMemoryRange, writing: bool) c_int {
    if (device == null or count > 64 or (count != 0 and ranges == null)) return c.VK_ERROR_MEMORY_MAP_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_MEMORY_MAP_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    // Validate the complete array before the first host side effect.
    if (count != 0) for (ranges[0..count]) |range| {
        if (range.sType != c.VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE or range.pNext != null or range.memory == null)
            return c.VK_ERROR_MEMORY_MAP_FAILED;
        const record = child_object(@intFromPtr(range.memory.?), c.VK_OBJECT_TYPE_DEVICE_MEMORY, parent.id) orelse return c.VK_ERROR_MEMORY_MAP_FAILED;
        const state = resource_state(record);
        if (state.mapped_bytes == null or range.offset < state.mapped_offset or range.offset >= state.mapped_offset + state.mapped_size or range.size == 0)
            return c.VK_ERROR_MEMORY_MAP_FAILED;
        const size = if (range.size == std.math.maxInt(u64)) state.allocation_size - range.offset else range.size;
        if (size > state.mapped_offset + state.mapped_size - range.offset) return c.VK_ERROR_MEMORY_MAP_FAILED;
    };
    if (count != 0) for (ranges[0..count]) |range| {
        const state = resource_state(child_object(@intFromPtr(range.memory.?), c.VK_OBJECT_TYPE_DEVICE_MEMORY, parent.id).?);
        const size = if (range.size == std.math.maxInt(u64)) state.allocation_size - range.offset else range.size;
        const status = copy_mapping(state, range.offset, size, writing);
        if (status != c.RingOk) return if (lost != c.RingOk) c.VK_ERROR_DEVICE_LOST else c.VK_ERROR_MEMORY_MAP_FAILED;
    };
    return c.VK_SUCCESS;
}
/// Copy validated noncoherent mapped ranges to actual receiver allocation storage.
/// @param[in] device Borrowed live parent; ranges borrowed count entries, NULL only for zero.
/// @return SUCCESS, MEMORY_MAP_FAILED for local range/resource errors, or sticky DEVICE_LOST.
/// @note Mutex serialized, no allocation; caller synchronizes GPU access and shadow writers.
fn flush_memory(device: c.VkDevice, count: u32, ranges: [*c]const c.VkMappedMemoryRange) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    return mapped_ranges(device, count, ranges, true);
}
/// Acquire actual receiver allocation bytes into validated noncoherent shadow ranges.
/// @param[in] device Borrowed live parent; ranges borrowed count entries, NULL only for zero.
/// @return SUCCESS, MEMORY_MAP_FAILED for local range/resource errors, or sticky DEVICE_LOST.
/// @note Mutex serialized, no allocation; caller waits GPU completion before invalidation.
fn invalidate_memory(device: c.VkDevice, count: u32, ranges: [*c]const c.VkMappedMemoryRange) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    return mapped_ranges(device, count, ranges, false);
}
/// Bind a buffer to memory_handle using actual host requirements and overflow-safe bounds.
/// @param[in] device Nonnull private parent, borrowed for call.
/// @param[in] buffer Nonnull private unbound buffer of this device.
/// @param[in] memory_handle Nonnull private allocation of this device, retained by buffer.
/// @param[in] offset Byte offset aligned to requirements, within allocation extent.
/// @return Host result, local initialization error or sticky device loss.
/// @note Mutex serialized, allocation-free; publishes relationship only after success.
fn bind_buffer_memory(
    device: c.VkDevice,
    buffer: c.VkBuffer,
    memory_handle: c.VkDeviceMemory,
    offset: u64,
) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (device == null or buffer == null or memory_handle == null)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const buffer_record = child_object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_BUFFER,
        parent.id,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const memory_record = child_object(
        @intFromPtr(memory_handle.?),
        c.VK_OBJECT_TYPE_DEVICE_MEMORY,
        parent.id,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const buffer_state = resource_state(buffer_record);
    const memory_state = resource_state(memory_record);
    if (buffer_state.bound_memory != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (buffer_state.requirements.size == 0) {
        const value = query_buffer_requirements(
            parent.id,
            buffer_record.id,
            buffer_state.buffer_size,
        ) orelse return c.VK_ERROR_DEVICE_LOST;
        buffer_state.requirements = value;
    }
    const requirements = buffer_state.requirements;
    if (requirements.memoryTypeBits & (@as(u32, 1) << @as(
        u5,
        @intCast(memory_state.type_index),
    )) == 0 or offset % requirements.alignment != 0 or
        offset > memory_state.allocation_size or
        requirements.size > memory_state.allocation_size - offset) return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = writer_t{};
    writer.header(28, parent.id);
    writer.put(u64, buffer_record.id);
    writer.put(u64, memory_record.id);
    writer.put(u64, offset);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 28, 0);
    if (result == c.VK_SUCCESS) {
        buffer_state.bound_memory = memory_record.handle;
        buffer_state.memory_offset = offset;
    }
    return result;
}
/// Create a core command pool with private device parent and configured queue family.
/// @param[in] device Nonnull live private parent, borrowed for call.
/// @param[in] info Nonnull canonical native info, no pNext; flags0..3 supported.
/// @param[in] allocator Nullable unused callbacks, no pointer retained.
/// @param[out] output Nonnull borrowed handle storage, NULL on failure.
/// @return Host result, initialization/exhaustion error or sticky device loss.
/// @note Allocation-free, mutex serialized; owned until validated host destruction.
fn create_command_pool(
    device: c.VkDevice,
    info: [*c]const c.VkCommandPoolCreateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkCommandPool,
) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or
        info.*.sType != c.VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO or info.*.pNext != null or
        info.*.flags & ~@as(u32, 3) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const entry = device_cache(parent.handle).?;
    if (std.mem.indexOfScalar(
        u32,
        entry.families[0..entry.family_count],
        info.*.queueFamilyIndex,
    ) == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        parent.id,
        0,
        &record,
    ) != c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var writer = writer_t{};
    writer.header(85, parent.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    writer.put(u64, 0);
    writer.put(u32, info.*.flags);
    writer.put(u32, info.*.queueFamilyIndex);
    writer.put(u64, 0);
    writer.put(u64, 1);
    writer.put(u64, record.*.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, 85, record.*.id, true) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0) return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_COMMAND_POOL, 0);
        return result;
    }
    resource_state(record).* = .{
        .id = record.*.id,
        .pool_family = info.*.queueFamilyIndex,
        .pool_flags = info.*.flags,
    };
    output.* = @ptrFromInt(record.*.handle);
    return c.VK_SUCCESS;
}
/// Destroy a quiescent pool and its implicit command-buffer children.
/// @param[in] device Nullable private parent, borrowed; invalid handle ignored.
/// @param[in] pool Nullable private device-owned token; consumed after exact reply.
/// @param[in] allocator Nullable unused callbacks, no pointer retained.
/// @note Mutex serialized, allocation-free; caller retires pending GPU uses first.
/// Transport/peer loss retains all uncertain identities until receiver abandonment.
fn destroy_command_pool(
    device: c.VkDevice,
    pool: c.VkCommandPool,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (device == null or pool == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(
        @intFromPtr(pool.?),
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        parent.id,
    ) orelse return;
    for (slots, 0..) |child, index|
        if (child.parent_id == record.id and (child.kind != c.VK_OBJECT_TYPE_COMMAND_BUFFER or
            resource_states[index].command_state == .Pending)) return;
    var writer = writer_t{};
    writer.header(86, parent.id);
    writer.put(u64, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 86) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for (&slots, 0..) |*child, index| if (child.parent_id == record.id) {
        release_command_profile(&resource_states[index]);
        resource_states[index] = .{};
        std.debug.assert(
            c.venus_objects_release(
                &objects,
                child.handle,
                c.VK_OBJECT_TYPE_COMMAND_BUFFER,
                1,
            ) == c.RingOk,
        );
    };
    resource_state(record).* = .{};
    std.debug.assert(
        c.venus_objects_release(
            &objects,
            record.handle,
            c.VK_OBJECT_TYPE_COMMAND_POOL,
            0,
        ) == c.RingOk,
    );
}
/// Reset a quiescent pool after exact device-parent validation.
/// @param[in] device Nullable private parent, borrowed for call.
/// @param[in] pool Nullable private device-owned token, no ownership transfer.
/// @param[in] flags Core0/1 release-resources flags only.
/// @return Host result, local initialization error or sticky device loss.
/// @note Mutex serialized, allocation-free; caller ensures no child is pending GPU use.
fn reset_command_pool(device: c.VkDevice, pool: c.VkCommandPool, flags: u32) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (device == null or pool == null or flags > 1) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = child_object(
        @intFromPtr(pool.?),
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        parent.id,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    for (slots, 0..) |child, index|
        if (child.parent_id == record.id and resource_states[index].command_state == .Pending)
            return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = writer_t{};
    writer.header(87, parent.id);
    writer.put(u64, record.id);
    writer.put(u32, flags);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 87, 0);
    if (result == c.VK_SUCCESS) for (slots, 0..) |child, index| if (child.parent_id == record.id) {
        resource_states[index].command_state = .Initial;
        resource_states[index].command_flags = 0;
        resource_states[index].buffer_references = [_]u64{0} ** 8;
        reset_command_profile(&resource_states[index]);
    };
    return result;
}
fn command_pool_for(record: *const c.venus_object_t) ?*c.venus_object_t {
    for (&slots) |*slot|
        if (slot.id == record.parent_id and slot.kind == c.VK_OBJECT_TYPE_COMMAND_POOL) return slot;
    return null;
}
// Successful reset retains the live command reservation, only scrubbing its binding definitions.
fn reset_command_profile(state: *resource_state_t) void {
    state.descriptor_uses = [_]u64{0} ** 8;
    state.index_buffer = 0; state.index_offset = 0; state.index_size = 0; state.index_type = 0;
    if (state.command_profile_index != 0) {
        profiles.get_profile(&command_registry.commands, state.command_profile_index).?.* = .{};
        graphics_state.reset(&graphics_recordings[state.command_profile_index - 1]);
    }
}
// Successful native retirement refunds metadata; internal legacy test records may own no profile.
fn release_command_profile(state: *const resource_state_t) void {
    if (state.command_profile_index != 0) {
        graphics_state.reset(&graphics_recordings[state.command_profile_index - 1]);
        std.debug.assert(profiles.release_slot(&command_registry.commands, state.command_profile_index));
    }
}
fn command_buffers_reply(bytes: []const u8, ids: []const u64) !c_int {
    var reader = reader_t{ .bytes = bytes };
    if (try reader.scalar(u32) != 88) return error.Value;
    const result = try reader.scalar(i32);
    if (result > 0 or try reader.scalar(u64) != ids.len) return error.Value;
    for (ids) |id| {
        const received = try reader.scalar(u64);
        if (received != id and (result == 0 or received != 0)) return error.Value;
    }
    return result;
}
/// Allocate1..64 private command buffers transactionally under one pool.
/// @param[in] device Nonnull private live device parent, borrowed for call.
/// @param[in] info Nonnull canonical tag40/no pNext/level0..1 and private pool.
/// @param[out] output Borrowed accessible handles[count]; NULL on validated bounded failures.
/// @return Host result, local initialization/exhaustion or sticky device loss.
/// @note Allocation-free and mutex serialized; count>64 leaves output untouched.
/// Records are pool-owned, published only after all exact host IDs validate.
/// A fixed64 global metadata quota is reserved before native allocation; resets retain that quota.
fn allocate_command_buffers(
    device: c.VkDevice,
    info: [*c]const c.VkCommandBufferAllocateInfo,
    output: [*c]c.VkCommandBuffer,
) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (info == null or output == null or info.*.commandBufferCount == 0)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    const native_info = info.*;
    const count = native_info.commandBufferCount;
    if (count > 64) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    @memset(output[0..count], null);
    if (device == null or native_info.sType != c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO or
        native_info.pNext != null or (native_info.level != 0 and native_info.level != 1) or
        native_info.commandPool == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const pool = child_object(
        @intFromPtr(native_info.commandPool.?),
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        parent.id,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var metadata_indices: [64]u8 = undefined;
    var metadata_count: usize = 0;
    while (metadata_count < count) : (metadata_count += 1) {
        metadata_indices[metadata_count] = profiles.reserve_slot(&command_registry.commands, compute_state.command_profile_t{}) catch {
            for (metadata_indices[0..metadata_count]) |index| std.debug.assert(profiles.release_slot(&command_registry.commands, index));
            return c.VK_ERROR_OUT_OF_HOST_MEMORY;
        };
    }
    var records: [64][*c]c.venus_object_t = undefined;
    var ids: [64]u64 = undefined;
    var reserved: usize = 0;
    while (reserved < count) : (reserved += 1) {
        var record: [*c]c.venus_object_t = null;
        if (c.venus_objects_reserve(
            &objects,
            c.VK_OBJECT_TYPE_COMMAND_BUFFER,
            pool.id,
            1,
            &record,
        ) != c.RingOk) {
            for (records[0..reserved]) |entry| _ = c.venus_objects_release(
                &objects,
                entry.*.handle,
                c.VK_OBJECT_TYPE_COMMAND_BUFFER,
                1,
            );
            for (metadata_indices[0..count]) |index| std.debug.assert(profiles.release_slot(&command_registry.commands, index));
            return c.VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        records[reserved] = record;
        ids[reserved] = record.*.id;
    }
    var writer = writer_t{};
    writer.header(88, parent.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    writer.put(u64, 0);
    writer.put(u64, pool.id);
    writer.put(u32, native_info.level);
    writer.put(u32, count);
    writer.put(u64, count);
    for (ids[0..count]) |id| writer.put(u64, id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = command_buffers_reply(reply, ids[0..count]) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result != c.VK_SUCCESS) {
        for (records[0..count]) |entry| _ = c.venus_objects_release(
            &objects,
            entry.*.handle,
            c.VK_OBJECT_TYPE_COMMAND_BUFFER,
            1,
        );
        for (metadata_indices[0..count]) |index| std.debug.assert(profiles.release_slot(&command_registry.commands, index));
        return result;
    }
    for (records[0..count], 0..) |record, index| {
        resource_state(record).* = .{ .id = record.*.id, .command_level = native_info.level, .command_profile_index = metadata_indices[index] };
        output[index] = @ptrFromInt(record.*.handle);
    }
    return c.VK_SUCCESS;
}
/// Free a validated0..64 pool-owned command-buffer batch after GPU retirement.
/// @param[in] device Nullable private parent, borrowed; invalid handles ignored.
/// @param[in] pool Nullable private same-device pool.
/// @param[in] count0..64 accessible handle extent; zero is a no-op.
/// @param[in] buffers Nullable only for zero; borrowed and immutable for call.
/// @note Mutex serialized, allocation-free; duplicates/foreign/stale entries ignored.
/// Retire all private records only after host reply; loss retains uncertain ownership.
fn free_command_buffers(
    device: c.VkDevice,
    pool: c.VkCommandPool,
    count: u32,
    buffers: [*c]const c.VkCommandBuffer,
) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (count == 0 or count > 64 or device == null or pool == null or buffers == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const pool_record = child_object(
        @intFromPtr(pool.?),
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        parent.id,
    ) orelse return;
    var records: [64]*c.venus_object_t = undefined;
    for (buffers[0..count], 0..) |buffer, index| {
        if (buffer == null) return;
        const record = object(@intFromPtr(buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
        if (record.parent_id != pool_record.id or resource_state(record).command_state == .Pending)
            return;
        for (records[0..index]) |prior| if (prior.id == record.id) return;
        records[index] = record;
    }
    var writer = writer_t{};
    writer.header(89, parent.id);
    writer.put(u64, pool_record.id);
    writer.put(u32, count);
    writer.put(u64, count);
    for (records[0..count]) |record| writer.put(u64, record.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 89) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for (records[0..count]) |record| {
        release_command_profile(resource_state(record));
        resource_state(record).* = .{};
        std.debug.assert(
            c.venus_objects_release(
                &objects,
                record.handle,
                c.VK_OBJECT_TYPE_COMMAND_BUFFER,
                1,
            ) == c.RingOk,
        );
    }
}
/// Begin primary or bounded transfer/compute secondary recording.
/// @param[in] buffer Nonnull live private buffer, borrowed for call.
/// @param[in] info Nonnull canonical tag42/no pNext and flags confined to1|4.
/// Secondary inheritance tag41 is borrowed; render/query inheritance initially unsupported.
/// @return Host result, initialization error or sticky device loss.
/// @note Mutex serialized, allocation-free; primary inheritance ignored without dereference.
fn begin_command_buffer(
    buffer: c.VkCommandBuffer,
    info: [*c]const c.VkCommandBufferBeginInfo,
) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (buffer == null or info == null or
        info.*.sType != c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO or info.*.pNext != null or
        info.*.flags & ~@as(u32, 5) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const pool = command_pool_for(record) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const state = resource_state(record);
    if (state.command_state == .Recording or state.command_state == .Pending)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    if (state.command_state != .Initial and resource_state(pool).pool_flags & 2 == 0)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    const secondary = state.command_level == 1;
    if (!secondary and info.*.flags == 5) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (secondary) {
        const inheritance = info.*.pInheritanceInfo;
        if (inheritance == null or
            inheritance.*.sType != c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO or
            inheritance.*.pNext != null or inheritance.*.renderPass != null or
            inheritance.*.framebuffer != null or inheritance.*.subpass != 0 or
            inheritance.*.occlusionQueryEnable != 0 or inheritance.*.queryFlags != 0 or
            inheritance.*.pipelineStatistics != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    var writer = writer_t{};
    writer.header(90, record.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    writer.put(u64, 0);
    writer.put(u32, info.*.flags);
    writer.put(u64, if (secondary) 1 else 0);
    if (secondary) {
        writer.put(u32, c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO);
        writer.put(u64, 0);
        writer.put(u64, 0);
        writer.put(u32, 0);
        writer.put(u64, 0);
        writer.put(u32, 0);
        writer.put(u32, 0);
        writer.put(u32, 0);
    }
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 90, 0);
    if (result == c.VK_SUCCESS) {
        state.command_state = .Recording;
        state.command_flags = info.*.flags;
        state.buffer_references = [_]u64{0} ** 8;
        reset_command_profile(state);
    } else if (lost == c.RingOk) state.command_state = .Invalid;
    return result;
}
/// Finish a live recording; host success enters Executable, native error Invalid.
/// @param[in] buffer Nonnull private borrowed handle, never dereferenced as native pointer.
/// @return Host result, local initialization error or sticky device loss.
/// @note Mutex serialized, no allocation or ownership transfer.
fn end_command_buffer(buffer: c.VkCommandBuffer) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (buffer == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const state = resource_state(record);
    if (state.command_state != .Recording) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (state.command_profile_index != 0) graphics_state.finish(graphics_recording(state)) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = writer_t{};
    writer.header(91, record.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 91, 0);
    if (lost == c.RingOk)
        state.command_state = if (result == c.VK_SUCCESS) .Executable else .Invalid;
    return result;
}
fn command_profile(record: *const c.venus_object_t) *compute_state.command_profile_t {
    return profiles.get_profile(&command_registry.commands, resource_state(record).command_profile_index).?;
}
fn command_reference(state: *resource_state_t, target: *const c.venus_object_t) void {
    const index = resource_index(target);
    state.buffer_references[index / 64] |= @as(u64, 1) << @as(u6, @intCast(index % 64));
}
fn command_acknowledged(writer: *const compute_wire.writer_t, opcode: u32) bool {
    const reply = transact(writer.bytes[0..writer.used]) orelse return false;
    if (reply.len < 4 or std.mem.readInt(u32, reply[0..4], .little) != opcode) {
        _ = failure(c.RingCorrupt);
        return false;
    }
    return true;
}
fn ensure_graphics_queue_flags(device: *const c.venus_object_t) bool {
    const entry = device_cache(device.handle).?;
    if (entry.graphics_queue_ready) return true;
    var physical: ?*c.venus_object_t = null;
    for (&slots) |*slot| if (slot.id == device.parent_id and slot.kind == c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) {
        physical = slot;
        break;
    };
    const reply = physical_request(@ptrFromInt(physical.?.handle), 7, &.{}, 64) orelse return false;
    var reader = reader_t{ .bytes = reply };
    var family_values: [64]c.VkQueueFamilyProperties = undefined;
    var count: u32 = 64;
    array_values(c.VkQueueFamilyProperties, &reader, 7, 64, true, &count, &family_values) catch {
        _ = failure(c.RingCorrupt);
        return false;
    };
    for (entry.families[0..entry.family_count], entry.counts[0..entry.family_count]) |family, requested| {
        if (family >= count or requested > family_values[family].queueCount) {
            _ = failure(c.RingCorrupt);
            return false;
        }
    }
    for (family_values[0..count], 0..) |value, index| entry.graphics_queue_flags[index] = value.queueFlags;
    entry.graphics_queue_count = count;
    entry.graphics_queue_ready = true;
    return true;
}
fn graphics_family_supported(pool: *const c.venus_object_t) bool {
    var device: ?*c.venus_object_t = null;
    for (&slots) |*slot| if (slot.id == pool.parent_id and slot.kind == c.VK_OBJECT_TYPE_DEVICE) {
        device = slot;
        break;
    };
    if (!ensure_graphics_queue_flags(device.?)) return false;
    const entry = device_cache(device.?.handle).?;
    const family = resource_state(pool).pool_family;
    return family < entry.graphics_queue_count and entry.graphics_queue_flags[family] & c.VK_QUEUE_GRAPHICS_BIT != 0;
}
/// Begin canonical inline color pass. [in] nullable borrowed command/info/clear array; contents INLINE.
/// Void; invalid recording/dependencies invalidate locally. Native acknowledgment precedes copied state
/// and pass/framebuffer/view/image/memory references. No caller pointers retained; mutex serialized.
fn begin_render_pass(command_buffer: c.VkCommandBuffer, info: [*c]const c.VkRenderPassBeginInfo, contents: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    const pool = command_pool_for(record) orelse return;
    if (state.command_level != 0 or info == null or info.*.renderPass == null or info.*.framebuffer == null) {
        state.command_state = .Invalid;
        return;
    }
    const pass = child_object(@intFromPtr(info.*.renderPass.?), c.VK_OBJECT_TYPE_RENDER_PASS, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return;
    };
    const framebuffer = child_object(@intFromPtr(info.*.framebuffer.?), c.VK_OBJECT_TYPE_FRAMEBUFFER, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return;
    };
    const framebuffer_state = resource_state(framebuffer);
    const format = resource_state(pass).render_format;
    const view = child_object(framebuffer_state.framebuffer_view, c.VK_OBJECT_TYPE_IMAGE_VIEW, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return;
    };
    const writer = graphics_wire.begin_render_pass(@ptrCast(info), record.id, pass.id, framebuffer.id, contents) catch {
        state.command_state = .Invalid;
        return;
    };
    if (format != framebuffer_state.render_format or !framebuffer_attachment(view, framebuffer_state.framebuffer_extent[0], framebuffer_state.framebuffer_extent[1], format) or
        @as(u32, @intCast(info.*.renderArea.offset.x)) > framebuffer_state.framebuffer_extent[0] or
        @as(u32, @intCast(info.*.renderArea.offset.y)) > framebuffer_state.framebuffer_extent[1] or
        info.*.renderArea.extent.width > framebuffer_state.framebuffer_extent[0] - @as(u32, @intCast(info.*.renderArea.offset.x)) or
        info.*.renderArea.extent.height > framebuffer_state.framebuffer_extent[1] - @as(u32, @intCast(info.*.renderArea.offset.y))) {
        state.command_state = .Invalid;
        return;
    }
    if (!graphics_family_supported(pool)) {
        if (lost == c.RingOk) state.command_state = .Invalid;
        return;
    }
    const image = child_object(resource_state(view).view_image, c.VK_OBJECT_TYPE_IMAGE, pool.parent_id).?;
    const image_state = resource_state(image);
    if (resource_state(pass).render_final_layout == c.VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL and
        image_state.image_usage & (c.VK_IMAGE_USAGE_SAMPLED_BIT | c.VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) == 0) {
        state.command_state = .Invalid;
        return;
    }
    var staged = graphics_recording(state).*;
    graphics_state.begin_pass(&staged, format) catch {
        state.command_state = .Invalid;
        return;
    };
    const allocation = child_object(resource_state(image).bound_memory, c.VK_OBJECT_TYPE_DEVICE_MEMORY, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return;
    };
    if (!command_acknowledged(&writer, 133)) return;
    graphics_recording(state).* = staged;
    for ([_]*c.venus_object_t{ pass, framebuffer, view, image, allocation }) |target| command_reference(state, target);
}
/// End active pass. [in] nullable borrowed command token. Invalid ordering invalidates locally.
/// Void; exact opcode135 acknowledgment exits pass while preserving pipeline; mutex serialized.
fn end_render_pass(command_buffer: c.VkCommandBuffer) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    var staged = graphics_recording(state).*;
    graphics_state.end_pass(&staged) catch {
        state.command_state = .Invalid;
        return;
    };
    const writer = graphics_command_wire.end_render_pass(record.id) catch unreachable;
    if (!command_acknowledged(&writer, 135)) return;
    graphics_recording(state).* = staged;
}
/// Draw canonical vertexless pipeline. [in] nullable borrowed command token and full-u32 scalars.
/// Void; requires active compatible pass and live graphics pipeline. No vertex arrays retained.
/// Exact acknowledgment publishes pipeline reference; invalid ordering invalidates; mutex serialized.
fn draw(command_buffer: c.VkCommandBuffer, vertex_count: u32, instance_count: u32, first_vertex: u32, first_instance: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    const pool = command_pool_for(record) orelse return;
    const token = graphics_state.draw_pipeline(graphics_recording(state)) catch {
        state.command_state = .Invalid;
        return;
    };
    const pipeline = child_object(token, c.VK_OBJECT_TYPE_PIPELINE, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return;
    };
    if (resource_state(pipeline).pipeline_bind_point != 0) {
        state.command_state = .Invalid;
        return;
    }
    if (!validate_draw_descriptors(pool.parent_id,state,command_profile(record),0x1f)) { state.command_state=.Invalid; return; }
    const writer = graphics_command_wire.draw(record.id, .{ vertex_count, instance_count, first_vertex, first_instance }) catch unreachable;
    if (!command_acknowledged(&writer, 106)) return;
    command_reference(state, pipeline);
    retain_draw_descriptors(pool.parent_id,state,command_profile(record),0x1f);
}

/// Copy bounded color image to bound transfer buffer. [in] nullable borrowed command/image/buffer;
/// regions accessible count1..64 records for this call, no retention. Layout GENERAL/TRANSFER_SRC.
/// Void; rejects inside pass, usages/sample/format/ranges before native work. Exact opcode116 ack
/// retains image/buffer and both memories until GPU retirement. Mutex serialized, allocation-free.

/// Bind graphics or compute pipeline without disturbing the other bind point or descriptor/push state.
/// [in] nullable private borrowed command/pipeline tokens and core point0/1.
/// Void; malformed recording inputs invalidate. Mutex serialized, allocation-free.
/// Exact opcode acknowledgment precedes local state and lifetime reference publication.
fn bind_pipeline(command_buffer: c.VkCommandBuffer, point: u32, pipeline: c.VkPipeline) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    const pool = command_pool_for(record) orelse return;
    const target = if (pipeline) |value| child_object(@intFromPtr(value), c.VK_OBJECT_TYPE_PIPELINE, pool.parent_id) else null;
    if (point > 1 or target == null or resource_state(target.?).pipeline_bind_point != point) {
        state.command_state = .Invalid;
        return;
    }
    var staged_graphics: graphics_state.recording_t = .{};
    if (point == 0) {
        if (!graphics_family_supported(pool)) {
            if (lost == c.RingOk) state.command_state = .Invalid;
            return;
        }
        staged_graphics = graphics_recording(state).*;
        graphics_state.bind_pipeline(&staged_graphics, target.?.handle, resource_state(target.?).render_format) catch unreachable;
    }
    const writer = compute_wire.bind_pipeline(record.id, target.?.id, point) catch unreachable;
    if (!command_acknowledged(&writer, 93)) return;
    if (point == 0) graphics_recording(state).* = staged_graphics else command_profile(record).pipeline = target.?.handle;
    command_reference(state, target.?);
}
/// Bind copied-definition-compatible static buffer descriptor sets, including before pipeline binding.
/// [in] command/layout/set tokens borrowed; count1..16-first; sets must be nonnull.
/// [in] dynamic_count must be zero; dynamic_offsets borrowed unused, rejected before dereference.
/// Void; invalid recordings invalidate. Mutex serialized, allocation-free; owns copied definitions.
fn bind_descriptor_sets(command_buffer: c.VkCommandBuffer, point: u32, layout: c.VkPipelineLayout, first: u32, count: u32, sets: [*c]const c.VkDescriptorSet, dynamic_count: u32, dynamic_offsets: [*c]const u32) callconv(.C) void {
    _ = dynamic_offsets;
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    const pool = command_pool_for(record) orelse return;
    if (point > 1 or first >= 16 or count == 0 or count > 16 - first or dynamic_count != 0 or (count != 0 and sets == null) or layout == null) {
        state.command_state = .Invalid;
        return;
    }
    const layout_record = child_object(@intFromPtr(layout.?), c.VK_OBJECT_TYPE_PIPELINE_LAYOUT, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return;
    };
    const definition = profiles.get_profile(&profile_registry.pipeline_layouts, resource_state(layout_record).profile_index).?;
    if (first + count > definition.set_count) {
        state.command_state = .Invalid;
        return;
    }
    var targets: [16]*c.venus_object_t = undefined;
    var ids: [16]u64 = undefined;
    var tokens: [16]u64 = undefined;
    if (count != 0) for (sets[0..count], 0..) |handle, index| {
        const target = descriptor_set_for(handle, pool.parent_id) orelse {
            state.command_state = .Invalid;
            return;
        };
        const profile = profiles.get_profile(&profile_registry.sets, resource_state(target).profile_index).?;
        if (!std.meta.eql(profile.layout, definition.sets[first + index])) {
            state.command_state = .Invalid;
            return;
        }
        targets[index] = target;
        ids[index] = target.id;
        tokens[index] = target.handle;
    };
    const writer = compute_wire.bind_descriptor_sets(record.id, layout_record.id, point, first, ids[0..count], &.{}) catch unreachable;
    if (!command_acknowledged(&writer, 103)) return;
    if (point == 0) compute_state.bind_graphics_sets(command_profile(record), definition, first, tokens[0..count]) catch unreachable else compute_state.bind_sets(command_profile(record), definition, first, tokens[0..count]) catch unreachable;
    for (targets[0..count]) |target| command_reference(state, target);
}
/// Record push bytes covered by declared ranges, including before pipeline binding.
/// [in] command/layout tokens borrowed; stages core mask; values nonnull accessible size4..256.
/// Void; malformed recording invalidates. Mutex serialized, no allocation/retained input pointer.
/// Copied per-stage compatibility and initialized-byte state publishes after native acknowledgment.
fn push_constants(command_buffer: c.VkCommandBuffer, layout: c.VkPipelineLayout, stages: u32, offset: u32, size: u32, values: ?*const anyopaque) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    const pool = command_pool_for(record) orelse return;
    if (layout == null or values == null or stages == 0 or stages & ~@as(u32, 0x3f) != 0 or offset % 4 != 0 or size == 0 or size % 4 != 0 or offset > profiles.MaxPushBytes or size > profiles.MaxPushBytes - offset) {
        state.command_state = .Invalid;
        return;
    }
    const target = child_object(@intFromPtr(layout.?), c.VK_OBJECT_TYPE_PIPELINE_LAYOUT, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return;
    };
    const definition = profiles.get_profile(&profile_registry.pipeline_layouts, resource_state(target).profile_index).?;
    var next = command_profile(record).*;
    compute_state.push_bytes(&next, definition, stages, offset, size) catch {
        state.command_state = .Invalid;
        return;
    };
    const bytes: [*]const u8 = @ptrCast(values.?);
    const writer = compute_wire.push_constants(record.id, target.id, stages, offset, bytes[0..size]) catch unreachable;
    if (command_acknowledged(&writer, 132)) command_profile(record).* = next;
}
/// Dispatch compute using compatible currently bound static buffer definitions.
/// [in] private command token borrowed; groups checked against actual queried device limits.
/// Void; malformed recording invalidates. Mutex serialized, no allocation or pointer retention.
/// Acknowledged dispatch retains consumed buffers; core descriptor updates invalidate recordings.
fn dispatch(command_buffer: c.VkCommandBuffer, x: u32, y: u32, z: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    if (!outside_render_pass(state)) return;
    const pool = command_pool_for(record) orelse return;
    var parent: ?*c.venus_object_t = null;
    for (&slots) |*slot| if (slot.id == pool.parent_id and slot.kind == c.VK_OBJECT_TYPE_DEVICE) {
        parent = slot;
        break;
    };
    if (!ensure_descriptor_limits(parent.?)) return;
    const groups = [3]u32{ x, y, z };
    const limits = device_cache(parent.?.handle).?.compute_group_limits;
    for (groups, limits) |value, maximum| if (value > maximum) {
        state.command_state = .Invalid;
        return;
    };
    const metadata = command_profile(record);
    _ = child_object(metadata.pipeline, c.VK_OBJECT_TYPE_PIPELINE, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return;
    };
    if (!validate_draw_descriptors(pool.parent_id,state,metadata,0x20)) { state.command_state=.Invalid; return; }
    const writer = compute_wire.dispatch(record.id, groups) catch unreachable;
    if (!command_acknowledged(&writer, 110)) return;
    retain_draw_descriptors(pool.parent_id,state,metadata,0x20);
}

/// Record a bounded fill of a private device buffer; CPU acknowledgment only.
/// @param[in] command_buffer Nullable private borrowed handle; invalid states ignored.
/// @param[in] buffer Nullable same-device bound TRANSFER_DST token, no ownership transfer.
/// @param[in] offset Four-byte aligned offset below requested buffer size.
/// @param[in] size Positive four-byte multiple within bounds, or UINT64_MAX WHOLE_SIZE.
/// @param[in] data Repeated native word, serialized under the little-endian contract.
/// @return Void; local invalid Recording inputs invalidate recording, loss poisons binding.
/// @note Mutex serialized, allocation-free; caller satisfies native queue capabilities.
fn fill_buffer(
    command_buffer: c.VkCommandBuffer,
    buffer: c.VkBuffer,
    offset: u64,
    size: u64,
    data: u32,
) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(
        @intFromPtr(command_buffer.?),
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
    ) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    if (!outside_render_pass(state)) return;
    const pool = command_pool_for(record) orelse return;
    const target = if (buffer != null) child_object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_BUFFER,
        pool.parent_id,
    ) else null;
    if (target == null) {
        state.command_state = .Invalid;
        return;
    }
    const destination = resource_state(target.?);
    if (destination.bound_memory == 0 or destination.buffer_usage & 2 == 0 or
        offset % 4 != 0 or offset >= destination.buffer_size or
        (size != std.math.maxInt(u64) and (size == 0 or size % 4 != 0 or
        size > destination.buffer_size - offset)))
    {
        state.command_state = .Invalid;
        return;
    }
    var writer = writer_t{};
    writer.header(118, record.id);
    writer.put(u64, target.?.id);
    writer.put(u64, offset);
    writer.put(u64, size);
    writer.put(u32, data);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 118) {
        _ = failure(c.RingCorrupt);
        return;
    }
    const index = resource_index(target.?);
    state.buffer_references[index / 64] |= @as(u64, 1) << @as(u6, @intCast(index % 64));
}
/// Record byte-granular bounded buffer copies after complete alias-range validation.
/// @param[in] command_buffer Nullable private borrowed Recording handle.
/// @param[in] source Nullable same-device bound TRANSFER_SRC token, borrowed.
/// @param[in] destination Nullable same-device bound TRANSFER_DST token, borrowed.
/// @param[in] count Number of borrowed regions,1..64; no partial recording on invalid input.
/// @param[in] regions Nonnull accessible array of count byte ranges; no pointer retained.
/// @return Void; local invalid Recording inputs invalidate recording; loss poisons binding.
/// @note Mutex serialized, no heap allocation. Caller supplies native synchronization.
fn copy_buffer(
    command_buffer: c.VkCommandBuffer,
    source: c.VkBuffer,
    destination: c.VkBuffer,
    count: u32,
    regions: [*c]const c.VkBufferCopy,
) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(
        @intFromPtr(command_buffer.?),
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
    ) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    if (!outside_render_pass(state)) return;
    const pool = command_pool_for(record) orelse return;
    const source_record = if (source != null) child_object(
        @intFromPtr(source.?),
        c.VK_OBJECT_TYPE_BUFFER,
        pool.parent_id,
    ) else null;
    const destination_record = if (destination != null) child_object(
        @intFromPtr(destination.?),
        c.VK_OBJECT_TYPE_BUFFER,
        pool.parent_id,
    ) else null;
    if (source_record == null or destination_record == null or
        count == 0 or count > 64 or regions == null)
    {
        state.command_state = .Invalid;
        return;
    }
    const source_state = resource_state(source_record.?);
    const destination_state = resource_state(destination_record.?);
    if (source_state.bound_memory == 0 or destination_state.bound_memory == 0 or
        source_state.buffer_usage & 1 == 0 or destination_state.buffer_usage & 2 == 0)
    {
        state.command_state = .Invalid;
        return;
    }
    for (regions[0..count]) |region| {
        if (region.size == 0 or region.srcOffset >= source_state.buffer_size or
            region.dstOffset >= destination_state.buffer_size or
            region.size > source_state.buffer_size - region.srcOffset or
            region.size > destination_state.buffer_size - region.dstOffset)
        {
            state.command_state = .Invalid;
            return;
        }
    }
    if (source_state.bound_memory == destination_state.bound_memory) {
        for (regions[0..count]) |source_region| {
            const source_start = source_state.memory_offset + source_region.srcOffset;
            for (regions[0..count]) |destination_region| {
                const destination_start =
                    destination_state.memory_offset + destination_region.dstOffset;
                if (source_start < destination_start + destination_region.size and
                    destination_start < source_start + source_region.size)
                {
                    state.command_state = .Invalid;
                    return;
                }
            }
        }
    }
    var writer = writer_t{};
    writer.header(112, record.id);
    writer.put(u64, source_record.?.id);
    writer.put(u64, destination_record.?.id);
    writer.put(u32, count);
    writer.put(u64, count);
    for (regions[0..count]) |region| {
        writer.put(u64, region.srcOffset);
        writer.put(u64, region.dstOffset);
        writer.put(u64, region.size);
    }
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 112) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for ([_]*c.venus_object_t{ source_record.?, destination_record.? }) |buffer_record| {
        const index = resource_index(buffer_record);
        state.buffer_references[index / 64] |= @as(u64, 1) << @as(u6, @intCast(index % 64));
    }
}
/// Capture and record a complete bounded inline update, including the65536-byte limit.
/// @param[in] command_buffer Nullable private borrowed Recording handle.
/// @param[in] buffer Nullable same-device bound TRANSFER_DST token, borrowed.
/// @param[in] offset Four-byte aligned offset below requested buffer size.
/// @param[in] data_size Positive four-byte multiple at most65536 and within buffer bounds.
/// @param[in] data Nonnull accessible source[data_size], copied before dispatch; not retained.
/// @return Void; invalid Recording inputs invalidate; peer/transport loss poisons binding.
/// @note Mutex serialized; no heap allocation. Private staging scrubbed before unlock.
fn update_buffer(
    command_buffer: c.VkCommandBuffer,
    buffer: c.VkBuffer,
    offset: u64,
    data_size: u64,
    data: ?*const anyopaque,
) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(
        @intFromPtr(command_buffer.?),
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
    ) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    if (!outside_render_pass(state)) return;
    const pool = command_pool_for(record) orelse return;
    const target = if (buffer != null) child_object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_BUFFER,
        pool.parent_id,
    ) else null;
    if (target == null or data == null or data_size == 0 or data_size > MaxUpdateBytes or
        data_size % 4 != 0 or offset % 4 != 0)
    {
        state.command_state = .Invalid;
        return;
    }
    const destination = resource_state(target.?);
    if (destination.bound_memory == 0 or destination.buffer_usage & 2 == 0 or
        offset >= destination.buffer_size or data_size > destination.buffer_size - offset)
    {
        state.command_state = .Invalid;
        return;
    }
    var writer = writer_t{};
    writer.header(117, record.id);
    writer.put(u64, target.?.id);
    writer.put(u64, offset);
    writer.put(u64, data_size);
    writer.put(u64, data_size);
    std.debug.assert(writer.used == 48);
    const length = writer.used + @as(usize, @intCast(data_size));
    defer @memset(update_encoded[0..length], 0);
    defer @memset(tx[0 .. CommandPrefixBytes + length], 0);
    @memcpy(update_encoded[0..writer.used], writer.bytes[0..writer.used]);
    @memcpy(
        update_encoded[writer.used..length],
        @as([*]const u8, @ptrCast(data.?))[0..@intCast(data_size)],
    );
    const reply = transact(update_encoded[0..length]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 117) {
        _ = failure(c.RingCorrupt);
        return;
    }
    const index = resource_index(target.?);
    state.buffer_references[index / 64] |= @as(u64, 1) << @as(u6, @intCast(index % 64));
}
fn configured_family_pair(parent_id: u64, source: u32, destination: u32) bool {
    if (source == std.math.maxInt(u32) or destination == std.math.maxInt(u32))
        return source == destination;
    for (device_caches) |entry| {
        if (entry.handle == 0 or object(entry.handle, c.VK_OBJECT_TYPE_DEVICE).?.id != parent_id)
            continue;
        return std.mem.indexOfScalar(u32, entry.families[0..entry.family_count], source) != null and
            std.mem.indexOfScalar(u32, entry.families[0..entry.family_count], destination) != null;
    }
    return false;
}
/// Record core execution/global/buffer dependencies after validating complete arrays.
/// @param[in] command_buffer Nullable private borrowed Recording handle.
/// @param[in] source_stage Nonzero core Vulkan1.0 mask; caller ensures native compatibility.
/// @param[in] destination_stage Nonzero core mask; caller supplies feature/queue validity.
/// @param[in] dependency_flags Core0/1 BY_REGION only.
/// @param[in] memory_count Borrowed global barrier count0..64.
/// @param[in] memory_barriers Nullable only for zero count; canonical borrowed records.
/// @param[in] buffer_count Borrowed private buffer barrier count0..64.
/// @param[in] buffer_barriers Nullable only for zero count; same-device bound byte ranges.
/// @param[in] image_count Borrowed private image barrier count0..64; aggregate packet <=8192.
/// @param[in] image_barriers Nullable only for zero count; same-device bound images and valid ranges.
/// @return Void; invalid Recording inputs invalidate; peer/transport loss poisons binding.
/// @note Mutex serialized, no allocations; CPU acknowledgment is not GPU retirement.
fn pipeline_barrier(
    command_buffer: c.VkCommandBuffer,
    source_stage: u32,
    destination_stage: u32,
    dependency_flags: u32,
    memory_count: u32,
    memory_barriers: [*c]const c.VkMemoryBarrier,
    buffer_count: u32,
    buffer_barriers: [*c]const c.VkBufferMemoryBarrier,
    image_count: u32,
    image_barriers: [*c]const c.VkImageMemoryBarrier,
) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(
        @intFromPtr(command_buffer.?),
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
    ) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    if (!outside_render_pass(state)) return;
    const pool = command_pool_for(record) orelse return;
    if (source_stage == 0 or destination_stage == 0 or
        (source_stage | destination_stage) & ~@as(u32, 0x1ffff) != 0 or
        dependency_flags > 1 or image_count > 64 or memory_count > 64 or buffer_count > 64 or
        @as(u64, 64) + @as(u64, memory_count) * 20 + @as(u64, buffer_count) * 52 + @as(u64, image_count) * 64 > 8192 or
        (image_count != 0 and image_barriers == null) or
        (memory_count != 0 and memory_barriers == null) or
        (buffer_count != 0 and buffer_barriers == null))
    {
        state.command_state = .Invalid;
        return;
    }
    if (memory_count != 0) for (memory_barriers[0..memory_count]) |barrier| {
        if (barrier.sType != c.VK_STRUCTURE_TYPE_MEMORY_BARRIER or barrier.pNext != null or
            (barrier.srcAccessMask | barrier.dstAccessMask) & ~@as(u32, 0x1ffff) != 0)
        {
            state.command_state = .Invalid;
            return;
        }
    };
    var references: [64]*c.venus_object_t = undefined;
    if (buffer_count != 0) for (buffer_barriers[0..buffer_count], 0..) |barrier, index| {
        const target = if (barrier.buffer != null) child_object(
            @intFromPtr(barrier.buffer.?),
            c.VK_OBJECT_TYPE_BUFFER,
            pool.parent_id,
        ) else null;
        if (target == null or barrier.sType != c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER or
            barrier.pNext != null or
            (barrier.srcAccessMask | barrier.dstAccessMask) & ~@as(u32, 0x1ffff) != 0 or
            !configured_family_pair(
            pool.parent_id,
            barrier.srcQueueFamilyIndex,
            barrier.dstQueueFamilyIndex,
        )) {
            state.command_state = .Invalid;
            return;
        }
        const destination = resource_state(target.?);
        if (destination.bound_memory == 0 or barrier.offset >= destination.buffer_size or
            (barrier.size != std.math.maxInt(u64) and (barrier.size == 0 or
            barrier.size > destination.buffer_size - barrier.offset)))
        {
            state.command_state = .Invalid;
            return;
        }
        references[index] = target.?;
    };
    var image_references: [64]*c.venus_object_t = undefined;
    var image_encoded = render_wire.writer_t{};
    if (image_count != 0) for (image_barriers[0..image_count], 0..) |barrier, index| {
        const target = if (barrier.image != null) child_object(@intFromPtr(barrier.image.?), c.VK_OBJECT_TYPE_IMAGE, pool.parent_id) else null;
        if (target == null or !configured_family_pair(pool.parent_id, barrier.srcQueueFamilyIndex, barrier.dstQueueFamilyIndex)) {
            state.command_state = .Invalid;
            return;
        }
        const destination = resource_state(target.?);
        if (destination.bound_memory == 0 or !image_range_valid(destination, barrier.subresourceRange)) {
            state.command_state = .Invalid;
            return;
        }
        var translated = barrier;
        var device_handle: u64 = 0;
        for (device_caches) |entry| if (entry.handle != 0 and object(entry.handle, c.VK_OBJECT_TYPE_DEVICE).?.id == pool.parent_id) {
            device_handle = entry.handle; break;
        };
        if (wsi.is_present_image(&wsi_state, device_handle, target.?.handle)) {
            if (translated.oldLayout == c.VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) translated.oldLayout = c.VK_IMAGE_LAYOUT_GENERAL;
            if (translated.newLayout == c.VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) translated.newLayout = c.VK_IMAGE_LAYOUT_GENERAL;
        }
        render_wire.image_barrier(&image_encoded, @ptrCast(&translated), target.?.id) catch {
            state.command_state = .Invalid;
            return;
        };
        image_references[index] = target.?;
    };
    var writer = writer_t{};
    writer.header(126, record.id);
    writer.put(u32, source_stage);
    writer.put(u32, destination_stage);
    writer.put(u32, dependency_flags);
    writer.put(u32, memory_count);
    writer.put(u64, memory_count);
    if (memory_count != 0) for (memory_barriers[0..memory_count]) |barrier| {
        writer.put(u32, c.VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        writer.put(u64, 0);
        writer.put(u32, barrier.srcAccessMask);
        writer.put(u32, barrier.dstAccessMask);
    };
    writer.put(u32, buffer_count);
    writer.put(u64, buffer_count);
    if (buffer_count != 0) for (buffer_barriers[0..buffer_count], 0..) |barrier, index| {
        writer.put(u32, c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
        writer.put(u64, 0);
        writer.put(u32, barrier.srcAccessMask);
        writer.put(u32, barrier.dstAccessMask);
        writer.put(u32, barrier.srcQueueFamilyIndex);
        writer.put(u32, barrier.dstQueueFamilyIndex);
        writer.put(u64, references[index].id);
        writer.put(u64, barrier.offset);
        writer.put(u64, barrier.size);
    };
    writer.put(u32, image_count);
    writer.put(u64, image_count);
    @memcpy(writer.bytes[writer.used..][0..image_encoded.used], image_encoded.bytes[0..image_encoded.used]);
    writer.used += image_encoded.used;
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 126) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for (image_references[0..image_count]) |image_record| {
        const index = resource_index(image_record);
        state.buffer_references[index / 64] |= @as(u64, 1) << @as(u6, @intCast(index % 64));
    }
    for (references[0..buffer_count]) |buffer_record| {
        const index = resource_index(buffer_record);
        state.buffer_references[index / 64] |= @as(u64, 1) << @as(u6, @intCast(index % 64));
    }
}
/// Reset an individual nonpending buffer from a reset-capable private pool.
/// @param[in] buffer Nonnull private borrowed handle; no ownership transfer.
/// @param[in] flags0/1 release-resources only.
/// @return Host result, initialization error or sticky device loss.
/// @note Mutex serialized, allocation-free; changes state only after exact host success.
fn reset_command_buffer(buffer: c.VkCommandBuffer, flags: u32) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (buffer == null or flags > 1) return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const pool = command_pool_for(record) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (resource_state(pool).pool_flags & 2 == 0 or
        resource_state(record).command_state == .Pending) return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = writer_t{};
    writer.header(92, record.id);
    writer.put(u32, flags);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 92, 0);
    if (result == c.VK_SUCCESS) {
        resource_state(record).command_state = .Initial;
        resource_state(record).command_flags = 0;
        resource_state(record).buffer_references = [_]u64{0} ** 8;
        reset_command_profile(resource_state(record));
    }
    return result;
}
fn encode_fences(command_id: u32, device: c.VkDevice, fences: []const c.VkFence) ?writer_t {
    if (device == null or fences.len == 0 or fences.len > 64) return null;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return null;
    var writer = writer_t{};
    writer.header(command_id, parent.id);
    writer.put(u32, @intCast(fences.len));
    writer.put(u64, fences.len);
    for (fences) |fence| {
        if (fence == null) return null;
        const record = child_object(
            @intFromPtr(fence.?),
            c.VK_OBJECT_TYPE_FENCE,
            parent.id,
        ) orelse return null;
        writer.put(u64, record.id);
    }
    return writer;
}
/// Reset1..64 device-owned fences after the caller retires their GPU work.
/// @param[in] device Nonnull live private parent.
/// @param[in] count Accessible native input count1..64; validated before slicing.
/// @param[in] fences Nonnull borrowed immutable handles[count], retained only for call.
/// @return Host result, local device error or sticky transport/peer device loss.
/// @note Mutex serialized, allocation-free; never resets a foreign device's object.
fn reset_fences(device: c.VkDevice, count: u32, fences: [*c]const c.VkFence) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (count == 0 or count > 64 or fences == null) return c.VK_ERROR_DEVICE_LOST;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const writer = encode_fences(37, device, fences[0..count]) orelse return c.VK_ERROR_DEVICE_LOST;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE).?;
    for (fences[0..count]) |fence|
        if (resource_state(child_object(@intFromPtr(fence.?), c.VK_OBJECT_TYPE_FENCE, parent.id).?).inflight_count != 0)
            return c.VK_ERROR_INITIALIZATION_FAILED;
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    return result_reply(reply, 37, 0);
}
/// Poll one native fence without blocking host execution.
/// @param[in] device Nonnull live private parent, not dereferenced as native pointer.
/// @param[in] fence Nonnull device-owned token; borrowed until call ends.
/// @return VK_SUCCESS, VK_NOT_READY, negative host result or sticky device loss.
/// @note Mutex serialized, no allocation; CPU completion does not imply signaled GPU fence.
fn get_fence_status(device: c.VkDevice, fence: c.VkFence) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (device == null or fence == null or lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_DEVICE_LOST;
    const record = child_object(
        @intFromPtr(fence.?),
        c.VK_OBJECT_TYPE_FENCE,
        parent.id,
    ) orelse return c.VK_ERROR_DEVICE_LOST;
    return fence_status_locked(parent, record);
}
fn fence_status_locked(parent: *const c.venus_object_t, record: *const c.venus_object_t) c_int {
    var writer = writer_t{};
    writer.header(38, parent.id);
    writer.put(u64, record.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 38, c.VK_NOT_READY);
    if (result == c.VK_SUCCESS) {
        retire_fence(record.handle);
        return synchronize_device_mappings(parent.id, false);
    }
    return result;
}

fn wait_round(device: c.VkDevice, fences: []const c.VkFence, all: u32) c_int {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var writer = encode_fences(39, device, fences) orelse return c.VK_ERROR_DEVICE_LOST;
    writer.put(u32, all);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 39, c.VK_TIMEOUT);
    if (result != c.VK_SUCCESS) return result;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE).?;
    for (fences) |fence| {
        const record = child_object(@intFromPtr(fence.?), c.VK_OBJECT_TYPE_FENCE, parent.id).?;
        if (all != 0) {
            retire_fence(record.handle);
        } else {
            const status = fence_status_locked(parent, record);
            if (status != c.VK_SUCCESS and status != c.VK_NOT_READY) return status;
        }
    }
    return synchronize_device_mappings(parent.id, false);
}
/// Wait with nonblocking host rounds and the caller's monotonic timeout.
/// @param[in] device Nonnull live parent, must remain live throughout call.
/// @param[in] count Input count1..64.
/// @param[in] fences Nonnull borrowed immutable handles[count]; snapshot privately, no retention.
/// @param[in] all Canonical0/1 any/all selection; waited fences must remain live.
/// @param[in] timeout Nanoseconds, zero still polls once; UINT64_MAX effectively indefinite.
/// @return VK_SUCCESS, VK_TIMEOUT or explicit negative host/local/device error.
/// @note No allocation. Mutex released between rounds so other threads can submit signaling work.
fn wait_fences(
    device: c.VkDevice,
    count: u32,
    fences: [*c]const c.VkFence,
    all: u32,
    timeout: u64,
) callconv(.C) c_int {
    if (count == 0 or count > 64 or fences == null or all > 1) return c.VK_ERROR_DEVICE_LOST;
    var snapshot: [64]c.VkFence = undefined;
    @memcpy(snapshot[0..count], fences[0..count]);
    var timer = std.time.Timer.start() catch {
        lock_icd();
        defer unlock_icd();
        return failure(c.RingInvalid);
    };
    while (true) {
        const result = wait_round(device, snapshot[0..count], all);
        if (result != c.VK_TIMEOUT) return result;
        const elapsed = timer.read();
        if (elapsed >= timeout) return c.VK_TIMEOUT;
        std.time.sleep(@min(std.time.ns_per_ms, timeout - elapsed));
    }
}
const IdleDeadlineNs: u64 = std.time.ns_per_s;
var gpu_fences = [_]u64{0} ** 64;
fn gpu_exchange(kind: u32, ring: u32, fence: u64, response: *c.venus_request_t) c_int {
    var request = std.mem.zeroes(c.venus_request_t);
    request.kind = kind;
    request.argument_zero = ring;
    request.argument_one = fence;
    response.* = std.mem.zeroes(c.venus_request_t);
    const status = command.exchange.?(command.context, &request, null, 0, response, null, 0);
    if (status != c.RingOk) return status;
    if (response.kind != kind or response.direction != 1 or response.status != 0 or
        response.resource_id != 0 or response.flags != 0 or response.payload_bytes != 0 or
        response.argument_one != 0 or (kind == c.RequestGpuPoll and response.argument_zero != 0))
        return c.RingCorrupt;
    return c.RingOk;
}
fn end_idle(handle: u64, kind: u32, id: u64, binding_namespace: u32) void {
    if (objects.namespace_id != binding_namespace) return;
    const record = object(handle, kind) orelse return;
    if (record.id != id) return;
    const state = resource_state(record);
    std.debug.assert(state.idle_refs == 1);
    state.idle_refs = 0;
}
fn ring_idle(ring: u32, timer: *std.time.Timer) c_int {
    const binding_namespace = objects.namespace_id;
    var fence: u64 = 0;
    while (true) {
        if (objects.namespace_id != binding_namespace or command.exchange == null or lost != c.RingOk)
            return c.VK_ERROR_DEVICE_LOST;
        if (timer.read() >= IdleDeadlineNs) return failure(c.RingTimeout);
        var response: c.venus_request_t = undefined;
        const kind: u32 = if (fence == 0) c.RequestGpuFence else c.RequestGpuPoll;
        const status = gpu_exchange(kind, ring, fence, &response);
        if (timer.read() >= IdleDeadlineNs) return failure(c.RingTimeout);
        if (status == c.RingOk) {
            if (fence != 0) return c.VK_SUCCESS;
            if (response.argument_zero <= gpu_fences[ring]) return failure(c.RingCorrupt);
            fence = response.argument_zero;
            gpu_fences[ring] = fence;
        } else if (status != c.RingAgain) {
            return failure(status);
        }
        unlock_icd();
        if (status == c.RingAgain) std.time.sleep(std.time.ns_per_ms) else std.Thread.yield() catch {};
        lock_icd();
    }
}
/// Wait for actual retirement of every initialized queue in the borrowed device.
/// @param[in] device Nullable private handle, validated without dereference.
/// @return VK_SUCCESS or sticky VK_ERROR_DEVICE_LOST on invalid/clock/deadline/peer errors.
/// @note Mutex serialized; no allocation; pending GPU loss requires receiver retirement/abandon.
fn device_wait_idle(device: c.VkDevice) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (device == null or lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const entry = device_cache(@intFromPtr(device.?)) orelse return c.VK_ERROR_DEVICE_LOST;
    const record = object(entry.handle, c.VK_OBJECT_TYPE_DEVICE).?;
    if (resource_state(record).idle_refs != 0) return c.VK_ERROR_DEVICE_LOST;
    for (entry.queues) |handle| if (handle != 0) {
        if (resource_state(object(handle, c.VK_OBJECT_TYPE_QUEUE).?).idle_refs != 0)
            return c.VK_ERROR_DEVICE_LOST;
    };
    resource_state(record).idle_refs = 1;
    const idle_handle = record.handle;
    const idle_id = record.id;
    const idle_namespace = objects.namespace_id;
    defer end_idle(idle_handle, c.VK_OBJECT_TYPE_DEVICE, idle_id, idle_namespace);
    var timer = std.time.Timer.start() catch return failure(c.RingInvalid);
    for (entry.rings, 0..) |ring, index| if (entry.ready[index]) {
        const result = ring_idle(ring, &timer);
        if (result != c.VK_SUCCESS) return result;
        retire_queue(entry.queues[index]);
    };
    return synchronize_device_mappings(record.id, false);
}
/// Wait for actual GPU retirement of the queue using its receiver timeline.
/// @param[in] queue Nullable private handle, validated without dereference.
/// @return VK_SUCCESS or VK_ERROR_DEVICE_LOST; same deadline/lifetime contract as device idle.
/// @note No allocation; mutex held per exchange, released between polls so other queues progress.
/// Active idle reference prevents queue/device destruction and same-queue submission.
fn queue_wait_idle(queue: c.VkQueue) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (queue == null or lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const record = object(
        @intFromPtr(queue.?),
        c.VK_OBJECT_TYPE_QUEUE,
    ) orelse return c.VK_ERROR_DEVICE_LOST;
    for (&device_caches) |*entry| {
        if (entry.handle == 0) continue;
        for (entry.queues, 0..) |handle, index| if (handle == record.handle and entry.ready[index]) {
            const parent = object(entry.handle, c.VK_OBJECT_TYPE_DEVICE).?;
            if (resource_state(record).idle_refs != 0 or resource_state(parent).idle_refs != 0)
                return c.VK_ERROR_DEVICE_LOST;
            resource_state(record).idle_refs = 1;
            const idle_handle = record.handle;
            const idle_id = record.id;
            const idle_namespace = objects.namespace_id;
            defer end_idle(idle_handle, c.VK_OBJECT_TYPE_QUEUE, idle_id, idle_namespace);
            var timer = std.time.Timer.start() catch return failure(c.RingInvalid);
            const result = ring_idle(entry.rings[index], &timer);
            if (result == c.VK_SUCCESS) {
                retire_queue(idle_handle);
                return synchronize_device_mappings(parent.id, false);
            }
            return result;
        };
    }
    return failure(c.RingCorrupt);
}
/// Submit bounded canonical core work to the native host queue.
/// @param[in] queue Nonnull initialized private queue, borrowed until device destruction.
/// @param[in] count Number of borrowed submit records0..16; aggregate arrays each at most64.
/// @param[in] submits Nullable iff count0; accessible records/arrays, retained only during call.
/// @param[in] fence Nullable unsignaled device-owned fence; retained until actual completion.
/// @return Native result, initialization/capacity error or sticky device loss.
/// @note Mutex serialized, allocation-free. Success owns fixed pending tickets until GPU proof.
fn queue_submit(
    queue: c.VkQueue,
    count: u32,
    submits: [*c]const c.VkSubmitInfo,
    fence: c.VkFence,
) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (queue == null or (count != 0 and submits == null)) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (count > 16) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    const record = object(@intFromPtr(queue.?), c.VK_OBJECT_TYPE_QUEUE) orelse
        return c.VK_ERROR_INITIALIZATION_FAILED;
    if (resource_state(record).id != record.id) return c.VK_ERROR_INITIALIZATION_FAILED;
    var available: ?*submission_ticket_t = null;
    for (&submission_tickets) |*ticket| if (ticket.queue == 0) {
        available = ticket;
        break;
    };
    const target = available orelse return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    if (submission_sequence == std.math.maxInt(u64)) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    const parent = object(device_cache_for_queue(record).handle, c.VK_OBJECT_TYPE_DEVICE).?;
    if (resource_state(record).idle_refs != 0 or resource_state(parent).idle_refs != 0)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    var staged = submission_ticket_t{ .queue = record.handle };
    var fence_record: ?*c.venus_object_t = null;
    if (fence != null) {
        fence_record = child_object(@intFromPtr(fence.?), c.VK_OBJECT_TYPE_FENCE, record.parent_id) orelse
            return c.VK_ERROR_INITIALIZATION_FAILED;
        if (resource_state(fence_record.?).inflight_count != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
        staged.fence = fence_record.?.handle;
        _ = include_reference(&staged, fence_record.?);
    }
    var waits: u32 = 0;
    var signals: u32 = 0;
    var buffers: u32 = 0;
    if (count != 0) for (submits[0..count]) |info| {
        if (info.sType != c.VK_STRUCTURE_TYPE_SUBMIT_INFO or info.pNext != null)
            return c.VK_ERROR_INITIALIZATION_FAILED;
        if (info.waitSemaphoreCount > 64 - waits or info.signalSemaphoreCount > 64 - signals or
            info.commandBufferCount > 64 - buffers) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
        waits += info.waitSemaphoreCount;
        signals += info.signalSemaphoreCount;
        buffers += info.commandBufferCount;
        if ((info.waitSemaphoreCount != 0 and (info.pWaitSemaphores == null or info.pWaitDstStageMask == null)) or
            (info.signalSemaphoreCount != 0 and info.pSignalSemaphores == null) or
            (info.commandBufferCount != 0 and info.pCommandBuffers == null))
            return c.VK_ERROR_INITIALIZATION_FAILED;
        if (info.waitSemaphoreCount != 0) for (info.pWaitSemaphores[0..info.waitSemaphoreCount], 0..) |semaphore, index| {
            if (semaphore == null or info.pWaitDstStageMask[index] == 0 or
                info.pWaitDstStageMask[index] & ~@as(u32, 0x1ffff) != 0)
                return c.VK_ERROR_INITIALIZATION_FAILED;
            const child = child_object(@intFromPtr(semaphore.?), c.VK_OBJECT_TYPE_SEMAPHORE, record.parent_id) orelse
                return c.VK_ERROR_INITIALIZATION_FAILED;
            _ = include_reference(&staged, child);
        };
        if (info.signalSemaphoreCount != 0) for (info.pSignalSemaphores[0..info.signalSemaphoreCount]) |semaphore| {
            if (semaphore == null) return c.VK_ERROR_INITIALIZATION_FAILED;
            const child = child_object(@intFromPtr(semaphore.?), c.VK_OBJECT_TYPE_SEMAPHORE, record.parent_id) orelse
                return c.VK_ERROR_INITIALIZATION_FAILED;
            _ = include_reference(&staged, child);
        };
        if (info.commandBufferCount != 0) for (info.pCommandBuffers[0..info.commandBufferCount]) |buffer| {
            if (buffer == null) return c.VK_ERROR_INITIALIZATION_FAILED;
            const child = object(@intFromPtr(buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse
                return c.VK_ERROR_INITIALIZATION_FAILED;
            const pool = command_pool_for(child) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
            const state = resource_state(child);
            if (pool.parent_id != record.parent_id or resource_state(pool).pool_family != resource_state(record).queue_family or
                state.command_level != 0 or (state.command_state != .Executable and
                !(state.command_state == .Pending and state.command_flags & 4 != 0)))
                return c.VK_ERROR_INITIALIZATION_FAILED;
            if (include_reference(&staged, child) and state.command_flags & 4 == 0)
                return c.VK_ERROR_INITIALIZATION_FAILED;
        };
    };
    // Retain every recorded identity, rather than only the submitted command buffer.
    // Successful descriptor updates invalidate recordings; dispatch records exact consumed buffers.
    for (slots, 0..) |child, index| {
        const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
        if (staged.references[index / 64] & bit == 0 or child.kind != c.VK_OBJECT_TYPE_COMMAND_BUFFER) continue;
        if (!refresh_command_descriptors(@constCast(&slots[index]))) return c.VK_ERROR_INITIALIZATION_FAILED;
        for (&staged.references, resource_states[index].buffer_references) |*word, references| word.* |= references;
    }
    for (slots, 0..) |child, index| {
        const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
        if (staged.references[index / 64] & bit == 0) continue;
        if (child.id == 0) return c.VK_ERROR_INITIALIZATION_FAILED;
        if (child.kind == c.VK_OBJECT_TYPE_COMMAND_BUFFER) continue;
        if (child.kind == c.VK_OBJECT_TYPE_DESCRIPTOR_SET) {
            const pool = descriptor_pool_for(&child) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
            if (pool.parent_id != parent.id) return c.VK_ERROR_INITIALIZATION_FAILED;
        } else if (child.parent_id != parent.id) return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    if (fence_record) |selected| {
        const status = fence_status_locked(parent, selected);
        if (status == c.VK_SUCCESS) return c.VK_ERROR_INITIALIZATION_FAILED;
        if (status != c.VK_NOT_READY) return status;
    }
    var writer = writer_t{};
    writer.header(18, record.id);
    writer.put(u32, count);
    writer.put(u64, count);
    if (count != 0) for (submits[0..count]) |info| {
        writer.put(u32, c.VK_STRUCTURE_TYPE_SUBMIT_INFO);
        writer.put(u64, 0);
        writer.put(u32, info.waitSemaphoreCount);
        writer.put(u64, info.waitSemaphoreCount);
        if (info.waitSemaphoreCount != 0) for (info.pWaitSemaphores[0..info.waitSemaphoreCount]) |semaphore|
            writer.put(u64, child_object(@intFromPtr(semaphore.?), c.VK_OBJECT_TYPE_SEMAPHORE, record.parent_id).?.id);
        writer.put(u64, info.waitSemaphoreCount);
        if (info.waitSemaphoreCount != 0) for (info.pWaitDstStageMask[0..info.waitSemaphoreCount]) |stage| writer.put(u32, stage);
        writer.put(u32, info.commandBufferCount);
        writer.put(u64, info.commandBufferCount);
        if (info.commandBufferCount != 0) for (info.pCommandBuffers[0..info.commandBufferCount]) |buffer|
            writer.put(u64, object(@intFromPtr(buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER).?.id);
        writer.put(u32, info.signalSemaphoreCount);
        writer.put(u64, info.signalSemaphoreCount);
        if (info.signalSemaphoreCount != 0) for (info.pSignalSemaphores[0..info.signalSemaphoreCount]) |semaphore|
            writer.put(u64, child_object(@intFromPtr(semaphore.?), c.VK_OBJECT_TYPE_SEMAPHORE, record.parent_id).?.id);
    };
    writer.put(u64, if (fence_record) |selected| selected.id else 0);
    const mapping_result = synchronize_device_mappings(parent.id, true);
    if (mapping_result != c.VK_SUCCESS) return mapping_result;
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 18, 0);
    if (result == c.VK_SUCCESS) {
        submission_sequence += 1;
        staged.sequence = submission_sequence;
        target.* = staged;
        retain_submission_mapping_spans(parent.id, staged.references);
        for (&resource_states, 0..) |*state, index| {
            const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
            if (staged.references[index / 64] & bit == 0) continue;
            std.debug.assert(state.inflight_count < submission_tickets.len);
            state.inflight_count += 1;
            if (slots[index].kind == c.VK_OBJECT_TYPE_COMMAND_BUFFER) state.command_state = .Pending;
        }
    }
    return result;
}
fn device_cache_for_queue(record: *const c.venus_object_t) *device_cache_t {
    for (&device_caches) |*entry| if (entry.handle != 0) {
        const parent = object(entry.handle, c.VK_OBJECT_TYPE_DEVICE).?;
        if (parent.id == record.parent_id) return entry;
    };
    unreachable;
}

fn device_proc(name: []const u8) c.PFN_vkVoidFunction {
    const Entries = .{
        .{ "vkCreateSampler", &create_sampler },
        .{ "vkDestroySampler", &destroy_sampler },
        .{ "vkCreateBufferView", &create_buffer_view },
        .{ "vkDestroyBufferView", &destroy_buffer_view },
        .{ "vkCreateQueryPool", &create_query_pool },
        .{ "vkDestroyQueryPool", &destroy_query_pool },
        .{ "vkGetQueryPoolResults", &get_query_pool_results },
        .{ "vkResetQueryPool", &reset_query_pool },
        .{ "vkResetQueryPoolEXT", &reset_query_pool },
        .{ "vkCmdBeginQuery", &cmd_begin_query },
        .{ "vkCmdEndQuery", &cmd_end_query },
        .{ "vkCmdResetQueryPool", &cmd_reset_query_pool },
        .{ "vkCmdWriteTimestamp", &cmd_write_timestamp },
        .{ "vkCmdWriteTimestamp2", &cmd_write_timestamp2 },
        .{ "vkCmdWriteTimestamp2KHR", &cmd_write_timestamp2 },
        .{ "vkCreatePipelineCache", &create_pipeline_cache },
        .{ "vkDestroyPipelineCache", &destroy_pipeline_cache },
        .{ "vkGetPipelineCacheData", &get_pipeline_cache_data },
        .{ "vkMergePipelineCaches", &merge_pipeline_caches },
        .{ "vkCreateEvent", &create_event },
        .{ "vkDestroyEvent", &destroy_event },
        .{ "vkGetEventStatus", &get_event_status },
        .{ "vkSetEvent", &set_event },
        .{ "vkResetEvent", &reset_event },

        .{ "vkGetDeviceProcAddr", &get_device_proc },
        .{ "vkDestroyDevice", &destroy_device },
        .{ "vkCreateSwapchainKHR", &create_swapchain },
        .{ "vkDestroySwapchainKHR", &destroy_swapchain },
        .{ "vkGetSwapchainImagesKHR", &swapchain_images },
        .{ "vkAcquireNextImageKHR", &acquire_next_image },
        .{ "vkQueuePresentKHR", &queue_present },
        .{ "vkGetDeviceQueue", &get_device_queue },
        .{ "vkDeviceWaitIdle", &device_wait_idle },
        .{ "vkQueueWaitIdle", &queue_wait_idle },
        .{ "vkQueueSubmit", &queue_submit },
        .{ "vkCreateDescriptorUpdateTemplate", &create_descriptor_update_template },
        .{ "vkCreateDescriptorUpdateTemplateKHR", &create_descriptor_update_template },
        .{ "vkDestroyDescriptorUpdateTemplate", &destroy_descriptor_update_template },
        .{ "vkDestroyDescriptorUpdateTemplateKHR", &destroy_descriptor_update_template },
        .{ "vkUpdateDescriptorSetWithTemplate", &update_descriptor_set_with_template },
        .{ "vkUpdateDescriptorSetWithTemplateKHR", &update_descriptor_set_with_template },
        .{ "vkGetDeviceQueue2", &get_device_queue2 },
        .{ "vkCmdSetEvent2", &cmd_set_event2 },
        .{ "vkCmdSetEvent2KHR", &cmd_set_event2 },
        .{ "vkCmdResetEvent2", &cmd_reset_event2 },
        .{ "vkCmdResetEvent2KHR", &cmd_reset_event2 },
        .{ "vkCmdWaitEvents2", &cmd_wait_events2 },
        .{ "vkCmdWaitEvents2KHR", &cmd_wait_events2 },
        .{ "vkCmdSetViewport", &set_viewport },
        .{ "vkCmdSetViewportWithCount", &set_viewport_with_count },
        .{ "vkCmdSetScissor", &set_scissor },
        .{ "vkCmdSetScissorWithCount", &set_scissor_with_count },
        .{ "vkCmdSetCullMode", &set_cull_mode },
        .{ "vkCmdSetFrontFace", &set_front_face },
        .{ "vkCmdSetPrimitiveTopology", &set_primitive_topology },
        .{ "vkCmdSetDepthTestEnable", &set_depth_test_enable },
        .{ "vkCmdSetDepthWriteEnable", &set_depth_write_enable },
        .{ "vkCmdSetDepthCompareOp", &set_depth_compare_op },
        .{ "vkCmdSetDepthBoundsTestEnable", &set_depth_bounds_test_enable },
        .{ "vkCmdSetStencilTestEnable", &set_stencil_test_enable },
        .{ "vkCmdSetRasterizerDiscardEnable", &set_rasterizer_discard_enable },
        .{ "vkCmdSetDepthBiasEnable", &set_depth_bias_enable },
        .{ "vkCmdSetPrimitiveRestartEnable", &set_primitive_restart_enable },
        .{ "vkCmdSetStencilCompareMask", &set_stencil_compare_mask },
        .{ "vkCmdSetStencilWriteMask", &set_stencil_write_mask },
        .{ "vkCmdSetStencilReference", &set_stencil_reference },
        .{ "vkCmdSetDepthBias", &set_depth_bias },
        .{ "vkCmdSetDepthBounds", &set_depth_bounds },
        .{ "vkCmdSetBlendConstants", &set_blend_constants },
        .{ "vkCmdSetStencilOp", &set_stencil_op },
        .{ "vkCmdBindVertexBuffers", &bind_vertex_buffers },
        .{ "vkCmdBindVertexBuffers2", &bind_vertex_buffers2 },
        .{ "vkCmdBindIndexBuffer", &bind_index_buffer },
        .{ "vkCmdBindIndexBuffer2KHR", &bind_index_buffer2 },
        .{ "vkCmdDrawIndexed", &draw_indexed },
        .{ "vkCmdDrawIndirect", &draw_indirect },
        .{ "vkCmdDrawIndexedIndirect", &draw_indexed_indirect },
        .{ "vkCmdCopyImage", &copy_image },
        .{ "vkCmdCopyBufferToImage", &copy_buffer_to_image },
        .{ "vkCmdBlitImage", &blit_image },
        .{ "vkCmdResolveImage", &cmd_resolve_image },
        .{ "vkCmdClearColorImage", &clear_color_image },
        .{ "vkCmdClearDepthStencilImage", &clear_depth_stencil_image },
        .{ "vkCmdCopyBuffer2", &copy_buffer2 },
        .{ "vkCmdCopyBuffer2KHR", &copy_buffer2 },
        .{ "vkCmdCopyImage2", &copy_image2 },
        .{ "vkCmdCopyImage2KHR", &copy_image2 },
        .{ "vkCmdBlitImage2", &blit_image2 },
        .{ "vkCmdBlitImage2KHR", &blit_image2 },
        .{ "vkCmdCopyBufferToImage2", &copy_buffer_to_image2 },
        .{ "vkCmdCopyBufferToImage2KHR", &copy_buffer_to_image2 },
        .{ "vkCmdCopyImageToBuffer2", &copy_image_to_buffer2 },
        .{ "vkCmdCopyImageToBuffer2KHR", &copy_image_to_buffer2 },
        .{ "vkCmdResolveImage2", &resolve_image2 },
        .{ "vkCmdResolveImage2KHR", &resolve_image2 },
        .{ "vkGetBufferMemoryRequirements2", &buffer_requirements2 },
        .{ "vkGetBufferMemoryRequirements2KHR", &buffer_requirements2 },
        .{ "vkGetImageMemoryRequirements2", &image_requirements2 },
        .{ "vkGetImageMemoryRequirements2KHR", &image_requirements2 },
        .{ "vkGetDeviceBufferMemoryRequirements", &device_buffer_requirements },
        .{ "vkGetDeviceImageMemoryRequirements", &device_image_requirements },
        .{ "vkBindBufferMemory2", &bind_buffer_memory2 },
        .{ "vkBindBufferMemory2KHR", &bind_buffer_memory2 },
        .{ "vkBindImageMemory2", &bind_image_memory2 },
        .{ "vkBindImageMemory2KHR", &bind_image_memory2 },
        .{ "vkGetImageSubresourceLayout", &image_subresource_layout },
        .{ "vkGetImageSubresourceLayout2KHR", &image_subresource_layout2 },
        .{ "vkGetDeviceImageSubresourceLayoutKHR", &device_image_subresource_layout },
        .{ "vkQueueSubmit2", &queue_submit2 },
        .{ "vkQueueSubmit2KHR", &queue_submit2 },
        .{ "vkCmdPipelineBarrier2", &pipeline_barrier2 },
        .{ "vkCmdPipelineBarrier2KHR", &pipeline_barrier2 },
        .{ "vkGetSemaphoreCounterValue", &get_semaphore_counter_value },
        .{ "vkGetSemaphoreCounterValueKHR", &get_semaphore_counter_value },
        .{ "vkSignalSemaphore", &signal_semaphore },
        .{ "vkSignalSemaphoreKHR", &signal_semaphore },
        .{ "vkWaitSemaphores", &wait_semaphores },
        .{ "vkWaitSemaphoresKHR", &wait_semaphores },
        .{ "vkGetBufferDeviceAddress", &get_buffer_device_address },
        .{ "vkGetBufferDeviceAddressKHR", &get_buffer_device_address },
        .{ "vkGetRenderAreaGranularity", &render_area_granularity },
        .{ "vkGetRenderingAreaGranularityKHR", &rendering_area_granularity },
        .{ "vkCreateFence", &create_fence },
        .{ "vkCreateSemaphore", &create_semaphore },
        .{ "vkDestroySemaphore", &destroy_semaphore },
        .{ "vkCmdBeginRenderPass", &begin_render_pass },
        .{ "vkCmdBeginRendering", &begin_rendering },
        .{ "vkCmdBeginRenderingKHR", &begin_rendering },
        .{ "vkCmdEndRendering", &end_rendering },
        .{ "vkCmdEndRenderingKHR", &end_rendering },
        .{ "vkCmdEndRenderPass", &end_render_pass },
        .{ "vkCmdDraw", &draw },
        .{ "vkCmdCopyImageToBuffer", &copy_image_to_buffer },
        .{ "vkCmdBindPipeline", &bind_pipeline },
        .{ "vkCmdBindDescriptorSets", &bind_descriptor_sets },
        .{ "vkCmdPushConstants", &push_constants },
        .{ "vkCmdDispatch", &dispatch },
        .{ "vkCreateGraphicsPipelines", &create_graphics_pipelines },
        .{ "vkCreateComputePipelines", &create_compute_pipelines },
        .{ "vkDestroyPipeline", &destroy_pipeline },
        .{ "vkCreateShaderModule", &create_shader_module },
        .{ "vkDestroyShaderModule", &destroy_shader_module },
        .{ "vkGetDescriptorSetLayoutSupport", &descriptor_layout_support },
        .{ "vkGetDescriptorSetLayoutSupportKHR", &descriptor_layout_support },
        .{ "vkCreateDescriptorSetLayout", &create_descriptor_layout },
        .{ "vkDestroyDescriptorSetLayout", &destroy_descriptor_layout },
        .{ "vkCreatePipelineLayout", &create_pipeline_layout },
        .{ "vkDestroyPipelineLayout", &destroy_pipeline_layout },
        .{ "vkCreateDescriptorPool", &create_descriptor_pool },
        .{ "vkDestroyDescriptorPool", &destroy_descriptor_pool },
        .{ "vkResetDescriptorPool", &reset_descriptor_pool },
        .{ "vkAllocateDescriptorSets", &allocate_descriptor_sets },
        .{ "vkFreeDescriptorSets", &free_descriptor_sets },
        .{ "vkUpdateDescriptorSets", &update_descriptor_sets },
        .{ "vkCreateFramebuffer", &create_framebuffer },
        .{ "vkDestroyFramebuffer", &destroy_framebuffer },
        .{ "vkCreateRenderPass", &create_render_pass },
        .{ "vkDestroyRenderPass", &destroy_render_pass },
        .{ "vkCreateImage", &create_image },
        .{ "vkDestroyImage", &destroy_image },
        .{ "vkGetImageMemoryRequirements", &image_requirements },
        .{ "vkBindImageMemory", &bind_image_memory },
        .{ "vkCreateImageView", &create_image_view },
        .{ "vkDestroyImageView", &destroy_image_view },
        .{ "vkCreateBuffer", &create_buffer },
        .{ "vkCreateCommandPool", &create_command_pool },
        .{ "vkAllocateCommandBuffers", &allocate_command_buffers },
        .{ "vkFreeCommandBuffers", &free_command_buffers },
        .{ "vkBeginCommandBuffer", &begin_command_buffer },
        .{ "vkCmdFillBuffer", &fill_buffer },
        .{ "vkCmdCopyBuffer", &copy_buffer },
        .{ "vkCmdUpdateBuffer", &update_buffer },
        .{ "vkCmdPipelineBarrier", &pipeline_barrier },
        .{ "vkEndCommandBuffer", &end_command_buffer },
        .{ "vkResetCommandBuffer", &reset_command_buffer },
        .{ "vkDestroyCommandPool", &destroy_command_pool },
        .{ "vkResetCommandPool", &reset_command_pool },
        .{ "vkAllocateMemory", &allocate_memory },
        .{ "vkFreeMemory", &free_memory },
        .{ "vkMapMemory", &map_memory },
        .{ "vkUnmapMemory", &unmap_memory },
        .{ "vkFlushMappedMemoryRanges", &flush_memory },
        .{ "vkInvalidateMappedMemoryRanges", &invalidate_memory },
        .{ "vkBindBufferMemory", &bind_buffer_memory },
        .{ "vkDestroyBuffer", &destroy_buffer },
        .{ "vkGetBufferMemoryRequirements", &buffer_requirements },
        .{ "vkDestroyFence", &destroy_fence },
        .{ "vkResetFences", &reset_fences },
        .{ "vkGetFenceStatus", &get_fence_status },
        .{ "vkWaitForFences", &wait_fences },
    };
    inline for (Entries) |entry| if (std.mem.eql(u8, name, entry[0])) return @ptrCast(entry[1]);
    return null;
}
/// Resolve implemented device functions for a validated live device; no native pointer dereference.
/// Mutex serialized, static borrowed function pointers remain accessible for process lifetime.
/// @param[in] device Nullable handle; must validate as live to resolve any procedure.
/// @param[in] name Nullable accessible NUL-terminated native bytes, at most256 bytes.
/// @return Borrowed static function pointer or NULL for unsupported/invalid inputs.
fn get_device_proc(device: c.VkDevice, name: [*c]const u8) callconv(.C) c.PFN_vkVoidFunction {
    const valid_name = bounded_name(name) orelse return null;
    lock_icd();
    defer unlock_icd();
    if (device == null or device_cache(@intFromPtr(device.?)) == null) return null;
    return device_proc(valid_name);
}
fn bounded_name(name: [*c]const u8) ?[]const u8 {
    if (name == null) return null;
    for (0..256) |index| if (name[index] == 0) return name[0..index];
    return null;
}
fn physical_proc(name: []const u8) c.PFN_vkVoidFunction {
    if (negotiated_capabilities_ready and (std.mem.eql(u8, name, "vkGetPhysicalDeviceFeatures2") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceFeatures2KHR"))) return @ptrCast(&features2);
    if (negotiated_capabilities_ready and (std.mem.eql(u8, name, "vkGetPhysicalDeviceProperties2") or
        std.mem.eql(u8, name, "vkGetPhysicalDeviceProperties2KHR"))) return @ptrCast(&properties2);
    const Entries = .{
        .{ "vkGetPhysicalDeviceFormatProperties2", &format_properties2 },
        .{ "vkGetPhysicalDeviceFormatProperties2KHR", &format_properties2 },
        .{ "vkGetPhysicalDeviceImageFormatProperties2", &image_properties2 },
        .{ "vkGetPhysicalDeviceImageFormatProperties2KHR", &image_properties2 },
        .{ "vkGetPhysicalDeviceQueueFamilyProperties2", &queue_properties2 },
        .{ "vkGetPhysicalDeviceQueueFamilyProperties2KHR", &queue_properties2 },
        .{ "vkGetPhysicalDeviceMemoryProperties2", &memory2 },
        .{ "vkGetPhysicalDeviceMemoryProperties2KHR", &memory2 },
        .{ "vkGetPhysicalDeviceSparseImageFormatProperties2", &sparse_properties2 },
        .{ "vkGetPhysicalDeviceSparseImageFormatProperties2KHR", &sparse_properties2 },

        .{ "vkGetPhysicalDeviceExternalBufferProperties", &external_buffer_properties },
        .{ "vkGetPhysicalDeviceExternalFenceProperties", &external_fence_properties },
        .{ "vkGetPhysicalDeviceExternalSemaphoreProperties", &external_semaphore_properties },
        .{ "vkGetPhysicalDeviceToolProperties", &tool_properties },
        .{ "vkGetPhysicalDeviceProperties", &properties },
        .{ "vkGetPhysicalDeviceFeatures", &features },
        .{ "vkGetPhysicalDeviceMemoryProperties", &memory },
        .{ "vkGetPhysicalDeviceFormatProperties", &format_properties },
        .{ "vkGetPhysicalDeviceImageFormatProperties", &image_properties },
        .{ "vkGetPhysicalDeviceQueueFamilyProperties", &queue_properties },
        .{ "vkGetPhysicalDeviceSparseImageFormatProperties", &sparse_properties },
        .{ "vkEnumerateDeviceExtensionProperties", &device_extensions },
        .{ "vkCreateDevice", &create_device },
        .{ "vkCreateWin32SurfaceKHR", &create_win32_surface },
        .{ "vkGetPhysicalDeviceWin32PresentationSupportKHR", &win32_presentation_support },
        .{ "vkDestroySurfaceKHR", &destroy_surface },
        .{ "vkGetPhysicalDeviceSurfaceSupportKHR", &surface_support },
        .{ "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", &surface_capabilities },
        .{ "vkGetPhysicalDeviceSurfaceFormatsKHR", &surface_formats },
        .{ "vkGetPhysicalDeviceSurfacePresentModesKHR", &surface_modes },
    };
    inline for (Entries) |entry| if (std.mem.eql(u8, name, entry[0])) return @ptrCast(entry[1]);
    return null;
}
/// Exact global/instance lookup; header defines names, lifetimes and no handle dereference.
export fn venus_icd_get_instance_proc_addr(
    instance: c.VkInstance,
    optional_name: [*c]const u8,
) c.PFN_vkVoidFunction {
    const name = bounded_name(optional_name) orelse return null;
    lock_icd();
    defer unlock_icd();
    if (instance != null and cache(@intFromPtr(instance.?)) == null) return null;
    const Globals = .{
        .{ "vkGetInstanceProcAddr", &venus_icd_get_instance_proc_addr },
        .{ "vkCreateInstance", &create_instance },
        .{ "vkEnumerateInstanceExtensionProperties", &enumerate_instance_extensions },
        .{ "vkEnumerateInstanceVersion", &enumerate_version },
    };
    inline for (Globals) |entry| if (std.mem.eql(u8, name, entry[0])) return @ptrCast(entry[1]);
    if (instance == null) return null;
    const Entries = .{
        .{ "vkDestroyInstance", &destroy_instance },
        .{ "vkEnumeratePhysicalDevices", &enumerate_physical },
        .{ "vkGetDeviceProcAddr", &get_device_proc },
    };
    inline for (Entries) |entry| if (std.mem.eql(u8, name, entry[0])) return @ptrCast(entry[1]);
    if (!instance_proc_allowed(@intFromPtr(instance.?), name)) return null;
    return physical_proc(name) orelse device_proc(name);
}
/// Physical query lookup; header defines validated instance and supported procedure scope.
export fn venus_icd_get_physical_proc_addr(
    instance: c.VkInstance,
    optional_name: [*c]const u8,
) c.PFN_vkVoidFunction {
    const name = bounded_name(optional_name) orelse return null;
    lock_icd();
    defer unlock_icd();
    if (instance == null or cache(@intFromPtr(instance.?)) == null) return null;
    if (!instance_proc_allowed(@intFromPtr(instance.?), name)) return null;
    return physical_proc(name);
}
comptime {
    @export(venus_icd_negotiate_loader, .{ .name = "vk_icdNegotiateLoaderICDInterfaceVersion" });
    @export(venus_icd_get_instance_proc_addr, .{ .name = "vk_icdGetInstanceProcAddr" });
    @export(venus_icd_get_physical_proc_addr, .{ .name = "vk_icdGetPhysicalDeviceProcAddr" });
}


// WSI callbacks borrow one stack-owned device/queue context for this synchronous transaction.
const wsi_callback_context_t = struct { device: u64, queue: c.VkQueue = null };
fn wsi_device(context: ?*anyopaque) c.VkDevice {
    return @ptrFromInt(@as(*const wsi_callback_context_t, @ptrCast(@alignCast(context.?))).device);
}
fn wsi_queue(context: ?*anyopaque) c.VkQueue {
    const private: *const wsi_callback_context_t = @ptrCast(@alignCast(context.?));
    if (private.queue != null) return private.queue;
    const device = wsi_device(context);
    const entry = device_cache(@intFromPtr(device.?)) orelse return null;
    if (entry.family_count == 0) return null;
    var queue: c.VkQueue = null;
    get_device_queue(device, entry.families[0], 0, &queue);
    return queue;
}
fn wsi_memory_type(device: c.VkDevice, bits: u32, flags: u32) ?u32 {
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return null;
    var physical: c.VkPhysicalDevice = null;
    for (slots) |entry| if (entry.kind == c.VK_OBJECT_TYPE_PHYSICAL_DEVICE and entry.id == parent.parent_id) {
        physical = @ptrFromInt(entry.handle);
        break;
    };
    const reply = query(physical, 8) orelse return null;
    var properties_value: c.VkPhysicalDeviceMemoryProperties = undefined;
    if (c.venus_values_memory_decode(&properties_value, reply.ptr, reply.len) != c.RingOk) {
        _ = failure(c.RingCorrupt);
        return null;
    }
    for (properties_value.memoryTypes[0..properties_value.memoryTypeCount], 0..) |value, index| {
        if (bits & (@as(u32, 1) << @as(u5, @intCast(index))) != 0 and value.propertyFlags & flags == flags)
            return @intCast(index);
    }
    return null;
}
fn wsi_create_image(context: ?*anyopaque, width: u32, height: u32, format: u32, usage: u32,
    image_out: *u64, memory_out: *u64) callconv(.C) c_int {
    const device = wsi_device(context);
    const info = c.VkImageCreateInfo{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = c.VK_IMAGE_TYPE_2D, .format = format, .extent = .{ .width = width, .height = height, .depth = 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = c.VK_SAMPLE_COUNT_1_BIT, .tiling = c.VK_IMAGE_TILING_OPTIMAL,
        .usage = usage | c.VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .sharingMode = c.VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = c.VK_IMAGE_LAYOUT_UNDEFINED };
    var image: c.VkImage = null;
    var result = create_image(device, &info, null, &image);
    if (result != c.VK_SUCCESS) return result;
    var requirements = std.mem.zeroes(c.VkMemoryRequirements);
    image_requirements(device, image, &requirements);
    const index = wsi_memory_type(device, requirements.memoryTypeBits, 0) orelse {
        destroy_image(device, image, null);
        return if (lost != c.RingOk) c.VK_ERROR_DEVICE_LOST else c.VK_ERROR_OUT_OF_DEVICE_MEMORY;
    };
    const allocation = c.VkMemoryAllocateInfo{ .sType = c.VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = index };
    var memory_handle: c.VkDeviceMemory = null;
    result = allocate_memory(device, &allocation, null, &memory_handle);
    if (result != c.VK_SUCCESS) { destroy_image(device, image, null); return result; }
    result = bind_image_memory(device, image, memory_handle, 0);
    if (result != c.VK_SUCCESS) {
        destroy_image(device, image, null);
        free_memory(device, memory_handle, null);
        return result;
    }
    image_out.* = @intFromPtr(image.?);
    memory_out.* = @intFromPtr(memory_handle.?);
    return c.VK_SUCCESS;
}
fn wsi_destroy_image(context: ?*anyopaque, image: u64, memory_handle: u64) callconv(.C) void {
    const device = wsi_device(context);
    destroy_image(device, @ptrFromInt(image), null);
    free_memory(device, @ptrFromInt(memory_handle), null);
}
fn wsi_acquire(context: ?*anyopaque, semaphore: u64, fence: u64) callconv(.C) c_int {
    const queue = wsi_queue(context);
    if (queue == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const signal: c.VkSemaphore = if (semaphore != 0) @ptrFromInt(semaphore) else null;
    const submission = c.VkSubmitInfo{ .sType = c.VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = if (signal != null) 1 else 0, .pSignalSemaphores = if (signal != null) &signal else null };
    return queue_submit(queue, 1, &submission, if (fence != 0) @ptrFromInt(fence) else null);
}
fn wsi_readback(context: ?*anyopaque, image_handle: u64, width: u32, height: u32, _: u32,
    pixels: [*]u8, length: usize, wait_count: u32, waits: [*c]const c.VkSemaphore) callconv(.C) c_int {
    const device = wsi_device(context);
    const queue = wsi_queue(context);
    if (queue == null or wait_count > 64 or length > MaxMappedBytes) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    const image: c.VkImage = @ptrFromInt(image_handle);
    const queue_family = resource_state(object(@intFromPtr(queue.?), c.VK_OBJECT_TYPE_QUEUE).?).queue_family;
    const buffer_info = c.VkBufferCreateInfo{ .sType = c.VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = length, .usage = c.VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = c.VK_SHARING_MODE_EXCLUSIVE };
    var buffer: c.VkBuffer = null;
    var result = create_buffer(device, &buffer_info, null, &buffer);
    if (result != c.VK_SUCCESS) return result;
    defer destroy_buffer(device, buffer, null);
    var requirements = std.mem.zeroes(c.VkMemoryRequirements);
    buffer_requirements(device, buffer, &requirements);
    const index = wsi_memory_type(device, requirements.memoryTypeBits,
        c.VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | c.VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) orelse
        return if (lost != c.RingOk) c.VK_ERROR_DEVICE_LOST else c.VK_ERROR_OUT_OF_DEVICE_MEMORY;
    const allocation = c.VkMemoryAllocateInfo{ .sType = c.VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = index };
    var memory_handle: c.VkDeviceMemory = null;
    result = allocate_memory(device, &allocation, null, &memory_handle);
    if (result != c.VK_SUCCESS) return result;
    // Buffer binding must be removed before freeing its backing memory.
    defer {
        destroy_buffer(device, buffer, null);
        buffer = null;
        free_memory(device, memory_handle, null);
    }
    result = bind_buffer_memory(device, buffer, memory_handle, 0);
    if (result != c.VK_SUCCESS) return result;
    const pool_info = c.VkCommandPoolCreateInfo{ .sType = c.VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = c.VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = queue_family };
    var pool: c.VkCommandPool = null;
    result = create_command_pool(device, &pool_info, null, &pool);
    if (result != c.VK_SUCCESS) return result;
    defer destroy_command_pool(device, pool, null);
    const command_info = c.VkCommandBufferAllocateInfo{ .sType = c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = c.VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    var command_buffer: c.VkCommandBuffer = null;
    result = allocate_command_buffers(device, &command_info, &command_buffer);
    if (result != c.VK_SUCCESS) return result;
    defer free_command_buffers(device, pool, 1, &command_buffer);
    const begin_info = c.VkCommandBufferBeginInfo{ .sType = c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = c.VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    result = begin_command_buffer(command_buffer, &begin_info);
    if (result != c.VK_SUCCESS) return result;
    const barrier = c.VkImageMemoryBarrier{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = c.VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = c.VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = c.VK_IMAGE_LAYOUT_GENERAL, .newLayout = c.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = c.VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = c.VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = .{ .aspectMask = c.VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 } };
    pipeline_barrier(command_buffer, c.VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, c.VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, null, 0, null, 1, &barrier);
    const region = c.VkBufferImageCopy{ .imageSubresource = .{ .aspectMask = c.VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = .{ .width = width, .height = height, .depth = 1 } };
    copy_image_to_buffer(command_buffer, image, c.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    var restore = barrier;
    restore.srcAccessMask = c.VK_ACCESS_TRANSFER_READ_BIT;
    restore.dstAccessMask = c.VK_ACCESS_MEMORY_READ_BIT;
    restore.oldLayout = c.VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    restore.newLayout = c.VK_IMAGE_LAYOUT_GENERAL;
    pipeline_barrier(command_buffer, c.VK_PIPELINE_STAGE_TRANSFER_BIT, c.VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, null, 0, null, 1, &restore);
    result = end_command_buffer(command_buffer);
    if (result != c.VK_SUCCESS) return result;
    var stages = [_]u32{c.VK_PIPELINE_STAGE_TRANSFER_BIT} ** 64;
    const submission = c.VkSubmitInfo{ .sType = c.VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = wait_count, .pWaitSemaphores = waits, .pWaitDstStageMask = &stages,
        .commandBufferCount = 1, .pCommandBuffers = &command_buffer };
    result = queue_submit(queue, 1, &submission, null);
    if (result != c.VK_SUCCESS) return result;
    result = queue_wait_idle(queue);
    if (result != c.VK_SUCCESS) return result;
    var mapping: ?*anyopaque = null;
    result = map_memory(device, memory_handle, 0, length, 0, &mapping);
    if (result != c.VK_SUCCESS) return result;
    defer unmap_memory(device, memory_handle);
    const range = c.VkMappedMemoryRange{ .sType = c.VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = memory_handle, .offset = 0, .size = length };
    result = invalidate_memory(device, 1, &range);
    if (result != c.VK_SUCCESS) return result;
    @memcpy(pixels[0..length], @as([*]const u8, @ptrCast(mapping.?))[0..length]);
    return c.VK_SUCCESS;
}
fn wsi_backend(context: *const wsi_callback_context_t) wsi.backend_t {
    return .{ .context = @constCast(context), .create = wsi_create_image, .destroy = wsi_destroy_image,
        .acquire = wsi_acquire, .readback = @ptrCast(&wsi_readback) };
}

// Native Win32 ABI represented without requiring Windows SDK typedefs on Linux builds.
const win32_surface_info_t = extern struct {
    type_tag: u32, next: ?*const anyopaque, flags: u32, instance: ?*anyopaque, window: ?*anyopaque,
};
/// Create a surface borrowing the caller HWND until destruction; no native window ownership transfer.
fn create_win32_surface(instance: c.VkInstance, info_address: ?*const anyopaque,
    _: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkSurfaceKHR) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (instance == null or info_address == null or @intFromPtr(info_address.?) % @alignOf(win32_surface_info_t) != 0)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    const owner = object(@intFromPtr(instance.?), c.VK_OBJECT_TYPE_INSTANCE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const info: *const win32_surface_info_t = @ptrCast(@alignCast(info_address.?));
    if (info.type_tag != c.VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR or info.next != null or info.flags != 0 or info.window == null)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    var id: u64 = 0;
    const result = wsi.create_surface(&wsi_state, owner.id, @intFromPtr(info.window.?), &id);
    if (result == c.VK_SUCCESS) output.* = @ptrFromInt(id);
    return result;
}
/// Destroy only a surface owned by this live instance; borrowed native window remains caller owned.
fn destroy_surface(instance: c.VkInstance, surface: c.VkSurfaceKHR, _: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (instance == null or surface == null) return;
    const owner = object(@intFromPtr(instance.?), c.VK_OBJECT_TYPE_INSTANCE) orelse return;
    _ = wsi.destroy_surface(&wsi_state, owner.id, @intFromPtr(surface.?));
}
fn surface_owned(physical: c.VkPhysicalDevice, surface: c.VkSurfaceKHR) bool {
    if (physical == null or surface == null) return false;
    const owner = object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) orelse return false;
    return wsi.supports_surface(&wsi_state, owner.parent_id, @intFromPtr(surface.?));
}
/// Report presentation support for a live surface and existing graphics queue family; borrowed output.
fn surface_support(physical: c.VkPhysicalDevice, family: u32, surface: c.VkSurfaceKHR, output: [*c]u32) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (output == null or !surface_owned(physical, surface)) return c.VK_ERROR_SURFACE_LOST_KHR;
    var count: u32 = 64;
    var values: [64]c.VkQueueFamilyProperties = undefined;
    queue_properties(physical, &count, &values);
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    output.* = @intFromBool(family < count and values[family].queueCount != 0 and
        values[family].queueFlags & c.VK_QUEUE_GRAPHICS_BIT != 0);
    return c.VK_SUCCESS;
}
/// Report native presentation eligibility for an actual graphics queue family; no HWND retained.
fn win32_presentation_support(physical: c.VkPhysicalDevice, family: u32) callconv(.C) u32 {
    lock_icd(); defer unlock_icd();
    if ((!builtin.is_test and builtin.os.tag != .windows) or physical == null or
        object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) == null) return 0;
    var count: u32 = 64;
    var values: [64]c.VkQueueFamilyProperties = undefined;
    queue_properties(physical, &count, &values);
    if (lost != c.RingOk) return 0;
    return @intFromBool(family < count and values[family].queueCount != 0 and
        values[family].queueFlags & c.VK_QUEUE_GRAPHICS_BIT != 0);
}
/// Return synchronized actual HWND dimensions and bounded swapchain capabilities; caller owns output.
fn surface_capabilities(physical: c.VkPhysicalDevice, surface: c.VkSurfaceKHR, output: [*c]c.VkSurfaceCapabilitiesKHR) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (output == null or !surface_owned(physical, surface)) return c.VK_ERROR_SURFACE_LOST_KHR;
    return wsi.capabilities(&wsi_state, @intFromPtr(surface.?), @ptrCast(output));
}
/// Enumerate native sink byte formats; count/fill output borrowed for this synchronous call.
fn surface_formats(physical: c.VkPhysicalDevice, surface: c.VkSurfaceKHR, count: [*c]u32, output: [*c]c.VkSurfaceFormatKHR) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (count == null or !surface_owned(physical, surface)) return c.VK_ERROR_SURFACE_LOST_KHR;
    return wsi.formats(&wsi_state, @intFromPtr(surface.?), @ptrCast(count), if (output != null) @ptrCast(output) else null);
}
/// Enumerate native sink presentation timing; output/count borrowed, no allocation.
fn surface_modes(physical: c.VkPhysicalDevice, surface: c.VkSurfaceKHR, count: [*c]u32, output: [*c]c.VkPresentModeKHR) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (count == null or !surface_owned(physical, surface)) return c.VK_ERROR_SURFACE_LOST_KHR;
    return wsi.modes(&wsi_state, @intFromPtr(surface.?), @ptrCast(count), if (output != null) @ptrCast(output) else null);
}
/// Create a device-owned swapchain with real bound host images; callbacks publish only complete owners.
fn create_swapchain(device: c.VkDevice, info: [*c]const c.VkSwapchainCreateInfoKHR,
    _: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkSwapchainKHR) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or device_cache(@intFromPtr(device.?)) == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (builtin.os.tag == .windows and device_cache(@intFromPtr(device.?)).?.enabled_state.extension_mask & 1 == 0) return c.VK_ERROR_EXTENSION_NOT_PRESENT;
    const handle = @intFromPtr(device.?);
    const device_record = object(handle, c.VK_OBJECT_TYPE_DEVICE).?;
    var physical: c.VkPhysicalDevice = null;
    for (slots) |candidate| if (candidate.kind == c.VK_OBJECT_TYPE_PHYSICAL_DEVICE and candidate.id == device_record.parent_id) {
        physical = @ptrFromInt(candidate.handle); break;
    };
    if (!surface_owned(physical, info.*.surface)) return c.VK_ERROR_SURFACE_LOST_KHR;
    const callback_context = wsi_callback_context_t{ .device = handle };
    const backend = wsi_backend(&callback_context);
    var id: u64 = 0;
    const result = wsi.create_swapchain(&wsi_state, &backend, handle, @ptrCast(info), &id);
    if (result == c.VK_SUCCESS) output.* = @ptrFromInt(id);
    return result;
}
/// Destroy owned swapchain images before their memory; uncertain transport retains opaque core owners.
fn destroy_swapchain(device: c.VkDevice, chain: c.VkSwapchainKHR, _: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (device == null or chain == null or device_cache(@intFromPtr(device.?)) == null) return;
    const handle = @intFromPtr(device.?);
    const callback_context = wsi_callback_context_t{ .device = handle };
    const backend = wsi_backend(&callback_context);
    wsi.destroy_swapchain(&wsi_state, &backend, handle, @intFromPtr(chain.?));
}
/// Enumerate immutable borrowed host-image handles; image lifetime follows swapchain ownership.
fn swapchain_images(device: c.VkDevice, chain: c.VkSwapchainKHR, count: [*c]u32, output: [*c]c.VkImage) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (device == null or chain == null or count == null or device_cache(@intFromPtr(device.?)) == null)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    return wsi.get_images(&wsi_state, @intFromPtr(device.?), @intFromPtr(chain.?), @ptrCast(count),
        if (output != null) @ptrCast(output) else null);
}
/// Acquire one owned image and signal requested real Vulkan sync objects before publishing its index.
fn acquire_next_image(device: c.VkDevice, chain: c.VkSwapchainKHR, timeout: u64,
    semaphore: c.VkSemaphore, fence: c.VkFence, index: [*c]u32) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (device == null or chain == null or index == null or device_cache(@intFromPtr(device.?)) == null)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    const handle = @intFromPtr(device.?);
    const callback_context = wsi_callback_context_t{ .device = handle };
    const backend = wsi_backend(&callback_context);
    return wsi.acquire_next(&wsi_state, &backend, handle, @intFromPtr(chain.?), timeout,
        if (semaphore) |value| @intFromPtr(value) else 0, if (fence) |value| @intFromPtr(value) else 0, @ptrCast(index));
}
/// Present real GPU pixels after consuming wait semaphores and synchronous completion. Temporary
/// host-visible buffers/maps are deterministically retired on success; failed transport requires abandonment.
fn queue_present(queue: c.VkQueue, info: [*c]const c.VkPresentInfoKHR) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (queue == null or info == null or info.*.sType != c.VK_STRUCTURE_TYPE_PRESENT_INFO_KHR or info.*.pNext != null or
        info.*.swapchainCount == 0 or info.*.swapchainCount > wsi.MaxSwapchains or info.*.pSwapchains == null or
        info.*.pImageIndices == null or info.*.waitSemaphoreCount > 64 or
        (info.*.waitSemaphoreCount != 0 and info.*.pWaitSemaphores == null)) return c.VK_ERROR_INITIALIZATION_FAILED;
    const owner = object(@intFromPtr(queue.?), c.VK_OBJECT_TYPE_QUEUE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const device = device_cache_for_queue(owner);
    const handle = device.handle;
    const callback_context = wsi_callback_context_t{ .device = handle, .queue = queue };
    const backend = wsi_backend(&callback_context);
    if (info.*.waitSemaphoreCount != 0) {
        var stages = [_]u32{c.VK_PIPELINE_STAGE_ALL_COMMANDS_BIT} ** 64;
        const wait_submission = c.VkSubmitInfo{ .sType = c.VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = info.*.waitSemaphoreCount, .pWaitSemaphores = info.*.pWaitSemaphores,
            .pWaitDstStageMask = &stages };
        const submitted = queue_submit(queue, 1, &wait_submission, null);
        if (submitted != c.VK_SUCCESS) return submitted;
        const completed = queue_wait_idle(queue);
        if (completed != c.VK_SUCCESS) return completed;
    }
    var overall: c_int = c.VK_SUCCESS;
    for (info.*.pSwapchains[0..info.*.swapchainCount], info.*.pImageIndices[0..info.*.swapchainCount], 0..) |chain, index, item| {
        const waits: []const c.VkSemaphore = &.{}; // Shared wait objects were consumed once above.
        const result = if (chain != null) wsi.present(&wsi_state, &backend, MappingAllocator, handle,
            @intFromPtr(chain.?), index, @ptrCast(waits)) else c.VK_ERROR_INITIALIZATION_FAILED;
        if (info.*.pResults != null) info.*.pResults[item] = result;
        if (overall == c.VK_SUCCESS and result != c.VK_SUCCESS) overall = result;
    }
    return overall;
}

/// Create device-owned sampler. [in] device/info borrowed nonnull; callbacks nullable unused.
/// [out] output nonnull, NULL on failure; success transfers token until destruction.
/// Native validation/loss/OOM returned; mutex serialized, packet and metadata owned locally.
fn create_sampler(device: c.VkDevice, info: [*c]const c.VkSamplerCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkSampler) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var packet = extra_wire.create_sampler(parent.id, 1, @ptrCast(info)) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_SAMPLER, &packet, &handle);
    if (result == c.VK_SUCCESS) output.* = @ptrFromInt(handle);
    return result;
}

/// Create device-owned query_pool. [in] device/info borrowed nonnull; callbacks nullable unused.
/// [out] output nonnull, NULL on failure; success transfers token until destruction.
/// Native validation/loss/OOM returned; mutex serialized, packet and metadata owned locally.
fn create_query_pool(device: c.VkDevice, info: [*c]const c.VkQueryPoolCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkQueryPool) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var packet = extra_wire.create_query_pool(parent.id, 1, @ptrCast(info)) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_QUERY_POOL, &packet, &handle);

    if (result == c.VK_SUCCESS) {
        const record = child_object(handle, c.VK_OBJECT_TYPE_QUERY_POOL, parent.id).?;
        const state = resource_state(record);
        state.buffer_size = info.*.queryCount;
        state.buffer_usage = info.*.queryType;
        state.descriptor_max_sets = info.*.pipelineStatistics;
    }
    if (result == c.VK_SUCCESS) output.* = @ptrFromInt(handle);
    return result;
}

/// Create device-owned pipeline_cache. [in] device/info borrowed nonnull; callbacks nullable unused.
/// [out] output nonnull, NULL on failure; success transfers token until destruction.
/// Native validation/loss/OOM returned; mutex serialized, packet and metadata owned locally.
fn create_pipeline_cache(device: c.VkDevice, info: [*c]const c.VkPipelineCacheCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkPipelineCache) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var packet = extra_wire.create_pipeline_cache(parent.id, 1, @ptrCast(info)) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_PIPELINE_CACHE, &packet, &handle);
    if (result == c.VK_SUCCESS) output.* = @ptrFromInt(handle);
    return result;
}

/// Create device-owned event. [in] device/info borrowed nonnull; callbacks nullable unused.
/// [out] output nonnull, NULL on failure; success transfers token until destruction.
/// Native validation/loss/OOM returned; mutex serialized, packet and metadata owned locally.
fn create_event(device: c.VkDevice, info: [*c]const c.VkEventCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkEvent) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var packet = extra_wire.create_event(parent.id, 1, @ptrCast(info)) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_EVENT, &packet, &handle);

    if (result == c.VK_SUCCESS) resource_state(child_object(handle, c.VK_OBJECT_TYPE_EVENT, parent.id).?).buffer_usage = info.*.flags;
    if (result == c.VK_SUCCESS) output.* = @ptrFromInt(handle);
    return result;
}

/// Create buffer view. [in] device/info borrowed, callbacks nullable unused.
/// [out] output NULL on failure, otherwise owned device token retaining backing buffer.
/// Return native/local invalid/OOM/loss; fixed metadata and packet, mutex serialized.
fn create_buffer_view(device: c.VkDevice, info: [*c]const c.VkBufferViewCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkBufferView) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or info.*.buffer == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const buffer = child_object(@intFromPtr(info.*.buffer.?), c.VK_OBJECT_TYPE_BUFFER, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const state = resource_state(buffer);
    if (state.buffer_usage & (c.VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | c.VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT) == 0 or
        info.*.offset >= state.buffer_size or (info.*.range != c.VK_WHOLE_SIZE and (info.*.range == 0 or info.*.range > state.buffer_size - info.*.offset))) return c.VK_ERROR_INITIALIZATION_FAILED;
    var packet = extra_wire.create_buffer_view(parent.id, 1, buffer.id, @ptrCast(info)) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_BUFFER_VIEW, &packet, &handle);
    if (result == c.VK_SUCCESS) {
        resource_state(child_object(handle, c.VK_OBJECT_TYPE_BUFFER_VIEW, parent.id).?).view_image = buffer.handle;
        output.* = @ptrFromInt(handle);
    }
    return result;
}

/// Destroy sampler. [in] nullable borrowed device/token/callbacks; no output.
/// Exact host completion retires guest identity; pending or lost owners stay retained.
/// Mutex serialized; no allocation, ownership transfer or guessed destruction.
fn destroy_sampler(device: c.VkDevice, value: c.VkSampler, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (value) |pointer| {
        const token = @intFromPtr(pointer);
        for (profile_registry.descriptor_layouts) |entry| if (entry.occupied) {
            if (std.mem.indexOfScalar(u64,entry.profile.immutable_samplers[0..entry.profile.immutable_count],token) != null) return;
        };
        for (profile_registry.sets) |entry| if (entry.occupied) {
            if (std.mem.indexOfScalar(u64,entry.profile.layout.immutable_samplers[0..entry.profile.layout.immutable_count],token) != null) return;
        };
    }
    destroy_render_resource(device, if (value) |pointer| @intFromPtr(pointer) else 0, c.VK_OBJECT_TYPE_SAMPLER, 71);
}

/// Destroy query_pool. [in] nullable borrowed device/token/callbacks; no output.
/// Exact host completion retires guest identity; pending or lost owners stay retained.
/// Mutex serialized; no allocation, ownership transfer or guessed destruction.
fn destroy_query_pool(device: c.VkDevice, value: c.VkQueryPool, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (value) |pointer| @intFromPtr(pointer) else 0, c.VK_OBJECT_TYPE_QUERY_POOL, 48);
}

/// Destroy pipeline_cache. [in] nullable borrowed device/token/callbacks; no output.
/// Exact host completion retires guest identity; pending or lost owners stay retained.
/// Mutex serialized; no allocation, ownership transfer or guessed destruction.
fn destroy_pipeline_cache(device: c.VkDevice, value: c.VkPipelineCache, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (value) |pointer| @intFromPtr(pointer) else 0, c.VK_OBJECT_TYPE_PIPELINE_CACHE, 62);
}

/// Destroy event. [in] nullable borrowed device/token/callbacks; no output.
/// Exact host completion retires guest identity; pending or lost owners stay retained.
/// Mutex serialized; no allocation, ownership transfer or guessed destruction.
fn destroy_event(device: c.VkDevice, value: c.VkEvent, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (value) |pointer| @intFromPtr(pointer) else 0, c.VK_OBJECT_TYPE_EVENT, 43);
}

/// Destroy buffer_view. [in] nullable borrowed device/token/callbacks; no output.
/// Exact host completion retires guest identity; pending or lost owners stay retained.
/// Mutex serialized; no allocation, ownership transfer or guessed destruction.
fn destroy_buffer_view(device: c.VkDevice, value: c.VkBufferView, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    destroy_render_resource(device, if (value) |pointer| @intFromPtr(pointer) else 0, c.VK_OBJECT_TYPE_BUFFER_VIEW, 53);
}

fn event_operation_locked(device: c.VkDevice, event: c.VkEvent, opcode: u32) c_int {
    if (device == null or event == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = child_object(@intFromPtr(event.?), c.VK_OBJECT_TYPE_EVENT, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (resource_state(record).buffer_usage & c.VK_EVENT_CREATE_DEVICE_ONLY_BIT != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const packet = extra_wire.event_operation(parent.id, record.id, opcode) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    const reply = transact(packet.bytes[0..packet.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch return failure(c.RingCorrupt);
    const result = reader.scalar(i32) catch return failure(c.RingCorrupt);
    if (received != opcode) return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0 and !(opcode == 44 and (result == c.VK_EVENT_SET or result == c.VK_EVENT_RESET))) return failure(c.RingCorrupt);
    return result;
}

/// Host event operation. [in] live borrowed device/event, no ownership transfer.
/// Returns actual event status/native result or local invalid/loss; mutex serialized.
fn get_event_status(device: c.VkDevice, event: c.VkEvent) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    return event_operation_locked(device, event, 44);
}

/// Host event operation. [in] live borrowed device/event, no ownership transfer.
/// Returns actual event status/native result or local invalid/loss; mutex serialized.
fn set_event(device: c.VkDevice, event: c.VkEvent) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (device == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const mapping_result = synchronize_device_mappings(parent.id, true);
    if (mapping_result != c.VK_SUCCESS) return mapping_result;
    return event_operation_locked(device, event, 45);
}

/// Host event operation. [in] live borrowed device/event, no ownership transfer.
/// Returns actual event status/native result or local invalid/loss; mutex serialized.
fn reset_event(device: c.VkDevice, event: c.VkEvent) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    return event_operation_locked(device, event, 46);
}

/// Read actual pool results. [in] device/pool live borrowed; output memory borrowed for call.
/// [out] data receives validated native bytes including availability on NOT_READY.
/// Returns native result/local invalid/OOM/loss; caller owns data, mutex serialized.
fn get_query_pool_results(device: c.VkDevice, pool: c.VkQueryPool, first: u32, count: u32, data_size: usize, data: ?*anyopaque, stride: u64, flags: u32) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (device == null or pool == null or data == null or count == 0 or data_size == 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = child_object(@intFromPtr(pool.?), c.VK_OBJECT_TYPE_QUERY_POOL, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const state = resource_state(record);
    if (first > state.buffer_size or count > state.buffer_size - first) return c.VK_ERROR_INITIALIZATION_FAILED;
    const words: u64 = if (state.buffer_usage == c.VK_QUERY_TYPE_PIPELINE_STATISTICS) @popCount(state.descriptor_max_sets) else 1;
    const word_size: u64 = if (flags & c.VK_QUERY_RESULT_64_BIT != 0) 8 else 4;
    const minimum = (words + @as(u64, if (flags & c.VK_QUERY_RESULT_WITH_AVAILABILITY_BIT != 0) 1 else 0)) * word_size;
    if (stride < minimum or stride % word_size != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const preceding = std.math.mul(u64, stride, count - 1) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    const required = std.math.add(u64, preceding, minimum) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    if (required > data_size) return c.VK_ERROR_INITIALIZATION_FAILED;
    const packet = extra_wire.query_results(parent.id, record.id, first, count, data_size, stride, flags) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    const reply = transact(packet.bytes[0..packet.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = extra_wire.decode_query_results(reply, @as([*]u8, @ptrCast(data.?))[0..data_size]) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    return result;
}
/// Obtain driver cache_handle bytes. [in] live borrowed device/cache_handle, nullable data query_index.
/// [in,out] size nonnull capacity/returned size; caller data owned through return.
/// Native success/incomplete or invalid/loss returned; bounded4096 transfer, mutex serialized.
fn get_pipeline_cache_data(device: c.VkDevice, cache_handle: c.VkPipelineCache, size: [*c]usize, data: ?*anyopaque) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (device == null or cache_handle == null or size == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = child_object(@intFromPtr(cache_handle.?), c.VK_OBJECT_TYPE_PIPELINE_CACHE, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const capacity = if (data == null) 0 else @min(size.*, 4096);
    const packet = extra_wire.cache_data(parent.id, record.id, capacity, data != null) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    const reply = transact(packet.bytes[0..packet.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const output: ?[]u8 = if (data) |pointer| @as([*]u8, @ptrCast(pointer))[0..capacity] else null;
    const result = extra_wire.decode_cache_data(reply, output) catch return failure(c.RingCorrupt);
    size.* = result.size;
    if (result.result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    return result.result;
}
/// Merge bounded same-device driver caches. [in] destination/sources borrowed through return.
/// Native result/local invalid/OOM/loss; no ownership transfer, mutex serialized.
fn merge_pipeline_caches(device: c.VkDevice, destination: c.VkPipelineCache, count: u32, sources: [*c]const c.VkPipelineCache) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (device == null or destination == null or count == 0 or count > 64 or sources == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const target = child_object(@intFromPtr(destination.?), c.VK_OBJECT_TYPE_PIPELINE_CACHE, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var ids: [64]u64 = undefined;
    for (sources[0..count], 0..) |source, index| {
        if (source == null or source == destination) return c.VK_ERROR_INITIALIZATION_FAILED;
        const record = child_object(@intFromPtr(source.?), c.VK_OBJECT_TYPE_PIPELINE_CACHE, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
        ids[index] = record.id;
    }
    const packet = extra_wire.merge_caches(parent.id, target.id, ids[0..count]) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    const reply = transact(packet.bytes[0..packet.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    return result_reply(reply, 64, 0);
}

fn query_command_locked(command_buffer: c.VkCommandBuffer, pool: c.VkQueryPool, opcode: u32, first: u32, count: u32, flags: u32, stage: u64) void {
    if (command_buffer == null or pool == null or lost != c.RingOk) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const command_pool = command_pool_for(record) orelse return;
    const target = child_object(@intFromPtr(pool.?), c.VK_OBJECT_TYPE_QUERY_POOL, command_pool.parent_id) orelse return;
    const state = resource_state(record);
    const target_state = resource_state(target);
    if (state.command_state != .Recording or count == 0 or first > target_state.buffer_size or count > target_state.buffer_size - first) {
        state.command_state = .Invalid;
        return;
    }
    const packet = extra_wire.query_command(record.id, target.id, opcode, first, count, flags, stage) catch {
        state.command_state = .Invalid;
        return;
    };
    if (command_acknowledged(&packet, opcode)) command_reference(state, target);
}
/// Record a query_index begin; borrowed command_buffer/pool and scalar query_index/control values.
/// Void; invalid recording is invalidated, ownership retained through pending GPU use.
/// Mutex serialized; no heap allocation or pointer retention.
fn cmd_begin_query(command_buffer: c.VkCommandBuffer, pool: c.VkQueryPool, query_index: u32, flags: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    query_command_locked(command_buffer, pool, 127, query_index, 1, flags, 0);
}
/// Record a query_index end. [in] command_buffer/pool borrowed live owners, query_index index copied.
/// Void; malformed usage invalidates recording; mutex serialized, no allocation.
fn cmd_end_query(command_buffer: c.VkCommandBuffer, pool: c.VkQueryPool, query_index: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    query_command_locked(command_buffer, pool, 128, query_index, 1, 0, 0);
}
/// Record pool-range reset. [in] borrowed live command_buffer/pool, copied bounded range.
/// Void; invalid usage invalidates recording; references persist through GPU completion.
fn cmd_reset_query_pool(command_buffer: c.VkCommandBuffer, pool: c.VkQueryPool, first: u32, count: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    query_command_locked(command_buffer, pool, 129, first, count, 0, 0);
}
/// Record classic timestamp. [in] borrowed command_buffer/pool, copied stage/query_index.
/// Void; native timestamp support governs valid stages; mutex serialized/no allocation.
fn cmd_write_timestamp(command_buffer: c.VkCommandBuffer, stage: u32, pool: c.VkQueryPool, query_index: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    query_command_locked(command_buffer, pool, 130, query_index, 1, 0, stage);
}
/// Record sync2 timestamp. [in] borrowed command_buffer/pool, copied stage64/query_index.
/// Void; enabled sync2 required by Vulkan device contract; mutex serialized.
fn cmd_write_timestamp2(command_buffer: c.VkCommandBuffer, stage: u64, pool: c.VkQueryPool, query_index: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    query_command_locked(command_buffer, pool, 205, query_index, 1, 0, stage);
}
/// Reset actual host query_index state. [in] device/pool borrowed, range copied.
/// Void; invalid/pending calls ignored, native loss retains owners; mutex serialized.
fn reset_query_pool(device: c.VkDevice, pool: c.VkQueryPool, first: u32, count: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (device == null or pool == null or lost != c.RingOk or count == 0) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(@intFromPtr(pool.?), c.VK_OBJECT_TYPE_QUERY_POOL, parent.id) orelse return;
    const state = resource_state(record);
    if (state.inflight_count != 0 or first > state.buffer_size or count > state.buffer_size - first) return;
    const packet = modern_sync.reset_query_pool(parent.id, record.id, first, count) catch return;
    _ = command_acknowledged(&packet, 171);
}

// Merge into ICD: const requirements2_wire=@import("venus_requirements2_wire.zig");
// physical_proc entries and KHR names listed at end. Unknown output payloads are
// preserved; all results originate in actual host queries. Recursive ICD lock
// permits composition without exposing the shared reply scratch to another thread.
fn query2_chain(next: ?*anyopaque) !?*c.VkFormatProperties3 {
    var addresses: [64]usize = undefined;
    var count: usize = 0;
    var current = next;
    var format3: ?*c.VkFormatProperties3 = null;
    while (current) |address| {
        const bits = @intFromPtr(address);
        if (bits % @alignOf(c.VkBaseOutStructure) != 0 or count == addresses.len) return error.Invalid;
        for (addresses[0..count]) |prior| if (prior == bits) return error.Invalid;
        addresses[count] = bits; count += 1;
        const header: *c.VkBaseOutStructure = @ptrCast(@alignCast(address));
        if (header.sType == c.VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            if (format3 != null or bits % @alignOf(c.VkFormatProperties3) != 0) return error.Invalid;
            format3 = @ptrCast(@alignCast(address));
        }
        current = @ptrCast(header.pNext);
    }
    return format3;
}
/// [in] live borrowed physical, format, canonical output header and bounded chain.
/// [out] Actual32-bit flags and requested actual64-bit flags; unknown payloads and
/// headers preserved. Invalid chain or host failure preserves output. No retention,
/// allocations; process mutex serializes the actual transport transaction.
fn format_properties2(physical: c.VkPhysicalDevice, format: c.VkFormat, output: [*c]c.VkFormatProperties2) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (physical == null or output == null or @intFromPtr(output) % @alignOf(c.VkFormatProperties2) != 0 or output.*.sType != c.VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2) return;
    const extended = query2_chain(output.*.pNext) catch return;
    const record = object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) orelse return;
    const packet = requirements2_wire.format_properties2(record.id, format, extended != null) catch return;
    const reply = transact(packet.bytes[0..packet.used]) orelse return;
    const value = requirements2_wire.decode_format(reply, extended != null) catch { _ = failure(c.RingCorrupt); return; };
    output.*.formatProperties = .{ .linearTilingFeatures = value.core.linearTilingFeatures,
        .optimalTilingFeatures = value.core.optimalTilingFeatures, .bufferFeatures = value.core.bufferFeatures };
    if (extended) |node| { node.linearTilingFeatures = value.extended[0]; node.optimalTilingFeatures = value.extended[1]; node.bufferFeatures = value.extended[2]; }
}
/// [in] live physical, canonical core-only image request and output headers.
/// [out] Actual host image constraints or exact native error. Input extension
/// semantics are rejected until implemented; unknown output nodes stay untouched.
/// No retained pointers/heap; serialized transaction and commit-on-success output.
fn image_properties2(physical: c.VkPhysicalDevice, info: [*c]const c.VkPhysicalDeviceImageFormatInfo2, output: [*c]c.VkImageFormatProperties2) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (info == null or output == null or @intFromPtr(info) % @alignOf(c.VkPhysicalDeviceImageFormatInfo2) != 0 or @intFromPtr(output) % @alignOf(c.VkImageFormatProperties2) != 0 or info.*.sType != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 or output.*.sType != c.VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2) return c.VK_ERROR_INITIALIZATION_FAILED;
    _ = query2_chain(output.*.pNext) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    if (info.*.pNext != null) return c.VK_ERROR_FORMAT_NOT_SUPPORTED;
    var value: c.VkImageFormatProperties = undefined;
    const result = image_properties(physical, info.*.format, info.*.type, info.*.tiling, info.*.usage, info.*.flags, &value);
    if (result == c.VK_SUCCESS) output.*.imageFormatProperties = value;
    return result;
}
/// [in] live physical, count and nullable fill array; each filled header canonical.
/// [out] Actual host family count/values bounded64, preserves chain/header. Failure
/// preserves count and payload. No ownership transfer/heap; mutex serialized.
fn queue_properties2(physical: c.VkPhysicalDevice, count: [*c]u32, output: [*c]c.VkQueueFamilyProperties2) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (count == null or physical == null or object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) == null or lost != c.RingOk) return;
    if (output == null) { queue_properties(physical, count, null); return; }
    const capacity: u32 = @min(count.*, 64);
    for (output[0..capacity]) |item| { if (item.sType != c.VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2) return; _ = query2_chain(item.pNext) catch return; }
    var values: [64]c.VkQueueFamilyProperties = undefined;
    var copied = capacity;
    queue_properties(physical, &copied, &values);
    if (lost != c.RingOk or copied > capacity) return;
    for (0..copied) |index| output[index].queueFamilyProperties = values[index];
    count.* = copied;
}
/// [in] borrowed live physical and canonical memory output/chain. [out] Actual
/// projected host types/heaps, preserving headers and unknown extension payloads.
/// Failure preserves payload; no retained pointers/heap; serialized transport.
fn memory2(physical: c.VkPhysicalDevice, output: [*c]c.VkPhysicalDeviceMemoryProperties2) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (physical == null or object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) == null or lost != c.RingOk or output == null or @intFromPtr(output) % @alignOf(c.VkPhysicalDeviceMemoryProperties2) != 0 or output.*.sType != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2) return;
    _ = query2_chain(output.*.pNext) catch return;
    var value = std.mem.zeroes(c.VkPhysicalDeviceMemoryProperties);
    memory(physical, &value);
    if (lost != c.RingOk or value.memoryTypeCount == 0) return;
    output.*.memoryProperties = value;
}
/// [in] borrowed physical/core sparse request and count/nullable canonical array.
/// [out] Actual host sparse granularity/aspects and count; unknown nodes/header
/// preserved. Error preserves count/output, no retention/heap; mutex serialized.
fn sparse_properties2(physical: c.VkPhysicalDevice, info: [*c]const c.VkPhysicalDeviceSparseImageFormatInfo2, count: [*c]u32, output: [*c]c.VkSparseImageFormatProperties2) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (physical == null or object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) == null or lost != c.RingOk or count == null or info == null or @intFromPtr(info) % @alignOf(c.VkPhysicalDeviceSparseImageFormatInfo2) != 0 or info.*.sType != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2 or info.*.pNext != null) return;
    if (output == null) { sparse_properties(physical, info.*.format, info.*.type, info.*.samples, info.*.usage, info.*.tiling, count, null); return; }
    const capacity: u32 = @min(count.*, 64);
    for (output[0..capacity]) |item| { if (item.sType != c.VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2) return; _ = query2_chain(item.pNext) catch return; }
    var values: [64]c.VkSparseImageFormatProperties = undefined;
    var copied = capacity;
    sparse_properties(physical, info.*.format, info.*.type, info.*.samples, info.*.usage, info.*.tiling, &copied, &values);
    if (lost != c.RingOk or copied > capacity) return;
    for (0..copied) |index| output[index].properties = values[index];
    count.* = copied;
}
// physical_proc: vkGetPhysicalDeviceFormatProperties2[KHR] -> format_properties2
// vkGetPhysicalDeviceImageFormatProperties2[KHR] -> image_properties2
// vkGetPhysicalDeviceQueueFamilyProperties2[KHR] -> queue_properties2
// vkGetPhysicalDeviceMemoryProperties2[KHR] -> memory2
// vkGetPhysicalDeviceSparseImageFormatProperties2[KHR] -> sparse_properties2

// Requires root properties2 query_chain helper renamed query2_chain if retained;
// independent dedicated output walk here never mutates unknown payloads.
fn dedicated_output(next: ?*anyopaque) !?*c.VkMemoryDedicatedRequirements {
    var seen: [64]usize = undefined; var count: usize = 0; var current = next;
    var result: ?*c.VkMemoryDedicatedRequirements = null;
    while (current) |pointer| {
        const address = @intFromPtr(pointer);
        if (count == seen.len or address % @alignOf(c.VkBaseOutStructure) != 0) return error.Invalid;
        for (seen[0..count]) |prior| if (prior == address) return error.Invalid;
        seen[count] = address; count += 1;
        const header: *c.VkBaseOutStructure = @ptrCast(@alignCast(pointer));
        if (header.sType == c.VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            if (result != null or address % @alignOf(c.VkMemoryDedicatedRequirements) != 0) return error.Invalid;
            result = @ptrCast(@alignCast(pointer));
        }
        current = @ptrCast(header.pNext);
    }
    return result;
}
/// [in] canonical output and resolved parent/resource identities, borrowed.
/// [out] Actual host requirements and dedicated flags; preserve output on failure
/// and unknown/header fields always. Host/cache ownership retained until actual
/// acknowledgement; allocation-free and called under the recursive ICD mutex.
fn resource_requirements2(parent: *const c.venus_object_t, record: *c.venus_object_t, image: bool, output: [*c]c.VkMemoryRequirements2) void {
    if (output == null or @intFromPtr(output) % @alignOf(c.VkMemoryRequirements2) != 0 or output.*.sType != c.VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2) return;
    const dedicated = dedicated_output(output.*.pNext) catch return;
    const packet = requirements2_wire.memory_requirements2(parent.id, record.id, image, dedicated != null) catch return;
    const reply = transact(packet.bytes[0..packet.used]) orelse return;
    const value = requirements2_wire.decode_memory(reply, image, dedicated != null) catch { _ = failure(c.RingCorrupt); return; };
    const requirements = c.VkMemoryRequirements{ .size = value.requirements.size, .alignment = value.requirements.alignment, .memoryTypeBits = value.requirements.memoryTypeBits };
    resource_state(record).requirements = requirements;
    output.*.memoryRequirements = requirements;
    if (dedicated) |node| { node.prefersDedicatedAllocation = value.preferred; node.requiresDedicatedAllocation = value.required; }
}
/// [in] nonnull live same-device buffer and canonical query/output, no input chain.
/// [out] Exact host requirements; invalid/lost inputs preserve output. No heap or
/// retention; recursive process mutex protects metadata and reply scratch.
fn buffer_requirements2(device: c.VkDevice, info: [*c]const c.VkBufferMemoryRequirementsInfo2, output: [*c]c.VkMemoryRequirements2) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (device == null or info == null or @intFromPtr(info) % @alignOf(c.VkBufferMemoryRequirementsInfo2) != 0 or info.*.sType != c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 or info.*.pNext != null or info.*.buffer == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(@intFromPtr(info.*.buffer.?), c.VK_OBJECT_TYPE_BUFFER, parent.id) orelse return;
    resource_requirements2(parent, record, false, output);
}
/// [in] nonnull live same-device image and canonical query/output; planar input
/// nodes unsupported. [out] Actual requirements; invalid/lost preserves output.
/// No heap/retention; process mutex protects host operation and cached values.
fn image_requirements2(device: c.VkDevice, info: [*c]const c.VkImageMemoryRequirementsInfo2, output: [*c]c.VkMemoryRequirements2) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (device == null or info == null or @intFromPtr(info) % @alignOf(c.VkImageMemoryRequirementsInfo2) != 0 or info.*.sType != c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 or info.*.pNext != null or info.*.image == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(@intFromPtr(info.*.image.?), c.VK_OBJECT_TYPE_IMAGE, parent.id) orelse return;
    resource_requirements2(parent, record, true, output);
}
/// [in] borrowed canonical buffer-create request; native callbacks not involved.
/// [out] Actual host requirements obtained by owned temporary create/query/destroy.
/// No fabricated sizes or flags. Failure preserves output; uncertain host teardown
/// remains in the existing registry until session retirement. Mutex serialized.
fn device_buffer_requirements(device: c.VkDevice, info: [*c]const c.VkDeviceBufferMemoryRequirements, output: [*c]c.VkMemoryRequirements2) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (info == null or info.*.sType != c.VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS or info.*.pNext != null or info.*.pCreateInfo == null) return;
    var buffer: c.VkBuffer = null;
    if (create_buffer(device, info.*.pCreateInfo, null, &buffer) != c.VK_SUCCESS) return;
    defer destroy_buffer(device, buffer, null);
    var query_info = c.VkBufferMemoryRequirementsInfo2{.sType=c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,.buffer=buffer};
    buffer_requirements2(device, &query_info, output);
}
/// [in] borrowed canonical single-plane image-create request. [out] Actual host
/// requirements through temporary image create/query/acknowledged destroy; error
/// preserves output. No synthesized requirements; session owns uncertain resources.
fn device_image_requirements(device: c.VkDevice, info: [*c]const c.VkDeviceImageMemoryRequirements, output: [*c]c.VkMemoryRequirements2) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (info == null or info.*.sType != c.VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS or info.*.pNext != null or info.*.pCreateInfo == null or info.*.planeAspect != 0) return;
    var image: c.VkImage = null;
    if (create_image(device, info.*.pCreateInfo, null, &image) != c.VK_SUCCESS) return;
    defer destroy_image(device, image, null);
    var query_info = c.VkImageMemoryRequirementsInfo2{.sType=c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=image};
    image_requirements2(device, &query_info, output);
}
/// [in] same-device canonical unbound buffer/memory pairs, count<=64, array nullable
/// only at zero. [out] SUCCESS or exact first native failure. Prior successful binds
/// remain published on subsequent failure, as permitted by Vulkan. No heap, pointers
/// retained only as existing object IDs; process mutex serializes binding metadata.
fn bind_buffer_memory2(device: c.VkDevice, count: u32, infos: [*c]const c.VkBindBufferMemoryInfo) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (device == null or count > 64 or (count != 0 and infos == null)) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (count != 0) { for (infos[0..count]) |info| if (info.sType != c.VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO or info.pNext != null) return c.VK_ERROR_INITIALIZATION_FAILED; }
    if (count != 0) { for (infos[0..count]) |info| { const result = bind_buffer_memory(device, info.buffer, info.memory, info.memoryOffset); if (result != c.VK_SUCCESS) return result; } }
    return c.VK_SUCCESS;
}
/// [in] canonical same-device single-plane image binds, nullable at zero, count<=64.
/// [out] SUCCESS or exact first native failure; prior acknowledged binds retained.
/// No allocation/native pointer retention, process mutex serializes ownership.
fn bind_image_memory2(device: c.VkDevice, count: u32, infos: [*c]const c.VkBindImageMemoryInfo) callconv(.C) c_int {
    lock_icd(); defer unlock_icd();
    if (device == null or count > 64 or (count != 0 and infos == null)) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (count != 0) { for (infos[0..count]) |info| if (info.sType != c.VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO or info.pNext != null) return c.VK_ERROR_INITIALIZATION_FAILED; }
    if (count != 0) { for (infos[0..count]) |info| { const result = bind_image_memory(device, info.image, info.memory, info.memoryOffset); if (result != c.VK_SUCCESS) return result; } }
    return c.VK_SUCCESS;
}
// Device names core/KHR: vkGetBufferMemoryRequirements2, vkGetImageMemoryRequirements2,
// vkBindBufferMemory2, vkBindImageMemory2, vkGetDeviceBufferMemoryRequirements,
// vkGetDeviceImageMemoryRequirements.

/// [in] same-device image and borrowed accessible single-aspect subresource.
/// [out] Actual host byte layout, preserved on invalid/lost/malformed response.
/// No heap/retention; recursive process mutex serializes actual query scratch.
fn image_subresource_layout(device: c.VkDevice, image: c.VkImage, info: [*c]const c.VkImageSubresource, output: [*c]c.VkSubresourceLayout) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (device == null or image == null or info == null or output == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(@intFromPtr(image.?), c.VK_OBJECT_TYPE_IMAGE, parent.id) orelse return;
    const state = resource_state(record);
    if (info.*.mipLevel >= state.image_levels or info.*.arrayLayer >= state.image_layers or info.*.aspectMask & ~image_aspects(state.image_format) != 0) return;
    const packet = extra_wire.subresource_layout(parent.id, record.id, @ptrCast(info)) catch return;
    const reply = transact(packet.bytes[0..packet.used]) orelse return;
    const value = extra_wire.decode_subresource(reply) catch { _ = failure(c.RingCorrupt); return; };
    output.* = .{ .offset = value.offset, .size = value.size, .rowPitch = value.rowPitch, .arrayPitch = value.arrayPitch, .depthPitch = value.depthPitch };
}
/// [in] canonical maintenance5 core-only subresource and output chains. [out]
/// Actual host core layout with original headers retained. Unsupported extension
/// payload remains untouched; malformed input preserves output. No retention/heap.
fn image_subresource_layout2(device: c.VkDevice, image: c.VkImage, info: [*c]const c.VkImageSubresource2KHR, output: [*c]c.VkSubresourceLayout2KHR) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (info == null or output == null or info.*.sType != c.VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_KHR or output.*.sType != c.VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_KHR or info.*.pNext != null) return;
    _ = query2_chain(output.*.pNext) catch return;
    var value = std.mem.zeroes(c.VkSubresourceLayout);
    image_subresource_layout(device, image, &info.*.imageSubresource, &value);
    if (lost == c.RingOk and value.size != 0) output.*.subresourceLayout = value;
}
// dispatch vkGetImageSubresourceLayout -> image_subresource_layout
// vkGetImageSubresourceLayout2KHR/EXT -> image_subresource_layout2
/// [in] borrowed maintenance5 image-create/subresource request and canonical
/// output. [out] Actual host layout obtained through owned temporary image;
/// original output headers retained. Failure preserves output; uncertain host
/// destruction remains registered until confirmed session retirement. No fake
/// pitch/layout calculation; recursive process mutex serializes lifecycle.
fn device_image_subresource_layout(device: c.VkDevice, info: [*c]const c.VkDeviceImageSubresourceInfoKHR, output: [*c]c.VkSubresourceLayout2KHR) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (info == null or output == null or @intFromPtr(info) % @alignOf(c.VkDeviceImageSubresourceInfoKHR) != 0 or info.*.sType != c.VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO_KHR or info.*.pNext != null or info.*.pCreateInfo == null or info.*.pSubresource == null) return;
    var image: c.VkImage = null;
    if (create_image(device, info.*.pCreateInfo, null, &image) != c.VK_SUCCESS) return;
    defer destroy_image(device, image, null);
    image_subresource_layout2(device, image, info.*.pSubresource, output);
}
// vkGetDeviceImageSubresourceLayoutKHR -> device_image_subresource_layout

/// [in] same-device live render pass, borrowed; [out] actual host granularity.
/// Failure preserves output. No heap/retention; recursive mutex serializes reply.
fn render_area_granularity(device: c.VkDevice, pass: c.VkRenderPass, output: [*c]c.VkExtent2D) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (device == null or pass == null or output == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(@intFromPtr(pass.?), c.VK_OBJECT_TYPE_RENDER_PASS, parent.id) orelse return;
    const packet = extra_wire.render_granularity(parent.id, record.id) catch return;
    const reply = transact(packet.bytes[0..packet.used]) orelse return;
    const extent = extra_wire.decode_granularity(reply, 84) catch { _ = failure(c.RingCorrupt); return; };
    output.* = .{ .width = extent.width, .height = extent.height };
}
/// [in] maintenance5 area request and live device; [out] actual host extent.
/// Invalid/lost response preserves output. No retained pointers/heap, mutex
/// serialized; multiview unsupported, accepts zero viewMask and <=8 formats.
fn rendering_area_granularity(device: c.VkDevice, info: [*c]const c.VkRenderingAreaInfoKHR, output: [*c]c.VkExtent2D) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (device == null or info == null or output == null or info.*.viewMask != 0) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const packet = extra_wire.rendering_granularity(parent.id, @ptrCast(info)) catch return;
    const reply = transact(packet.bytes[0..packet.used]) orelse return;
    const extent = extra_wire.decode_granularity(reply, 280) catch { _ = failure(c.RingCorrupt); return; };
    output.* = .{ .width = extent.width, .height = extent.height };
}
// vkGetRenderAreaGranularity -> render_area_granularity
// vkGetRenderingAreaGranularityKHR -> rendering_area_granularity

/// Submit sync2 translated work. [in] queue live borrowed, submits0..16 borrowed;
/// nullable fence retained after native acceptance. [out] no caller storage.
/// Returns host/local invalid/OOM/loss; mutex serialized. Validated host ACK
/// transfers command/resource references into existing GPU completion tickets.
fn queue_submit2(
    queue: c.VkQueue,
    count: u32,
    submits: [*c]const c.VkSubmitInfo2,
    fence: c.VkFence,
) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (queue == null or (count != 0 and submits == null)) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (count > 16) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    const record = object(@intFromPtr(queue.?), c.VK_OBJECT_TYPE_QUEUE) orelse
        return c.VK_ERROR_INITIALIZATION_FAILED;
    if (resource_state(record).id != record.id) return c.VK_ERROR_INITIALIZATION_FAILED;
    var available: ?*submission_ticket_t = null;
    for (&submission_tickets) |*ticket| if (ticket.queue == 0) {
        available = ticket;
        break;
    };
    const target = available orelse return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    if (submission_sequence == std.math.maxInt(u64)) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    const parent = object(device_cache_for_queue(record).handle, c.VK_OBJECT_TYPE_DEVICE).?;
    if (resource_state(record).idle_refs != 0 or resource_state(parent).idle_refs != 0)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    var staged = submission_ticket_t{ .queue = record.handle };
    var fence_record: ?*c.venus_object_t = null;
    if (fence != null) {
        fence_record = child_object(@intFromPtr(fence.?), c.VK_OBJECT_TYPE_FENCE, record.parent_id) orelse
            return c.VK_ERROR_INITIALIZATION_FAILED;
        if (resource_state(fence_record.?).inflight_count != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
        staged.fence = fence_record.?.handle;
        _ = include_reference(&staged, fence_record.?);
    }
    var wait_count: u32 = 0;
    var signal_count: u32 = 0;
    var command_count: u32 = 0;
    var normalized: [16]c.VkSubmitInfo2 = undefined;
    var wait_storage: [64]c.VkSemaphoreSubmitInfo = undefined;
    var signal_storage: [64]c.VkSemaphoreSubmitInfo = undefined;
    var command_storage: [64]c.VkCommandBufferSubmitInfo = undefined;
    if (count != 0) for (submits[0..count], 0..) |info, submit_index| {
        if (info.sType != c.VK_STRUCTURE_TYPE_SUBMIT_INFO_2 or info.pNext != null or info.flags != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
        if (info.waitSemaphoreInfoCount > 64 - wait_count or info.signalSemaphoreInfoCount > 64 - signal_count or info.commandBufferInfoCount > 64 - command_count) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
        if ((info.waitSemaphoreInfoCount != 0 and info.pWaitSemaphoreInfos == null) or
            (info.signalSemaphoreInfoCount != 0 and info.pSignalSemaphoreInfos == null) or
            (info.commandBufferInfoCount != 0 and info.pCommandBufferInfos == null)) return c.VK_ERROR_INITIALIZATION_FAILED;
        normalized[submit_index] = info;
        const first_wait = wait_count;
        const first_signal = signal_count;
        const first_command = command_count;
        if (info.waitSemaphoreInfoCount != 0) for (info.pWaitSemaphoreInfos[0..info.waitSemaphoreInfoCount]) |entry| {
            if (entry.sType != c.VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO or entry.pNext != null or entry.semaphore == null or entry.deviceIndex != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
            const child = child_object(@intFromPtr(entry.semaphore.?), c.VK_OBJECT_TYPE_SEMAPHORE, record.parent_id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
            if (resource_state(child).buffer_usage == 0 and entry.value != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
            _ = include_reference(&staged, child);
            wait_storage[wait_count] = entry;
            wait_storage[wait_count].semaphore = @ptrFromInt(child.id);
            wait_count += 1;
        };
        if (info.signalSemaphoreInfoCount != 0) for (info.pSignalSemaphoreInfos[0..info.signalSemaphoreInfoCount]) |entry| {
            if (entry.sType != c.VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO or entry.pNext != null or entry.semaphore == null or entry.deviceIndex != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
            const child = child_object(@intFromPtr(entry.semaphore.?), c.VK_OBJECT_TYPE_SEMAPHORE, record.parent_id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
            if (resource_state(child).buffer_usage == 0 and entry.value != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
            _ = include_reference(&staged, child);
            signal_storage[signal_count] = entry;
            signal_storage[signal_count].semaphore = @ptrFromInt(child.id);
            signal_count += 1;
        };
        if (info.commandBufferInfoCount != 0) for (info.pCommandBufferInfos[0..info.commandBufferInfoCount]) |entry| {
            if (entry.sType != c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO or entry.pNext != null or entry.commandBuffer == null or entry.deviceMask > 1) return c.VK_ERROR_INITIALIZATION_FAILED;
            const child = object(@intFromPtr(entry.commandBuffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
            const pool = command_pool_for(child) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
            const state = resource_state(child);
            if (pool.parent_id != record.parent_id or resource_state(pool).pool_family != resource_state(record).queue_family or
                state.command_level != 0 or (state.command_state != .Executable and !(state.command_state == .Pending and state.command_flags & 4 != 0))) return c.VK_ERROR_INITIALIZATION_FAILED;
            if (include_reference(&staged, child) and state.command_flags & 4 == 0) return c.VK_ERROR_INITIALIZATION_FAILED;
            command_storage[command_count] = entry;
            command_storage[command_count].commandBuffer = @ptrFromInt(child.id);
            command_count += 1;
        };
        normalized[submit_index].pWaitSemaphoreInfos = if (info.waitSemaphoreInfoCount == 0) null else @ptrCast(&wait_storage[first_wait]);
        normalized[submit_index].pSignalSemaphoreInfos = if (info.signalSemaphoreInfoCount == 0) null else @ptrCast(&signal_storage[first_signal]);
        normalized[submit_index].pCommandBufferInfos = if (info.commandBufferInfoCount == 0) null else @ptrCast(&command_storage[first_command]);
    };
    // Retain every recorded identity, rather than only the submitted command buffer.
    // Successful descriptor updates invalidate recordings; dispatch records exact consumed buffers.
    for (slots, 0..) |child, index| {
        const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
        if (staged.references[index / 64] & bit == 0 or child.kind != c.VK_OBJECT_TYPE_COMMAND_BUFFER) continue;
        if (!refresh_command_descriptors(@constCast(&slots[index]))) return c.VK_ERROR_INITIALIZATION_FAILED;
        for (&staged.references, resource_states[index].buffer_references) |*word, references| word.* |= references;
    }
    for (slots, 0..) |child, index| {
        const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
        if (staged.references[index / 64] & bit == 0) continue;
        if (child.id == 0) return c.VK_ERROR_INITIALIZATION_FAILED;
        if (child.kind == c.VK_OBJECT_TYPE_COMMAND_BUFFER) continue;
        if (child.kind == c.VK_OBJECT_TYPE_DESCRIPTOR_SET) {
            const pool = descriptor_pool_for(&child) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
            if (pool.parent_id != parent.id) return c.VK_ERROR_INITIALIZATION_FAILED;
        } else if (child.parent_id != parent.id) return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    if (fence_record) |selected| {
        const status = fence_status_locked(parent, selected);
        if (status == c.VK_SUCCESS) return c.VK_ERROR_INITIALIZATION_FAILED;
        if (status != c.VK_NOT_READY) return status;
    }
    const writer = modern_sync.queue_submit2(record.id, @ptrCast(normalized[0..count]), if (fence_record) |selected| selected.id else 0) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    const mapping_result = synchronize_device_mappings(parent.id, true);
    if (mapping_result != c.VK_SUCCESS) return mapping_result;
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 206, 0);
    if (result == c.VK_SUCCESS) {
        submission_sequence += 1;
        staged.sequence = submission_sequence;
        target.* = staged;
        retain_submission_mapping_spans(parent.id, staged.references);
        for (&resource_states, 0..) |*state, index| {
            const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
            if (staged.references[index / 64] & bit == 0) continue;
            std.debug.assert(state.inflight_count < submission_tickets.len);
            state.inflight_count += 1;
            if (slots[index].kind == c.VK_OBJECT_TYPE_COMMAND_BUFFER) state.command_state = .Pending;
        }
    }
    return result;
}

/// Record synchronization2 dependency with translated host resources.
/// [in] command_buffer/info borrowed immutable during call; native arrays at most64 each.
/// Invalid Recording inputs invalidate command; no heap/pointers retained. After ACK,
/// bound image/buffer and allocation owners remain retained through command retirement.
/// Caller contract requires enabled synchronization2 and valid per-queue stage/access scopes.

/// [in] live device and borrowed canonical info; optional single semaphore-type
/// node. [out] owned semaphore or null. Serialized; host errors retain no output.

/// [in] live device/timeline token; [out] borrowed counter, zero on failure.
/// Mutex serialized; native counter visibility does not retire GPU owners.
fn get_semaphore_counter_value(device: c.VkDevice, semaphore: c.VkSemaphore, output: [*c]u64) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = 0;
    if (device == null or semaphore == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = child_object(@intFromPtr(semaphore.?), c.VK_OBJECT_TYPE_SEMAPHORE, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (resource_state(record).buffer_usage != 1) return c.VK_ERROR_INITIALIZATION_FAILED;
    const writer = modern_sync.semaphore_counter(parent.id, record.id) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const decoded = modern_sync.decode_counter(reply) catch return failure(c.RingCorrupt);
    if (decoded.result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (decoded.result != c.VK_SUCCESS) return decoded.result;
    const mapping_result = synchronize_device_mappings(parent.id, false);
    if (mapping_result != c.VK_SUCCESS) return mapping_result;
    output.* = decoded.value;
    return c.VK_SUCCESS;
}
/// [in] live device and borrowed single timeline signal. Mutex serialized;
/// host signal ACK does not prove GPU completion and retains submission owners.
fn signal_semaphore(device: c.VkDevice, info: [*c]const c.VkSemaphoreSignalInfo) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    if (device == null or info == null or info.*.sType != c.VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO or info.*.pNext != null or info.*.semaphore == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = child_object(@intFromPtr(info.*.semaphore.?), c.VK_OBJECT_TYPE_SEMAPHORE, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (resource_state(record).buffer_usage != 1) return c.VK_ERROR_INITIALIZATION_FAILED;
    const writer = modern_sync.signal_semaphore(parent.id, record.id, info.*.value) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    const mapping_result = synchronize_device_mappings(parent.id, true);
    if (mapping_result != c.VK_SUCCESS) return mapping_result;
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 174, 0);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    return result;
}
/// [in] live device and borrowed wait arrays1..64; timeout in nanoseconds.
/// Retains semaphore owners during unlocked bounded polls; concurrent submission
/// and host signal can progress. Success does not retire GPU references.
fn wait_semaphores(device: c.VkDevice, info: [*c]const c.VkSemaphoreWaitInfo, timeout: u64) callconv(.C) c_int {
    var timer = std.time.Timer.start() catch return c.VK_ERROR_INITIALIZATION_FAILED;
    lock_icd();
    if (device == null or info == null or info.*.sType != c.VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO or info.*.pNext != null or info.*.flags > 1 or info.*.semaphoreCount == 0 or info.*.semaphoreCount > 64 or info.*.pSemaphores == null or info.*.pValues == null) {
        unlock_icd();
        return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse {
        unlock_icd();
        return c.VK_ERROR_INITIALIZATION_FAILED;
    };
    const count = info.*.semaphoreCount;
    const flags = info.*.flags;
    const saved_namespace = objects.namespace_id;
    const saved_parent_id = parent.id;
    var ids: [64]u64 = undefined;
    var handles: [64]u64 = undefined;
    var values: [64]u64 = undefined;
    for (0..count) |index| {
        const native = info.*.pSemaphores[index];
        if (native == null) {
            unlock_icd();
            return c.VK_ERROR_INITIALIZATION_FAILED;
        }
        const record = child_object(@intFromPtr(native.?), c.VK_OBJECT_TYPE_SEMAPHORE, saved_parent_id) orelse {
            unlock_icd();
            return c.VK_ERROR_INITIALIZATION_FAILED;
        };
        if (resource_state(record).buffer_usage != 1 or resource_state(record).idle_refs > std.math.maxInt(u32) - 64) {
            unlock_icd();
            return c.VK_ERROR_INITIALIZATION_FAILED;
        }
        ids[index] = record.id;
        handles[index] = record.handle;
        values[index] = info.*.pValues[index];
    }
    for (handles[0..count]) |handle| resource_state(child_object(handle, c.VK_OBJECT_TYPE_SEMAPHORE, saved_parent_id).?).idle_refs += 1;
    unlock_icd();
    defer {
        lock_icd();
        if (objects.namespace_id == saved_namespace) for (handles[0..count], ids[0..count]) |handle, id| {
            if (child_object(handle, c.VK_OBJECT_TYPE_SEMAPHORE, saved_parent_id)) |record| {
                if (record.id == id) {
                    std.debug.assert(resource_state(record).idle_refs != 0);
                    resource_state(record).idle_refs -= 1;
                }
            }
        };
        unlock_icd();
    }
    while (true) {
        lock_icd();
        if (objects.namespace_id != saved_namespace or lost != c.RingOk) {
            unlock_icd();
            return c.VK_ERROR_DEVICE_LOST;
        }
        for (handles[0..count], ids[0..count]) |handle, id| {
            const current = child_object(handle, c.VK_OBJECT_TYPE_SEMAPHORE, saved_parent_id) orelse {
                unlock_icd(); return c.VK_ERROR_DEVICE_LOST;
            };
            if (current.id != id) { unlock_icd(); return c.VK_ERROR_DEVICE_LOST; }
        }
        const writer = modern_sync.wait_semaphores(saved_parent_id, ids[0..count], values[0..count], flags, 0) catch {
            unlock_icd();
            return c.VK_ERROR_INITIALIZATION_FAILED;
        };
        const reply = transact(writer.bytes[0..writer.used]) orelse {
            unlock_icd();
            return c.VK_ERROR_DEVICE_LOST;
        };
        const result = result_reply(reply, 173, c.VK_TIMEOUT);
        if (result == c.VK_ERROR_DEVICE_LOST) {
            const status = failure(c.RingClosed);
            unlock_icd();
            return status;
        }
        const visible_result = if (result == c.VK_SUCCESS) synchronize_device_mappings(saved_parent_id, false) else result;
        unlock_icd();
        if (result != c.VK_TIMEOUT) return visible_result;
        if (timeout != std.math.maxInt(u64) and timer.read() >= timeout) return c.VK_TIMEOUT;
        std.time.sleep(if (timeout == std.math.maxInt(u64)) 1_000_000 else @min(1_000_000, timeout -| timer.read()));
    }
}
/// [in] live device and borrowed buffer-address info. Returns0 for malformed,
/// foreign, unbound or transport failure; caller enables the address feature.
/// Bound allocation and buffer remain caller owned through GPU address use.
fn get_buffer_device_address(device: c.VkDevice, info: [*c]const c.VkBufferDeviceAddressInfo) callconv(.C) u64 {
    lock_icd();
    defer unlock_icd();
    if (device == null or info == null or info.*.sType != c.VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO or info.*.pNext != null or info.*.buffer == null) return 0;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return 0;
    if (!device_address_enabled(parent)) return 0;
    const record = child_object(@intFromPtr(info.*.buffer.?), c.VK_OBJECT_TYPE_BUFFER, parent.id) orelse return 0;
    const state = resource_state(record);
    if (state.bound_memory == 0 or state.buffer_usage & c.VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT == 0) return 0;
    const allocation = child_object(state.bound_memory, c.VK_OBJECT_TYPE_DEVICE_MEMORY, parent.id) orelse return 0;
    if (resource_state(allocation).allocation_flags & c.VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT == 0) return 0;
    const writer = modern_sync.buffer_device_address(parent.id, record.id) catch return 0;
    const reply = transact(writer.bytes[0..writer.used]) orelse return 0;
    const value = modern_sync.decode_value(reply, 175) catch {
        _ = failure(c.RingCorrupt);
        return 0;
    };
    if (value != 0) state.address_exposed = true;
    return value;
}

/// Read one immutable requested feature word under the ICD mutex; absent means disabled.
fn device_feature(parent: *const c.venus_object_t, tag: u32, index: usize) bool {
    const entry = device_cache(parent.handle) orelse return false;
    for (entry.enabled_state.features.nodes[0..entry.enabled_state.features.count]) |node| {
        if (node.type_tag == tag and index < node.flag_count) return node.flags[index] != 0;
    }
    return false;
}
fn device_address_enabled(parent: *const c.venus_object_t) bool {
    const index = (@offsetOf(c.VkPhysicalDeviceVulkan12Features, "bufferDeviceAddress") - @offsetOf(c.VkPhysicalDeviceVulkan12Features, "samplerMirrorClampToEdge")) / 4;
    return device_feature(parent, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, index);
}
fn timeline_enabled(parent: *const c.venus_object_t) bool {
    const index = (@offsetOf(c.VkPhysicalDeviceVulkan12Features, "timelineSemaphore") - @offsetOf(c.VkPhysicalDeviceVulkan12Features, "samplerMirrorClampToEdge")) / 4;
    return device_feature(parent, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, index);
}

// Owner imports const general_graphics = @import("venus_graphics_general_wire.zig");
// pipeline_stage_inputs_t also used by compute pipeline wrapper for mandatory maintenance5
// inline VkShaderModuleCreateInfo inputs. Temporary real modules release after pipeline ACK.
const pipeline_stage_inputs_t = struct {
    stages: [5]c.VkPipelineShaderStageCreateInfo = undefined,
    ids: [5]u64 = undefined,
    temporary: [5]c.VkShaderModule = [_]c.VkShaderModule{null} ** 5,
    count: u32 = 0,
};
fn release_pipeline_stages(device: c.VkDevice, inputs: *pipeline_stage_inputs_t) void {
    for (inputs.temporary[0..inputs.count]) |module| if (module != null) destroy_shader_module(device, module, null);
    inputs.temporary = [_]c.VkShaderModule{null} ** 5;
}
fn resolve_pipeline_stages(device: c.VkDevice, parent_id: u64, stages: [*c]const c.VkPipelineShaderStageCreateInfo, count: u32, output: *pipeline_stage_inputs_t) c_int {
    if (stages == null or count == 0 or count > 5) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.count = count;
    for (stages[0..count], 0..) |stage, index| {
        if (stage.sType != c.VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO) return c.VK_ERROR_INITIALIZATION_FAILED;
        output.stages[index] = stage;
        var module = stage.module;
        if (stage.pNext) |pointer| {
            if (module != null or @intFromPtr(pointer) % @alignOf(c.VkShaderModuleCreateInfo) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
            const inline_info: *const c.VkShaderModuleCreateInfo = @ptrCast(@alignCast(pointer));
            if (inline_info.sType != c.VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO or inline_info.pNext != null) return c.VK_ERROR_INITIALIZATION_FAILED;
            const result = create_shader_module(device, inline_info, null, &module);
            if (result != c.VK_SUCCESS) return result;
            output.temporary[index] = module;
            output.stages[index].pNext = null;
            output.stages[index].module = module;
        }
        if (module == null) return c.VK_ERROR_INITIALIZATION_FAILED;
        const shader = child_object(@intFromPtr(module.?), c.VK_OBJECT_TYPE_SHADER_MODULE, parent_id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
        output.ids[index] = shader.id;
    }
    return c.VK_SUCCESS;
}
fn general_pipeline_first_format(info: *const c.VkGraphicsPipelineCreateInfo, pass: ?*c.venus_object_t) ?u32 {
    if (pass) |record| return resource_state(record).render_format;
    var next = info.pNext;
    var count: usize = 0;
    while (next) |pointer| {
        if (count == 2 or @intFromPtr(pointer) % @alignOf(c.VkBaseInStructure) != 0) return null;
        count += 1;
        const header: *const c.VkBaseInStructure = @ptrCast(@alignCast(pointer));
        if (header.sType == c.VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            const rendering: *const c.VkPipelineRenderingCreateInfo = @ptrCast(header);
            if (rendering.colorAttachmentCount > 8 or (rendering.colorAttachmentCount != 0 and rendering.pColorAttachmentFormats == null)) return null;
            if (rendering.colorAttachmentCount != 0) for (rendering.pColorAttachmentFormats[0..rendering.colorAttachmentCount]) |format| if (format != c.VK_FORMAT_UNDEFINED) return format;
            return graphics_state.NoColor;
        }
        next = if (header.pNext) |value| @ptrCast(value) else null;
    }
    return null;
}
/// Create general graphics pipelines, preserving copied descriptor/push layout profiles.
/// Native arrays borrowed synchronously; cache/layout/shaders resolve to same-device owners.
/// Inline maintenance5 shaders become real temporary modules; every success/failure releases
/// them after pipeline completion, except transport loss retains host owners for abandon.
/// Output handles initialized NULL and successful earlier outputs survive later failure.
/// Caller additionally validates enabled graphics features and full attachment signature.
fn create_graphics_pipelines(
    device: c.VkDevice,
    pipeline_cache: c.VkPipelineCache,
    count: u32,
    infos: [*c]const c.VkGraphicsPipelineCreateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkPipeline,
) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null or count == 0 or count > 16) return c.VK_ERROR_INITIALIZATION_FAILED;
    @memset(output[0..count], null);
    if (device == null or infos == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const cache_id = if (pipeline_cache) |handle| (child_object(@intFromPtr(handle), c.VK_OBJECT_TYPE_PIPELINE_CACHE, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED).id else 0;
    var first_error: c_int = c.VK_SUCCESS;
    for (infos[0..count], 0..) |*info, index| {
        const result = create_general_graphics_pipeline(device, parent, cache_id, @ptrCast(info), &output[index]);
        if (result != c.VK_SUCCESS and first_error == c.VK_SUCCESS) first_error = result;
        if (lost != c.RingOk) break;
    }
    return first_error;
}
fn create_general_graphics_pipeline(device: c.VkDevice, parent: *c.venus_object_t, cache_id: u64, info: *const c.VkGraphicsPipelineCreateInfo, output: [*c]c.VkPipeline) c_int {
    if (info.sType != c.VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO or info.stageCount > 5 or (info.stageCount != 0 and info.pStages == null) or info.layout == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    inline for (.{ "pVertexInputState", "pInputAssemblyState", "pTessellationState", "pViewportState", "pRasterizationState", "pMultisampleState", "pDepthStencilState", "pColorBlendState", "pDynamicState" }) |field| {
        const pointer = @field(info.*, field);
        if (pointer != null and @intFromPtr(pointer) % @alignOf(@TypeOf(pointer.*)) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    if (info.pInputAssemblyState != null and (info.pInputAssemblyState.*.topology > c.VK_PRIMITIVE_TOPOLOGY_PATCH_LIST or info.pInputAssemblyState.*.primitiveRestartEnable > 1)) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (info.pRasterizationState != null and (info.pRasterizationState.*.rasterizerDiscardEnable > 1 or info.pRasterizationState.*.depthClampEnable > 1 or info.pRasterizationState.*.depthBiasEnable > 1)) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (info.pColorBlendState != null) {
        const blend = info.pColorBlendState.*;
        if (blend.attachmentCount > 8 or (blend.attachmentCount != 0 and blend.pAttachments == null)) return c.VK_ERROR_INITIALIZATION_FAILED;
        if (blend.attachmentCount != 0) for (blend.pAttachments[0..blend.attachmentCount]) |attachment| if (attachment.blendEnable > 1) return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    const layout = child_object(@intFromPtr(info.layout.?), c.VK_OBJECT_TYPE_PIPELINE_LAYOUT, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const pass = if (info.renderPass) |handle| child_object(@intFromPtr(handle), c.VK_OBJECT_TYPE_RENDER_PASS, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED else null;
    const format = general_pipeline_first_format(info, pass) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    var chain: pipeline_chain_inputs_t = .{};
    const library_count = resolve_pipeline_chain(parent.id,info.pNext,&chain) catch return c.VK_ERROR_INITIALIZATION_FAILED;
    if(info.stageCount==0 and library_count==0) return c.VK_ERROR_INITIALIZATION_FAILED;
    var stages: pipeline_stage_inputs_t = .{};
    defer release_pipeline_stages(device, &stages);
    const stage_result = if(info.stageCount!=0) resolve_pipeline_stages(device, parent.id, info.pStages, info.stageCount, &stages) else c.VK_SUCCESS;
    if (stage_result != c.VK_SUCCESS) return stage_result;
    var normalized = info.*;
    normalized.pStages = if(info.stageCount==0) null else &stages.stages;
    normalized.pNext = chain.first;
    var writer = general_graphics.create_graphics_pipeline_cached(parent.id, cache_id, @ptrCast(&normalized), stages.ids[0..stages.count], layout.id, if (pass) |record| record.id else 0, 1) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    const profile = profiles.get_profile(&profile_registry.pipeline_layouts, resource_state(layout).profile_index).?.*;
    const profile_index = profiles.reserve_slot(&profile_registry.pipelines, profile) catch return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_PIPELINE, &writer, &handle);
    if (result != c.VK_SUCCESS) {
        if (lost == c.RingOk) std.debug.assert(profiles.release_slot(&profile_registry.pipelines, profile_index));
        return result;
    }
    const state = resource_state(child_object(handle, c.VK_OBJECT_TYPE_PIPELINE, parent.id).?);
    state.profile_index = profile_index;
    state.pipeline_bind_point = c.VK_PIPELINE_BIND_POINT_GRAPHICS;
    state.render_format = format;
    output.* = @ptrFromInt(handle);
    return c.VK_SUCCESS;
}

// Main ICD owner imports const dynamic_rendering = @import("venus_dynamic_rendering_wire.zig");
// graphics_state.NoColor is a copied compatibility key, never a transmitted VkFormat.
fn resolve_rendering_attachment(
    parent_id: u64,
    info: *const c.VkRenderingAttachmentInfo,
    area: c.VkRect2D,
    layer_count: u32,
    view_mask: u32,
    aspect: u32,
    references: *[60]*c.venus_object_t,
    reference_count: *usize,
    first_format: *u32,
) ?dynamic_rendering.attachment_ids_t {
    var ids = dynamic_rendering.attachment_ids_t{};
    if (info.imageView == null) {
        if (info.resolveImageView != null or info.resolveMode != 0) return null;
        return ids;
    }
    const view = child_object(@intFromPtr(info.imageView.?), c.VK_OBJECT_TYPE_IMAGE_VIEW, parent_id) orelse return null;
    const view_state = resource_state(view);
    const image = child_object(view_state.view_image, c.VK_OBJECT_TYPE_IMAGE, parent_id) orelse return null;
    const image_state = resource_state(image);
    const memory_record = child_object(image_state.bound_memory, c.VK_OBJECT_TYPE_DEVICE_MEMORY, parent_id) orelse return null;
    if ((view_state.view_type != c.VK_IMAGE_VIEW_TYPE_2D and view_state.view_type != c.VK_IMAGE_VIEW_TYPE_2D_ARRAY) or
        !image_range_valid(image_state, view_state.view_range) or view_state.view_range.aspectMask & aspect == 0 or
        (view_state.view_range.levelCount != 1 and !(view_state.view_range.levelCount == std.math.maxInt(u32) and view_state.view_range.baseMipLevel + 1 == image_state.image_levels)) or
        view_state.image_usage & (if (aspect == c.VK_IMAGE_ASPECT_COLOR_BIT) @as(u32, c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) else c.VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) return null;
    const shift: u5 = @intCast(@min(view_state.view_range.baseMipLevel, 31));
    const width = @max(@as(u32, 1), image_state.image_extent[0] >> shift);
    const height = @max(@as(u32, 1), image_state.image_extent[1] >> shift);
    const layers = if (view_state.view_range.layerCount == std.math.maxInt(u32)) image_state.image_layers - view_state.view_range.baseArrayLayer else view_state.view_range.layerCount;
    const required_layers = if (view_mask == 0) layer_count else 32 - @clz(view_mask);
    if (area.offset.x < 0 or area.offset.y < 0 or @as(u64, @intCast(area.offset.x)) + area.extent.width > width or
        @as(u64, @intCast(area.offset.y)) + area.extent.height > height or required_layers > layers) return null;
    if (aspect == c.VK_IMAGE_ASPECT_COLOR_BIT and first_format.* == graphics_state.NoColor) {
        first_format.* = if (view_state.image_format != 0) view_state.image_format else image_state.image_format;
    }
    ids.view = view.id;
    references[reference_count.*] = view;
    references[reference_count.* + 1] = image;
    references[reference_count.* + 2] = memory_record;
    reference_count.* += 3;
    if (info.resolveImageView) |handle| {
        const resolve_view = child_object(@intFromPtr(handle), c.VK_OBJECT_TYPE_IMAGE_VIEW, parent_id) orelse return null;
        const resolve_view_state = resource_state(resolve_view);
        const resolve_image = child_object(resolve_view_state.view_image, c.VK_OBJECT_TYPE_IMAGE, parent_id) orelse return null;
        const resolve_state = resource_state(resolve_image);
        const resolve_memory = child_object(resolve_state.bound_memory, c.VK_OBJECT_TYPE_DEVICE_MEMORY, parent_id) orelse return null;
        if (image_state.image_samples <= 1 or resolve_state.image_samples != 1 or resolve_view_state.image_format != view_state.image_format or
            resolve_view_state.image_usage & (if (aspect == c.VK_IMAGE_ASPECT_COLOR_BIT) @as(u32, c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) else c.VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0 or
            !image_range_valid(resolve_state, resolve_view_state.view_range) or resolve_view_state.view_range.aspectMask & aspect == 0 or resolve_view_state.view_range.levelCount != 1) return null;
        const resolve_shift: u5 = @intCast(@min(resolve_view_state.view_range.baseMipLevel, 31));
        const resolve_layers = if (resolve_view_state.view_range.layerCount == std.math.maxInt(u32)) resolve_state.image_layers - resolve_view_state.view_range.baseArrayLayer else resolve_view_state.view_range.layerCount;
        if (@as(u64, @intCast(area.offset.x)) + area.extent.width > @max(@as(u32, 1), resolve_state.image_extent[0] >> resolve_shift) or
            @as(u64, @intCast(area.offset.y)) + area.extent.height > @max(@as(u32, 1), resolve_state.image_extent[1] >> resolve_shift) or required_layers > resolve_layers) return null;
        ids.resolve = resolve_view.id;
        references[reference_count.*] = resolve_view;
        references[reference_count.* + 1] = resolve_image;
        references[reference_count.* + 2] = resolve_memory;
        reference_count.* += 3;
    }
    return ids;
}
/// Begin actual host dynamic rendering. Native inputs borrowed synchronously, no pointer
/// retention/heap. Local invalid Recording calls invalidate; ownership refs publish after ACK.
fn begin_rendering(command_buffer: c.VkCommandBuffer, info: [*c]const c.VkRenderingInfo) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    const pool = command_pool_for(record) orelse return;
    if (info == null or state.command_profile_index == 0 or graphics_recording(state).active_format != 0 or
        info.*.sType != c.VK_STRUCTURE_TYPE_RENDERING_INFO or info.*.pNext != null or info.*.colorAttachmentCount > 8 or
        (info.*.colorAttachmentCount != 0 and info.*.pColorAttachments == null) or !graphics_family_supported(pool))
    {
        if (lost == c.RingOk) state.command_state = .Invalid;
        return;
    }
    var references: [60]*c.venus_object_t = undefined;
    var reference_count: usize = 0;
    var first_format: u32 = graphics_state.NoColor;
    var colors: [8]dynamic_rendering.attachment_ids_t = undefined;
    if (info.*.colorAttachmentCount != 0) for (info.*.pColorAttachments[0..info.*.colorAttachmentCount], 0..) |*attachment, index| {
        colors[index] = resolve_rendering_attachment(pool.parent_id, @ptrCast(attachment), info.*.renderArea, info.*.layerCount, info.*.viewMask, c.VK_IMAGE_ASPECT_COLOR_BIT, &references, &reference_count, &first_format) orelse {
            state.command_state = .Invalid;
            return;
        };
    };
    const depth = if (info.*.pDepthAttachment != null) resolve_rendering_attachment(pool.parent_id, @ptrCast(info.*.pDepthAttachment), info.*.renderArea, info.*.layerCount, info.*.viewMask, c.VK_IMAGE_ASPECT_DEPTH_BIT, &references, &reference_count, &first_format) orelse {
        state.command_state = .Invalid;
        return;
    } else dynamic_rendering.attachment_ids_t{};
    const stencil = if (info.*.pStencilAttachment != null) resolve_rendering_attachment(pool.parent_id, @ptrCast(info.*.pStencilAttachment), info.*.renderArea, info.*.layerCount, info.*.viewMask, c.VK_IMAGE_ASPECT_STENCIL_BIT, &references, &reference_count, &first_format) orelse {
        state.command_state = .Invalid;
        return;
    } else dynamic_rendering.attachment_ids_t{};
    var staged = graphics_recording(state).*;
    graphics_state.begin_dynamic(&staged, first_format) catch {
        state.command_state = .Invalid;
        return;
    };
    const packet = dynamic_rendering.begin_rendering(record.id, @ptrCast(info), colors[0..info.*.colorAttachmentCount], depth, stencil) catch {
        state.command_state = .Invalid;
        return;
    };
    if (!command_acknowledged(&packet, 213)) return;
    graphics_recording(state).* = staged;
    for (references[0..reference_count]) |target| command_reference(state, target);
}
/// End actual dynamic rendering. Recording scope metadata changes only after host ACK;
/// no heap/native pointer retention; invalid local ordering invalidates command.
fn end_rendering(command_buffer: c.VkCommandBuffer) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording or state.command_profile_index == 0) return;
    var staged = graphics_recording(state).*;
    graphics_state.end_dynamic(&staged) catch {
        state.command_state = .Invalid;
        return;
    };
    const packet = dynamic_rendering.end_rendering(record.id) catch unreachable;
    if (command_acknowledged(&packet, 214)) graphics_recording(state).* = staged;
}

/// Resolve device owner by private protocol identity under the process mutex.
fn device_by_id(id: u64) ?*c.venus_object_t { for (&slots) |*slot| if(slot.id == id and slot.kind == c.VK_OBJECT_TYPE_DEVICE) return slot; return null; }
/// Validate live mixed descriptors visible to a draw or dispatch. No reference mutation.
fn validate_draw_descriptors(parent_id: u64, state: *resource_state_t, metadata: *compute_state.command_profile_t, stages: u32) bool {
    const parent=device_by_id(parent_id) orelse return false;
    const token=if(stages == 0x20) metadata.pipeline else graphics_recording(state).pipeline;
    const pipeline=child_object(token,c.VK_OBJECT_TYPE_PIPELINE,parent_id) orelse return false;
    const expected=profiles.get_profile(&profile_registry.pipelines,resource_state(pipeline).profile_index) orelse return false;
    if (expected.set_count == 0) return true;
    if (!ensure_descriptor_limits(parent)) return false;
    const ready=if(stages == 0x20) metadata.descriptor_layout_ready else metadata.graphics_descriptor_layout_ready;
    const layout=if(stages == 0x20) &metadata.descriptor_layout else &metadata.graphics_descriptor_layout;
    const tokens=if(stages == 0x20) &metadata.sets else &metadata.graphics_sets;
    for(expected.sets[0..expected.set_count],0..) |definition,set_index| {
        var required=false;
        for(definition.bindings[0..definition.binding_count]) |binding| if(binding.descriptor_count != 0 and binding.stage_flags & stages != 0) { required=true; break; };
        if(!required) continue;
        if(!ready or !compute_state.layouts_compatible(layout,expected,set_index)) return false;
        const set=descriptor_set_for(if(tokens[set_index] != 0) @ptrFromInt(tokens[set_index]) else null,parent_id) orelse return false;
        const profile=profiles.get_profile(&profile_registry.sets,resource_state(set).profile_index) orelse return false;
        if(!std.meta.eql(definition,profile.layout)) return false;
        for(definition.bindings[0..definition.binding_count]) |binding| {
            if(binding.stage_flags & stages == 0) continue;
            const count=if(profile.has_variable_count and binding.binding == profile.variable_binding) profile.variable_count else binding.descriptor_count;
            var written: usize=0;
            for(profile.descriptors[0..profile.descriptor_count]) |descriptor| if(descriptor.binding == binding.binding) {
                if(descriptor.array_element >= count or !descriptor_resource_valid(parent,&descriptor)) return false;
                written+=1;
            };
            if(binding.binding_flags & c.VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT == 0 and written<count) return false;
        }
    }
    return true;
}
/// Retain every visible real descriptor owner after native command acknowledgment.
/// Zero enabled null identities retain nothing. Caller validated all nonzero identities.
fn retain_draw_descriptors(parent_id: u64,state: *resource_state_t,metadata: *compute_state.command_profile_t,stages:u32) void {
    const token=if(stages == 0x20) metadata.pipeline else graphics_recording(state).pipeline;
    const pipeline=child_object(token,c.VK_OBJECT_TYPE_PIPELINE,parent_id).?;
    const expected=profiles.get_profile(&profile_registry.pipelines,resource_state(pipeline).profile_index).?;
    const tokens=if(stages == 0x20) &metadata.sets else &metadata.graphics_sets;
    for(expected.sets[0..expected.set_count],0..) |definition,set_index| {
        var visible=false; for(definition.bindings[0..definition.binding_count]) |binding| if(binding.descriptor_count != 0 and binding.stage_flags & stages != 0) { visible=true; break; };
        if(!visible) continue;
        const set=descriptor_set_for(@ptrFromInt(tokens[set_index]),parent_id).?;
        command_reference(state,set);
        const set_index_private = resource_index(set);
        state.descriptor_uses[set_index_private/64] |= @as(u64,1)<<@as(u6,@intCast(set_index_private%64));
        const profile=profiles.get_profile(&profile_registry.sets,resource_state(set).profile_index).?;
        for(profile.descriptors[0..profile.descriptor_count]) |descriptor| {
            var used=false; for(definition.bindings[0..definition.binding_count]) |binding| if(binding.binding == descriptor.binding and binding.stage_flags & stages != 0) {used=true;break;};
            if(!used) continue;
            retain_descriptor(parent_id,state,&descriptor);
        }
    }
}

const descriptor_layout_preflight_t = struct { profile: profiles.descriptor_layout_t, writer: mixed_wire.writer_t };
// Validates the same owned shape/features as creation without host or registry mutation.
fn preflight_descriptor_layout(parent: *const c.venus_object_t, info: [*c]const c.VkDescriptorSetLayoutCreateInfo) !descriptor_layout_preflight_t {
    if (info == null or info.*.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO or info.*.bindingCount > profiles.MaxBindings or (info.*.bindingCount != 0 and (info.*.pBindings == null or @intFromPtr(info.*.pBindings) % @alignOf(c.VkDescriptorSetLayoutBinding) != 0))) return error.Invalid;
    var flags: ?*const c.VkDescriptorSetLayoutBindingFlagsCreateInfo = null;
    if (info.*.pNext) |pointer| {
        if (@intFromPtr(pointer) % @alignOf(c.VkDescriptorSetLayoutBindingFlagsCreateInfo) != 0) return error.Invalid;
        flags = @ptrCast(@alignCast(pointer));
        const value = flags.?;
        if (value.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO or value.pNext != null or (value.bindingCount != 0 and (value.bindingCount != info.*.bindingCount or value.pBindingFlags == null or @intFromPtr(value.pBindingFlags) % @alignOf(u32) != 0))) return error.Invalid;
    }
    var profile = profiles.descriptor_layout_t{ .binding_count = info.*.bindingCount };
    var immutable = [_]mixed_wire.immutable_samplers_t{.{}} ** profiles.MaxBindings;
    var sampler_ids: [128]u64 = undefined;
    var native_bindings: [profiles.MaxBindings]c.VkDescriptorSetLayoutBinding = undefined;
    if (info.*.bindingCount != 0) for (info.*.pBindings[0..info.*.bindingCount], 0..) |binding, index| {
        native_bindings[index] = binding;
        var definition = profiles.binding_t{ .binding = binding.binding, .descriptor_type = binding.descriptorType, .descriptor_count = binding.descriptorCount, .stage_flags = binding.stageFlags, .binding_flags = if (flags != null and flags.?.bindingCount != 0) flags.?.pBindingFlags[index] else 0, .immutable_offset = @intCast(profile.immutable_count) };
        if (binding.descriptorCount == 0) native_bindings[index].pImmutableSamplers = null;
        if (binding.descriptorCount != 0 and binding.pImmutableSamplers != null) {
            if (binding.descriptorType > 1 or @intFromPtr(binding.pImmutableSamplers) % @alignOf(c.VkSampler) != 0) return error.Invalid;
            if (binding.descriptorCount > 128 - profile.immutable_count) return error.Limit;
            const first = profile.immutable_count;
            for (binding.pImmutableSamplers[0..binding.descriptorCount]) |handle| {
                if (handle == null) return error.Invalid;
                const sampler = child_object(@intFromPtr(handle.?), c.VK_OBJECT_TYPE_SAMPLER, parent.id) orelse return error.Invalid;
                sampler_ids[profile.immutable_count] = sampler.id;
                profile.immutable_samplers[profile.immutable_count] = sampler.handle;
                profile.immutable_count += 1;
            }
            definition.immutable_count = @intCast(binding.descriptorCount);
            immutable[index].ids = sampler_ids[first..profile.immutable_count];
        }
        var destination = index;
        while (destination > 0 and profile.bindings[destination - 1].binding > definition.binding) : (destination -= 1) profile.bindings[destination] = profile.bindings[destination - 1];
        if (destination > 0 and profile.bindings[destination - 1].binding == definition.binding) return error.Invalid;
        profile.bindings[destination] = definition;
    };
    _ = profiles.create_sparse_set_profile(&profile) catch return error.Invalid;
    var normalized = info.*;
    normalized.pBindings = if (info.*.bindingCount == 0) null else @ptrCast(&native_bindings);
    const writer = mixed_wire.create_layout(parent.id, 1, @ptrCast(&normalized), immutable[0..info.*.bindingCount]) catch |err| return if (err == error.Limit) error.Limit else error.Invalid;
    for (profile.bindings[0..profile.binding_count]) |binding| {
        if (binding.binding_flags & c.VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT != 0 and !update_after_bind_supported(parent,binding.descriptor_type)) return error.FeatureNotPresent;
        if (binding.binding_flags & c.VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT != 0 and !descriptor_feature(parent,"descriptorBindingUpdateUnusedWhilePending")) return error.FeatureNotPresent;
        if (binding.binding_flags & c.VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT != 0 and !descriptor_feature(parent,"descriptorBindingPartiallyBound")) return error.FeatureNotPresent;
        if (binding.binding_flags & c.VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT != 0 and !descriptor_feature(parent,"descriptorBindingVariableDescriptorCount")) return error.FeatureNotPresent;
    }
    return .{ .profile = profile, .writer = writer };
}
/// Create owned sorted mixed descriptor metadata. [in] borrowed live device/native
/// input; flags chain and immutable samplers copied. [out] ownedlayout or null.
/// Caller immutable feature policy required; serialized no caller pointers retained.
fn create_descriptor_layout(device: c.VkDevice, info: [*c]const c.VkDescriptorSetLayoutCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkDescriptorSetLayout) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or info.*.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO or info.*.bindingCount > profiles.MaxBindings or (info.*.bindingCount != 0 and (info.*.pBindings == null or @intFromPtr(info.*.pBindings) % @alignOf(c.VkDescriptorSetLayoutBinding) != 0))) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const prepared = preflight_descriptor_layout(parent, info) catch |err| return switch (err) {
        error.FeatureNotPresent => c.VK_ERROR_FEATURE_NOT_PRESENT,
        error.Limit => c.VK_ERROR_OUT_OF_HOST_MEMORY,
        else => c.VK_ERROR_INITIALIZATION_FAILED,
    };
    const profile = prepared.profile;
    var writer = prepared.writer;
    const index = profiles.reserve_slot(&profile_registry.descriptor_layouts, profile) catch return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, &writer, &handle);
    if (result != c.VK_SUCCESS) {
        if (lost == c.RingOk) std.debug.assert(profiles.release_slot(&profile_registry.descriptor_layouts, index));
        return result;
    }
    resource_state(child_object(handle, c.VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, parent.id).?).profile_index = index;
    output.* = @ptrFromInt(handle);
    return c.VK_SUCCESS;
}
/// Create real large-capacity descriptor pool without proportional metadata.
/// [in] borrowed native info; [out] owned identity/null. Serialized; actual sets
/// retain fixed registry metadata independently of declared poolscalar budgets.
fn create_descriptor_pool(device: c.VkDevice, info: [*c]const c.VkDescriptorPoolCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkDescriptorPool) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or (info.*.poolSizeCount != 0 and (info.*.pPoolSizes == null or @intFromPtr(info.*.pPoolSizes) % @alignOf(c.VkDescriptorPoolSize) != 0))) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (info.*.flags & c.VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT != 0 and !descriptor_feature(parent,"descriptorBindingSampledImageUpdateAfterBind") and !descriptor_feature(parent,"descriptorBindingStorageImageUpdateAfterBind") and !descriptor_feature(parent,"descriptorBindingUniformBufferUpdateAfterBind") and !descriptor_feature(parent,"descriptorBindingStorageBufferUpdateAfterBind")) return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = mixed_wire.create_pool(parent.id, 1, @ptrCast(info)) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    var handle: u64 = 0;
    const result = create_render_resource(parent, c.VK_OBJECT_TYPE_DESCRIPTOR_POOL, &writer, &handle);
    if (result != c.VK_SUCCESS) return result;
    const state = resource_state(child_object(handle, c.VK_OBJECT_TYPE_DESCRIPTOR_POOL, parent.id).?);
    state.pool_flags = info.*.flags;
    state.descriptor_max_sets = info.*.maxSets;
    if (info.*.poolSizeCount != 0) for (info.*.pPoolSizes[0..info.*.poolSizeCount]) |size| {
        state.descriptor_capacity[size.type] += size.descriptorCount;
    };
    output.* = @ptrFromInt(handle);
    return c.VK_SUCCESS;
}

/// Allocate owned sparse descriptor sets with original compatible layout definition
/// and explicit actual variablecounts. [in] nativearraysborrowed throughcall; [out]
/// null-onfailure tokens. Nativefailure rollsbackall reservedmetadata; loss retains
/// uncertainowners until trustedretirement. Poolbudgets charge/refundactualcounts.
fn allocate_descriptor_sets(device: c.VkDevice, info: [*c]const c.VkDescriptorSetAllocateInfo, output: [*c]c.VkDescriptorSet) callconv(.C) c_int {
    lock_icd();
    defer unlock_icd();
    defer @memset(&descriptor_allocation_snapshots, .{});
    if (info == null or output == null or info.*.descriptorSetCount == 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const count = info.*.descriptorSetCount;
    if (count > 64) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    @memset(output[0..count], null);
    if (device == null or info.*.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO or info.*.descriptorPool == null or info.*.pSetLayouts == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    var variable: ?*const c.VkDescriptorSetVariableDescriptorCountAllocateInfo = null;
    if (info.*.pNext) |pointer| {
        if (@intFromPtr(pointer) % @alignOf(c.VkDescriptorSetVariableDescriptorCountAllocateInfo) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
        variable = @ptrCast(@alignCast(pointer));
        const value = variable.?;
        if (value.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO or value.pNext != null or (value.descriptorSetCount != 0 and (value.descriptorSetCount != count or value.pDescriptorCounts == null or @intFromPtr(value.pDescriptorCounts) % @alignOf(u32) != 0))) return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const pool = child_object(@intFromPtr(info.*.descriptorPool.?), c.VK_OBJECT_TYPE_DESCRIPTOR_POOL, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const owner = resource_state(pool);
    if (count > owner.descriptor_max_sets - owner.descriptor_live_sets) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var layouts: [64]u64 = undefined;
    const snapshots = &descriptor_allocation_snapshots;
    var needed = [_]u32{0} ** 11;
    for (info.*.pSetLayouts[0..count], 0..) |handle, index| {
        if (handle == null) return c.VK_ERROR_INITIALIZATION_FAILED;
        const layout = child_object(@intFromPtr(handle.?), c.VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
        const profile = profiles.get_profile(&profile_registry.descriptor_layouts, resource_state(layout).profile_index).?;
        snapshots[index] = profiles.create_sparse_set_profile(profile) catch return c.VK_ERROR_INITIALIZATION_FAILED;
        var found_variable = false;
        for (profile.bindings[0..profile.binding_count]) |binding| if (binding.binding_flags & c.VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT != 0) {
            if (found_variable) return c.VK_ERROR_INITIALIZATION_FAILED;
            found_variable = true;
            const actual = if (variable != null and variable.?.descriptorSetCount != 0) variable.?.pDescriptorCounts[index] else 0;
            if (actual > binding.descriptor_count) return c.VK_ERROR_INITIALIZATION_FAILED;
            snapshots[index].has_variable_count = true;
            snapshots[index].variable_binding = binding.binding;
            snapshots[index].variable_count = actual;
        };
        if (variable != null and variable.?.descriptorSetCount != 0 and !found_variable and variable.?.pDescriptorCounts[index] != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
        layouts[index] = layout.id;
        for (profile.bindings[0..profile.binding_count]) |binding| {
            const actual = if (snapshots[index].has_variable_count and binding.binding == snapshots[index].variable_binding) snapshots[index].variable_count else binding.descriptor_count;
            needed[binding.descriptor_type] = std.math.add(u32, needed[binding.descriptor_type], actual) catch return c.VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    for (needed, 0..) |value, kind| if (value > owner.descriptor_capacity[kind] - owner.descriptor_used[kind]) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var provisional_ids: [64]u64 = undefined;
    for (provisional_ids[0..count], 0..) |*id, index| id.* = index + 1;
    _ = mixed_wire.allocate_sets(parent.id, pool.id, @ptrCast(info), layouts[0..count], provisional_ids[0..count]) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    var records: [64]*c.venus_object_t = undefined;
    var ids: [64]u64 = undefined;
    var reserved: usize = 0;
    while (reserved < count) : (reserved += 1) {
        const index = profiles.reserve_slot(&profile_registry.sets, snapshots[reserved]) catch {
            rollback_descriptor_sets(records[0..reserved]);
            return c.VK_ERROR_OUT_OF_HOST_MEMORY;
        };
        var record: [*c]c.venus_object_t = null;
        if (c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DESCRIPTOR_SET, pool.id, 0, &record) != c.RingOk) {
            std.debug.assert(profiles.release_slot(&profile_registry.sets, index));
            rollback_descriptor_sets(records[0..reserved]);
            return c.VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        resource_state(record).* = .{ .id = record.*.id, .profile_index = index };
        records[reserved] = record;
        ids[reserved] = record.*.id;
    }
    const writer = mixed_wire.allocate_sets(parent.id, pool.id, @ptrCast(info), layouts[0..count], ids[0..count]) catch unreachable;
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = descriptor_sets_reply(reply, ids[0..count]) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result != c.VK_SUCCESS) {
        rollback_descriptor_sets(records[0..count]);
        return result;
    }
    owner.descriptor_live_sets += count;
    for (needed, 0..) |value, kind| owner.descriptor_used[kind] += value;
    for (records[0..count], 0..) |record, index| output[index] = @ptrFromInt(record.handle);
    return c.VK_SUCCESS;
}

// Replace original pipeline_barrier2 body with wrapper below; normalization is
// shared and produces no host command_buffer or ownership change before complete validation.
const dependency_result_t = struct { packet: modern_sync.writer_t, references: [8]u64 };
/// [in] borrowed live recording command_buffer and canonical dependency. [out] owned
/// translated packet/reference bitmap or null with invalid command_buffer. No heap or
/// retained native pointers; existing process mutex must be held.
fn normalize_dependency(record: *c.venus_object_t, info: [*c]const c.VkDependencyInfo) ?dependency_result_t {
    const state = resource_state(record);
    if (state.command_state != .Recording) return null;
    if (!outside_render_pass(state)) return null;
    const pool = command_pool_for(record) orelse return null;
    if (info == null or info.*.sType != c.VK_STRUCTURE_TYPE_DEPENDENCY_INFO or info.*.pNext != null or
        info.*.dependencyFlags & ~@as(u32, 7) != 0 or info.*.memoryBarrierCount > 64 or
        info.*.bufferMemoryBarrierCount > 64 or info.*.imageMemoryBarrierCount > 64 or
        (info.*.memoryBarrierCount != 0 and info.*.pMemoryBarriers == null) or
        (info.*.bufferMemoryBarrierCount != 0 and info.*.pBufferMemoryBarriers == null) or
        (info.*.imageMemoryBarrierCount != 0 and info.*.pImageMemoryBarriers == null))
    {
        state.command_state = .Invalid;
        return null;
    }
    var buffers: [64]c.VkBufferMemoryBarrier2 = undefined;
    var images: [64]c.VkImageMemoryBarrier2 = undefined;
    var references: [256]*c.venus_object_t = undefined;
    var reference_count: usize = 0;
    if (info.*.bufferMemoryBarrierCount != 0) for (info.*.pBufferMemoryBarriers[0..info.*.bufferMemoryBarrierCount], 0..) |barrier, index| {
        const target = if (barrier.buffer) |handle| child_object(@intFromPtr(handle), c.VK_OBJECT_TYPE_BUFFER, pool.parent_id) else null;
        if (target == null or !configured_family_pair(pool.parent_id, barrier.srcQueueFamilyIndex, barrier.dstQueueFamilyIndex)) {
            state.command_state = .Invalid;
            return null;
        }
        const resource = resource_state(target.?);
        const memory_record = child_object(resource.bound_memory, c.VK_OBJECT_TYPE_DEVICE_MEMORY, pool.parent_id);
        if (memory_record == null or barrier.offset >= resource.buffer_size or
            (barrier.size != std.math.maxInt(u64) and (barrier.size == 0 or barrier.size > resource.buffer_size - barrier.offset)))
        {
            state.command_state = .Invalid;
            return null;
        }
        buffers[index] = barrier;
        buffers[index].buffer = @ptrFromInt(target.?.id);
        references[reference_count] = target.?;
        references[reference_count + 1] = memory_record.?;
        reference_count += 2;
    };
    var device_handle: u64 = 0;
    for (device_caches) |entry| if (entry.handle != 0 and object(entry.handle, c.VK_OBJECT_TYPE_DEVICE).?.id == pool.parent_id) {
        device_handle = entry.handle;
        break;
    };
    if (info.*.imageMemoryBarrierCount != 0) for (info.*.pImageMemoryBarriers[0..info.*.imageMemoryBarrierCount], 0..) |barrier, index| {
        const target = if (barrier.image) |handle| child_object(@intFromPtr(handle), c.VK_OBJECT_TYPE_IMAGE, pool.parent_id) else null;
        if (target == null or !configured_family_pair(pool.parent_id, barrier.srcQueueFamilyIndex, barrier.dstQueueFamilyIndex)) {
            state.command_state = .Invalid;
            return null;
        }
        const resource = resource_state(target.?);
        const memory_record = child_object(resource.bound_memory, c.VK_OBJECT_TYPE_DEVICE_MEMORY, pool.parent_id);
        if (memory_record == null or !image_range_valid(resource, barrier.subresourceRange) or
            barrier.newLayout == c.VK_IMAGE_LAYOUT_UNDEFINED or barrier.newLayout == c.VK_IMAGE_LAYOUT_PREINITIALIZED)
        {
            state.command_state = .Invalid;
            return null;
        }
        images[index] = barrier;
        images[index].image = @ptrFromInt(target.?.id);
        if (wsi.is_present_image(&wsi_state, device_handle, target.?.handle)) {
            if (images[index].oldLayout == c.VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) images[index].oldLayout = c.VK_IMAGE_LAYOUT_GENERAL;
            if (images[index].newLayout == c.VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) images[index].newLayout = c.VK_IMAGE_LAYOUT_GENERAL;
        }
        references[reference_count] = target.?;
        references[reference_count + 1] = memory_record.?;
        reference_count += 2;
    };
    var translated = info.*;
    translated.pBufferMemoryBarriers = if (info.*.bufferMemoryBarrierCount != 0) &buffers else null;
    translated.pImageMemoryBarriers = if (info.*.imageMemoryBarrierCount != 0) &images else null;
    const packet = modern_sync.pipeline_barrier2(record.id, @ptrCast(&translated)) catch {
        state.command_state = .Invalid;
        return null;
    };
    var retained = [_]u64{0} ** 8;
    for (references[0..reference_count]) |target| { const index = resource_index(target); retained[index / 64] |= @as(u64, 1) << @as(u6, @intCast(index % 64)); }
    return .{.packet=packet,.references=retained};
}

fn dependency_retain(state: *resource_state_t, references: [8]u64) void {
    for (&state.buffer_references, references) |*current, bits| current.* |= bits;
}
/// [in] borrowed command_buffer/dependency. Actual host barriers and owners publish only
/// after complete normalization and acknowledged204. No heap, mutex serialized.
fn pipeline_barrier2(command_buffer: c.VkCommandBuffer, info: [*c]const c.VkDependencyInfo) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const normalized = normalize_dependency(record, info) orelse return;
    if (command_acknowledged(&normalized.packet, 204)) dependency_retain(resource_state(record), normalized.references);
}
fn append_dependency_bytes(writer: *modern_sync.writer_t, bytes: []const u8) !void {
    if (writer.used > writer.bytes.len or bytes.len > writer.bytes.len-writer.used) return error.Limit;
    @memcpy(writer.bytes[writer.used..][0..bytes.len], bytes); writer.used += bytes.len;
}
fn command_event(record: *c.venus_object_t, event: c.VkEvent) ?*c.venus_object_t {
    const pool = command_pool_for(record) orelse return null;
    if (event == null) return null;
    return child_object(@intFromPtr(event.?), c.VK_OBJECT_TYPE_EVENT, pool.parent_id);
}
/// [in] same-device command_buffer/event and borrowed dependency. [out] Actual201 host
/// signal/dependency; command_buffer retains event and all resources only after ACK.
/// Invalid input invalidates recording, no heap/native pointer retention, mutex.
fn cmd_set_event2(command_buffer: c.VkCommandBuffer, event: c.VkEvent, info: [*c]const c.VkDependencyInfo) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    const selected = command_event(record, event) orelse { state.command_state = .Invalid; return; };
    const normalized = normalize_dependency(record, info) orelse return;
    var packet: modern_sync.writer_t = .{};
    packet.header(201, record.id) catch unreachable; packet.put(u64, selected.id) catch unreachable;
    append_dependency_bytes(&packet, normalized.packet.bytes[16..normalized.packet.used]) catch { state.command_state=.Invalid; return; };
    if (!command_acknowledged(&packet, 201)) return;
    dependency_retain(state, normalized.references); command_reference(state, selected);
}
/// [in] recording command_buffer/event/full64-bit stage. [out] actual202 and retained
/// event after ACK; invalid/lost never publishes owners. No heap, mutex serialized.
fn cmd_reset_event2(command_buffer: c.VkCommandBuffer, event: c.VkEvent, stage: c.VkPipelineStageFlags2) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording or !outside_render_pass(state)) return;
    const selected = command_event(record, event) orelse { state.command_state=.Invalid; return; };
    const packet = modern_sync.reset_event2(record.id, selected.id, stage) catch {state.command_state=.Invalid; return;};
    if (command_acknowledged(&packet,202)) command_reference(state,selected);
}
/// [in]1..64 same-device events and matching borrowed dependencies. [out] one
/// actual203 host command_buffer after complete normalization; retains bitmap of every
/// event/resource on ACK. Quota/capacity failure invalidates command_buffer, no partial
/// host packet; allocation-free and process mutex serialized.
fn cmd_wait_events2(command_buffer: c.VkCommandBuffer, count: u32, events: [*c]const c.VkEvent, infos: [*c]const c.VkDependencyInfo) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording or !outside_render_pass(state)) return;
    if (count==0 or count>64 or events==null or infos==null) {state.command_state=.Invalid; return;}
    var ids: [64]u64 = undefined;
    var references = [_]u64{0} ** 8;
    for (events[0..count],0..) |event,index| {
        const selected=command_event(record,event) orelse {state.command_state=.Invalid; return;};
        ids[index]=selected.id; const slot=resource_index(selected);
        references[slot/64] |= @as(u64,1)<<@as(u6,@intCast(slot%64));
    }
    var packet: modern_sync.writer_t = .{};
    packet.header(203,record.id) catch unreachable;
    packet.put(u32,count) catch unreachable; packet.put(u64,count) catch unreachable;
    for (ids[0..count]) |id| packet.put(u64,id) catch unreachable;
    packet.put(u64,count) catch unreachable;
    for (infos[0..count]) |*info| {
        const normalized=normalize_dependency(record,info) orelse return;
        append_dependency_bytes(&packet,normalized.packet.bytes[24..normalized.packet.used]) catch {state.command_state=.Invalid; return;};
        for (&references,normalized.references) |*current,bits| current.* |= bits;
    }
    if(command_acknowledged(&packet,203))dependency_retain(state,references);
}
// dispatch core/KHR vkCmdSetEvent2, vkCmdResetEvent2, vkCmdWaitEvents2

/// [in] live device and borrowed canonical unprotected queue request. [out]
/// Borrowed same-device queue or null on failure, no ownership transfer. Protected
/// queues unsupported and never advertised; no heap, recursive mutex serialized.
fn get_device_queue2(device: c.VkDevice, info: [*c]const c.VkDeviceQueueInfo2, output: [*c]c.VkQueue) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (output == null) return;
    output.* = null;
    if (info == null or @intFromPtr(info) % @alignOf(c.VkDeviceQueueInfo2) != 0 or info.*.sType != c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2 or info.*.pNext != null or info.*.flags != 0) return;
    get_device_queue(device, info.*.queueFamilyIndex, info.*.queueIndex, output);
}
// vkGetDeviceQueue2 -> get_device_queue2 (1.1 route).

// Owner imports const dynamic_graphics = @import("venus_graphics_dynamic_wire.zig");
// All functions execute under the existing ICD mutex. Caller integrates enabled feature
// checks and draw descriptor validation with its mixed-descriptor state helper.
fn dynamic_command_record(command_buffer: c.VkCommandBuffer) ?*c.venus_object_t {
    if (lost != c.RingOk or command_buffer == null) return null;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return null;
    if (resource_state(record).command_state != .Recording) return null;
    return record;
}
fn dynamic_command_ack(record: *c.venus_object_t, writer: ?dynamic_graphics.writer_t) void {
    const encoded = writer orelse {
        resource_state(record).command_state = .Invalid;
        return;
    };
    _ = command_acknowledged(&encoded, std.mem.readInt(u32, encoded.bytes[0..4], .little));
}
fn set_viewport(command_buffer: c.VkCommandBuffer, first: u32, count: u32, values: [*c]const c.VkViewport) callconv(.C) void {
    set_viewport_impl(command_buffer, first, count, values, false);
}
fn set_viewport_with_count(command_buffer: c.VkCommandBuffer, count: u32, values: [*c]const c.VkViewport) callconv(.C) void {
    set_viewport_impl(command_buffer, 0, count, values, true);
}
fn set_viewport_impl(command_buffer: c.VkCommandBuffer, first: u32, count: u32, values: [*c]const c.VkViewport, with_count: bool) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    if (count == 0 or count > 16 or values == null) {
        resource_state(record).command_state = .Invalid;
        return;
    }
    dynamic_command_ack(record, dynamic_graphics.viewports(record.id, first, @ptrCast(values[0..count]), with_count) catch null);
}
fn set_scissor(command_buffer: c.VkCommandBuffer, first: u32, count: u32, values: [*c]const c.VkRect2D) callconv(.C) void {
    set_scissor_impl(command_buffer, first, count, values, false);
}
fn set_scissor_with_count(command_buffer: c.VkCommandBuffer, count: u32, values: [*c]const c.VkRect2D) callconv(.C) void {
    set_scissor_impl(command_buffer, 0, count, values, true);
}
fn set_scissor_impl(command_buffer: c.VkCommandBuffer, first: u32, count: u32, values: [*c]const c.VkRect2D, with_count: bool) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    if (count == 0 or count > 16 or values == null) {
        resource_state(record).command_state = .Invalid;
        return;
    }
    dynamic_command_ack(record, dynamic_graphics.scissors(record.id, first, @ptrCast(values[0..count]), with_count) catch null);
}
fn set_cull_mode(command_buffer: c.VkCommandBuffer, value: c.VkCullModeFlags) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.scalar(record.id, .CullMode, value) catch null);
}
fn set_front_face(command_buffer: c.VkCommandBuffer, value: c.VkFrontFace) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.scalar(record.id, .FrontFace, value) catch null);
}
fn set_primitive_topology(command_buffer: c.VkCommandBuffer, value: c.VkPrimitiveTopology) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.scalar(record.id, .PrimitiveTopology, value) catch null);
}
fn set_depth_test_enable(command_buffer: c.VkCommandBuffer, value: c.VkBool32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.scalar(record.id, .DepthTestEnable, value) catch null);
}
fn set_depth_write_enable(command_buffer: c.VkCommandBuffer, value: c.VkBool32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.scalar(record.id, .DepthWriteEnable, value) catch null);
}
fn set_depth_compare_op(command_buffer: c.VkCommandBuffer, value: c.VkCompareOp) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.scalar(record.id, .DepthCompareOp, value) catch null);
}
fn set_depth_bounds_test_enable(command_buffer: c.VkCommandBuffer, value: c.VkBool32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.scalar(record.id, .DepthBoundsTestEnable, value) catch null);
}
fn set_stencil_test_enable(command_buffer: c.VkCommandBuffer, value: c.VkBool32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.scalar(record.id, .StencilTestEnable, value) catch null);
}
fn set_rasterizer_discard_enable(command_buffer: c.VkCommandBuffer, value: c.VkBool32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.scalar(record.id, .RasterizerDiscardEnable, value) catch null);
}
fn set_depth_bias_enable(command_buffer: c.VkCommandBuffer, value: c.VkBool32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.scalar(record.id, .DepthBiasEnable, value) catch null);
}
fn set_primitive_restart_enable(command_buffer: c.VkCommandBuffer, value: c.VkBool32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.scalar(record.id, .PrimitiveRestartEnable, value) catch null);
}
fn set_stencil_compare_mask(command_buffer: c.VkCommandBuffer, faces: c.VkStencilFaceFlags, value: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.stencil_mask(record.id, .CompareMask, faces, value) catch null);
}
fn set_stencil_write_mask(command_buffer: c.VkCommandBuffer, faces: c.VkStencilFaceFlags, value: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.stencil_mask(record.id, .WriteMask, faces, value) catch null);
}
fn set_stencil_reference(command_buffer: c.VkCommandBuffer, faces: c.VkStencilFaceFlags, value: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.stencil_mask(record.id, .Reference, faces, value) catch null);
}
fn set_depth_bias(command_buffer: c.VkCommandBuffer, constant: f32, clamp: f32, slope: f32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.depth_bias(record.id, .{ constant, clamp, slope }) catch null);
}
fn set_depth_bounds(command_buffer: c.VkCommandBuffer, minimum: f32, maximum: f32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.depth_bounds(record.id, minimum, maximum) catch null);
}
fn set_blend_constants(command_buffer: c.VkCommandBuffer, values: [*c]const f32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    if (values == null) {
        resource_state(record).command_state = .Invalid;
        return;
    }
    dynamic_command_ack(record, dynamic_graphics.blend_constants(record.id, values[0..4].*) catch null);
}
fn set_stencil_op(command_buffer: c.VkCommandBuffer, faces: c.VkStencilFaceFlags, fail: c.VkStencilOp, pass: c.VkStencilOp, depth_fail: c.VkStencilOp, compare: c.VkCompareOp) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    dynamic_command_ack(record, dynamic_graphics.stencil_ops(record.id, faces, .{ fail, pass, depth_fail, compare }) catch null);
}
fn bind_vertex_buffers(command_buffer: c.VkCommandBuffer, first: u32, count: u32, buffers: [*c]const c.VkBuffer, offsets: [*c]const c.VkDeviceSize) callconv(.C) void {
    bind_vertex_buffers_impl(command_buffer, first, count, buffers, offsets, null, null, false);
}
fn bind_vertex_buffers2(command_buffer: c.VkCommandBuffer, first: u32, count: u32, buffers: [*c]const c.VkBuffer, offsets: [*c]const c.VkDeviceSize, sizes: [*c]const c.VkDeviceSize, strides: [*c]const c.VkDeviceSize) callconv(.C) void {
    bind_vertex_buffers_impl(command_buffer, first, count, buffers, offsets, sizes, strides, true);
}
fn bind_vertex_buffers_impl(command_buffer: c.VkCommandBuffer, first: u32, count: u32, buffers: [*c]const c.VkBuffer, offsets: [*c]const c.VkDeviceSize, sizes: [*c]const c.VkDeviceSize, strides: [*c]const c.VkDeviceSize, v2: bool) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    const state = resource_state(record);
    const pool = command_pool_for(record) orelse return;
    if (count == 0 or count > 32 or first > 32 - count or buffers == null or offsets == null) {
        state.command_state = .Invalid;
        return;
    }
    var ids: [32]u64 = undefined;
    var refs: [64]*c.venus_object_t = undefined;
    for (buffers[0..count], 0..) |handle, index| {
        const buffer = if (handle) |token| child_object(@intFromPtr(token), c.VK_OBJECT_TYPE_BUFFER, pool.parent_id) else null;
        if (buffer == null) {
            state.command_state = .Invalid;
            return;
        }
        const meta = resource_state(buffer.?);
        const allocation = child_object(meta.bound_memory, c.VK_OBJECT_TYPE_DEVICE_MEMORY, pool.parent_id) orelse {
            state.command_state = .Invalid;
            return;
        };
        if (meta.buffer_usage & c.VK_BUFFER_USAGE_VERTEX_BUFFER_BIT == 0 or offsets[index] > meta.buffer_size or (sizes != null and sizes[index] != c.VK_WHOLE_SIZE and sizes[index] > meta.buffer_size - offsets[index])) {
            state.command_state = .Invalid;
            return;
        }
        ids[index] = buffer.?.id;
        refs[index * 2] = buffer.?;
        refs[index * 2 + 1] = allocation;
    }
    const writer = dynamic_graphics.bind_vertex_buffers(record.id, first, ids[0..count], offsets[0..count], if (sizes != null) sizes[0..count] else null, if (strides != null) strides[0..count] else null, v2) catch {
        state.command_state = .Invalid;
        return;
    };
    if (!command_acknowledged(&writer, if (v2) 220 else 105)) return;
    for (refs[0 .. count * 2]) |reference| command_reference(state, reference);
}
fn bind_index_buffer(command_buffer: c.VkCommandBuffer, buffer: c.VkBuffer, offset: c.VkDeviceSize, index_type: c.VkIndexType) callconv(.C) void {
    bind_index_buffer_impl(command_buffer, buffer, offset, null, index_type);
}
fn bind_index_buffer2(command_buffer: c.VkCommandBuffer, buffer: c.VkBuffer, offset: c.VkDeviceSize, size: c.VkDeviceSize, index_type: c.VkIndexType) callconv(.C) void {
    bind_index_buffer_impl(command_buffer, buffer, offset, size, index_type);
}
fn bind_index_buffer_impl(command_buffer: c.VkCommandBuffer, handle: c.VkBuffer, offset: c.VkDeviceSize, size: ?c.VkDeviceSize, index_type: c.VkIndexType) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    const state = resource_state(record);
    const pool = command_pool_for(record) orelse return;
    const buffer = if (handle) |token| child_object(@intFromPtr(token), c.VK_OBJECT_TYPE_BUFFER, pool.parent_id) else null;
    if (buffer == null) {
        state.command_state = .Invalid;
        return;
    }
    const meta = resource_state(buffer.?);
    const allocation = child_object(meta.bound_memory, c.VK_OBJECT_TYPE_DEVICE_MEMORY, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return;
    };
    if (meta.buffer_usage & c.VK_BUFFER_USAGE_INDEX_BUFFER_BIT == 0 or offset > meta.buffer_size or (size != null and size.? != c.VK_WHOLE_SIZE and size.? > meta.buffer_size - offset)) {
        state.command_state = .Invalid;
        return;
    }
    const writer = dynamic_graphics.bind_index_buffer(record.id, buffer.?.id, offset, index_type, size) catch {
        state.command_state = .Invalid;
        return;
    };
    if (!command_acknowledged(&writer, if (size != null) 279 else 104)) return;
    command_reference(state, buffer.?);
    command_reference(state, allocation);
    state.index_buffer = @intFromPtr(handle.?);
    state.index_offset = offset;
    state.index_size = if (size != null and size.? != c.VK_WHOLE_SIZE) size.? else meta.buffer_size - offset;
    state.index_type = index_type;
}

// Import dynamic_graphics alongside dynamic_graphics.zig entrypoint draft.
// Owner supplies validate_draw_descriptors(parent_id,state,metadata,stage_mask) bool,
// and retain_draw_descriptors(parent_id,state,metadata,stage_mask) void after ACK.
// Metadata here must be the separate graphics command profile selected by owner.
fn validate_graphics_draw(record: *c.venus_object_t) ?*c.venus_object_t {
    const state = resource_state(record);
    const pool = command_pool_for(record) orelse return null;
    const token = graphics_state.draw_pipeline(graphics_recording(state)) catch {
        state.command_state = .Invalid;
        return null;
    };
    const pipeline = child_object(token, c.VK_OBJECT_TYPE_PIPELINE, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return null;
    };
    if (resource_state(pipeline).pipeline_bind_point != c.VK_PIPELINE_BIND_POINT_GRAPHICS or !validate_draw_descriptors(pool.parent_id, state, command_profile(record), 0x1f)) {
        state.command_state = .Invalid;
        return null;
    }
    return pipeline;
}
fn bound_index_for_draw(record: *c.venus_object_t, first: u32, count: u32) ?*c.venus_object_t {
    const state = resource_state(record);
    const pool = command_pool_for(record) orelse return null;
    const buffer = child_object(state.index_buffer, c.VK_OBJECT_TYPE_BUFFER, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return null;
    };
    const meta = resource_state(buffer);
    if (meta.bound_memory == 0 or meta.buffer_usage & c.VK_BUFFER_USAGE_INDEX_BUFFER_BIT == 0 or state.index_type > 1 or state.index_offset > meta.buffer_size) {
        state.command_state = .Invalid;
        return null;
    }
    const element_bytes: u64 = if (state.index_type == c.VK_INDEX_TYPE_UINT16) 2 else 4;
    const size = @min(state.index_size, meta.buffer_size - state.index_offset);
    const first_bytes = @as(u64, first) * element_bytes;
    const count_bytes = @as(u64, count) * element_bytes;
    if (first_bytes > size or count_bytes > size - first_bytes) {
        state.command_state = .Invalid;
        return null;
    }
    return buffer;
}
fn draw_indexed(command_buffer: c.VkCommandBuffer, count: u32, instances: u32, first: u32, vertex_offset: i32, first_instance: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    const pipeline = validate_graphics_draw(record) orelse return;
    const index = bound_index_for_draw(record, first, count) orelse return;
    const writer = dynamic_graphics.draw_indexed(record.id, count, instances, first, vertex_offset, first_instance) catch {
        resource_state(record).command_state = .Invalid;
        return;
    };
    if (!command_acknowledged(&writer, 107)) return;
    const pool = command_pool_for(record).?;
    retain_draw_descriptors(pool.parent_id, resource_state(record), command_profile(record), 0x1f);
    command_reference(resource_state(record), pipeline);
    command_reference(resource_state(record), index);
}
fn draw_indirect(command_buffer: c.VkCommandBuffer, buffer: c.VkBuffer, offset: c.VkDeviceSize, count: u32, stride: u32) callconv(.C) void {
    draw_indirect_impl(command_buffer, buffer, offset, count, stride, false);
}
fn draw_indexed_indirect(command_buffer: c.VkCommandBuffer, buffer: c.VkBuffer, offset: c.VkDeviceSize, count: u32, stride: u32) callconv(.C) void {
    draw_indirect_impl(command_buffer, buffer, offset, count, stride, true);
}
fn draw_indirect_impl(command_buffer: c.VkCommandBuffer, handle: c.VkBuffer, offset: c.VkDeviceSize, count: u32, stride: u32, indexed: bool) void {
    lock_icd();
    defer unlock_icd();
    const record = dynamic_command_record(command_buffer) orelse return;
    const state = resource_state(record);
    const pool = command_pool_for(record) orelse return;
    const pipeline = validate_graphics_draw(record) orelse return;
    const index = if (indexed) bound_index_for_draw(record, 0, 0) orelse return else null;
    const buffer = if (handle) |token| child_object(@intFromPtr(token), c.VK_OBJECT_TYPE_BUFFER, pool.parent_id) else null;
    if (buffer == null) {
        state.command_state = .Invalid;
        return;
    }
    const meta = resource_state(buffer.?);
    const allocation = child_object(meta.bound_memory, c.VK_OBJECT_TYPE_DEVICE_MEMORY, pool.parent_id) orelse {
        state.command_state = .Invalid;
        return;
    };
    const command_bytes: u64 = if (indexed) 20 else 16;
    const bytes: u64 = if (count == 0) 0 else @as(u64, count - 1) * stride + command_bytes;
    if (meta.buffer_usage & c.VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT == 0 or offset > meta.buffer_size or bytes > meta.buffer_size - offset) {
        state.command_state = .Invalid;
        return;
    }
    const writer = dynamic_graphics.draw_indirect(record.id, buffer.?.id, offset, count, stride, indexed) catch {
        state.command_state = .Invalid;
        return;
    };
    if (!command_acknowledged(&writer, if (indexed) 109 else 108)) return;
    retain_draw_descriptors(pool.parent_id, state, command_profile(record), 0x1f);
    command_reference(state, pipeline);
    command_reference(state, buffer.?);
    command_reference(state, allocation);
    if (index) |record_index| command_reference(state, record_index);
}

// Owner imports image_transfer = venus_image_transfer_wire.zig and
// image_geometry = venus_image_transfer_native.zig, transfer2 = venus_transfer2_native.zig.
// Wrapper pointers are Vulkan valid-call borrowed inputs; counts bounded before slicing.
const image_transfer_record_t = struct { record: *c.venus_object_t, state: *resource_state_t, parent_id: u64 };
fn image_transfer_record(command_buffer: c.VkCommandBuffer) ?image_transfer_record_t {
    if (lost != c.RingOk or command_buffer == null) return null;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return null;
    const state = resource_state(record);
    if (state.command_state != .Recording) return null;
    if (!outside_render_pass(state)) return null;
    const pool = command_pool_for(record) orelse return null;
    return .{ .record = record, .state = state, .parent_id = pool.parent_id };
}
const image_transfer_resource_t = struct { record: *c.venus_object_t, allocation: *c.venus_object_t, state: *resource_state_t };
fn image_transfer_resource(context: image_transfer_record_t, handle: u64, kind: u32, usage: u32) ?image_transfer_resource_t {
    const record = child_object(handle, kind, context.parent_id) orelse {
        context.state.command_state = .Invalid;
        return null;
    };
    const state = resource_state(record);
    const actual_usage = if (kind == c.VK_OBJECT_TYPE_IMAGE) state.image_usage else state.buffer_usage;
    const allocation = child_object(state.bound_memory, c.VK_OBJECT_TYPE_DEVICE_MEMORY, context.parent_id) orelse {
        context.state.command_state = .Invalid;
        return null;
    };
    if (actual_usage & usage != usage) {
        context.state.command_state = .Invalid;
        return null;
    }
    return .{ .record = record, .allocation = allocation, .state = state };
}
fn image_geometry_metadata(state: *const resource_state_t) image_geometry.image_t {
    return .{ .format = state.image_format, .image_type = state.image_type, .extent = state.image_extent, .levels = state.image_levels, .layers = state.image_layers };
}
fn image_transfer_retain(context: image_transfer_record_t, resource: image_transfer_resource_t) void {
    command_reference(context.state, resource.record);
    command_reference(context.state, resource.allocation);
}
fn image_transfer_fail(command_buffer: c.VkCommandBuffer) void {
    if (command_buffer == null) return;
    const record = object(@intFromPtr(command_buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
    if (resource_state(record).command_state == .Recording) resource_state(record).command_state = .Invalid;
}
fn transfers_memory_overlap(source: image_transfer_resource_t, target: image_transfer_resource_t, source_offset: u64, source_size: u64, target_offset: u64, target_size: u64) bool {
    if (source.state.bound_memory != target.state.bound_memory) return false;
    const first = std.math.add(u64, source.state.memory_offset, source_offset) catch return true;
    const second = std.math.add(u64, target.state.memory_offset, target_offset) catch return true;
    const first_end = std.math.add(u64, first, source_size) catch return true;
    const second_end = std.math.add(u64, second, target_size) catch return true;
    return first < second_end and second < first_end;
}
fn copy_regions_overlap(source: c.VkImageCopy, target: c.VkImageCopy) bool {
    const a = source.srcSubresource;
    const b = target.dstSubresource;
    if (a.mipLevel != b.mipLevel or a.aspectMask & b.aspectMask == 0 or a.baseArrayLayer >= b.baseArrayLayer + b.layerCount or b.baseArrayLayer >= a.baseArrayLayer + a.layerCount) return false;
    const first = [_]i32{ source.srcOffset.x, source.srcOffset.y, source.srcOffset.z };
    const second = [_]i32{ target.dstOffset.x, target.dstOffset.y, target.dstOffset.z };
    const sizes_a = [_]u32{ source.extent.width, source.extent.height, source.extent.depth };
    const sizes_b = [_]u32{ target.extent.width, target.extent.height, target.extent.depth };
    for (first, second, sizes_a, sizes_b) |x, y, width, height| if (@as(i64, x) >= @as(i64, y) + height or @as(i64, y) >= @as(i64, x) + width) return false;
    return true;
}
/// Validate bound same-device resources, exact block/mip/layer geometry and disjoint copy
/// memory before sending opcode113. Retains both images/allocations only after host ACK.
fn copy_image(command_buffer: c.VkCommandBuffer, source: c.VkImage, source_layout: u32, target: c.VkImage, target_layout: u32, count: u32, regions: [*c]const c.VkImageCopy) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const context = image_transfer_record(command_buffer) orelse return;
    if (source == null or target == null or count == 0 or count > 64 or regions == null) {
        context.state.command_state = .Invalid;
        return;
    }
    const src = image_transfer_resource(context, @intFromPtr(source.?), c.VK_OBJECT_TYPE_IMAGE, c.VK_IMAGE_USAGE_TRANSFER_SRC_BIT) orelse return;
    const dst = image_transfer_resource(context, @intFromPtr(target.?), c.VK_OBJECT_TYPE_IMAGE, c.VK_IMAGE_USAGE_TRANSFER_DST_BIT) orelse return;
    if (src.state.image_samples != dst.state.image_samples) {
        context.state.command_state = .Invalid;
        return;
    }
    for (regions[0..count]) |region| {
        const first = image_geometry.region(image_geometry_metadata(src.state), @bitCast(region.srcSubresource), @bitCast(region.srcOffset), @bitCast(region.extent)) catch {
            context.state.command_state = .Invalid;
            return;
        };
        const second = image_geometry.region(image_geometry_metadata(dst.state), @bitCast(region.dstSubresource), @bitCast(region.dstOffset), @bitCast(region.extent)) catch {
            context.state.command_state = .Invalid;
            return;
        };
        if (first.bytes != second.bytes or first.width != second.width or first.height != second.height or (region.srcSubresource.aspectMask != 1 and src.state.image_format != dst.state.image_format)) {
            context.state.command_state = .Invalid;
            return;
        }
    }
    if (src.record.id == dst.record.id) {
        for (regions[0..count]) |first| for (regions[0..count]) |second| if (copy_regions_overlap(first, second)) {
            context.state.command_state = .Invalid;
            return;
        };
    } else if (transfers_memory_overlap(src, dst, 0, src.state.requirements.size, 0, dst.state.requirements.size)) {
        context.state.command_state = .Invalid;
        return;
    }
    const writer = image_transfer.copy_image(context.record.id, src.record.id, source_layout, dst.record.id, target_layout, @ptrCast(regions[0..count])) catch {
        context.state.command_state = .Invalid;
        return;
    };
    if (!command_acknowledged(&writer, 113)) return;
    image_transfer_retain(context, src);
    image_transfer_retain(context, dst);
}
/// Upload/readback shared validation follows compressed row/slice and 3D/array semantics;
/// actual buffer and image allocation overlap is rejected. Native data borrowed for call.
fn buffer_image_transfer(command_buffer: c.VkCommandBuffer, buffer: c.VkBuffer, image: c.VkImage, layout: u32, count: u32, regions: [*c]const c.VkBufferImageCopy, upload: bool) void {
    lock_icd();
    defer unlock_icd();
    const context = image_transfer_record(command_buffer) orelse return;
    if (buffer == null or image == null or count == 0 or count > 64 or regions == null) {
        context.state.command_state = .Invalid;
        return;
    }
    const buf = image_transfer_resource(context, @intFromPtr(buffer.?), c.VK_OBJECT_TYPE_BUFFER, if (upload) c.VK_BUFFER_USAGE_TRANSFER_SRC_BIT else c.VK_BUFFER_USAGE_TRANSFER_DST_BIT) orelse return;
    const img = image_transfer_resource(context, @intFromPtr(image.?), c.VK_OBJECT_TYPE_IMAGE, if (upload) c.VK_IMAGE_USAGE_TRANSFER_DST_BIT else c.VK_IMAGE_USAGE_TRANSFER_SRC_BIT) orelse return;
    if (img.state.image_samples != 1) {
        context.state.command_state = .Invalid;
        return;
    }
    for (regions[0..count],0..) |region,index| {
        const span = image_geometry.buffer_span(image_geometry_metadata(img.state), @bitCast(region)) catch {
            context.state.command_state = .Invalid;
            return;
        };
        if (region.bufferOffset > buf.state.buffer_size or span > buf.state.buffer_size - region.bufferOffset or transfers_memory_overlap(buf, img, region.bufferOffset, span, 0, img.state.requirements.size)) {
            context.state.command_state = .Invalid;
            return;
        }
        if (!upload) for (regions[0..index]) |prior| {
            const prior_span = image_geometry.buffer_span(image_geometry_metadata(img.state),@bitCast(prior)) catch unreachable;
            if (region.bufferOffset < prior.bufferOffset + prior_span and prior.bufferOffset < region.bufferOffset + span) { context.state.command_state = .Invalid; return; }
        };
    }
    const writer = if (upload) image_transfer.copy_buffer_to_image(context.record.id, buf.record.id, img.record.id, layout, @ptrCast(regions[0..count])) catch {
        context.state.command_state = .Invalid;
        return;
    } else image_transfer.copy_image_to_buffer(context.record.id, img.record.id, buf.record.id, layout, @ptrCast(regions[0..count])) catch {
        context.state.command_state = .Invalid;
        return;
    };
    if (!command_acknowledged(&writer, if (upload) 115 else 116)) return;
    image_transfer_retain(context, buf);
    image_transfer_retain(context, img);
}
fn copy_buffer_to_image(command_buffer: c.VkCommandBuffer, buffer: c.VkBuffer, image: c.VkImage, layout: u32, count: u32, regions: [*c]const c.VkBufferImageCopy) callconv(.C) void {
    buffer_image_transfer(command_buffer, buffer, image, layout, count, regions, true);
}
// Replaces the existing narrow copy_image_to_buffer wrapper.
fn copy_image_to_buffer(command_buffer: c.VkCommandBuffer, image: c.VkImage, layout: u32, buffer: c.VkBuffer, count: u32, regions: [*c]const c.VkBufferImageCopy) callconv(.C) void {
    buffer_image_transfer(command_buffer, buffer, image, layout, count, regions, false);
}
fn blit_box(image: *const resource_state_t, layers: c.VkImageSubresourceLayers, offsets: [2]c.VkOffset3D) bool {
    const first = [_]i32{ offsets[0].x, offsets[0].y, offsets[0].z };
    const last = [_]i32{ offsets[1].x, offsets[1].y, offsets[1].z };
    var starts: [3]i32 = undefined;
    var sizes: [3]u32 = undefined;
    for (first, last, 0..) |a, b, index| {
        starts[index] = @min(a, b);
        sizes[index] = @intCast(@abs(@as(i64, b) - a));
    }
    const shape = image_geometry.region(image_geometry_metadata(image), @bitCast(layers), .{ .x = starts[0], .y = starts[1], .z = starts[2] }, .{ .width = sizes[0], .height = sizes[1], .depth = sizes[2] }) catch return false;
    return shape.width == 1 and shape.height == 1;
}
/// Blit uncompressed bound images with exact endpoint/mip/layer bounds and sample1.
/// Owner must additionally check actual source/destination format BLIT features and
/// linear-filter feature before advertising/reaching this entrypoint.
fn blit_image(command_buffer: c.VkCommandBuffer, source: c.VkImage, source_layout: u32, target: c.VkImage, target_layout: u32, count: u32, regions: [*c]const c.VkImageBlit, filter: u32) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const context = image_transfer_record(command_buffer) orelse return;
    if (source == null or target == null or count == 0 or count > 64 or regions == null or filter > 1) {
        context.state.command_state = .Invalid;
        return;
    }
    const src = image_transfer_resource(context, @intFromPtr(source.?), c.VK_OBJECT_TYPE_IMAGE, c.VK_IMAGE_USAGE_TRANSFER_SRC_BIT) orelse return;
    const dst = image_transfer_resource(context, @intFromPtr(target.?), c.VK_OBJECT_TYPE_IMAGE, c.VK_IMAGE_USAGE_TRANSFER_DST_BIT) orelse return;
    if (src.state.image_samples != 1 or dst.state.image_samples != 1 or transfers_memory_overlap(src, dst, 0, src.state.requirements.size, 0, dst.state.requirements.size)) {
        context.state.command_state = .Invalid;
        return;
    }
    if (!blit_format_supported(context.parent_id, src.state, dst.state, filter)) { context.state.command_state = .Invalid; return; }
    for (regions[0..count]) |region| {
        image_geometry.blit_compatible(src.state.image_format,dst.state.image_format,region.srcSubresource.aspectMask,filter) catch { context.state.command_state = .Invalid; return; };
        if (!blit_box(src.state, region.srcSubresource, region.srcOffsets) or !blit_box(dst.state, region.dstSubresource, region.dstOffsets) or (region.srcSubresource.aspectMask != 1 and (src.state.image_format != dst.state.image_format or filter != 0))) {
            context.state.command_state = .Invalid;
            return;
        }
    }
    const writer = image_transfer.blit_image(context.record.id, src.record.id, source_layout, dst.record.id, target_layout, @ptrCast(regions[0..count]), filter) catch {
        context.state.command_state = .Invalid;
        return;
    };
    if (!command_acknowledged(&writer, 114)) return;
    image_transfer_retain(context, src);
    image_transfer_retain(context, dst);
}
/// Resolve same-format multisample color source to sample1 destination, with exact
/// mip/layer/geometry and nonoverlapping allocation checks. Both owners survive GPU work.
fn cmd_resolve_image(command_buffer: c.VkCommandBuffer, source: c.VkImage, source_layout: u32, target: c.VkImage, target_layout: u32, count: u32, regions: [*c]const c.VkImageResolve) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    const context = image_transfer_record(command_buffer) orelse return;
    if (source == null or target == null or count == 0 or count > 64 or regions == null) {
        context.state.command_state = .Invalid;
        return;
    }
    const src = image_transfer_resource(context, @intFromPtr(source.?), c.VK_OBJECT_TYPE_IMAGE, c.VK_IMAGE_USAGE_TRANSFER_SRC_BIT) orelse return;
    const dst = image_transfer_resource(context, @intFromPtr(target.?), c.VK_OBJECT_TYPE_IMAGE, c.VK_IMAGE_USAGE_TRANSFER_DST_BIT) orelse return;
    if (src.state.image_samples <= 1 or dst.state.image_samples != 1 or src.state.image_format != dst.state.image_format or transfers_memory_overlap(src, dst, 0, src.state.requirements.size, 0, dst.state.requirements.size)) {
        context.state.command_state = .Invalid;
        return;
    }
    for (regions[0..count]) |region| {
        _ = image_geometry.region(image_geometry_metadata(src.state), @bitCast(region.srcSubresource), @bitCast(region.srcOffset), @bitCast(region.extent)) catch {
            context.state.command_state = .Invalid;
            return;
        };
        _ = image_geometry.region(image_geometry_metadata(dst.state), @bitCast(region.dstSubresource), @bitCast(region.dstOffset), @bitCast(region.extent)) catch {
            context.state.command_state = .Invalid;
            return;
        };
    }
    const writer = image_transfer.resolve_image(context.record.id, src.record.id, source_layout, dst.record.id, target_layout, @ptrCast(regions[0..count])) catch {
        context.state.command_state = .Invalid;
        return;
    };
    if (!command_acknowledged(&writer, 122)) return;
    image_transfer_retain(context, src);
    image_transfer_retain(context, dst);
}
fn clear_image(command_buffer: c.VkCommandBuffer, image: c.VkImage, layout: u32, value: ?*const anyopaque, count: u32, ranges: [*c]const c.VkImageSubresourceRange, depth: bool) void {
    lock_icd();
    defer unlock_icd();
    const context = image_transfer_record(command_buffer) orelse return;
    if (image == null or value == null or count == 0 or count > 64 or ranges == null) {
        context.state.command_state = .Invalid;
        return;
    }
    const img = image_transfer_resource(context, @intFromPtr(image.?), c.VK_OBJECT_TYPE_IMAGE, c.VK_IMAGE_USAGE_TRANSFER_DST_BIT) orelse return;
    const aspects = image_aspects(img.state.image_format);
    if ((depth and aspects == 1) or (!depth and aspects != 1)) {
        context.state.command_state = .Invalid;
        return;
    }
    if (!depth) {
        const shape = image_geometry.block(img.state.image_format,1) catch { context.state.command_state = .Invalid; return; };
        if (shape.width != 1 or shape.height != 1) { context.state.command_state = .Invalid; return; }
    }
    for (ranges[0..count]) |range| if (!image_range_valid(img.state, range)) {
        context.state.command_state = .Invalid;
        return;
    };
    var color: [4]u32 = undefined;
    var depth_value: c.VkClearDepthStencilValue = undefined;
    if (depth) @memcpy(std.mem.asBytes(&depth_value), @as([*]const u8, @ptrCast(value.?))[0..@sizeOf(@TypeOf(depth_value))]) else @memcpy(std.mem.asBytes(&color), @as([*]const u8, @ptrCast(value.?))[0..16]);
    const writer = if (depth) image_transfer.clear_depth_stencil(context.record.id, img.record.id, layout, @bitCast(depth_value), @ptrCast(ranges[0..count])) catch {
        context.state.command_state = .Invalid;
        return;
    } else image_transfer.clear_color(context.record.id, img.record.id, layout, color, @ptrCast(ranges[0..count])) catch {
        context.state.command_state = .Invalid;
        return;
    };
    if (!command_acknowledged(&writer, if (depth) 120 else 119)) return;
    image_transfer_retain(context, img);
}
fn clear_color_image(command_buffer: c.VkCommandBuffer, image: c.VkImage, layout: u32, color: [*c]const c.VkClearColorValue, count: u32, ranges: [*c]const c.VkImageSubresourceRange) callconv(.C) void {
    clear_image(command_buffer, image, layout, color, count, ranges, false);
}
fn clear_depth_stencil_image(command_buffer: c.VkCommandBuffer, image: c.VkImage, layout: u32, value: [*c]const c.VkClearDepthStencilValue, count: u32, ranges: [*c]const c.VkImageSubresourceRange) callconv(.C) void {
    clear_image(command_buffer, image, layout, value, count, ranges, true);
}

// Six core/KHR CopyCommands2 entrypoints normalize into owned classic regions.
// Existing wrappers provide identical ownership, geometry and host ACK retention.
fn copy_buffer2(command_buffer: c.VkCommandBuffer, info: [*c]const c.VkCopyBufferInfo2) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (info == null) {
        image_transfer_fail(command_buffer);
        return;
    }
    const normalized = transfer2.copy_buffer(@ptrCast(info)) catch {
        image_transfer_fail(command_buffer);
        return;
    };
    copy_buffer(command_buffer, @ptrCast(normalized.source), @ptrCast(normalized.target), normalized.count, @ptrCast(&normalized.regions));
}
fn copy_image2(command_buffer: c.VkCommandBuffer, info: [*c]const c.VkCopyImageInfo2) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (info == null) {
        image_transfer_fail(command_buffer);
        return;
    }
    const normalized = transfer2.copy_image(@ptrCast(info)) catch {
        image_transfer_fail(command_buffer);
        return;
    };
    copy_image(command_buffer, @ptrCast(normalized.source), normalized.source_layout, @ptrCast(normalized.target), normalized.target_layout, normalized.count, @ptrCast(&normalized.regions));
}
fn blit_image2(command_buffer: c.VkCommandBuffer, info: [*c]const c.VkBlitImageInfo2) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (info == null) {
        image_transfer_fail(command_buffer);
        return;
    }
    const normalized = transfer2.blit_image(@ptrCast(info)) catch {
        image_transfer_fail(command_buffer);
        return;
    };
    blit_image(command_buffer, @ptrCast(normalized.source), normalized.source_layout, @ptrCast(normalized.target), normalized.target_layout, normalized.count, @ptrCast(&normalized.regions), normalized.filter);
}
fn copy_buffer_to_image2(command_buffer: c.VkCommandBuffer, info: [*c]const c.VkCopyBufferToImageInfo2) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (info == null) {
        image_transfer_fail(command_buffer);
        return;
    }
    const normalized = transfer2.copy_buffer_to_image(@ptrCast(info)) catch {
        image_transfer_fail(command_buffer);
        return;
    };
    copy_buffer_to_image(command_buffer, @ptrCast(normalized.buffer), @ptrCast(normalized.image), normalized.layout, normalized.count, @ptrCast(&normalized.regions));
}
fn copy_image_to_buffer2(command_buffer: c.VkCommandBuffer, info: [*c]const c.VkCopyImageToBufferInfo2) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (info == null) {
        image_transfer_fail(command_buffer);
        return;
    }
    const normalized = transfer2.copy_image_to_buffer(@ptrCast(info)) catch {
        image_transfer_fail(command_buffer);
        return;
    };
    copy_image_to_buffer(command_buffer, @ptrCast(normalized.image), normalized.layout, @ptrCast(normalized.buffer), normalized.count, @ptrCast(&normalized.regions));
}
fn resolve_image2(command_buffer: c.VkCommandBuffer, info: [*c]const c.VkResolveImageInfo2) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (info == null) {
        image_transfer_fail(command_buffer);
        return;
    }
    const normalized = transfer2.resolve_image(@ptrCast(info)) catch {
        image_transfer_fail(command_buffer);
        return;
    };
    cmd_resolve_image(command_buffer, @ptrCast(normalized.source), normalized.source_layout, @ptrCast(normalized.target), normalized.target_layout, normalized.count, @ptrCast(&normalized.regions));
}


fn descriptor_pool_for(record: *const c.venus_object_t) ?*c.venus_object_t {
    for (&slots) |*slot| if (slot.id != 0 and slot.id == record.parent_id and slot.kind == c.VK_OBJECT_TYPE_DESCRIPTOR_POOL) return slot;
    return null;
}
fn descriptor_set_for(handle: c.VkDescriptorSet, device_id: u64) ?*c.venus_object_t {
    var found: [*c]c.venus_object_t = null;
    if (c.venus_objects_lookup(&objects, if (handle) |value| @intFromPtr(value) else 0, c.VK_OBJECT_TYPE_DESCRIPTOR_SET, 0, &found) != c.RingOk) return null;
    const record: *c.venus_object_t = @ptrCast(found);
    const pool = descriptor_pool_for(record) orelse return null;
    return if (pool.parent_id == device_id) record else null;
}
fn descriptor_set_idle(record: *const c.venus_object_t) bool {
    return resource_state(record).inflight_count == 0;
}
fn retire_descriptor_set(record: *c.venus_object_t, pool: *c.venus_object_t) void {
    const state = resource_state(record);
    const profile = profiles.get_profile(&profile_registry.sets, state.profile_index).?;
    const owner = resource_state(pool);
    for (profile.layout.bindings[0..profile.layout.binding_count]) |binding| owner.descriptor_used[binding.descriptor_type] -= binding.descriptor_count;
    owner.descriptor_live_sets -= 1;
    const index = resource_index(record);
    const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
    for (&resource_states) |*command_state| if (command_state.buffer_references[index / 64] & bit != 0) {
        command_state.command_state = .Invalid;
        command_state.buffer_references = [_]u64{0} ** 8;
    };
    std.debug.assert(profiles.release_slot(&profile_registry.sets, state.profile_index));
    state.* = .{};
    std.debug.assert(c.venus_objects_release(&objects, record.handle, c.VK_OBJECT_TYPE_DESCRIPTOR_SET, 0) == c.RingOk);
}
fn descriptor_pool_idle(pool: *const c.venus_object_t) bool {
    for (&slots) |*child| if (child.id != 0 and child.parent_id == pool.id and !descriptor_set_idle(child)) return false;
    return true;
}
fn retire_pool_sets(pool: *c.venus_object_t) void {
    for (&slots) |*child| if (child.id != 0 and child.parent_id == pool.id) retire_descriptor_set(child, pool);
}


fn outside_render_pass(state: *resource_state_t) bool {
    if (state.command_profile_index == 0 or graphics_recording(state).active_format == 0) return true;
    state.command_state = .Invalid;
    return false;
}
fn graphics_recording(state: *const resource_state_t) *graphics_state.recording_t {
    std.debug.assert(state.command_profile_index != 0);
    return &graphics_recordings[state.command_profile_index - 1];
}
/// Check real physical format features for the actual image tilings; no synthetic flags.
fn blit_format_supported(parent_id: u64, source: *const resource_state_t, target: *const resource_state_t, filter: u32) bool {
    const parent = device_by_id(parent_id) orelse return false;
    var physical: c.VkPhysicalDevice = null;
    for (slots) |slot| if (slot.id == parent.parent_id and slot.kind == c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) { physical = @ptrFromInt(slot.handle); break; };
    if (physical == null) return false;
    var src = std.mem.zeroes(c.VkFormatProperties);
    var dst = std.mem.zeroes(c.VkFormatProperties);
    format_properties(physical,source.image_format,&src);
    if (lost != c.RingOk) return false;
    format_properties(physical,target.image_format,&dst);
    if (lost != c.RingOk) return false;
    const src_flags = if (source.image_tiling == c.VK_IMAGE_TILING_LINEAR) src.linearTilingFeatures else src.optimalTilingFeatures;
    const dst_flags = if (target.image_tiling == c.VK_IMAGE_TILING_LINEAR) dst.linearTilingFeatures else dst.optimalTilingFeatures;
    return src_flags & c.VK_FORMAT_FEATURE_BLIT_SRC_BIT != 0 and dst_flags & c.VK_FORMAT_FEATURE_BLIT_DST_BIT != 0 and (filter == 0 or src_flags & c.VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT != 0);
}

/// Read one requested Vulkan12 descriptor-indexing feature by its named native offset.
fn descriptor_feature(parent: *const c.venus_object_t, comptime name: []const u8) bool {
    const index = (@offsetOf(c.VkPhysicalDeviceVulkan12Features,name)-@offsetOf(c.VkPhysicalDeviceVulkan12Features,"samplerMirrorClampToEdge"))/4;
    return device_feature(parent,c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,index);
}
fn update_after_bind_supported(parent: *const c.venus_object_t, kind: u32) bool {
    return switch(kind) {
        0,1,2 => descriptor_feature(parent,"descriptorBindingSampledImageUpdateAfterBind"),
        3 => descriptor_feature(parent,"descriptorBindingStorageImageUpdateAfterBind"),
        4 => descriptor_feature(parent,"descriptorBindingUniformTexelBufferUpdateAfterBind"),
        5 => descriptor_feature(parent,"descriptorBindingStorageTexelBufferUpdateAfterBind"),
        6 => descriptor_feature(parent,"descriptorBindingUniformBufferUpdateAfterBind"),
        7 => descriptor_feature(parent,"descriptorBindingStorageBufferUpdateAfterBind"),
        else => false,
    };
}

// Owner imports template_native = @import("venus_descriptor_template_native.zig");
// Guest-only registry nodes must not be sent to renderer destruction. Invoke
// retire_device_templates(parent_id) before device registry retirement; scrub ledger
// on receiver-retired abandonment/reset alongside objects registry. All calls lock ICD.
const template_owner_t = struct {
    id: u64 = 0,
    parent_id: u64 = 0,
    definition: template_native.snapshot_t = .{},
    layout: profiles.descriptor_layout_t = .{},
};
var template_owners: [32]template_owner_t = [_]template_owner_t{.{}} ** 32;
fn template_owner(id: u64) ?*template_owner_t {
    for (&template_owners) |*entry| if (entry.id == id and id != 0) return entry;
    return null;
}
/// Copy and own a descriptor-set template; pNext unsupported, allocator unused.
/// Output null on failure; synchronous owned entries and layout snapshot remain until destroy.
/// Guest-only object identity has no renderer counterpart or GPU lifetime.
fn create_descriptor_update_template(device: c.VkDevice, info: [*c]const c.VkDescriptorUpdateTemplateCreateInfo, allocator: [*c]const c.VkAllocationCallbacks, output: [*c]c.VkDescriptorUpdateTemplate) callconv(.C) c_int {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (info.*.descriptorSetLayout == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const layout_record = child_object(@intFromPtr(info.*.descriptorSetLayout.?), c.VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, parent.id) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const definition = template_native.snapshot(@ptrCast(info)) catch |err| return if (err == error.Limit) c.VK_ERROR_OUT_OF_HOST_MEMORY else c.VK_ERROR_INITIALIZATION_FAILED;
    const layout = profiles.get_profile(&profile_registry.descriptor_layouts, resource_state(layout_record).profile_index) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    for (definition.entries[0..definition.entry_count]) |native_entry| {
        var compatible = false;
        for (layout.bindings[0..layout.binding_count]) |binding| if (binding.binding == native_entry.dstBinding) {
            compatible = binding.descriptor_type == native_entry.descriptorType and native_entry.dstArrayElement <= binding.descriptor_count and native_entry.descriptorCount <= binding.descriptor_count - native_entry.dstArrayElement;
            break;
        };
        if (!compatible) return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    var available: ?*template_owner_t = null;
    for (&template_owners) |*entry| if (entry.id == 0) {
        available = entry;
        break;
    };
    const entry = available orelse return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, parent.id, 0, &record) != c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    entry.* = .{ .id = record.*.id, .parent_id = parent.id, .definition = definition, .layout = layout.* };
    output.* = @ptrFromInt(record.*.handle);
    return c.VK_SUCCESS;
}
/// Retire guest-only template storage synchronously; no pending GPU use exists because
/// updates expand/copy all entries before returning. Invalid tokens are ignored.
fn destroy_descriptor_update_template(device: c.VkDevice, handle: c.VkDescriptorUpdateTemplate, allocator: [*c]const c.VkAllocationCallbacks) callconv(.C) void {
    _ = allocator;
    lock_icd();
    defer unlock_icd();
    if (device == null or handle == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(@intFromPtr(handle.?), c.VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, parent.id) orelse return;
    const entry = template_owner(record.id) orelse return;
    std.debug.assert(c.venus_objects_release(&objects, record.handle, c.VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, 0) == c.RingOk);
    entry.* = .{};
}
/// Expand exact caller offsets/strides into owned local writes and delegate real host updates.
/// Data borrowed only for synchronous call; output arrays do not retain pData or template.
/// Owner implements mixed updater nullDescriptor/immutable sampler/usage/retention policy.
fn update_descriptor_set_with_template(device: c.VkDevice, set: c.VkDescriptorSet, handle: c.VkDescriptorUpdateTemplate, data: ?*const anyopaque) callconv(.C) void {
    lock_icd();
    defer unlock_icd();
    if (lost != c.RingOk or device == null or handle == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(@intFromPtr(handle.?), c.VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, parent.id) orelse return;
    const entry = template_owner(record.id) orelse return;
    const target = descriptor_set_for(set, parent.id) orelse return;
    const profile = profiles.get_profile(&profile_registry.sets, resource_state(target).profile_index) orelse return;
    if (!std.meta.eql(entry.layout, profile.layout)) return;
    var expanded: template_native.expanded_t = .{};
    template_native.expand(&entry.definition, @ptrCast(set), data, &expanded) catch return;
    update_descriptor_sets(device, @intCast(expanded.write_count), @ptrCast(&expanded.writes), 0, null);
}
fn retire_device_templates(parent_id: u64) void {
    for (&template_owners) |*entry| if (entry.id != 0 and entry.parent_id == parent_id) {
        var record: [*c]c.venus_object_t = null;
        if (c.venus_objects_lookup_id(&objects, entry.id, c.VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, &record) == c.RingOk)
            std.debug.assert(c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, 0) == c.RingOk);
        entry.* = .{};
    };
}

/// Bounded owned native pipeline chain with private library tokens translated to host IDs.
const pipeline_chain_inputs_t = struct {
    rendering: c.VkPipelineRenderingCreateInfo = undefined,
    flags: c.VkPipelineCreateFlags2CreateInfoKHR = undefined,
    library: c.VkPipelineLibraryCreateInfoKHR = undefined,
    libraries: [64]c.VkPipeline = undefined,
    first: ?*const anyopaque = null,
};
/// Copy the recognized chain in original order, retaining no caller addresses after return.
/// Library resources remain mutex-protected through synchronous native creation.
fn resolve_pipeline_chain(parent_id: u64, next: ?*const anyopaque, output: *pipeline_chain_inputs_t) !u32 {
    const chain = try pipeline_helpers.collect_chain(next,true);
    var headers: [3]*c.VkBaseOutStructure = undefined;
    for (chain.tags[0..chain.count],chain.addresses[0..chain.count],0..) |tag,address,index| {
        switch(tag) {
            c.VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO => {
                output.rendering = @as(*const c.VkPipelineRenderingCreateInfo,@ptrFromInt(address)).*;
                headers[index] = @ptrCast(&output.rendering);
            },
            c.VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO => {
                output.flags = @as(*const c.VkPipelineCreateFlags2CreateInfoKHR,@ptrFromInt(address)).*;
                headers[index] = @ptrCast(&output.flags);
            },
            c.VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR => {
                output.library = @as(*const c.VkPipelineLibraryCreateInfoKHR,@ptrFromInt(address)).*;
                if(output.library.libraryCount!=0) for(output.library.pLibraries[0..output.library.libraryCount],0..) |handle,element| {
                    const library = child_object(@intFromPtr(handle.?),c.VK_OBJECT_TYPE_PIPELINE,parent_id) orelse return error.Invalid;
                    if(resource_state(library).pipeline_bind_point!=0) return error.Invalid;
                    output.libraries[element] = @ptrFromInt(library.id);
                };
                output.library.pLibraries = if(output.library.libraryCount==0) null else &output.libraries;
                headers[index] = @ptrCast(&output.library);
            },
            else => unreachable,
        }
    }
    for(headers[0..chain.count],0..) |header,index| header.pNext=if(index+1<chain.count) headers[index+1] else null;
    output.first=if(chain.count==0) null else headers[0];
    return chain.library_count;
}

/// Retain one validated descriptor's actual resources and allocation owners.
fn retain_descriptor(parent_id: u64,state: *resource_state_t,value: *const profiles.descriptor_t) void {
    if(value.sampler!=0) command_reference(state,child_object(value.sampler,c.VK_OBJECT_TYPE_SAMPLER,parent_id).?);
    if(value.image_view!=0) {
        const view=child_object(value.image_view,c.VK_OBJECT_TYPE_IMAGE_VIEW,parent_id).?;
        const image=child_object(resource_state(view).view_image,c.VK_OBJECT_TYPE_IMAGE,parent_id).?;
        command_reference(state,view);command_reference(state,image);
        command_reference(state,child_object(resource_state(image).bound_memory,c.VK_OBJECT_TYPE_DEVICE_MEMORY,parent_id).?);
    }
    var buffer_token=value.buffer;
    if(value.texel_view!=0) {const view=child_object(value.texel_view,c.VK_OBJECT_TYPE_BUFFER_VIEW,parent_id).?;command_reference(state,view);buffer_token=resource_state(view).view_image;}
    if(buffer_token!=0) {const buffer=child_object(buffer_token,c.VK_OBJECT_TYPE_BUFFER,parent_id).?;command_reference(state,buffer);command_reference(state,child_object(resource_state(buffer).bound_memory,c.VK_OBJECT_TYPE_DEVICE_MEMORY,parent_id).?);}
}
/// Validate then retain newly updated descriptors from sets consumed by recorded
/// draws/dispatches. Old resource references remain retained; no premature release.
fn refresh_command_descriptors(record: *c.venus_object_t) bool {
    const pool=command_pool_for(record) orelse return false;
    const parent=device_by_id(pool.parent_id) orelse return false;
    const state=resource_state(record);
    for(slots,0..) |slot,index| {
        if(state.descriptor_uses[index/64] & (@as(u64,1)<<@as(u6,@intCast(index%64)))==0) continue;
        const set=descriptor_set_for(@ptrFromInt(slot.handle),parent.id) orelse return false;
        const profile=profiles.get_profile(&profile_registry.sets,resource_state(set).profile_index) orelse return false;
        for(profile.descriptors[0..profile.descriptor_count]) |value| if(!descriptor_resource_valid(parent,&value)) return false;
    }
    for(slots,0..) |slot,index| {
        if(state.descriptor_uses[index/64] & (@as(u64,1)<<@as(u6,@intCast(index%64)))==0) continue;
        const set=descriptor_set_for(@ptrFromInt(slot.handle),parent.id).?;
        const profile=profiles.get_profile(&profile_registry.sets,resource_state(set).profile_index).?;
        for(profile.descriptors[0..profile.descriptor_count]) |value| retain_descriptor(parent.id,state,&value);
    }
    return true;
}
/// Find declared binding flags in immutable set ownership; absent binding invalid.
fn descriptor_binding_flags(profile: *const profiles.descriptor_set_t,binding: u32) ?u32 {
    for(profile.layout.bindings[0..profile.layout.binding_count]) |definition| if(definition.binding==binding) return definition.binding_flags;
    return null;
}

/// Attach resources introduced by an acknowledged mutable descriptor update to
/// every already-pending submission using that set. Old references stay owned.
/// No GPU completion is inferred; exact ticket retirement releases each new use.
fn retain_pending_descriptor_update(parent: *const c.venus_object_t,set: *const c.venus_object_t,profile: *const profiles.descriptor_set_t) void {
    var references: resource_state_t = .{};
    for(profile.descriptors[0..profile.descriptor_count]) |value| {
        if(descriptor_resource_valid(parent,&value)) retain_descriptor(parent.id,&references,&value);
    }
    const set_slot=resource_index(set);
    const set_bit=@as(u64,1)<<@as(u6,@intCast(set_slot%64));
    for(&submission_tickets) |*ticket| {
        if(ticket.queue==0 or ticket.references[set_slot/64] & set_bit==0) continue;
        for(&slots,0..) |*record,index| {
            const bit=@as(u64,1)<<@as(u6,@intCast(index%64));
            if(references.buffer_references[index/64] & bit==0) continue;
            if(!include_reference(ticket,record)) resource_states[index].inflight_count+=1;
        }
        retain_submission_mapping_spans(parent.id,ticket.references);
    }
}

/// [in] live borrowed physical and immutable core external buffer query.
/// [out] caller-owned initialized output; headers/unknown chain bytes preserved.
/// No external allocation import/export path is implemented, so all support bits
/// are zero. Invalid input preserves output; serialized, no allocation/retention.
fn external_buffer_properties(physical: c.VkPhysicalDevice, info: [*c]const c.VkPhysicalDeviceExternalBufferInfo, output: [*c]c.VkExternalBufferProperties) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (physical == null or info == null or output == null or @intFromPtr(info) % @alignOf(@TypeOf(info.*)) != 0 or @intFromPtr(output) % @alignOf(c.VkExternalBufferProperties) != 0 or info.*.sType != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO or output.*.sType != c.VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES) return;
    _ = object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) orelse return;
    _ = query2_chain(output.*.pNext) catch return;
    output.*.externalMemoryProperties = std.mem.zeroes(c.VkExternalMemoryProperties);
}
/// [in] live borrowed physical/core fence query; [out] initialized caller storage.
/// Unsupported external fence capabilities are zero; headers/unknown nodes stay
/// intact. Invalid input preserves output. Serialized, no heap or retained pointer.
fn external_fence_properties(physical: c.VkPhysicalDevice, info: [*c]const c.VkPhysicalDeviceExternalFenceInfo, output: [*c]c.VkExternalFenceProperties) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (physical == null or info == null or output == null or @intFromPtr(info) % @alignOf(@TypeOf(info.*)) != 0 or @intFromPtr(output) % @alignOf(c.VkExternalFenceProperties) != 0 or info.*.sType != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO or output.*.sType != c.VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES) return;
    _ = object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) orelse return;
    _ = query2_chain(output.*.pNext) catch return;
    output.*.exportFromImportedHandleTypes = 0;
    output.*.compatibleHandleTypes = 0;
    output.*.externalFenceFeatures = 0;
}
/// [in] live borrowed physical/core semaphore query; [out] initialized storage.
/// No external semaphore handles supported: zero features/import/export bits.
/// Headers/unknown nodes preserved. Serialized, no allocation/retained pointer.
fn external_semaphore_properties(physical: c.VkPhysicalDevice, info: [*c]const c.VkPhysicalDeviceExternalSemaphoreInfo, output: [*c]c.VkExternalSemaphoreProperties) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (physical == null or info == null or output == null or @intFromPtr(info) % @alignOf(@TypeOf(info.*)) != 0 or @intFromPtr(output) % @alignOf(c.VkExternalSemaphoreProperties) != 0 or info.*.sType != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO or output.*.sType != c.VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES) return;
    _ = object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) orelse return;
    _ = query2_chain(output.*.pNext) catch return;
    output.*.exportFromImportedHandleTypes = 0;
    output.*.compatibleHandleTypes = 0;
    output.*.externalSemaphoreFeatures = 0;
}
/// [in] live borrowed physical; [in,out] count/optional array caller owned.
/// No installed guest ICD tools: count zero, success, array bytes untouched.
/// Invalid arguments return INITIALIZATION_FAILED. Serialized, no allocation.
fn tool_properties(physical: c.VkPhysicalDevice, count: [*c]u32, output: [*c]c.VkPhysicalDeviceToolProperties) callconv(.C) c_int {
    _ = output;
    lock_icd(); defer unlock_icd();
    if (physical == null or count == null or object(@intFromPtr(physical.?), c.VK_OBJECT_TYPE_PHYSICAL_DEVICE) == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    count.* = 0;
    return c.VK_SUCCESS;
}

/// [in] live borrowed device/native layout definition; [out] initialized caller
/// support storage and optional variable-count node. Shares creation preflight,
/// then queries actual host support. Unsupported local quotas/features yield false;
/// headers/unknown nodes preserved. Host failure preserves output and poisons binding.
/// Serialized; no allocations or retained pointers.
fn descriptor_layout_support(device: c.VkDevice, info: [*c]const c.VkDescriptorSetLayoutCreateInfo, output: [*c]c.VkDescriptorSetLayoutSupport) callconv(.C) void {
    lock_icd(); defer unlock_icd();
    if (device == null or info == null or output == null or @intFromPtr(output) % @alignOf(c.VkDescriptorSetLayoutSupport) != 0 or output.*.sType != c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    _ = query2_chain(output.*.pNext) catch return;
    var variable: ?*c.VkDescriptorSetVariableDescriptorCountLayoutSupport = null;
    var current = output.*.pNext;
    while (current) |pointer| {
        const header: *c.VkBaseOutStructure = @ptrCast(@alignCast(pointer));
        if (header.sType == c.VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT) {
            if (variable != null or @intFromPtr(pointer) % @alignOf(c.VkDescriptorSetVariableDescriptorCountLayoutSupport) != 0) return;
            variable = @ptrCast(@alignCast(pointer));
        }
        current = @ptrCast(header.pNext);
    }
    const prepared = preflight_descriptor_layout(parent, info) catch {
        output.*.supported = 0;
        if (variable) |value| value.maxVariableDescriptorCount = 0;
        return;
    };
    const packet = extra_wire.descriptor_layout_support(parent.id, prepared.writer.bytes[0..prepared.writer.used]) catch return;
    const reply = transact(packet.bytes[0..packet.used]) orelse return;
    const supported = extra_wire.decode_descriptor_layout_support(reply) catch { _ = failure(c.RingCorrupt); return; };
    output.*.supported = @intFromBool(supported);
    // Variable descriptor count is not advertised or admitted by implemented policy.
    if (variable) |value| value.maxVariableDescriptorCount = 0;
}

// Test-only fixtures.

extern fn venus_icd_native_fixture() c_int;
test "native ABI lifecycle churn routing and concurrent transport serialization" {
    try std.testing.expectEqual(@as(c_int, 0), venus_icd_native_fixture());
}
test "bounded fixed and array replies reject every truncation and invalid tags" {
    var bytes: [64]u8 = undefined;
    @memset(&bytes, 0);
    std.mem.writeInt(u32, bytes[0..4], 4, .little);
    std.mem.writeInt(u64, bytes[4..12], 1, .little);
    for (0..24) |length| {
        var reader = reader_t{ .bytes = bytes[0..length] };
        try std.testing.expectError(error.Bounds, fixed_value(c.VkFormatProperties, &reader, 4));
    }
    for ([_]usize{ 0, 4 }) |offset| {
        bytes[offset] = 9;
        var reader = reader_t{ .bytes = &bytes };
        try std.testing.expectError(error.Value, fixed_value(c.VkFormatProperties, &reader, 4));
        bytes[offset] = if (offset == 0) 4 else 1;
    }
    @memset(&bytes, 0);
    std.mem.writeInt(u32, bytes[0..4], 7, .little);
    std.mem.writeInt(u64, bytes[4..12], 1, .little);
    std.mem.writeInt(u32, bytes[12..16], 1, .little);
    std.mem.writeInt(u64, bytes[16..24], 1, .little);
    var count: u32 = 99;
    var queue: c.VkQueueFamilyProperties = std.mem.zeroes(c.VkQueueFamilyProperties);
    for (0..48) |length| {
        var reader = reader_t{ .bytes = bytes[0..length] };
        try std.testing.expectError(
            error.Bounds,
            array_values(c.VkQueueFamilyProperties, &reader, 7, 1, true, &count, &queue),
        );
        try std.testing.expectEqual(@as(u32, 99), count);
    }
    for ([_]usize{ 0, 4, 12, 16 }) |offset| {
        const before = bytes[offset];
        bytes[offset] = 99;
        var reader = reader_t{ .bytes = &bytes };
        try std.testing.expectError(
            error.Value,
            array_values(c.VkQueueFamilyProperties, &reader, 7, 1, true, &count, &queue),
        );
        bytes[offset] = before;
    }
    var reader = reader_t{ .bytes = &bytes };
    try std.testing.expectError(
        error.Value,
        array_values(c.VkQueueFamilyProperties, &reader, 7, 0, true, &count, &queue),
    );
    @memset(&bytes, 0);
    std.mem.writeInt(u32, bytes[0..4], 5, .little);
    std.mem.writeInt(u64, bytes[8..16], 1, .little);
    var image: c.VkImageFormatProperties = std.mem.zeroes(c.VkImageFormatProperties);
    for (0..48) |length| {
        reader = .{ .bytes = bytes[0..length] };
        try std.testing.expectError(error.Bounds, image_value(&reader, &image));
    }
    bytes[0] = 4;
    reader = .{ .bytes = &bytes };
    try std.testing.expectError(error.Value, image_value(&reader, &image));
    bytes[0] = 5;
    bytes[4] = 1;
    reader = .{ .bytes = &bytes };
    try std.testing.expectError(error.Value, image_value(&reader, &image));
    bytes[4] = 0;
    bytes[8] = 0;
    reader = .{ .bytes = &bytes };
    try std.testing.expectError(error.Value, image_value(&reader, &image));
    bytes[8] = 1;
    std.mem.writeInt(i32, bytes[4..8], -11, .little);
    reader = .{ .bytes = &bytes };
    try std.testing.expectEqual(@as(i32, -11), try image_value(&reader, &image));
}
test "bounded device input validation and identity reply truncations" {
    var priorities = [_]f32{ 0.25, 0.75 } ** 8;
    var queues = [_]c.VkDeviceQueueCreateInfo{.{
        .sType = c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = null,
        .flags = 0,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priorities,
    }} ** 16;
    var feature = std.mem.zeroes(c.VkPhysicalDeviceFeatures);
    var info = c.VkDeviceCreateInfo{
        .sType = c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = null,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queues,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = null,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = null,
        .pEnabledFeatures = &feature,
    };
    _ = try device_native.preflight(&info);
    feature.robustBufferAccess = 2;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    feature.robustBufferAccess = 1;
    try std.testing.expectError(error.FeatureNotPresent, device_native.preflight(&info));
    feature.robustBufferAccess = 0;
    info.enabledLayerCount = 1;
    try std.testing.expectError(error.LayerNotPresent, device_native.preflight(&info));
    info.enabledLayerCount = 0;
    info.enabledExtensionCount = 1;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    info.enabledExtensionCount = 0;
    var link = c.VkBaseInStructure{
        .sType = c.VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
        .pNext = null,
    };
    info.pNext = &link;
    _ = try device_native.preflight(&info);
    link.pNext = &link;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    link.pNext = null;
    link.sType = c.VK_STRUCTURE_TYPE_APPLICATION_INFO;
    _ = try device_native.preflight(&info); // Experimental unknown-header skip, not legal application use.
    info.pNext = null;
    info.queueCreateInfoCount = 2;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    for (&queues, 0..) |*queue, index| queue.queueFamilyIndex = @intCast(index);
    info.queueCreateInfoCount = 16;
    for (&queues) |*queue| queue.queueCount = 16;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    for (&queues) |*queue| queue.queueCount = 4;
    _ = try device_native.preflight(&info);
    info.queueCreateInfoCount = 1;
    for ([_]f32{ -1, 2, std.math.inf(f32), std.math.nan(f32) }) |priority| {
        priorities[0] = priority;
        try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    }
    priorities[0] = 0.5;
    queues[0].pQueuePriorities = null;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    queues[0].pQueuePriorities = &priorities;
    queues[0].queueCount = 0;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    queues[0].queueCount = 17;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    queues[0].queueCount = 1;
    queues[0].flags = 1;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    queues[0].flags = 0;
    queues[0].pNext = &link;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    queues[0].pNext = null;
    queues[0].sType = 0;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    queues[0].sType = c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    info.sType = 0;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    info.sType = c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.flags = 1;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    info.flags = 0;
    info.queueCreateInfoCount = 0;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    info.queueCreateInfoCount = 17;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = null;
    try std.testing.expectError(error.Invalid, device_native.preflight(&info));
    var bytes: [24]u8 = undefined;
    std.mem.writeInt(u32, bytes[0..4], 11, .little);
    std.mem.writeInt(i32, bytes[4..8], 0, .little);
    std.mem.writeInt(u64, bytes[8..16], 1, .little);
    std.mem.writeInt(u64, bytes[16..24], 2, .little);
    try std.testing.expectEqual(@as(i32, 0), try identity_reply(&bytes, 11, 2, true));
    for (0..24) |length| {
        if (identity_reply(bytes[0..length], 11, 2, true)) |_| {
            return error.AcceptedTruncation;
        } else |_| {}
    }
    for ([_]usize{ 0, 8, 16 }) |offset| {
        bytes[offset] ^= 1;
        try std.testing.expectError(error.Value, identity_reply(&bytes, 11, 2, true));
        bytes[offset] ^= 1;
    }
    std.mem.writeInt(u32, bytes[0..4], 155, .little);
    std.mem.writeInt(u64, bytes[4..12], 1, .little);
    std.mem.writeInt(u64, bytes[12..20], 2, .little);
    _ = try identity_reply(bytes[0..20], 155, 2, false);
    for (0..20) |length| {
        if (identity_reply(bytes[0..length], 155, 2, false)) |_| {
            return error.AcceptedTruncation;
        } else |_| {}
    }
}
test "fence result replies reject truncation malformed tags and unexpected positive statuses" {
    defer lost = c.RingOk;
    var bytes: [8]u8 = undefined;
    std.mem.writeInt(u32, bytes[0..4], 38, .little);
    std.mem.writeInt(i32, bytes[4..8], 0, .little);
    for (0..8) |length| {
        try std.testing.expectEqual(
            @as(c_int, c.VK_ERROR_DEVICE_LOST),
            @call(.never_inline, result_reply, .{ bytes[0..length], @as(u32, 38), @as(i32, 1) }),
        );
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), lost);
        lost = c.RingOk;
    }
    try std.testing.expectEqual(
        @as(c_int, 0),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
    std.mem.writeInt(i32, bytes[4..8], c.VK_NOT_READY, .little);
    try std.testing.expectEqual(
        @as(c_int, c.VK_NOT_READY),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
    std.mem.writeInt(i32, bytes[4..8], c.VK_ERROR_OUT_OF_HOST_MEMORY, .little);
    try std.testing.expectEqual(
        @as(c_int, c.VK_ERROR_OUT_OF_HOST_MEMORY),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
    std.mem.writeInt(i32, bytes[4..8], c.VK_ERROR_DEVICE_LOST, .little);
    try std.testing.expectEqual(
        @as(c_int, c.VK_ERROR_DEVICE_LOST),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
    try std.testing.expectEqual(@as(c_int, c.RingClosed), lost);
    lost = c.RingOk;
    std.mem.writeInt(i32, bytes[4..8], c.VK_INCOMPLETE, .little);
    try std.testing.expectEqual(
        @as(c_int, c.VK_ERROR_DEVICE_LOST),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
    std.mem.writeInt(i32, bytes[4..8], 0, .little);
    std.mem.writeInt(u32, bytes[0..4], 39, .little);
    try std.testing.expectEqual(
        @as(c_int, c.VK_ERROR_DEVICE_LOST),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
}

test "command buffer batch replies validate every truncation result count and identity" {
    const Ids = [_]u64{ 7, 9 };
    var writer = writer_t{};
    writer.put(u32, 88);
    writer.put(i32, 0);
    writer.put(u64, Ids.len);
    for (Ids) |id| writer.put(u64, id);
    for (0..writer.used) |length| {
        try std.testing.expectError(
            error.Bounds,
            @call(
                .never_inline,
                command_buffers_reply,
                .{ writer.bytes[0..length], &Ids },
            ),
        );
    }
    try std.testing.expectEqual(
        @as(c_int, c.VK_SUCCESS),
        try command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    writer.bytes[0] ^= 1;
    try std.testing.expectError(
        error.Value,
        command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    writer.bytes[0] ^= 1;
    std.mem.writeInt(u64, writer.bytes[8..16], 3, .little);
    try std.testing.expectError(
        error.Value,
        command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    std.mem.writeInt(u64, writer.bytes[8..16], Ids.len, .little);
    std.mem.writeInt(i32, writer.bytes[4..8], c.VK_NOT_READY, .little);
    try std.testing.expectError(
        error.Value,
        command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    std.mem.writeInt(i32, writer.bytes[4..8], 0, .little);
    std.mem.writeInt(u64, writer.bytes[24..32], 10, .little);
    try std.testing.expectError(
        error.Value,
        command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    std.mem.writeInt(u64, writer.bytes[16..24], 0, .little);
    std.mem.writeInt(u64, writer.bytes[24..32], 0, .little);
    try std.testing.expectError(
        error.Value,
        command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    std.mem.writeInt(i32, writer.bytes[4..8], c.VK_ERROR_OUT_OF_HOST_MEMORY, .little);
    try std.testing.expectEqual(
        @as(
            c_int,
            c.VK_ERROR_OUT_OF_HOST_MEMORY,
        ),
        try command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
}

test "pending references prevent buffer pool and semaphore destruction before GPU retirement" {
    const fixture_t = struct {
        fn exchange(
            _: ?*anyopaque,
            _: [*c]const c.venus_request_t,
            _: ?*const anyopaque,
            _: usize,
            _: [*c]c.venus_request_t,
            _: ?*anyopaque,
            _: usize,
        ) callconv(.C) c_int {
            return c.RingInvalid;
        }
    };
    var sentinel: u8 = 0;
    try std.testing.expectEqual(
        @as(c_int, c.RingOk),
        venus_icd_bind(fixture_t.exchange, &sentinel),
    );
    defer venus_icd_abandon();
    var device: [*c]c.venus_object_t = null;
    var buffer: [*c]c.venus_object_t = null;
    var recording: [*c]c.venus_object_t = null;
    var pool: [*c]c.venus_object_t = null;
    var semaphore: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_DEVICE,
        0,
        1,
        &device,
    ));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_BUFFER,
        device.*.id,
        0,
        &buffer,
    ));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        device.*.id,
        0,
        &pool,
    ));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
        pool.*.id,
        1,
        &recording,
    ));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_SEMAPHORE,
        device.*.id,
        0,
        &semaphore,
    ));
    resource_state(semaphore).inflight_count = 1;
    const index = resource_index(buffer);
    resource_state(recording).command_state = .Pending;
    resource_state(recording).buffer_references[index / 64] =
        @as(u64, 1) << @as(u6, @intCast(index % 64));
    destroy_buffer(@ptrFromInt(device.*.handle), @ptrFromInt(buffer.*.handle), null);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
    destroy_command_pool(@ptrFromInt(device.*.handle), @ptrFromInt(pool.*.handle), null);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
    destroy_semaphore(@ptrFromInt(device.*.handle), @ptrFromInt(semaphore.*.handle), null);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
    try std.testing.expectEqual(@as(usize, 5), objects.live_count);
    try std.testing.expectEqual(@as(u32, 1), resource_state(semaphore).inflight_count);
    try std.testing.expectEqual(command_state_t.Pending, resource_state(recording).command_state);
}

test "inline update staging captures input and scrubs success transport and malformed reply paths" {
    const fixture_t = struct {
        mode: u32,
        captured: bool = false,
        fn exchange(
            context: ?*anyopaque,
            request: [*c]const c.venus_request_t,
            input: ?*const anyopaque,
            length: usize,
            response: [*c]c.venus_request_t,
            output: ?*anyopaque,
            capacity: usize,
        ) callconv(.C) c_int {
            const fixture: *@This() = @ptrCast(@alignCast(context.?));
            response.* = std.mem.zeroes(c.venus_request_t);
            response.*.kind = request.*.kind;
            response.*.direction = 1;
            if (request.*.kind == c.RequestSubmit) {
                const bytes = @as([*]const u8, @ptrCast(input.?))[0..length];
                if (length != 92 or !std.mem.eql(u8, bytes[84..92], &.{ 1, 2, 3, 4, 5, 6, 7, 8 }))
                    return c.RingCorrupt;
                fixture.captured = true;
                if (fixture.mode == 1) return c.RingClosed;
                response.*.argument_zero = 1;
            } else if (request.*.kind == c.RequestReply) {
                const bytes = @as([*]u8, @ptrCast(output.?))[0..capacity];
                @memset(bytes, 0);
                std.mem.writeInt(u32, bytes[0..4], if (fixture.mode == 2) 118 else 117, .little);
                response.*.payload_bytes = @intCast(capacity);
            } else if (request.*.kind != c.RequestPoll) return c.RingInvalid;
            return c.RingOk;
        }
    };
    for (0..3) |mode| {
        var fixture = fixture_t{ .mode = @intCast(mode) };
        try std.testing.expectEqual(
            @as(c_int, c.RingOk),
            venus_icd_bind(fixture_t.exchange, &fixture),
        );
        defer venus_icd_abandon();
        var device: [*c]c.venus_object_t = null;
        var pool: [*c]c.venus_object_t = null;
        var recording: [*c]c.venus_object_t = null;
        var buffer: [*c]c.venus_object_t = null;
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
            &objects,
            c.VK_OBJECT_TYPE_DEVICE,
            0,
            1,
            &device,
        ));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
            &objects,
            c.VK_OBJECT_TYPE_COMMAND_POOL,
            device.*.id,
            0,
            &pool,
        ));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
            &objects,
            c.VK_OBJECT_TYPE_COMMAND_BUFFER,
            pool.*.id,
            1,
            &recording,
        ));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
            &objects,
            c.VK_OBJECT_TYPE_BUFFER,
            device.*.id,
            0,
            &buffer,
        ));
        resource_state(recording).command_state = .Recording;
        resource_state(buffer).* = .{ .buffer_size = 8, .buffer_usage = 2, .bound_memory = 1 };
        const data = [_]u8{ 1, 2, 3, 4, 5, 6, 7, 8 };
        update_buffer(@ptrFromInt(recording.*.handle), @ptrFromInt(buffer.*.handle), 0, 8, &data);
        try std.testing.expect(fixture.captured);
        for (update_encoded[0..56]) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
        for (tx[0..92]) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
        const index = resource_index(buffer);
        const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
        try std.testing.expectEqual(
            if (mode == 0) bit else @as(u64, 0),
            resource_state(recording).buffer_references[index / 64],
        );
        try std.testing.expectEqual(mode == 0, lost == c.RingOk);
    }
}

test "constructor identities allow null only after negative native results" {
    var bytes = [_]u8{0} ** 24;
    std.mem.writeInt(u32, bytes[0..4], 40, .little);
    std.mem.writeInt(u32, bytes[4..8], @bitCast(@as(i32, c.VK_ERROR_OUT_OF_DEVICE_MEMORY)), .little);
    std.mem.writeInt(u64, bytes[8..16], 1, .little);
    try std.testing.expectEqual(@as(i32, c.VK_ERROR_OUT_OF_DEVICE_MEMORY), try identity_reply(
        &bytes,
        40,
        123,
        true,
    ));
    for (0..bytes.len) |length| try std.testing.expectError(error.Bounds, identity_reply(
        bytes[0..length],
        40,
        123,
        true,
    ));
    std.mem.writeInt(u64, bytes[16..24], 123, .little);
    try std.testing.expectEqual(@as(i32, c.VK_ERROR_OUT_OF_DEVICE_MEMORY), try identity_reply(
        &bytes,
        40,
        123,
        true,
    ));
    std.mem.writeInt(u64, bytes[16..24], 124, .little);
    try std.testing.expectError(error.Value, identity_reply(&bytes, 40, 123, true));
    std.mem.writeInt(u64, bytes[16..24], 0, .little);
    std.mem.writeInt(u32, bytes[4..8], 0, .little);
    try std.testing.expectError(error.Value, identity_reply(&bytes, 40, 123, true));
    std.mem.writeInt(u32, bytes[4..8], 1, .little);
    try std.testing.expectError(error.Value, identity_reply(&bytes, 40, 123, true));
}

test "fence completion retires a queue prefix while simultaneous references remain pending" {
    const fixture_t = struct {
        fn exchange(_: ?*anyopaque, _: [*c]const c.venus_request_t, _: ?*const anyopaque, _: usize, _: [*c]c.venus_request_t, _: ?*anyopaque, _: usize) callconv(.C) c_int {
            return c.RingInvalid;
        }
    };
    var context: u8 = 0;
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &context));
    defer venus_icd_abandon();
    var records: [7][*c]c.venus_object_t = [_][*c]c.venus_object_t{null} ** 7;
    const Kinds = [_]u32{ c.VK_OBJECT_TYPE_DEVICE, c.VK_OBJECT_TYPE_QUEUE, c.VK_OBJECT_TYPE_QUEUE, c.VK_OBJECT_TYPE_COMMAND_BUFFER, c.VK_OBJECT_TYPE_COMMAND_BUFFER, c.VK_OBJECT_TYPE_SEMAPHORE, c.VK_OBJECT_TYPE_FENCE };
    for (Kinds, 0..) |kind, index| {
        const dispatchable: u32 = if (index < 5) 1 else 0;
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
            &objects,
            kind,
            if (index == 0) 0 else records[0].*.id,
            dispatchable,
            &records[index],
        ));
    }
    const first = resource_state(records[3]);
    const second = resource_state(records[4]);
    const semaphore = resource_state(records[5]);
    const fence = resource_state(records[6]);
    first.* = .{ .inflight_count = 3, .command_state = .Pending, .command_flags = 4 };
    second.* = .{ .inflight_count = 1, .command_state = .Pending, .command_flags = 1 };
    semaphore.inflight_count = 2;
    fence.inflight_count = 1;
    submission_tickets[0] = .{ .queue = records[1].*.handle, .sequence = 1 };
    submission_tickets[1] = .{ .queue = records[1].*.handle, .sequence = 2, .fence = records[6].*.handle };
    submission_tickets[2] = .{ .queue = records[2].*.handle, .sequence = 3 };
    for (submission_tickets[0..3], 0..) |_, index| _ = include_reference(&submission_tickets[index], records[3]);
    _ = include_reference(&submission_tickets[0], records[5]);
    _ = include_reference(&submission_tickets[1], records[6]);
    _ = include_reference(&submission_tickets[2], records[4]);
    _ = include_reference(&submission_tickets[2], records[5]);
    retire_fence(records[6].*.handle);
    try std.testing.expectEqual(@as(u32, 1), first.inflight_count);
    try std.testing.expectEqual(command_state_t.Pending, first.command_state);
    try std.testing.expectEqual(@as(u32, 1), semaphore.inflight_count);
    try std.testing.expectEqual(@as(u32, 0), fence.inflight_count);
    try std.testing.expectEqual(@as(u64, 0), submission_tickets[0].queue);
    try std.testing.expectEqual(@as(u64, 0), submission_tickets[1].queue);
    retire_queue(records[2].*.handle);
    try std.testing.expectEqual(command_state_t.Executable, first.command_state);
    try std.testing.expectEqual(command_state_t.Invalid, second.command_state);
    try std.testing.expectEqual(@as(u32, 0), semaphore.inflight_count);
    resource_state(records[1]).id = records[1].*.id;
    submission_sequence = std.math.maxInt(u64);
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_OUT_OF_HOST_MEMORY), queue_submit(
        @ptrFromInt(records[1].*.handle),
        0,
        null,
        null,
    ));
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
}

test "image barrier references retain pending images and invalidate recorded commands after destruction" {
    const fixture_t = struct {
        command_id: u32 = 0,
        submissions: usize = 0,
        fn exchange(context: ?*anyopaque, request: [*c]const c.venus_request_t, input: ?*const anyopaque, length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
            const fixture: *@This() = @ptrCast(@alignCast(context.?));
            response.* = std.mem.zeroes(c.venus_request_t);
            response.*.kind = request.*.kind;
            response.*.direction = 1;
            if (request.*.kind == c.RequestSubmit) {
                const bytes = @as([*]const u8, @ptrCast(input.?))[0..length];
                fixture.command_id = std.mem.readInt(u32, bytes[36..40], .little);
                fixture.submissions += 1;
                response.*.argument_zero = fixture.submissions;
            } else if (request.*.kind == c.RequestReply) {
                const bytes = @as([*]u8, @ptrCast(output.?))[0..capacity];
                @memset(bytes, 0);
                std.mem.writeInt(u32, bytes[0..4], fixture.command_id, .little);
                response.*.payload_bytes = @intCast(capacity);
            } else if (request.*.kind != c.RequestPoll) return c.RingInvalid;
            return c.RingOk;
        }
    };
    var fixture = fixture_t{};
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &fixture));
    defer venus_icd_abandon();
    var device: [*c]c.venus_object_t = null;
    var pool: [*c]c.venus_object_t = null;
    var recording: [*c]c.venus_object_t = null;
    var image: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE, 0, 1, &device));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_COMMAND_POOL, device.*.id, 0, &pool));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_COMMAND_BUFFER, pool.*.id, 1, &recording));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_IMAGE, device.*.id, 0, &image));
    resource_state(image).* = .{ .id = image.*.id, .bound_memory = 999, .image_levels = 1, .image_layers = 1, .image_format = 37 };
    resource_state(recording).command_state = .Recording;
    var barrier = c.VkImageMemoryBarrier{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .newLayout = c.VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = 0xffffffff, .dstQueueFamilyIndex = 0xffffffff, .image = @ptrFromInt(image.*.handle), .subresourceRange = .{ .aspectMask = 1, .levelCount = 1, .layerCount = 1 } };
    pipeline_barrier(@ptrFromInt(recording.*.handle), 1, 0x1000, 0, 0, null, 0, null, 1, &barrier);
    try std.testing.expectEqual(@as(usize, 1), fixture.submissions);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
    const index = resource_index(image);
    const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
    try std.testing.expect(resource_state(recording).buffer_references[index / 64] & bit != 0);
    resource_state(recording).command_state = .Pending;
    destroy_image(@ptrFromInt(device.*.handle), @ptrFromInt(image.*.handle), null);
    try std.testing.expectEqual(@as(usize, 1), fixture.submissions);
    resource_state(recording).command_state = .Executable;
    destroy_image(@ptrFromInt(device.*.handle), @ptrFromInt(image.*.handle), null);
    try std.testing.expectEqual(@as(usize, 2), fixture.submissions);
    try std.testing.expectEqual(command_state_t.Invalid, resource_state(recording).command_state);
    try std.testing.expectEqual(@as(usize, 3), objects.live_count);
    try std.testing.expectEqual([_]u64{0} ** 8, resource_state(recording).buffer_references);
    // An oversized combined packet rejects before dereferencing any array input.
    resource_state(recording).command_state = .Recording;
    pipeline_barrier(@ptrFromInt(recording.*.handle), 1, 1, 0, 64, @ptrFromInt(8), 64, @ptrFromInt(8), 64, @ptrFromInt(8));
    try std.testing.expectEqual(command_state_t.Invalid, resource_state(recording).command_state);
    try std.testing.expectEqual(@as(usize, 2), fixture.submissions);
}

test "descriptor batch replies reject truncation unexpected status count tag and identity" {
    const ids = [_]u64{ 42, 43 };
    var bytes: [32]u8 = undefined;
    std.mem.writeInt(u32, bytes[0..4], 77, .little);
    std.mem.writeInt(i32, bytes[4..8], 0, .little);
    std.mem.writeInt(u64, bytes[8..16], 2, .little);
    std.mem.writeInt(u64, bytes[16..24], 42, .little);
    std.mem.writeInt(u64, bytes[24..32], 43, .little);
    for (0..bytes.len) |length| try std.testing.expectError(error.Bounds, @call(.never_inline, descriptor_sets_reply, .{ bytes[0..length], &ids }));
    try std.testing.expectEqual(@as(c_int, 0), try @call(.never_inline, descriptor_sets_reply, .{ &bytes, &ids }));
    std.mem.writeInt(u32, bytes[0..4], 78, .little);
    try std.testing.expectError(error.Value, @call(.never_inline, descriptor_sets_reply, .{ &bytes, &ids }));
    std.mem.writeInt(u32, bytes[0..4], 77, .little);
    std.mem.writeInt(i32, bytes[4..8], 1, .little);
    try std.testing.expectError(error.Value, @call(.never_inline, descriptor_sets_reply, .{ &bytes, &ids }));
    std.mem.writeInt(i32, bytes[4..8], 0, .little);
    std.mem.writeInt(u64, bytes[8..16], 1, .little);
    try std.testing.expectError(error.Value, @call(.never_inline, descriptor_sets_reply, .{ &bytes, &ids }));
    std.mem.writeInt(u64, bytes[8..16], 2, .little);
    std.mem.writeInt(u64, bytes[24..32], 0, .little);
    try std.testing.expectError(error.Value, @call(.never_inline, descriptor_sets_reply, .{ &bytes, &ids }));
    std.mem.writeInt(i32, bytes[4..8], c.VK_ERROR_OUT_OF_DEVICE_MEMORY, .little);
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_OUT_OF_DEVICE_MEMORY), try @call(.never_inline, descriptor_sets_reply, .{ &bytes, &ids }));
    std.mem.writeInt(u64, bytes[24..32], 44, .little);
    try std.testing.expectError(error.Value, @call(.never_inline, descriptor_sets_reply, .{ &bytes, &ids }));
}

test "pending descriptor sets protect pool ownership and exact retirement refunds references" {
    const fixture_t = struct {
        fn exchange(_: ?*anyopaque, _: [*c]const c.venus_request_t, _: ?*const anyopaque, _: usize, _: [*c]c.venus_request_t, _: ?*anyopaque, _: usize) callconv(.C) c_int {
            return c.RingInvalid;
        }
    };
    var sentinel: u8 = 0;
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &sentinel));
    defer venus_icd_abandon();
    var device: [*c]c.venus_object_t = null;
    var pool: [*c]c.venus_object_t = null;
    var set: [*c]c.venus_object_t = null;
    var recording: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE, 0, 1, &device));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DESCRIPTOR_POOL, device.*.id, 0, &pool));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DESCRIPTOR_SET, pool.*.id, 0, &set));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_COMMAND_BUFFER, device.*.id, 1, &recording));
    const layout = try profiles.normalize_bindings(&.{.{ .binding = 3, .descriptor_type = 7, .descriptor_count = 1, .stage_flags = 32 }});
    resource_state(set).profile_index = try profiles.reserve_slot(&profile_registry.sets, try profiles.create_set_profile(&layout));
    resource_state(set).inflight_count = 1;
    resource_state(pool).pool_flags = 1;
    resource_state(pool).descriptor_live_sets = 1;
    resource_state(pool).descriptor_used[7] = 1;
    const handles = [_]c.VkDescriptorSet{@ptrFromInt(set.*.handle)};
    const native_device: c.VkDevice = @ptrFromInt(device.*.handle);
    const native_pool: c.VkDescriptorPool = @ptrFromInt(pool.*.handle);
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), free_descriptor_sets(native_device, native_pool, 1, &handles));
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), reset_descriptor_pool(native_device, native_pool, 0));
    destroy_descriptor_pool(native_device, native_pool, null);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
    try std.testing.expectEqual(@as(usize, 4), objects.live_count);
    const index = resource_index(set);
    resource_state(recording).command_state = .Executable;
    resource_state(recording).buffer_references[index / 64] |= @as(u64, 1) << @as(u6, @intCast(index % 64));
    resource_state(set).inflight_count = 0;
    retire_descriptor_set(@ptrCast(set), @ptrCast(pool));
    try std.testing.expectEqual(@as(u32, 0), resource_state(pool).descriptor_live_sets);
    try std.testing.expectEqual(@as(u32, 0), resource_state(pool).descriptor_used[7]);
    try std.testing.expectEqual(command_state_t.Invalid, resource_state(recording).command_state);
    try std.testing.expect(descriptor_set_for(handles[0], device.*.id) == null);
    try std.testing.expect(descriptor_pool_for(@ptrCast(recording)) == null);
}

test "descriptor staging publishes only acknowledged metadata and scrubs every outcome" {
    const fixture_t = struct {
        mode: usize,
        captured: bool = false,
        fn exchange(context: ?*anyopaque, request: [*c]const c.venus_request_t, input: ?*const anyopaque, length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
            const fixture: *@This() = @ptrCast(@alignCast(context.?));
            response.* = std.mem.zeroes(c.venus_request_t);
            response.*.kind = request.*.kind;
            response.*.direction = 1;
            if (request.*.kind == c.RequestSubmit) {
                if (length < 40 or std.mem.readInt(u32, @as([*]const u8, @ptrCast(input.?))[36..40], .little) != 79) return c.RingCorrupt;
                fixture.captured = true;
                if (fixture.mode == 1) return c.RingClosed;
                response.*.argument_zero = 1;
            } else if (request.*.kind == c.RequestReply) {
                const bytes = @as([*]u8, @ptrCast(output.?))[0..capacity];
                @memset(bytes, 0);
                std.mem.writeInt(u32, bytes[0..4], if (fixture.mode == 2) 78 else 79, .little);
                response.*.payload_bytes = @intCast(capacity);
            } else if (request.*.kind != c.RequestPoll) return c.RingInvalid;
            return c.RingOk;
        }
    };
    for (0..11) |mode| {
        var fixture = fixture_t{ .mode = mode };
        try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &fixture));
        defer venus_icd_abandon();
        var device: [*c]c.venus_object_t = null;
        var pool: [*c]c.venus_object_t = null;
        var set: [*c]c.venus_object_t = null;
        var destination_set: [*c]c.venus_object_t = null;
        var buffer: [*c]c.venus_object_t = null;
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE, 0, 1, &device));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DESCRIPTOR_POOL, device.*.id, 0, &pool));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DESCRIPTOR_SET, pool.*.id, 0, &set));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DESCRIPTOR_SET, pool.*.id, 0, &destination_set));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_BUFFER, device.*.id, 0, &buffer));
        device_caches[0] = .{ .handle = device.*.handle, .descriptor_limits_ready = true, .descriptor_alignments = .{ 16, 16 }, .descriptor_ranges = .{ 256, 512 } };
        const kind: u32 = if (mode == 4 or mode == 5) 6 else 7;
        const layout = try profiles.normalize_bindings(&.{.{ .binding = 3, .descriptor_type = kind, .descriptor_count = 1, .stage_flags = 32 }});
        var profile = try profiles.create_set_profile(&layout);
        if (mode == 7 or mode == 8 or mode == 10) {
            profile.descriptors[0].buffer = if (mode == 10) 1 else buffer.*.handle;
            profile.descriptors[0].range = 64;
        }
        resource_state(set).profile_index = try profiles.reserve_slot(&profile_registry.sets, profile);
        resource_state(destination_set).profile_index = try profiles.reserve_slot(&profile_registry.sets, try profiles.create_set_profile(&layout));
        resource_state(buffer).* = .{ .buffer_size = 512, .buffer_usage = c.VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | c.VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .bound_memory = 1 };
        if (mode == 3 or mode == 7) resource_state(set).inflight_count = 1;
        if (mode == 8) resource_state(destination_set).inflight_count = 1;
        var observers: [2][*c]c.venus_object_t = undefined;
        for (&observers, 0..) |*observer, observer_index| {
            try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_COMMAND_BUFFER, device.*.id, 1, observer));
            const observed = resource_index(if (mode >= 7 and observer_index == 1) destination_set else set);
            resource_state(observer.*).command_state = if (observer_index == 0) .Recording else .Executable;
            resource_state(observer.*).buffer_references[observed / 64] |= @as(u64, 1) << @as(u6, @intCast(observed % 64));
        }
        const info: c.VkDescriptorBufferInfo = .{ .buffer = @ptrFromInt(buffer.*.handle), .offset = 16, .range = if (mode == 5) 257 else 128 };
        const initial: c.VkWriteDescriptorSet = .{ .sType = c.VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = @ptrFromInt(set.*.handle), .dstBinding = 3, .descriptorCount = 1, .descriptorType = kind, .pBufferInfo = &info };
        var writes = [_]c.VkWriteDescriptorSet{initial} ** 2;
        writes[1].dstBinding = 99;
        const copy: c.VkCopyDescriptorSet = .{ .sType = c.VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = @ptrFromInt(set.*.handle), .srcBinding = 3, .dstSet = @ptrFromInt(destination_set.*.handle), .dstBinding = 3, .descriptorCount = 1 };
        if (mode >= 7) update_descriptor_sets(@ptrFromInt(device.*.handle), 0, null, 1, &copy) else update_descriptor_sets(@ptrFromInt(device.*.handle), if (mode == 6) 2 else 1, &writes, 0, null);
        const expected_success = mode == 0 or mode == 4;
        const current = profiles.get_profile(&profile_registry.sets, resource_state(set).profile_index).?;
        try std.testing.expectEqual(if (expected_success) buffer.*.handle else profile.descriptors[0].buffer, current.descriptors[0].buffer);
        try std.testing.expectEqual(expected_success or mode == 1 or mode == 2 or mode == 7 or mode == 9 or mode == 10, fixture.captured);
        const copied = profiles.get_profile(&profile_registry.sets, resource_state(destination_set).profile_index).?;
        try std.testing.expectEqual(if (mode == 7) buffer.*.handle else @as(u64, 0), copied.descriptors[0].buffer);
        try std.testing.expectEqual(mode != 1 and mode != 2, lost == c.RingOk);
        for (observers, 0..) |observer, observer_index| {
            const invalidated = expected_success or ((mode == 7 or mode == 9 or mode == 10) and observer_index == 1);
            try std.testing.expectEqual(if (invalidated) command_state_t.Invalid else if (observer_index == 0) command_state_t.Recording else command_state_t.Executable, resource_state(observer).command_state);
        }
        for (std.mem.asBytes(&descriptor_update_snapshots)) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
        for (std.mem.asBytes(&descriptor_wire_buffers)) |byte| try std.testing.expectEqual(@as(u8, 0), byte);
    }
}
test "compute and graphics acknowledgments publish no references or state changes on failures" {
    const fixture_t = struct {
        mode: usize,
        opcode: u32 = 0,
        fn exchange(context: ?*anyopaque, request: [*c]const c.venus_request_t, input: ?*const anyopaque, length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
            const fixture: *@This() = @ptrCast(@alignCast(context.?));
            response.* = std.mem.zeroes(c.venus_request_t);
            response.*.kind = request.*.kind;
            response.*.direction = 1;
            if (request.*.kind == c.RequestSubmit) {
                if (length < 40) return c.RingInvalid;
                fixture.opcode = std.mem.readInt(u32, @as([*]const u8, @ptrCast(input.?))[36..40], .little);
                if (fixture.mode == 1) return c.RingClosed;
                response.*.argument_zero = 1;
            } else if (request.*.kind == c.RequestReply) {
                const bytes = @as([*]u8, @ptrCast(output.?))[0..capacity];
                @memset(bytes, 0);
                std.mem.writeInt(u32, bytes[0..4], fixture.opcode + @as(u32, if (fixture.mode == 2) 1 else 0), .little);
                response.*.payload_bytes = @intCast(capacity);
            } else if (request.*.kind != c.RequestPoll) return c.RingInvalid;
            return c.RingOk;
        }
    };
    for (0..9) |operation| for (0..3) |mode| {
        var fixture = fixture_t{ .mode = mode };
        try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &fixture));
        defer venus_icd_abandon();
        var device: [*c]c.venus_object_t = null;
        var pool: [*c]c.venus_object_t = null;
        var recording: [*c]c.venus_object_t = null;
        var pipeline: [*c]c.venus_object_t = null;
        var layout: [*c]c.venus_object_t = null;
        var descriptor_pool: [*c]c.venus_object_t = null;
        var set: [*c]c.venus_object_t = null;
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE, 0, 1, &device));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_COMMAND_POOL, device.*.id, 0, &pool));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_COMMAND_BUFFER, pool.*.id, 1, &recording));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_PIPELINE, device.*.id, 0, &pipeline));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_PIPELINE_LAYOUT, device.*.id, 0, &layout));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DESCRIPTOR_POOL, device.*.id, 0, &descriptor_pool));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DESCRIPTOR_SET, descriptor_pool.*.id, 0, &set));
        const empty = profiles.descriptor_layout_t{};
        const definition = if (operation >= 4) try profiles.normalize_pipeline(&.{}, &.{}) else try profiles.normalize_pipeline(&.{empty}, &.{.{ .stage_flags = 32, .offset = 0, .size = 4 }});
        resource_state(layout).profile_index = try profiles.reserve_slot(&profile_registry.pipeline_layouts, definition);
        resource_state(pipeline).profile_index = try profiles.reserve_slot(&profile_registry.pipelines, definition);
        resource_state(pipeline).pipeline_bind_point = if (operation >= 4) 0 else 1;
        resource_state(pipeline).render_format = 37;
        resource_state(set).profile_index = try profiles.reserve_slot(&profile_registry.sets, try profiles.create_set_profile(&empty));
        resource_state(recording).command_profile_index = try profiles.reserve_slot(&command_registry.commands, compute_state.command_profile_t{ .pipeline = pipeline.*.handle });
        resource_state(recording).command_state = .Recording;
        device_caches[0] = .{ .handle = device.*.handle, .descriptor_limits_ready = true, .compute_group_limits = .{ 8, 8, 8 } };
        if (operation >= 4) {
            device_caches[0].graphics_queue_ready = true;
            device_caches[0].graphics_queue_count = 1;
            device_caches[0].graphics_queue_flags[0] = c.VK_QUEUE_GRAPHICS_BIT;
        }
        var pass: [*c]c.venus_object_t = null;
        var framebuffer: [*c]c.venus_object_t = null;
        var view: [*c]c.venus_object_t = null;
        var image: [*c]c.venus_object_t = null;
        var allocation: [*c]c.venus_object_t = null;
        var copy_target: [*c]c.venus_object_t = null;
        var copy_memory: [*c]c.venus_object_t = null;
        if (operation >= 5) {
            try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_RENDER_PASS, device.*.id, 0, &pass));
            try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_FRAMEBUFFER, device.*.id, 0, &framebuffer));
            try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_IMAGE_VIEW, device.*.id, 0, &view));
            try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_IMAGE, device.*.id, 0, &image));
            try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE_MEMORY, device.*.id, 0, &allocation));
            resource_state(pass).* = .{ .render_format = 37, .render_final_layout = c.VK_IMAGE_LAYOUT_GENERAL };
            resource_state(framebuffer).* = .{ .render_format = 37, .framebuffer_view = view.*.handle, .framebuffer_extent = .{ 64, 64 } };
            resource_state(view).* = .{ .image_format = 37, .image_usage = c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | c.VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .view_image = image.*.handle, .view_type = c.VK_IMAGE_VIEW_TYPE_2D, .view_range = .{ .aspectMask = c.VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 } };
            resource_state(image).* = .{ .bound_memory = allocation.*.handle, .image_type = c.VK_IMAGE_TYPE_2D, .image_samples = 1, .image_format = 37, .image_usage = c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | c.VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .image_levels = 1, .image_layers = 1, .image_extent = .{ 64, 64, 1 } };
            if (operation == 6 or operation == 7) graphics_recording(resource_state(recording)).active_format = 37;
            if (operation == 7) {
                graphics_recording(resource_state(recording)).pipeline = pipeline.*.handle;
                graphics_recording(resource_state(recording)).pipeline_format = 37;
            }
        }
        if (operation == 8) {
            try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_BUFFER, device.*.id, 0, &copy_target));
            try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE_MEMORY, device.*.id, 0, &copy_memory));
            resource_state(copy_target).* = .{ .buffer_size = 16384, .buffer_usage = c.VK_BUFFER_USAGE_TRANSFER_DST_BIT, .bound_memory = copy_memory.*.handle };
        }
        const copy_region: c.VkBufferImageCopy = .{ .imageSubresource = .{ .aspectMask = c.VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 }, .imageExtent = .{ .width = 64, .height = 64, .depth = 1 } };
        const clear_value = std.mem.zeroes(c.VkClearValue);
        const begin_info: c.VkRenderPassBeginInfo = .{ .sType = c.VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = @ptrFromInt(if (pass == null) 1 else pass.*.handle), .framebuffer = @ptrFromInt(if (framebuffer == null) 1 else framebuffer.*.handle), .renderArea = .{ .extent = .{ .width = 64, .height = 64 } }, .clearValueCount = 1, .pClearValues = &clear_value };
        const command_handle: c.VkCommandBuffer = @ptrFromInt(recording.*.handle);
        const handles = [_]c.VkDescriptorSet{@ptrFromInt(set.*.handle)};
        const value: u32 = 42;
        if (mode == 0 and operation == 0) {
            resource_state(pipeline).pipeline_bind_point = 0;
            bind_pipeline(command_handle, 1, @ptrFromInt(pipeline.*.handle));
            try std.testing.expectEqual(command_state_t.Invalid, resource_state(recording).command_state);
            resource_state(pipeline).pipeline_bind_point = 1;
            resource_state(recording).command_state = .Recording;
        }
        if (mode == 0 and operation == 1) {
            const profile = profiles.get_profile(&profile_registry.sets, resource_state(set).profile_index).?;
            profile.layout = try profiles.normalize_bindings(&.{.{ .binding = 4, .descriptor_type = 7, .descriptor_count = 1, .stage_flags = 32 }});
            bind_descriptor_sets(command_handle, 1, @ptrFromInt(layout.*.handle), 0, 1, &handles, 0, null);
            try std.testing.expectEqual(command_state_t.Invalid, resource_state(recording).command_state);
            profile.layout = empty;
            resource_state(recording).command_state = .Recording;
        }
        if (mode == 0 and operation == 3) {
            const profile = profiles.get_profile(&profile_registry.pipelines, resource_state(pipeline).profile_index).?;
            profile.sets[0] = try profiles.normalize_bindings(&.{.{ .binding = 0, .descriptor_type = 7, .descriptor_count = 1, .stage_flags = 32 }});
            dispatch(command_handle, 1, 1, 1);
            try std.testing.expectEqual(command_state_t.Invalid, resource_state(recording).command_state);
            profile.sets[0] = empty;
            resource_state(recording).command_state = .Recording;
        }
        if (mode == 0 and operation == 4) {
            device_caches[0].graphics_queue_flags[0] = c.VK_QUEUE_COMPUTE_BIT;
            bind_pipeline(command_handle, 0, @ptrFromInt(pipeline.*.handle));
            try std.testing.expectEqual(command_state_t.Invalid, resource_state(recording).command_state);
            try std.testing.expectEqual(@as(u64, 0), graphics_recording(resource_state(recording)).pipeline);
            try std.testing.expectEqual([_]u64{0} ** 8, resource_state(recording).buffer_references);
            device_caches[0].graphics_queue_flags[0] = c.VK_QUEUE_GRAPHICS_BIT;
            resource_state(recording).command_state = .Recording;
        }
        switch (operation) {
            0 => bind_pipeline(command_handle, 1, @ptrFromInt(pipeline.*.handle)),
            1 => bind_descriptor_sets(command_handle, 1, @ptrFromInt(layout.*.handle), 0, 1, &handles, 0, null),
            2 => push_constants(command_handle, @ptrFromInt(layout.*.handle), 32, 0, 4, &value),
            3 => dispatch(command_handle, 1, 1, 1),
            4 => bind_pipeline(command_handle, 0, @ptrFromInt(pipeline.*.handle)),
            5 => begin_render_pass(command_handle, &begin_info, c.VK_SUBPASS_CONTENTS_INLINE),
            6 => end_render_pass(command_handle),
            7 => draw(command_handle, 3, 1, 0, 0),
            8 => copy_image_to_buffer(command_handle, @ptrFromInt(image.*.handle), c.VK_IMAGE_LAYOUT_GENERAL, @ptrFromInt(copy_target.*.handle), 1, &copy_region),
            else => unreachable,
        }
        if (operation >= 5) {
            const expected_active: u32 = if (operation == 5) (if (mode == 0) 37 else 0) else if (operation == 6) (if (mode == 0) 0 else 37) else if (operation == 7) 37 else 0;
            try std.testing.expectEqual(expected_active, graphics_recording(resource_state(recording)).active_format);
            if ((operation == 5 or operation == 8) and mode == 0) {
                var referenced: usize = 0;
                for (resource_state(recording).buffer_references) |word| referenced += @popCount(word);
                try std.testing.expectEqual(@as(usize, if (operation == 5) 5 else 4), referenced);
            }
        }
        try std.testing.expectEqual(@as(c_int, if (mode == 0) c.RingOk else if (mode == 1) c.RingClosed else c.RingCorrupt), lost);
        if (mode != 0) {
            try std.testing.expectEqual([_]u64{0} ** 8, resource_state(recording).buffer_references);
            if (operation == 4) try std.testing.expectEqual(@as(u64, 0), graphics_recording(resource_state(recording)).pipeline);
            try std.testing.expect(!command_profile(recording).descriptor_layout_ready);
            try std.testing.expectEqual(@as(u64, 0), command_profile(recording).pushes[5].initialized[0]);
        } else if (operation == 2) try std.testing.expectEqual(@as(u64, 15), command_profile(recording).pushes[5].initialized[0]);
    };
}

test "graphics queue flags cache rejects malformed and impossible actual families before publication" {
    const fixture_t = struct {
        mode: u32,
        submitted: u32 = 0,
        fn exchange(context: ?*anyopaque, request: [*c]const c.venus_request_t, input: ?*const anyopaque, length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
            const fixture: *@This() = @ptrCast(@alignCast(context.?));
            response.* = std.mem.zeroes(c.venus_request_t);
            response.*.kind = request.*.kind;
            response.*.direction = 1;
            if (request.*.kind == c.RequestSubmit) {
                std.debug.assert(length >= 40 and input != null);
                std.debug.assert(std.mem.readInt(u32, @as([*]const u8, @ptrCast(input.?))[36..40], .little) == 7);
                fixture.submitted += 1;
                if (fixture.mode == 4) return c.RingClosed;
                response.*.argument_zero = 1;
            } else if (request.*.kind == c.RequestReply) {
                std.debug.assert(capacity >= 48);
                const bytes = @as([*]u8, @ptrCast(output.?))[0..capacity];
                @memset(bytes, 0);
                std.mem.writeInt(u32, bytes[0..4], 7, .little);
                std.mem.writeInt(u64, bytes[4..12], if (fixture.mode == 1) 0 else 1, .little);
                const count: u32 = if (fixture.mode == 2) 0 else 1;
                std.mem.writeInt(u32, bytes[12..16], count, .little);
                std.mem.writeInt(u64, bytes[16..24], count, .little);
                std.mem.writeInt(u32, bytes[24..28], c.VK_QUEUE_GRAPHICS_BIT, .little);
                std.mem.writeInt(u32, bytes[28..32], if (fixture.mode == 3) 0 else 1, .little);
                std.mem.writeInt(u32, bytes[32..36], 64, .little);
                for (0..3) |index| std.mem.writeInt(u32, bytes[36 + index * 4 ..][0..4], 1, .little);
                response.*.payload_bytes = @intCast(capacity);
            } else if (request.*.kind != c.RequestPoll) return c.RingInvalid;
            return c.RingOk;
        }
    };
    for (0..5) |mode| {
        var fixture = fixture_t{ .mode = @intCast(mode) };
        try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &fixture));
        var physical: [*c]c.venus_object_t = null;
        var device: [*c]c.venus_object_t = null;
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_PHYSICAL_DEVICE, 0, 1, &physical));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE, physical.*.id, 1, &device));
        device_caches[0] = .{ .handle = device.*.handle, .family_count = 1 };
        device_caches[0].counts[0] = 1;
        const accepted = @call(.never_inline, ensure_graphics_queue_flags, .{@as(*const c.venus_object_t, @ptrCast(device))});
        try std.testing.expectEqual(mode == 0, accepted);
        try std.testing.expectEqual(@as(u32, 1), fixture.submitted);
        try std.testing.expectEqual(@as(c_int, if (mode == 0) c.RingOk else if (mode == 4) c.RingClosed else c.RingCorrupt), lost);
        if (mode == 0) {
            try std.testing.expectEqual(@as(u32, c.VK_QUEUE_GRAPHICS_BIT), device_caches[0].graphics_queue_flags[0]);
            fixture.mode = 4;
            try std.testing.expect(@call(.never_inline, ensure_graphics_queue_flags, .{@as(*const c.venus_object_t, @ptrCast(device))}));
            try std.testing.expectEqual(@as(u32, 1), fixture.submitted);
        } else {
            try std.testing.expect(!device_caches[0].graphics_queue_ready);
            try std.testing.expectEqual([_]u32{0} ** 64, device_caches[0].graphics_queue_flags);
        }
        venus_icd_abandon();
        try std.testing.expectEqualDeep(device_cache_t{}, device_caches[0]);
    }
}

test "negotiated binding owns the entire profile and rejected calls preserve the live session" {
    const fixture_t = struct {
        fn exchange(_: ?*anyopaque, _: [*c]const c.venus_request_t, _: ?*const anyopaque, _: usize, _: [*c]c.venus_request_t, _: ?*anyopaque, _: usize) callconv(.C) c_int {
            return c.RingInvalid; // Binding never contacts the receiver.
        }
    };
    var context: u8 = 0;
    var capabilities = std.mem.zeroes(c.venus_capabilities_t);
    capabilities.wire_format_version = 1;
    capabilities.vk_xml_version = c.VenusPinnedXmlVersion;
    capabilities.vk_ext_command_serialization_spec_version = 1;
    capabilities.vk_mesa_venus_protocol_spec_version = 3;
    capabilities.supports_blob_id_0 = 1;
    capabilities.supports_multiple_timelines = 1;
    capabilities.vk_extension_mask1[0] = 1;
    capabilities.vk_extension_mask1[12] = 3;
    capabilities.vk_extension_mask1[31] = 0xa5a5a5a5;
    const expected = capabilities;
    for (0..8) |mode| {
        var invalid = expected;
        switch (mode) {
            3 => invalid.wire_format_version = 0,
            4 => invalid.allow_vk_wait_syncs = 2,
            5 => invalid.supports_blob_id_0 = 0,
            6 => invalid.vk_extension_mask1[0] = 0,
            7 => invalid.vk_extension_mask1[12] = 1,
            else => {},
        }
        const previous_namespace = namespace_id;
        try std.testing.expectEqual(@as(c_int, c.RingInvalid), venus_icd_bind_capabilities(if (mode == 0) null else fixture_t.exchange, if (mode == 1) null else &context, if (mode == 2) null else &invalid));
        try std.testing.expectEqual(previous_namespace, namespace_id);
        try std.testing.expect(command.exchange == null);
        try std.testing.expect(!negotiated_capabilities_ready);
        try std.testing.expectEqualDeep(std.mem.zeroes(c.venus_capabilities_t), negotiated_capabilities);
    }
    const previous_namespace = namespace_id;
    namespace_id = std.math.maxInt(u32);
    const exhausted = venus_icd_bind_capabilities(fixture_t.exchange, &context, &capabilities);
    namespace_id = previous_namespace;
    try std.testing.expectEqual(@as(c_int, c.RingLimit), exhausted);
    try std.testing.expect(!negotiated_capabilities_ready);
    try std.testing.expect(command.exchange == null);
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind_capabilities(fixture_t.exchange, &context, &capabilities));
    defer venus_icd_abandon();
    @memset(std.mem.asBytes(&capabilities), 0);
    try std.testing.expect(negotiated_capabilities_ready);
    try std.testing.expectEqualDeep(expected, negotiated_capabilities);
    const bound_namespace = namespace_id;
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), venus_icd_bind_capabilities(fixture_t.exchange, &context, &expected));
    try std.testing.expectEqual(@as(c_int, c.RingInvalid), venus_icd_bind(fixture_t.exchange, &context));
    try std.testing.expectEqual(bound_namespace, namespace_id);
    try std.testing.expectEqualDeep(expected, negotiated_capabilities);
    var instance: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_INSTANCE, 0, 1, &instance));
    try std.testing.expectEqual(@as(c_int, c.RingAgain), venus_icd_unbind());
    try std.testing.expect(negotiated_capabilities_ready);
    try std.testing.expectEqualDeep(expected, negotiated_capabilities);
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_release(&objects, instance.*.handle, c.VK_OBJECT_TYPE_INSTANCE, 1));
    // An accepted CPU command owns the binding even with no live Vulkan object.
    for ([_]u32{ c.CommandSubmitted, c.CommandReading, c.CommandReady }) |phase| {
        command.state = phase;
        command.reply_offset = if (phase == c.CommandReady) command.rx_bytes else 0;
        command.cpu_fence = 7;
        command.command_id = 137;
        const expected_command = command;
        try std.testing.expectEqual(@as(c_int, c.RingAgain), venus_icd_unbind());
        try std.testing.expectEqualDeep(expected_command, command);
        try std.testing.expectEqual(bound_namespace, namespace_id);
        try std.testing.expect(negotiated_capabilities_ready);
        try std.testing.expectEqualDeep(expected, negotiated_capabilities);
    }
    command.state = c.CommandIdle;
    command.reply_offset = 0;
    command.cpu_fence = 0;
    command.command_id = 0;
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_unbind());
    try std.testing.expect(!negotiated_capabilities_ready);
    try std.testing.expectEqualDeep(std.mem.zeroes(c.venus_capabilities_t), negotiated_capabilities);
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &context));
    try std.testing.expect(!negotiated_capabilities_ready);
    try std.testing.expectEqualDeep(std.mem.zeroes(c.venus_capabilities_t), negotiated_capabilities);
    venus_icd_abandon();
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind_capabilities(fixture_t.exchange, &context, &expected));
    venus_icd_abandon();
    try std.testing.expect(!negotiated_capabilities_ready);
    try std.testing.expectEqualDeep(std.mem.zeroes(c.venus_capabilities_t), negotiated_capabilities);
}

extern fn venus_features_test_query([*]const u32, usize, [*]u8) usize;
extern fn venus_features_test_reply([*]const u32, usize, [*]u8) usize;
extern fn venus_features_test_reply_one_hot([*]const u32, usize, usize, [*]u8) usize;
extern fn venus_properties_test_fixture([*]const u32, usize, *c.VkPhysicalDeviceProperties, [*]properties_wire.data_t) void;
extern fn venus_properties_test_encode([*]const u32, usize, *const c.VkPhysicalDeviceProperties, [*]const properties_wire.data_t, [*]u8) usize;
extern fn venus_values_test_encode(u32, [*]u8, usize) usize;
extern fn venus_values_test_properties(*const c.VkPhysicalDeviceProperties, [*]u8, usize) usize;
const feature_fixture_t = struct {
    api: u32 = c.VK_API_VERSION_1_3,
    tags: [8]u32 = .{ c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR },
    count: usize = 8,
    hot: ?usize = null,
    corrupt_word: ?usize = null,
    truncate: ?usize = null,
    fail_command: u32 = std.math.maxInt(u32),
    current: u32 = 0,
    commands: u32 = 0,
    feature_commands: u32 = 0,
    create_result: i32 = 0,
    corrupt_create: bool = false,
    corrupt_create_identity: bool = false,
    corrupt_destroy: bool = false,
    expected_device_info: ?*const c.VkDeviceCreateInfo = null,
    reply: [4096]u8 = undefined,
    bytes: usize = 0,
    fn exchange(context: ?*anyopaque, request: [*c]const c.venus_request_t, input: ?*const anyopaque, length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
        const self: *feature_fixture_t = @ptrCast(@alignCast(context.?));
        response.* = std.mem.zeroes(c.venus_request_t);
        response.*.kind = request.*.kind;
        response.*.direction = 1;
        switch (request.*.kind) {
            c.RequestSubmit => {
                std.debug.assert(input != null and length >= 44);
                const wire = @as([*]const u8, @ptrCast(input.?))[36..length];
                self.current = std.mem.readInt(u32, wire[0..4], .little);
                self.commands += 1;
                if (self.current == self.fail_command) return c.RingClosed;
                response.*.argument_zero = self.commands;
                @memset(&self.reply, 0);
                switch (self.current) {
                    0 => {
                        std.mem.writeInt(u32, self.reply[0..4], 0, .little);
                        std.mem.writeInt(i32, self.reply[4..8], c.VK_SUCCESS, .little);
                        std.mem.writeInt(u64, self.reply[8..16], 1, .little);
                        std.mem.writeInt(u64, self.reply[16..24], std.mem.readInt(u64, wire[wire.len - 8 ..][0..8], .little), .little);
                        self.bytes = 24;
                    },
                    6 => {
                        var value = std.mem.zeroes(c.VkPhysicalDeviceProperties);
                        value.apiVersion = self.api;
                        self.bytes = venus_values_test_properties(&value, &self.reply, self.reply.len);
                    },
                    1 => {
                        std.mem.writeInt(u32, self.reply[0..4], 1, .little);
                        self.bytes = 4;
                    },
                    3 => self.bytes = venus_values_test_encode(3, &self.reply, self.reply.len),
                    11 => {
                        const physical_id = std.mem.readInt(u64, wire[8..16], .little);
                        const device_id = std.mem.readInt(u64, wire[wire.len - 8 ..][0..8], .little);
                        if (self.expected_device_info) |info| {
                            var expected: [8192]u8 = undefined;
                            const used = venus_device_test_encode(info, physical_id, device_id, &expected);
                            std.debug.assert(std.mem.eql(u8, expected[0..used], wire));
                        }
                        std.mem.writeInt(u32, self.reply[0..4], if (self.corrupt_create) 99 else 11, .little);
                        std.mem.writeInt(i32, self.reply[4..8], self.create_result, .little);
                        std.mem.writeInt(u64, self.reply[8..16], 1, .little);
                        std.mem.writeInt(u64, self.reply[16..24], if (self.create_result < 0) 0 else if (self.corrupt_create_identity) device_id ^ 1 else device_id, .little);
                        self.bytes = 24;
                    },
                    12 => {
                        std.mem.writeInt(u32, self.reply[0..4], if (self.corrupt_destroy) 99 else 12, .little);
                        self.bytes = 4;
                    },
                    148 => {
                        var tags: [properties_wire.MaxNodes]u32 = undefined;
                        var count: usize = 0;
                        var offset: usize = 28;
                        while (std.mem.readInt(u64, wire[offset..][0..8], .little) != 0) : (offset += 12) {
                            tags[count] = std.mem.readInt(u32, wire[offset + 8 ..][0..4], .little);
                            count += 1;
                        }
                        var core: c.VkPhysicalDeviceProperties = undefined;
                        var nodes: [5]properties_wire.data_t = undefined;
                        venus_properties_test_fixture(&tags, count, &core, &nodes);
                        core.apiVersion = self.api;
                        self.bytes = venus_properties_test_encode(&tags, count, &core, &nodes, &self.reply);
                    },
                    147 => {
                        self.feature_commands += 1;
                        var expected: [4096]u8 = undefined;
                        const count = self.count;
                        const expected_bytes = venus_features_test_query(&self.tags, count, &expected);
                        std.mem.writeInt(u64, expected[8..16], std.mem.readInt(u64, wire[8..16], .little), .little);
                        std.debug.assert(std.mem.eql(u8, expected[0..expected_bytes], wire));
                        self.bytes = if (self.hot) |selected| venus_features_test_reply_one_hot(&self.tags, count, selected, &self.reply) else venus_features_test_reply(&self.tags, count, &self.reply);
                        if (self.corrupt_word) |word| std.mem.writeInt(u32, self.reply[word * 4 ..][0..4], 99, .little);
                    },
                    else => return c.RingInvalid,
                }
                std.debug.assert(self.bytes > 0);
            },
            c.RequestPoll => {},
            c.RequestReply => {
                std.debug.assert(output != null and capacity == 4096);
                const bytes = if (self.current == 147 and self.truncate != null) self.truncate.? else capacity;
                @memcpy(@as([*]u8, @ptrCast(output.?))[0..bytes], self.reply[0..bytes]);
                response.*.payload_bytes = @intCast(bytes);
            },
            else => return c.RingInvalid,
        }
        return c.RingOk;
    }
};
fn feature_test_capabilities() c.venus_capabilities_t {
    var value = std.mem.zeroes(c.venus_capabilities_t);
    value.wire_format_version = 1;
    value.vk_xml_version = c.VenusPinnedXmlVersion;
    value.vk_ext_command_serialization_spec_version = 1;
    value.vk_mesa_venus_protocol_spec_version = 3;
    value.supports_blob_id_0 = 1;
    value.supports_multiple_timelines = 1;
    value.vk_extension_mask1[0] = 1;
    value.vk_extension_mask1[12] = 3;
    for ([_]u32{ 29, 287, 471 }) |bit| value.vk_extension_mask1[bit / 32] |= @as(u32, 1) << @as(u5, @intCast(bit % 32));
    return value;
}
fn feature_test_physical(fixture: *feature_fixture_t, capabilities: *const c.venus_capabilities_t) !c.VkPhysicalDevice {
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind_capabilities(feature_fixture_t.exchange, fixture, capabilities));
    var instance: [*c]c.venus_object_t = null;
    var physical: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_INSTANCE, 0, 1, &instance));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_PHYSICAL_DEVICE, instance.*.id, 1, &physical));
    caches[0].handle = instance.*.handle;
    caches[0].ready = true;
    caches[0].count = 1;
    caches[0].physical[0] = physical.*.handle;
    return @ptrFromInt(physical.*.handle);
}
test "Features2 cached raw137 one-hots are immutable while public flags stay false" {
    const capabilities = feature_test_capabilities();
    for (0..137) |selected| {
        var fixture = feature_fixture_t{ .hot = selected };
        const physical = try feature_test_physical(&fixture, &capabilities);
        defer venus_icd_abandon();
        var output = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
        output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2(physical, &output);
        try std.testing.expectEqual(@as(u32, 2), fixture.commands);
        try std.testing.expectEqual(@as(u32, 1), fixture.feature_commands);
        const entry = physical_features_cache(physical).?;
        try std.testing.expect(entry.actual_api_ready and entry.raw_features_ready);
        try std.testing.expectEqual(@as(u32, c.VK_API_VERSION_1_3), entry.actual_api_version);
        var position: usize = 0;
        for (entry.raw.core) |flag| {
            try std.testing.expectEqual(@as(u32, @intFromBool(position == selected)), flag);
            position += 1;
        }
        for (entry.raw.nodes[0..entry.raw.count]) |node| for (node.flags[0..node.flag_count]) |flag| {
            try std.testing.expectEqual(@as(u32, @intFromBool(position == selected)), flag);
            position += 1;
        };
        try std.testing.expectEqual(@as(usize, 137), position);
        inline for (@typeInfo(c.VkPhysicalDeviceFeatures).Struct.fields) |field| try std.testing.expectEqual(@as(u32, 0), @field(output.features, field.name));
        const raw_before = entry.raw;
        @memset(std.mem.asBytes(&output.features), 0xa5);
        features2(physical, &output);
        var core = std.mem.zeroes(c.VkPhysicalDeviceFeatures);
        features(physical, &core);
        try std.testing.expectEqualDeep(std.mem.zeroes(c.VkPhysicalDeviceFeatures), core);
        try std.testing.expectEqualDeep(core, output.features);
        try std.testing.expectEqualDeep(raw_before, entry.raw);
        try std.testing.expectEqual(@as(u32, 2), fixture.commands);
    }
}
test "Features2 actual API gates aggregates correctly and never147 on API1.0" {
    const capabilities = feature_test_capabilities();
    for ([_]u32{ c.VK_API_VERSION_1_0, c.VK_API_VERSION_1_1, c.VK_API_VERSION_1_2, c.VK_API_VERSION_1_3 }) |api| {
        var fixture = feature_fixture_t{ .api = api };
        if (api == c.VK_API_VERSION_1_1) {
            fixture.tags = .{ c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, 0, 0, 0, 0 };
            fixture.count = 4;
        } else if (api == c.VK_API_VERSION_1_2) {
            fixture.tags = .{ c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, 0 };
            fixture.count = 7;
        }
        const physical = try feature_test_physical(&fixture, &capabilities);
        defer venus_icd_abandon();
        var output = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
        output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2(physical, &output);
        try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
        try std.testing.expectEqual(@as(u32, 2), fixture.commands);
        try std.testing.expectEqual(@as(u32, if (api == c.VK_API_VERSION_1_0) 0 else 1), fixture.feature_commands);
        const entry = physical_features_cache(physical).?;
        try std.testing.expectEqual(api, entry.actual_api_version);
        try std.testing.expectEqual(@as(u8, @intCast(if (api == c.VK_API_VERSION_1_0) 0 else fixture.count)), entry.raw.count);
    }
}
test "Features2 every truncated or corrupted initialized reply preserves complete output and raw cache" {
    const capabilities = feature_test_capabilities();
    for (0..2) |mode| {
        const attempts: usize = if (mode == 0) 668 else 668 / 4;
        for (0..attempts) |index| {
            var fixture = feature_fixture_t{};
            if (mode == 0) fixture.truncate = index else fixture.corrupt_word = index;
            const physical = try feature_test_physical(&fixture, &capabilities);
            defer venus_icd_abandon();
            var output = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
            output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            @memset(std.mem.asBytes(&output.features), 0xa5);
            const before = std.mem.asBytes(&output).*;
            features2(physical, &output);
            try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&output));
            try std.testing.expectEqual(@as(c_int, c.RingCorrupt), lost);
            const entry = physical_features_cache(physical).?;
            try std.testing.expect(entry.actual_api_ready and !entry.raw_features_ready);
            try std.testing.expectEqualDeep(features_wire.result_t{}, entry.raw);
            const commands = fixture.commands;
            features2(physical, &output);
            try std.testing.expectEqual(commands, fixture.commands);
        }
    }
}

test "Features2 native invalid topology cannot publish or transact" {
    const capabilities = feature_test_capabilities();
    var fixture = feature_fixture_t{};
    const physical = try feature_test_physical(&fixture, &capabilities);
    defer venus_icd_abandon();
    var outer = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
    outer.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    @memset(std.mem.asBytes(&outer.features), 0xa5);
    var node = std.mem.zeroes(c.VkPhysicalDeviceShaderDrawParametersFeatures);
    node.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES;
    node.shaderDrawParameters = 0xa5a5a5a5;
    var duplicate = node;
    var unknown = std.mem.zeroes(c.VkBaseOutStructure);
    unknown.sType = 999999;
    for (0..8) |mode| {
        outer.pNext = &node;
        node.pNext = null;
        switch (mode) {
            0 => outer.sType = 0,
            1 => node.pNext = &node,
            2 => {
                node.pNext = &duplicate;
                duplicate.pNext = null;
            },
            3 => outer.pNext = &outer,
            4 => outer.pNext = @ptrFromInt(@intFromPtr(&node) + 1),
            5 => {
                unknown.pNext = &unknown;
                outer.pNext = &unknown;
            },
            else => {},
        }
        const before = std.mem.asBytes(&outer).*;
        const node_before = std.mem.asBytes(&node).*;
        const address: ?*anyopaque = if (mode == 6) null else if (mode == 7) @ptrFromInt(@intFromPtr(&outer) + 1) else &outer;
        features2(physical, address);
        try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&outer));
        try std.testing.expectEqualSlices(u8, &node_before, std.mem.asBytes(&node));
        try std.testing.expectEqual(@as(u32, 0), fixture.commands);
        try std.testing.expect(!physical_features_cache(physical).?.actual_api_ready);
        outer.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    }
}
test "Features2 parser masks are copied and unavailable requested nodes publish false" {
    var capabilities = feature_test_capabilities();
    capabilities.vk_extension_mask1[29 / 32] &= ~(@as(u32, 1) << 29);
    capabilities.vk_extension_mask1[287 / 32] &= ~(@as(u32, 1) << 31);
    capabilities.vk_extension_mask1[471 / 32] &= ~(@as(u32, 1) << 23);
    var fixture = feature_fixture_t{ .count = 5 };
    const physical = try feature_test_physical(&fixture, &capabilities);
    defer venus_icd_abandon();
    @memset(std.mem.asBytes(&capabilities), 0xff);
    var transform = std.mem.zeroes(c.VkPhysicalDeviceTransformFeedbackFeaturesEXT);
    transform.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
    transform.transformFeedback = 1;
    transform.geometryStreams = 1;
    var robust = std.mem.zeroes(c.VkPhysicalDeviceRobustness2FeaturesEXT);
    robust.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    robust.robustBufferAccess2 = 1;
    robust.robustImageAccess2 = 1;
    robust.nullDescriptor = 1;
    transform.pNext = &robust;
    var outer = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
    outer.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    outer.pNext = &transform;
    features2(physical, &outer);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
    try std.testing.expectEqual(@as(u32, 0), transform.transformFeedback | transform.geometryStreams | robust.robustBufferAccess2 | robust.robustImageAccess2 | robust.nullDescriptor);
    try std.testing.expectEqual(@as(u8, 5), physical_features_cache(physical).?.raw.count);
    try std.testing.expectEqual(@as(?*anyopaque, &robust), transform.pNext);
}
test "Features2 raw API mismatch invalid versions and failed backend preserve callers" {
    const capabilities = feature_test_capabilities();
    for ([_]u32{ 0, c.VK_API_VERSION_1_3 | (@as(u32, 1) << 29), (@as(u32, 2) << 22), c.VK_API_VERSION_1_3 }) |api| {
        var fixture = feature_fixture_t{ .api = api };
        const physical = try feature_test_physical(&fixture, &capabilities);
        defer venus_icd_abandon();
        var outer = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
        outer.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        @memset(std.mem.asBytes(&outer.features), 0xa5);
        const before = std.mem.asBytes(&outer).*;
        if (api == c.VK_API_VERSION_1_3) fixture.fail_command = 147;
        features2(physical, &outer);
        try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&outer));
        try std.testing.expect(!physical_features_cache(physical).?.raw_features_ready);
        try std.testing.expectEqual(@as(c_int, if (api == c.VK_API_VERSION_1_3) c.RingClosed else c.RingCorrupt), lost);
    }
    var fixture = feature_fixture_t{};
    const physical = try feature_test_physical(&fixture, &capabilities);
    defer venus_icd_abandon();
    var outer = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
    outer.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2(physical, &outer);
    var value = std.mem.zeroes(c.VkPhysicalDeviceProperties);
    properties(physical, &value);
    try std.testing.expectEqual(@as(u32, c.VK_API_VERSION_1_0), value.apiVersion);
    fixture.api = c.VK_API_VERSION_1_2;
    @memset(std.mem.asBytes(&value), 0xa5);
    const before = std.mem.asBytes(&value).*;
    properties(physical, &value);
    try std.testing.expectEqualSlices(u8, &before, std.mem.asBytes(&value));
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), lost);
}
test "Features2 distinct physical caches clear only with acknowledged parent teardown" {
    const capabilities = feature_test_capabilities();
    var fixture = feature_fixture_t{ .hot = 0 };
    const first = try feature_test_physical(&fixture, &capabilities);
    defer venus_icd_abandon();
    var second: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_PHYSICAL_DEVICE, object(caches[0].handle, c.VK_OBJECT_TYPE_INSTANCE).?.id, 1, &second));
    caches[0].count = 2;
    caches[0].physical[1] = second.*.handle;
    var outer = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
    outer.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2(first, &outer);
    fixture.hot = 1;
    const next: c.VkPhysicalDevice = @ptrFromInt(second.*.handle);
    features2(next, &outer);
    try std.testing.expectEqual(@as(u32, 4), fixture.commands);
    try std.testing.expectEqual(@as(u32, 1), physical_features_cache(first).?.raw.core[0]);
    try std.testing.expectEqual(@as(u32, 0), physical_features_cache(first).?.raw.core[1]);
    try std.testing.expectEqual(@as(u32, 0), physical_features_cache(next).?.raw.core[0]);
    try std.testing.expectEqual(@as(u32, 1), physical_features_cache(next).?.raw.core[1]);
    const instance: c.VkInstance = @ptrFromInt(caches[0].handle);
    destroy_instance(instance, null);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
    try std.testing.expectEqualDeep(instance_cache_t{}, caches[0]);
    try std.testing.expect(physical_features_cache(first) == null and physical_features_cache(next) == null);
    features2(first, &outer);
    try std.testing.expectEqual(@as(u32, 5), fixture.commands);
}
test "Features2 complete reordered native chain preserves every non-Boolean byte and raw ownership" {
    const capabilities = feature_test_capabilities();
    var fixture = feature_fixture_t{};
    const physical = try feature_test_physical(&fixture, &capabilities);
    defer venus_icd_abandon();
    const native_t = struct {
        v11: c.VkPhysicalDeviceVulkan11Features,
        v12: c.VkPhysicalDeviceVulkan12Features,
        v13: c.VkPhysicalDeviceVulkan13Features,
        draw: c.VkPhysicalDeviceShaderDrawParametersFeatures,
        reset: c.VkPhysicalDeviceHostQueryResetFeatures,
        transform: c.VkPhysicalDeviceTransformFeedbackFeaturesEXT,
        robust: c.VkPhysicalDeviceRobustness2FeaturesEXT,
        maintenance: c.VkPhysicalDeviceMaintenance5FeaturesKHR,
    };
    const Fields = .{ "v11", "v12", "v13", "draw", "reset", "transform", "robust", "maintenance" };
    var native: native_t = undefined;
    @memset(std.mem.asBytes(&native), 0xa5);
    inline for (FeatureTags, 0..) |tag, index| {
        const field = Fields[index];
        @field(native, field).sType = tag;
        @field(native, field).pNext = if (index == 0) null else &@field(native, Fields[index - 1]);
    }
    var expected = std.mem.asBytes(&native).*;
    inline for (FeatureTags, 0..) |_, index| {
        const field = Fields[index];
        const node_t = @TypeOf(@field(native, field));
        inline for (@typeInfo(node_t).Struct.fields) |member| {
            if (comptime !std.mem.eql(u8, member.name, "sType") and !std.mem.eql(u8, member.name, "pNext")) {
                const offset = @offsetOf(@TypeOf(native), field) + @offsetOf(node_t, member.name);
                std.mem.writeInt(u32, expected[offset..][0..4], 0, .little);
            }
        }
    }
    var output = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
    output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    output.pNext = &native.maintenance;
    features2(physical, &output);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
    try std.testing.expectEqualSlices(u8, &expected, std.mem.asBytes(&native));
    const entry = physical_features_cache(physical).?;
    const raw = entry.raw;
    @memset(&fixture.reply, 0xff);
    @memset(std.mem.asBytes(&output.features), 0xa5);
    features2(physical, &output);
    try std.testing.expectEqualDeep(raw, entry.raw);
    try std.testing.expectEqual(@as(u32, 2), fixture.commands);
    try std.testing.expectEqualSlices(u8, &expected, std.mem.asBytes(&native));
}
test "Features2 failed parent teardown retains raw cache until explicit abandonment" {
    const capabilities = feature_test_capabilities();
    var fixture = feature_fixture_t{};
    const physical = try feature_test_physical(&fixture, &capabilities);
    var output = std.mem.zeroes(c.VkPhysicalDeviceFeatures2);
    output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2(physical, &output);
    const before = caches[0];
    fixture.fail_command = 1;
    destroy_instance(@ptrFromInt(caches[0].handle), null);
    try std.testing.expectEqual(@as(c_int, c.RingClosed), lost);
    try std.testing.expectEqualDeep(before, caches[0]);
    venus_icd_abandon();
    try std.testing.expectEqualDeep(instance_cache_t{}, caches[0]);
    try std.testing.expect(physical_features_cache(physical) == null);
    try std.testing.expect(physical_proc("vkGetPhysicalDeviceFeatures2") == null);
    try std.testing.expect(physical_proc("vkGetPhysicalDeviceFeatures2KHR") == null);
}

extern fn venus_device_test_encode(*const c.VkDeviceCreateInfo, u64, u64, [*]u8) usize;
const device_native_test_node_t = extern struct { type_tag: u32, next: ?*const anyopaque = null, flags: [55]u32 = [_]u32{0} ** 55 };
fn device_test_info(queues: []const c.VkDeviceQueueCreateInfo) c.VkDeviceCreateInfo {
    return .{ .sType = c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = @intCast(queues.len), .pQueueCreateInfos = queues.ptr };
}
fn device_test_queue(priorities: []const f32) c.VkDeviceQueueCreateInfo {
    return .{ .sType = c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = @intCast(priorities.len), .pQueuePriorities = priorities.ptr };
}
fn expect_disabled_device(entry: *const device_cache_t) !void {
    try std.testing.expectEqualDeep(disabled_device_state(), entry.enabled_state);
    try std.testing.expectEqual(@as(u8, 8), entry.enabled_state.features.count);
}

test "public device preflight rejects192 true flags before identity cache ring and transport mutation" {
    const priorities = [_]f32{1};
    const queues = [_]c.VkDeviceQueueCreateInfo{device_test_queue(&priorities)};
    var info = device_test_info(&queues);
    for ([_]bool{ false, true }) |warm| {
        var fixture = feature_fixture_t{ .hot = 0 };
        const physical = try feature_test_physical(&fixture, &feature_test_capabilities());
        defer venus_icd_abandon();
        if (warm) {
            var core: c.VkPhysicalDeviceFeatures = undefined;
            features(physical, &core);
            try std.testing.expectEqual(@as(u32, 1), physical_features_cache(physical).?.raw.core[0]);
        }
        const previous_objects = objects;
        const previous_slots = slots;
        const previous_caches = device_caches;
        const previous_rings = ring_slots;
        const previous_physical = caches;
        const previous_commands = fixture.commands;
        var output: c.VkDevice = null;
        var legacy = std.mem.zeroes(c.VkPhysicalDeviceFeatures);
        info.pEnabledFeatures = &legacy;
        inline for (@typeInfo(c.VkPhysicalDeviceFeatures).Struct.fields) |field| {
            @field(legacy, field.name) = 1;
            try std.testing.expectEqual(@as(c_int, c.VK_ERROR_FEATURE_NOT_PRESENT), create_device(physical, &info, null, @ptrCast(&output)));
            try std.testing.expect(output == null);
            @field(legacy, field.name) = 0;
        }
        info.pEnabledFeatures = null;
        const tags = [_]u32{c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2} ++ FeatureTags;
        const counts = [_]u8{55} ++ FeatureCounts;
        for (tags, counts) |tag, count| {
            var node = device_native_test_node_t{ .type_tag = tag };
            info.pNext = &node;
            for (0..count) |index| {
                node.flags[index] = 1;
                try std.testing.expectEqual(@as(c_int, c.VK_ERROR_FEATURE_NOT_PRESENT), create_device(physical, &info, null, @ptrCast(&output)));
                try std.testing.expect(output == null);
                node.flags[index] = 0;
            }
        }
        info.pNext = null;
        try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), create_device(physical, @ptrFromInt(1), null, @ptrCast(&output)));
        try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), create_device(physical, &info, null, @ptrFromInt(1)));
        const names = [_][*c]const u8{"VK_EXT_unsupported"};
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &names;
        try std.testing.expectEqual(@as(c_int, c.VK_ERROR_EXTENSION_NOT_PRESENT), create_device(physical, &info, null, @ptrCast(&output)));
        info.enabledExtensionCount = 0;
        info.enabledLayerCount = 1;
        try std.testing.expectEqual(@as(c_int, c.VK_ERROR_LAYER_NOT_PRESENT), create_device(physical, &info, null, @ptrCast(&output)));
        info.enabledLayerCount = 0;
        try std.testing.expectEqualDeep(previous_objects, objects);
        try std.testing.expectEqualDeep(previous_slots, slots);
        try std.testing.expectEqualDeep(previous_caches, device_caches);
        try std.testing.expectEqualDeep(previous_rings, ring_slots);
        try std.testing.expectEqualDeep(previous_physical, caches);
        try std.testing.expectEqual(previous_commands, fixture.commands);
    }
}

test "public device ACK owns immutable canonical state and exact independent minimal maximum packets" {
    var fixture = feature_fixture_t{};
    const physical = try feature_test_physical(&fixture, &feature_test_capabilities());
    defer venus_icd_abandon();
    var priorities = [_]f32{0.5} ** 16;
    var queues = [_]c.VkDeviceQueueCreateInfo{device_test_queue(priorities[0..4])} ** 16;
    for (&queues, 0..) |*queue, index| queue.queueFamilyIndex = @intCast(index);
    var info = device_test_info(&queues);
    var legacy = std.mem.zeroes(c.VkPhysicalDeviceFeatures);
    info.pEnabledFeatures = &legacy;
    const maximum = try device_native.preflight(&info);
    const maximum_packet = try encode_device(&maximum, 7, 42);
    var expected: [8192]u8 = undefined;
    const maximum_used = venus_device_test_encode(&info, 7, 42, &expected);
    try std.testing.expectEqual(@as(usize, 1096), maximum_packet.used);
    try std.testing.expectEqualSlices(u8, expected[0..maximum_used], maximum_packet.bytes[0..maximum_packet.used]);
    info.queueCreateInfoCount = 1;
    queues[0].queueCount = 1;
    fixture.expected_device_info = &info;
    var first: c.VkDevice = null;
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), create_device(physical, &info, null, @ptrCast(&first)));
    try std.testing.expectEqual(@as(u32, 1), fixture.commands); // No hidden API/features query.
    const first_entry = device_cache(@intFromPtr(first.?)).?;
    try expect_disabled_device(first_entry);
    const retained = first_entry.*;
    legacy.robustBufferAccess = 1;
    priorities[0] = 0.75;
    try std.testing.expectEqualDeep(retained, first_entry.*);
    info.pEnabledFeatures = null;
    var modern = device_native_test_node_t{ .type_tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    var extension = device_native_test_node_t{ .type_tag = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
    modern.next = &extension;
    info.pNext = &modern;
    var selected = info;
    selected.pNext = null;
    fixture.expected_device_info = &selected;
    var second: c.VkDevice = null;
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), create_device(physical, &info, null, @ptrCast(&second)));
    const second_entry = device_cache(@intFromPtr(second.?)).?;
    try std.testing.expect(first_entry != second_entry);
    try expect_disabled_device(second_entry);
    extension.flags[0] = 1;
    modern.flags[0] = 1;
    priorities[0] = 0.25;
    try expect_disabled_device(first_entry);
    try expect_disabled_device(second_entry);
    var child: [*c]c.venus_object_t = null;
    const first_record = object(@intFromPtr(first.?), c.VK_OBJECT_TYPE_DEVICE).?;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_BUFFER, first_record.id, 1, &child));
    const before_refusal = fixture.commands;
    destroy_device(first, null);
    try std.testing.expectEqual(before_refusal, fixture.commands);
    try expect_disabled_device(first_entry);
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_release(&objects, child.*.handle, c.VK_OBJECT_TYPE_BUFFER, 1));
    destroy_device(first, null);
    try std.testing.expectEqualDeep(device_cache_t{}, first_entry.*);
    try expect_disabled_device(second_entry);
    fixture.fail_command = 12;
    destroy_device(second, null);
    try expect_disabled_device(second_entry);
    try std.testing.expectEqual(@as(c_int, c.RingClosed), lost);
    venus_icd_abandon();
    try std.testing.expectEqualDeep([_]device_cache_t{.{}} ** 16, device_caches);
}

test "public device explicit failure rolls back while uncertain create retains only opaque owners" {
    const priorities = [_]f32{1};
    const queues = [_]c.VkDeviceQueueCreateInfo{device_test_queue(&priorities)};
    const info = device_test_info(&queues);
    for (0..4) |mode| {
        var fixture = feature_fixture_t{};
        const physical = try feature_test_physical(&fixture, &feature_test_capabilities());
        defer venus_icd_abandon();
        if (mode == 0) fixture.create_result = c.VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (mode == 1) fixture.corrupt_create = true;
        if (mode == 2) fixture.fail_command = 11;
        if (mode == 3) fixture.corrupt_create_identity = true;
        var output: c.VkDevice = null;
        const result = create_device(physical, &info, null, @ptrCast(&output));
        try std.testing.expect(output == null);
        try std.testing.expectEqual(@as(c_int, if (mode == 0) c.VK_ERROR_OUT_OF_DEVICE_MEMORY else c.VK_ERROR_DEVICE_LOST), result);
        try std.testing.expectEqualDeep([_]device_cache_t{.{}} ** 16, device_caches);
        var live_devices: usize = 0;
        var live_queues: usize = 0;
        for (slots) |slot| {
            if (slot.id != 0 and slot.kind == c.VK_OBJECT_TYPE_DEVICE) live_devices += 1;
            if (slot.id != 0 and slot.kind == c.VK_OBJECT_TYPE_QUEUE) live_queues += 1;
        }
        try std.testing.expectEqual(@as(usize, @intFromBool(mode != 0)), live_devices);
        try std.testing.expectEqual(live_devices, live_queues);
        try std.testing.expectEqual(mode != 0, ring_slots[1]);
        try std.testing.expectEqual(@as(c_int, if (mode == 0) c.RingOk else if (mode == 2) c.RingClosed else c.RingCorrupt), lost);
        try std.testing.expectEqual(@as(c_int, c.RingAgain), venus_icd_unbind());
        venus_icd_abandon();
        try std.testing.expectEqualDeep([_]device_cache_t{.{}} ** 16, device_caches);
        try std.testing.expectEqualDeep([_]bool{false} ** 64, ring_slots);
    }
}

test "public device full registry preflight precedence queue rollback and idle-owner refusal retain state" {
    var fixture = feature_fixture_t{};
    const physical = try feature_test_physical(&fixture, &feature_test_capabilities());
    defer venus_icd_abandon();
    const priorities = [_]f32{1};
    const queues = [_]c.VkDeviceQueueCreateInfo{device_test_queue(&priorities)};
    var info = device_test_info(&queues);
    var first: c.VkDevice = null;
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), create_device(physical, &info, null, @ptrCast(&first)));
    const first_entry = device_cache(@intFromPtr(first.?)).?;
    const first_record = object(@intFromPtr(first.?), c.VK_OBJECT_TYPE_DEVICE).?;
    const retained = first_entry.*;
    var handles: [508]u64 = undefined;
    for (&handles) |*handle| {
        var child: [*c]c.venus_object_t = null;
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_BUFFER, first_record.id, 0, &child));
        handle.* = child.*.handle;
    }
    try std.testing.expectEqual(@as(u32, 512), objects.live_count);
    const previous_id = objects.next_id;
    const previous_commands = fixture.commands;
    var output: c.VkDevice = null;
    var legacy = std.mem.zeroes(c.VkPhysicalDeviceFeatures);
    legacy.robustBufferAccess = 1;
    info.pEnabledFeatures = &legacy;
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_FEATURE_NOT_PRESENT), create_device(physical, &info, null, @ptrCast(&output)));
    try std.testing.expectEqual(previous_id, objects.next_id);
    info.pEnabledFeatures = null;
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_OUT_OF_HOST_MEMORY), create_device(physical, &info, null, @ptrCast(&output)));
    try std.testing.expectEqual(previous_id, objects.next_id);
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_release(&objects, handles[507], c.VK_OBJECT_TYPE_BUFFER, 0));
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_OUT_OF_HOST_MEMORY), create_device(physical, &info, null, @ptrCast(&output)));
    try std.testing.expectEqual(previous_id + 1, objects.next_id); // Queue failure consumed/released device only.
    try std.testing.expectEqual(@as(u32, 511), objects.live_count);
    try std.testing.expect(output == null);
    try std.testing.expectEqual(previous_commands, fixture.commands);
    try std.testing.expectEqualDeep(retained, first_entry.*);
    try std.testing.expect(ring_slots[1] and !ring_slots[2]);
    for (handles[0..507]) |handle| try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_release(&objects, handle, c.VK_OBJECT_TYPE_BUFFER, 0));
    resource_state(first_record).idle_refs = 1; // A live waiter owns this device until its completion.
    destroy_device(first, null);
    try std.testing.expectEqual(previous_commands, fixture.commands);
    try std.testing.expectEqualDeep(retained, first_entry.*);
    resource_state(first_record).idle_refs = 0;
    destroy_device(first, null);
    try std.testing.expectEqualDeep(device_cache_t{}, first_entry.*);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
}


test "public malformed device destruction retains immutable state with exact output physical loss precedence" {
    var fixture = feature_fixture_t{};
    const physical = try feature_test_physical(&fixture, &feature_test_capabilities());
    defer venus_icd_abandon();
    const priorities = [_]f32{1};
    const queues = [_]c.VkDeviceQueueCreateInfo{device_test_queue(&priorities)};
    const info = device_test_info(&queues);
    var output: c.VkDevice = null;
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), create_device(physical, &info, null, @ptrCast(&output)));
    const entry = device_cache(@intFromPtr(output.?)).?;
    const retained = entry.*;
    const live = objects.live_count;
    fixture.corrupt_destroy = true;
    destroy_device(output, null);
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), lost);
    try std.testing.expectEqualDeep(retained, entry.*);
    try std.testing.expectEqual(live, objects.live_count);
    try std.testing.expect(ring_slots[1]);
    const before = fixture.commands;
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_DEVICE_LOST), create_device(physical, @ptrFromInt(1), null, @ptrCast(&output)));
    try std.testing.expect(output == null);
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), create_device(null, @ptrFromInt(1), null, @ptrCast(&output)));
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), create_device(physical, @ptrFromInt(1), null, null));
    try std.testing.expectEqual(before, fixture.commands);
    try std.testing.expectEqual(@as(c_int, c.RingAgain), venus_icd_unbind());
    try std.testing.expectEqualDeep(retained, entry.*);
    venus_icd_abandon();
    try std.testing.expectEqualDeep([_]device_cache_t{.{}} ** 16, device_caches);
}

const timed_fixture_t = struct {
    now: u64 = 1,
    deadline: u64 = 0,
    command_id: u32 = 0,
    submitted: u32 = 0,
    reads: u32 = 0,
    last_offset: usize = 0,
    expire_reply: bool = false,
    reply: [extensions_wire.MaxReplyBytes]u8 = [_]u8{0} ** extensions_wire.MaxReplyBytes,
    fn clock(context: ?*anyopaque) callconv(.C) u64 {
        const self: *@This() = @ptrCast(@alignCast(context.?));
        return self.now;
    }
    fn exchange(context: ?*anyopaque, request: [*c]const c.venus_request_t, input: ?*const anyopaque,
        length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize, deadline: u64) callconv(.C) c_int {
        const self: *@This() = @ptrCast(@alignCast(context.?));
        std.debug.assert(deadline > self.now);
        response.* = std.mem.zeroes(c.venus_request_t);
        response.*.kind = request.*.kind;
        response.*.direction = 1;
        if (self.submitted == 0 and request.*.kind == c.RequestReply) {
            std.debug.assert(capacity == 1);
            if (request.*.argument_zero == TimedReplyBytes) {
                response.*.status = c.RequestInvalid;
                return c.RingInvalid;
            }
            std.debug.assert(request.*.argument_zero == TimedReplyBytes - 1);
            @as(*u8, @ptrCast(output.?)).* = 0;
            response.*.payload_bytes = 1;
            return c.RingOk;
        }
        switch (request.*.kind) {
            c.RequestSubmit => {
                const wire = @as([*]const u8, @ptrCast(input.?))[36..length];
                self.command_id = std.mem.readInt(u32, wire[0..4], .little);
                std.debug.assert(self.command_id == 14);
                self.submitted += 1;
                self.deadline = deadline;
                response.*.argument_zero = self.submitted;
                @memset(&self.reply, 0);
                std.mem.writeInt(u32, self.reply[0..4], 14, .little);
                std.mem.writeInt(u64, self.reply[8..16], 1, .little);
                std.mem.writeInt(u32, self.reply[16..20], 1024, .little);
                const count = std.mem.readInt(u32, wire[32..36], .little);
                std.mem.writeInt(u64, self.reply[20..28], count, .little);
                for (0..count) |index| {
                    const start = 28 + index * 268;
                    std.mem.writeInt(u64, self.reply[start..][0..8], 256, .little);
                    const name_bytes = std.fmt.bufPrint(self.reply[start + 8 ..][0..256], "VK_test_{d:0>4}", .{index}) catch unreachable;
                    self.reply[start + 8 + name_bytes.len] = 0;
                    std.mem.writeInt(u32, self.reply[start + 264 ..][0..4], 1, .little);
                }
                self.last_offset = 0;
                self.reads = 0;
            },
            c.RequestPoll => std.debug.assert(deadline == self.deadline),
            c.RequestReply => {
                std.debug.assert(deadline == self.deadline and request.*.argument_zero == self.last_offset);
                std.debug.assert(capacity <= 4096);
                @memcpy(@as([*]u8, @ptrCast(output.?))[0..capacity], self.reply[self.last_offset..][0..capacity]);
                self.last_offset += capacity;
                self.reads += 1;
                response.*.payload_bytes = @intCast(capacity);
                if (self.expire_reply) self.now = deadline;
            },
            else => return c.RingInvalid,
        }
        return c.RingOk;
    }
};
test "timed actual reply proof and1024 extension cache use68 bounded reads with one deadline" {
    var fixture = timed_fixture_t{};
    const capabilities = feature_test_capabilities();
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind_timed(timed_fixture_t.exchange,
        timed_fixture_t.clock, &fixture, &capabilities, TimedReplyBytes));
    defer venus_icd_abandon();
    var instance: [*c]c.venus_object_t = null;
    var physical: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_INSTANCE, 0, 1, &instance));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_PHYSICAL_DEVICE, instance.*.id, 1, &physical));
    const handle: c.VkPhysicalDevice = @ptrFromInt(physical.*.handle);
    const cached = try ensure_raw_extensions(handle);
    try std.testing.expectEqual(@as(usize, 1024), cached.records.?.len);
    try std.testing.expectEqual(@as(u32, 68), fixture.reads);
    try std.testing.expectEqual(@as(usize, extensions_wire.MaxReplyBytes), fixture.last_offset);
    try std.testing.expectEqual(@as(u32, 2), fixture.submitted);
    try std.testing.expectEqualStrings("VK_test_1023", std.mem.sliceTo(&cached.records.?[1023].name, 0));
    _ = try ensure_raw_extensions(handle);
    try std.testing.expectEqual(@as(u32, 2), fixture.submitted);
    fixture.now = 0;
    const request = try extensions_wire.encode_count(physical.*.id);
    try std.testing.expect(transact(&request.bytes) == null);
    try std.testing.expectEqual(@as(c_int, c.RingClosed), lost);
}

test "public Properties2 preserves headers and shares legacy guest limit projection" {
    var fixture = feature_fixture_t{};
    const capabilities = feature_test_capabilities();
    const physical = try feature_test_physical(&fixture, &capabilities);
    defer venus_icd_abandon();
    var node = std.mem.zeroes(c.VkPhysicalDeviceVulkan13Properties);
    node.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
    var output = std.mem.zeroes(c.VkPhysicalDeviceProperties2);
    output.sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    output.pNext = &node;
    properties2(physical, &output);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
    try std.testing.expectEqual(@as(u32, c.VK_API_VERSION_1_0), output.properties.apiVersion);
    try std.testing.expectEqual(@as(u64, 1), output.properties.limits.nonCoherentAtomSize);
    try std.testing.expectEqual(@as(usize, 4096), output.properties.limits.minMemoryMapAlignment);
    try std.testing.expectEqual(@as(u32, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES), node.sType);
    try std.testing.expect(output.pNext == @as(?*anyopaque, @ptrCast(&node)));
    try std.testing.expect(node.pNext == null);
    try std.testing.expect(physical_proc("vkGetPhysicalDeviceProperties2") != null);
    const before = output;
    output.sType = 0;
    const calls = fixture.commands;
    properties2(physical, &output);
    try std.testing.expectEqual(calls, fixture.commands);
    output.sType = before.sType;
    try std.testing.expectEqualDeep(before, output);
}

test "timed callback completion at whole deadline is sticky and never publishes its reply" {
    var fixture = timed_fixture_t{ .expire_reply = true };
    const capabilities = feature_test_capabilities();
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind_timed(timed_fixture_t.exchange,
        timed_fixture_t.clock, &fixture, &capabilities, TimedReplyBytes));
    defer venus_icd_abandon();
    const request = try extensions_wire.encode_count(1);
    try std.testing.expect(transact(&request.bytes) == null);
    try std.testing.expectEqual(@as(c_int, c.RingTimeout), lost);
    try std.testing.expectEqual(@as(u32, c.CommandLost), command.state);
    var view: ?*const anyopaque = null;
    var length: usize = 0;
    try std.testing.expectEqual(@as(c_int, c.RingTimeout), c.venus_command_take(&command, &view, &length));
    try std.testing.expect(view == null and length == 0);
}

test "public WSI surface namespace native queries and nested lock ownership preserve HWND lifetime" {
    var fixture = feature_fixture_t{};
    const capabilities = feature_test_capabilities();
    const physical = try feature_test_physical(&fixture, &capabilities);
    defer venus_icd_abandon();
    const instance: c.VkInstance = @ptrFromInt(caches[0].handle);
    const info = win32_surface_info_t{ .type_tag = c.VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .next = null, .flags = 0, .instance = @ptrFromInt(1), .window = @ptrFromInt(1) };
    var surface: c.VkSurfaceKHR = null;
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), create_win32_surface(instance, &info, null, &surface));
    var values = std.mem.zeroes(c.VkSurfaceCapabilitiesKHR);
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), surface_capabilities(physical, surface, &values));
    try std.testing.expectEqual(@as(u32, 64), values.currentExtent.width);
    var count: u32 = 0;
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), surface_formats(physical, surface, &count, null));
    try std.testing.expect(count >= 2);
    var formats: [4]c.VkSurfaceFormatKHR = undefined;
    count = 1;
    try std.testing.expectEqual(@as(c_int, c.VK_INCOMPLETE), surface_formats(physical, surface, &count, &formats));
    try std.testing.expectEqual(@as(u32, 1), count);
    try std.testing.expectEqual(@as(usize, 0), lock_depth);
    lock_icd();
    lock_icd();
    try std.testing.expectEqual(@as(usize, 2), lock_depth);
    unlock_icd();
    try std.testing.expectEqual(@as(usize, 1), lock_depth);
    unlock_icd();
    destroy_surface(instance, surface, null);
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_SURFACE_LOST_KHR), surface_capabilities(physical, surface, &values));
    try std.testing.expectEqual(@as(u32, 0), fixture.commands);
}

test "legal API1.0 guest Properties2 extension admission gates aliases without forwarding host names" {
    var fixture = feature_fixture_t{};
    const capabilities = feature_test_capabilities();
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind_capabilities(feature_fixture_t.exchange, &fixture, &capabilities));
    defer venus_icd_abandon();
    var count: u32 = 0;
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), enumerate_instance_extensions(null, &count, null));
    try std.testing.expectEqual(@as(u32, 3), count);
    var values: [3]c.VkExtensionProperties = undefined;
    count = 1;
    try std.testing.expectEqual(@as(c_int, c.VK_INCOMPLETE), enumerate_instance_extensions(null, &count, &values));
    try std.testing.expectEqualStrings("VK_KHR_get_physical_device_properties2", std.mem.sliceTo(&values[0].extensionName, 0));
    const names = [_][*c]const u8{"VK_KHR_get_physical_device_properties2"};
    var info = c.VkInstanceCreateInfo{ .sType = c.VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = names.len, .ppEnabledExtensionNames = &names };
    var instance: c.VkInstance = null;
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), create_instance(&info, null, &instance));
    try std.testing.expect(venus_icd_get_instance_proc_addr(instance, "vkGetPhysicalDeviceFeatures2KHR") != null);
    try std.testing.expect(venus_icd_get_physical_proc_addr(instance, "vkGetPhysicalDeviceProperties2KHR") != null);
    try std.testing.expect(venus_icd_get_instance_proc_addr(instance, "vkGetPhysicalDeviceFeatures2") == null);
    try std.testing.expect(venus_icd_get_instance_proc_addr(instance, "vkCreateWin32SurfaceKHR") == null);
    const prior = fixture.commands;
    const unknown = [_][*c]const u8{"VK_EXT_unsupported"};
    info.ppEnabledExtensionNames = &unknown;
    var rejected: c.VkInstance = @ptrFromInt(1);
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_EXTENSION_NOT_PRESENT), create_instance(&info, null, &rejected));
    try std.testing.expect(rejected == null);
    try std.testing.expectEqual(prior, fixture.commands);
    destroy_instance(instance, null);
    for (instance_advertisements) |entry| try std.testing.expect(entry.handle == 0);
}

test "mutable pending descriptors retain new allocation owners exactly until ticket completion" {
    const fixture_t = struct {
        fn exchange(_: ?*anyopaque, _: [*c]const c.venus_request_t, _: ?*const anyopaque, _: usize, _: [*c]c.venus_request_t, _: ?*anyopaque, _: usize) callconv(.C) c_int { return c.RingInvalid; }
    };
    var sentinel: u8 = 0;
    try std.testing.expectEqual(@as(c_int,c.RingOk),venus_icd_bind(fixture_t.exchange,&sentinel));
    defer venus_icd_abandon();
    var instance: [*c]c.venus_object_t = null;
    var physical: [*c]c.venus_object_t = null;
    var pool: [*c]c.venus_object_t = null;
    var device: [*c]c.venus_object_t = null;
    var queue: [*c]c.venus_object_t = null;
    var set: [*c]c.venus_object_t = null;
    var buffer: [*c]c.venus_object_t = null;
    var allocation: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int,c.RingOk),c.venus_objects_reserve(&objects,c.VK_OBJECT_TYPE_INSTANCE,0,1,&instance));
    try std.testing.expectEqual(@as(c_int,c.RingOk),c.venus_objects_reserve(&objects,c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,instance.*.id,1,&physical));
    try std.testing.expectEqual(@as(c_int,c.RingOk),c.venus_objects_reserve(&objects,c.VK_OBJECT_TYPE_DEVICE,physical.*.id,1,&device));
    try std.testing.expectEqual(@as(c_int,c.RingOk),c.venus_objects_reserve(&objects,c.VK_OBJECT_TYPE_QUEUE,device.*.id,1,&queue));
    try std.testing.expectEqual(@as(c_int,c.RingOk),c.venus_objects_reserve(&objects,c.VK_OBJECT_TYPE_DESCRIPTOR_POOL,device.*.id,0,&pool));
    try std.testing.expectEqual(@as(c_int,c.RingOk),c.venus_objects_reserve(&objects,c.VK_OBJECT_TYPE_DESCRIPTOR_SET,pool.*.id,0,&set));
    try std.testing.expectEqual(@as(c_int,c.RingOk),c.venus_objects_reserve(&objects,c.VK_OBJECT_TYPE_BUFFER,device.*.id,0,&buffer));
    try std.testing.expectEqual(@as(c_int,c.RingOk),c.venus_objects_reserve(&objects,c.VK_OBJECT_TYPE_DEVICE_MEMORY,device.*.id,0,&allocation));
    device_caches[0] = .{ .handle=device.*.handle,.descriptor_limits_ready=true,.descriptor_alignments=.{1,1},.descriptor_ranges=.{512,512} };
    resource_state(buffer).* = .{ .id=buffer.*.id,.bound_memory=allocation.*.handle,.buffer_usage=c.VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,.buffer_size=512 };
    var profile = profiles.descriptor_set_t{ .descriptor_count=1 };
    profile.descriptors[0] = .{ .descriptor_type=7,.buffer=buffer.*.handle,.range=128 };
    const ticket = &submission_tickets[0]; ticket.* = .{ .queue=queue.*.handle };
    _ = include_reference(ticket,set); resource_state(set).inflight_count=1;
    retain_pending_descriptor_update(device,set,&profile);
    retain_pending_descriptor_update(device,set,&profile);
    try std.testing.expectEqual(@as(u32,1),resource_state(buffer).inflight_count);
    try std.testing.expectEqual(@as(u32,1),resource_state(allocation).inflight_count);
    destroy_buffer(@ptrFromInt(device.*.handle),@ptrFromInt(buffer.*.handle),null);
    try std.testing.expect(child_object(buffer.*.handle,c.VK_OBJECT_TYPE_BUFFER,device.*.id)!=null);
    retire_ticket(ticket);
    try std.testing.expectEqual(@as(u32,0),resource_state(buffer).inflight_count);
    try std.testing.expectEqual(@as(u32,0),resource_state(allocation).inflight_count);
    try std.testing.expectEqual(@as(u64,0),ticket.queue);
}

test "coherent merge preserves CPU changes and replaces completed GPU bytes" {
    var current = [_]u8{ 1, 9, 3, 4 };
    var baseline = [_]u8{ 1, 2, 3, 4 };
    const incoming = [_]u8{ 5, 6, 7, 8 };
    merge_mapping(&current, &baseline, &incoming);
    try std.testing.expectEqualSlices(u8, &.{ 5, 9, 7, 8 }, &current);
    try std.testing.expectEqualSlices(u8, &incoming, &baseline);
}

test "modern admission intersects backend flags and publishes only owned requested features" {
    const prior = reply_profile_ready;
    reply_profile_ready = true;
    defer reply_profile_ready = prior;
    var raw = features_wire.result_t{};
    raw.core = [_]u32{1} ** 55;
    raw.count = 8;
    for (FeatureTags, FeatureCounts, 0..) |tag, count, index| {
        raw.nodes[index].type_tag = tag;
        raw.nodes[index].flag_count = count;
        @memset(raw.nodes[index].flags[0..count], 1);
    }
    const core = project_core(&raw.core);
    var core_count: usize = 0;
    for (core) |flag| core_count += flag;
    try std.testing.expectEqual(@as(usize, 22), core_count);
    raw.core[@offsetOf(c.VkPhysicalDeviceFeatures, "geometryShader") / 4] = 0;
    try std.testing.expectEqual(@as(u32, 0), project_core(&raw.core)[@offsetOf(c.VkPhysicalDeviceFeatures, "geometryShader") / 4]);
    const projected = project_device_feature_nodes(&raw, &DeviceExtensionNames);
    const unnamed = project_device_feature_nodes(&raw, &.{});
    try std.testing.expectEqualDeep([_]u32{0} ** 55, unnamed[6].flags);
    try std.testing.expectEqualDeep([_]u32{0} ** 55, unnamed[7].flags);
    const totals = [_]usize{ 1, 11, 5, 1, 1, 0, 2, 1 };
    for (projected, totals) |node, expected| {
        var count: usize = 0;
        for (node.flags) |flag| count += flag;
        try std.testing.expectEqual(expected, count);
    }
    var request = device_native.owned_request_t{};
    request.node_count = 1;
    request.nodes[0] = .{ .type_tag = FeatureTags[3], .flag_count = 1 };
    request.nodes[0].flags[0] = 1;
    request.extension_count = 2;
    request.extension_ids[0] = 2;
    request.extension_ids[1] = 0;
    const state = enabled_device_request(&request, &DeviceExtensionNames);
    const shader_index = (@offsetOf(c.VkPhysicalDeviceVulkan11Features, "shaderDrawParameters") - @offsetOf(c.VkPhysicalDeviceVulkan11Features, "storageBuffer16BitAccess")) / 4;
    try std.testing.expectEqual(@as(u32, 1), state.features.nodes[0].flags[shader_index]);
    try std.testing.expectEqual(@as(u32, 1), state.features.nodes[3].flags[0]);
    try std.testing.expectEqual(@as(u32, 5), state.extension_mask);
    try std.testing.expectEqualDeep([_]u32{0} ** 55, state.features.core);
    try std.testing.expectEqualDeep([_]u32{0} ** 47, state.features.nodes[1].flags);
}

test "unsupported external and tool queries preserve output headers and unknown payloads" {
    const transport_t = struct {
        request_count: u32 = 0,
        fn exchange(context: ?*anyopaque, _: [*c]const c.venus_request_t, _: ?*const anyopaque, _: usize, _: [*c]c.venus_request_t, _: ?*anyopaque, _: usize) callconv(.C) c_int {
            const state: *@This() = @ptrCast(@alignCast(context.?));
            state.request_count += 1;
            return c.RingInvalid;
        }
    };
    var transport = transport_t{};
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(transport_t.exchange, &transport));
    defer venus_icd_abandon();
    var instance: [*c]c.venus_object_t = null;
    var physical: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_INSTANCE, 0, 1, &instance));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_PHYSICAL_DEVICE, instance.*.id, 1, &physical));
    const handle: c.VkPhysicalDevice = @ptrFromInt(physical.*.handle);
    var unknown = extern struct { type_tag: u32, next: ?*anyopaque, payload: u64 }{ .type_tag = 0x7ffffff0, .next = null, .payload = 0x123456789abcdef0 };
    var buffer = c.VkExternalBufferProperties{ .sType = c.VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES, .pNext = &unknown, .externalMemoryProperties = .{ .externalMemoryFeatures = 7, .exportFromImportedHandleTypes = 7, .compatibleHandleTypes = 7 } };
    const input = c.VkPhysicalDeviceExternalBufferInfo{ .sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, .usage = c.VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .handleType = c.VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    external_buffer_properties(handle, &input, &buffer);
    try std.testing.expectEqualDeep(std.mem.zeroes(c.VkExternalMemoryProperties), buffer.externalMemoryProperties);
    try std.testing.expectEqual(@as(?*anyopaque, &unknown), buffer.pNext);
    try std.testing.expectEqual(@as(u64, 0x123456789abcdef0), unknown.payload);
    unknown.next = &unknown;
    buffer.externalMemoryProperties.externalMemoryFeatures = 7;
    external_buffer_properties(handle, &input, &buffer);
    try std.testing.expectEqual(@as(u32, 7), buffer.externalMemoryProperties.externalMemoryFeatures);
    unknown.next = null;
    var fence = c.VkExternalFenceProperties{ .sType = c.VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES, .pNext = &unknown, .externalFenceFeatures = 7, .exportFromImportedHandleTypes = 7, .compatibleHandleTypes = 7 };
    const fence_input = c.VkPhysicalDeviceExternalFenceInfo{ .sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO, .handleType = c.VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT };
    external_fence_properties(handle, &fence_input, &fence);
    try std.testing.expectEqual(@as(u32, 0), fence.externalFenceFeatures | fence.exportFromImportedHandleTypes | fence.compatibleHandleTypes);
    var semaphore = c.VkExternalSemaphoreProperties{ .sType = c.VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES, .pNext = &unknown, .externalSemaphoreFeatures = 7, .exportFromImportedHandleTypes = 7, .compatibleHandleTypes = 7 };
    const semaphore_input = c.VkPhysicalDeviceExternalSemaphoreInfo{ .sType = c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, .handleType = c.VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
    external_semaphore_properties(handle, &semaphore_input, &semaphore);
    try std.testing.expectEqual(@as(u32, 0), semaphore.externalSemaphoreFeatures | semaphore.exportFromImportedHandleTypes | semaphore.compatibleHandleTypes);
    var count: u32 = 42;
    var tool = std.mem.zeroes(c.VkPhysicalDeviceToolProperties);
    tool.purposes = 15;
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), tool_properties(handle, &count, &tool));
    try std.testing.expectEqual(@as(u32, 0), count);
    try std.testing.expectEqual(@as(u32, 15), tool.purposes);
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), tool_properties(null, &count, null));
    try std.testing.expectEqual(@as(u32, 0), transport.request_count);
}

test "timeline wait retains nondispatchable duplicate owners and releases only idle references" {
    const fixture_t = struct {
        mode: usize,
        device: c.VkDevice = null,
        semaphore: c.VkSemaphore = null,
        parent_id: u64 = 0,
        captured: bool = false,
        fn exchange(context: ?*anyopaque, request: [*c]const c.venus_request_t, input: ?*const anyopaque, length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
            const fixture: *@This() = @ptrCast(@alignCast(context.?));
            response.* = std.mem.zeroes(c.venus_request_t);
            response.*.kind = request.*.kind;
            response.*.direction = 1;
            if (request.*.kind == c.RequestSubmit) {
                if (length < 40 or std.mem.readInt(u32, @as([*]const u8, @ptrCast(input.?))[36..40], .little) != 173) return c.RingCorrupt;
                const record = child_object(@intFromPtr(fixture.semaphore.?), c.VK_OBJECT_TYPE_SEMAPHORE, fixture.parent_id) orelse return c.RingCorrupt;
                if (resource_state(record).idle_refs != 2 or resource_state(record).inflight_count != 1) return c.RingCorrupt;
                destroy_semaphore(fixture.device, fixture.semaphore, null);
                if (child_object(record.handle, c.VK_OBJECT_TYPE_SEMAPHORE, fixture.parent_id) == null) return c.RingCorrupt;
                fixture.captured = true;
                if (fixture.mode == 2) return c.RingClosed;
                response.*.argument_zero = 1;
            } else if (request.*.kind == c.RequestReply) {
                const bytes = @as([*]u8, @ptrCast(output.?))[0..capacity];
                @memset(bytes, 0);
                std.mem.writeInt(u32, bytes[0..4], 173, .little);
                std.mem.writeInt(i32, bytes[4..8], if (fixture.mode == 1) c.VK_TIMEOUT else c.VK_SUCCESS, .little);
                response.*.payload_bytes = @intCast(capacity);
            } else if (request.*.kind != c.RequestPoll) return c.RingInvalid;
            return c.RingOk;
        }
    };
    for (0..3) |mode| {
        var fixture = fixture_t{ .mode = mode };
        try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &fixture));
        defer venus_icd_abandon();
        var device: [*c]c.venus_object_t = null;
        var semaphore: [*c]c.venus_object_t = null;
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE, 0, 1, &device));
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_SEMAPHORE, device.*.id, 0, &semaphore));
        const state = resource_state(semaphore);
        state.buffer_usage = 1;
        state.inflight_count = 1;
        fixture.device = @ptrFromInt(device.*.handle);
        fixture.semaphore = @ptrFromInt(semaphore.*.handle);
        fixture.parent_id = device.*.id;
        const semaphores = [_]c.VkSemaphore{ fixture.semaphore, fixture.semaphore };
        const values = [_]u64{ 1, 1 };
        const info = c.VkSemaphoreWaitInfo{ .sType = c.VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 2, .pSemaphores = &semaphores, .pValues = &values };
        const expected: c_int = if (mode == 1) c.VK_TIMEOUT else if (mode == 2) c.VK_ERROR_DEVICE_LOST else c.VK_SUCCESS;
        try std.testing.expectEqual(expected, wait_semaphores(fixture.device, &info, 0));
        try std.testing.expect(fixture.captured);
        try std.testing.expectEqual(@as(u32, 0), state.idle_refs);
        try std.testing.expectEqual(@as(u32, 1), state.inflight_count);
        try std.testing.expect(child_object(semaphore.*.handle, c.VK_OBJECT_TYPE_SEMAPHORE, device.*.id) != null);
    }
}

test "device child result queries preserve ownership and publish only validated host bytes" {
    const fixture_t = struct {
        opcode: u32 = 0,
        result: i32 = c.VK_SUCCESS,
        submissions: usize = 0,
        fn exchange(context: ?*anyopaque, request: [*c]const c.venus_request_t, input: ?*const anyopaque, length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
            const fixture: *@This() = @ptrCast(@alignCast(context.?));
            response.* = std.mem.zeroes(c.venus_request_t);
            response.*.kind = request.*.kind;
            response.*.direction = 1;
            if (request.*.kind == c.RequestSubmit) {
                if (input == null or length < 40) return c.RingCorrupt;
                fixture.opcode = std.mem.readInt(u32, @as([*]const u8, @ptrCast(input.?))[36..40], .little);
                fixture.submissions += 1;
                response.*.argument_zero = 1;
            } else if (request.*.kind == c.RequestReply) {
                if (capacity < 40) return c.RingCorrupt;
                const bytes = @as([*]u8, @ptrCast(output.?))[0..capacity];
                @memset(bytes, 0);
                std.mem.writeInt(u32, bytes[0..4], fixture.opcode, .little);
                std.mem.writeInt(i32, bytes[4..8], fixture.result, .little);
                if (fixture.opcode == 49) {
                    std.mem.writeInt(u64, bytes[8..16], 8, .little);
                    @memset(bytes[16..24], 0xa9);
                } else if (fixture.opcode == 63) {
                    std.mem.writeInt(u64, bytes[8..16], 1, .little);
                    std.mem.writeInt(u64, bytes[16..24], 8, .little);
                    std.mem.writeInt(u64, bytes[24..32], 8, .little);
                    @memset(bytes[32..40], 0xbc);
                }
                response.*.payload_bytes = @intCast(capacity);
            } else if (request.*.kind != c.RequestPoll) return c.RingInvalid;
            return c.RingOk;
        }
    };
    var fixture = fixture_t{};
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &fixture));
    defer venus_icd_abandon();
    var device: [*c]c.venus_object_t = null;
    var other: [*c]c.venus_object_t = null;
    var event: [*c]c.venus_object_t = null;
    var query_pool: [*c]c.venus_object_t = null;
    var cache_record: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE, 0, 1, &device));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE, 0, 1, &other));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_EVENT, device.*.id, 0, &event));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_QUERY_POOL, device.*.id, 0, &query_pool));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_PIPELINE_CACHE, device.*.id, 0, &cache_record));
    const native_device: c.VkDevice = @ptrFromInt(device.*.handle);
    const foreign_device: c.VkDevice = @ptrFromInt(other.*.handle);
    const native_event: c.VkEvent = @ptrFromInt(event.*.handle);
    const native_pool: c.VkQueryPool = @ptrFromInt(query_pool.*.handle);
    const native_cache: c.VkPipelineCache = @ptrFromInt(cache_record.*.handle);
    resource_state(query_pool).* = .{ .id = query_pool.*.id, .buffer_size = 2, .buffer_usage = c.VK_QUERY_TYPE_TIMESTAMP };
    var output = [_]u8{0x77} ** 8;
    var size: usize = output.len;
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), get_event_status(foreign_device, native_event));
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), get_query_pool_results(foreign_device, native_pool, 0, 1, output.len, &output, 8, c.VK_QUERY_RESULT_64_BIT));
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), get_pipeline_cache_data(foreign_device, native_cache, &size, &output));
    try std.testing.expectEqual(@as(usize, 0), fixture.submissions);
    fixture.result = c.VK_EVENT_SET;
    try std.testing.expectEqual(@as(c_int, c.VK_EVENT_SET), get_event_status(native_device, native_event));
    fixture.result = c.VK_EVENT_RESET;
    try std.testing.expectEqual(@as(c_int, c.VK_EVENT_RESET), get_event_status(native_device, native_event));
    resource_state(event).buffer_usage = c.VK_EVENT_CREATE_DEVICE_ONLY_BIT;
    const before = fixture.submissions;
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), set_event(native_device, native_event));
    try std.testing.expectEqual(before, fixture.submissions);
    resource_state(event).buffer_usage = 0;
    fixture.result = c.VK_SUCCESS;
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), set_event(native_device, native_event));
    try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), reset_event(native_device, native_event));
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), get_query_pool_results(native_device, native_pool, 2, 1, output.len, &output, 8, c.VK_QUERY_RESULT_64_BIT));
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), get_query_pool_results(native_device, native_pool, 0, 1, output.len, &output, 7, c.VK_QUERY_RESULT_64_BIT));
    fixture.result = c.VK_NOT_READY;
    try std.testing.expectEqual(@as(c_int, c.VK_NOT_READY), get_query_pool_results(native_device, native_pool, 1, 1, output.len, &output, 8, c.VK_QUERY_RESULT_64_BIT));
    try std.testing.expectEqualSlices(u8, &([_]u8{0xa9} ** 8), &output);
    fixture.result = c.VK_ERROR_OUT_OF_DEVICE_MEMORY;
    @memset(&output, 0x55);
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_OUT_OF_DEVICE_MEMORY), get_query_pool_results(native_device, native_pool, 0, 1, output.len, &output, 8, c.VK_QUERY_RESULT_64_BIT));
    try std.testing.expectEqualSlices(u8, &([_]u8{0x55} ** 8), &output);
    fixture.result = c.VK_INCOMPLETE;
    try std.testing.expectEqual(@as(c_int, c.VK_INCOMPLETE), get_pipeline_cache_data(native_device, native_cache, &size, &output));
    try std.testing.expectEqual(@as(usize, 8), size);
    try std.testing.expectEqualSlices(u8, &([_]u8{0xbc} ** 8), &output);
    try std.testing.expect(child_object(cache_record.*.handle, c.VK_OBJECT_TYPE_PIPELINE_CACHE, device.*.id) != null);
    fixture.result = c.VK_EVENT_SET;
    try std.testing.expectEqual(@as(c_int, c.VK_ERROR_DEVICE_LOST), reset_event(native_device, native_event));
    try std.testing.expectEqual(@as(c_int, c.RingCorrupt), lost);
}
// Append-only integration draft for the exclusive ICD owner. No host or GPU dependency.
const image_ownership_fixture_t = struct {
    mode: u8 = 0,
    command_id: u32 = 0,
    submissions: usize = 0,
    fn exchange(context: ?*anyopaque, request: [*c]const c.venus_request_t, input: ?*const anyopaque, length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
        const fixture: *@This() = @ptrCast(@alignCast(context.?));
        response.* = std.mem.zeroes(c.venus_request_t);
        response.*.kind = request.*.kind;
        response.*.direction = 1;
        if (request.*.kind == c.RequestSubmit) {
            const bytes = @as([*]const u8, @ptrCast(input.?))[0..length];
            fixture.command_id = std.mem.readInt(u32, bytes[36..40], .little);
            fixture.submissions += 1;
            if (fixture.mode == 1) return c.RingClosed;
            response.*.argument_zero = fixture.submissions;
        } else if (request.*.kind == c.RequestReply) {
            const bytes = @as([*]u8, @ptrCast(output.?))[0..capacity];
            @memset(bytes, 0);
            std.mem.writeInt(u32, bytes[0..4], fixture.command_id + @as(u32, if (fixture.mode == 2) 1 else 0), .little);
            response.*.payload_bytes = @intCast(capacity);
        } else if (request.*.kind != c.RequestPoll) return c.RingInvalid;
        return c.RingOk;
    }
    fn reserve(kind: u32, parent: u64, dispatchable: u32) !*c.venus_object_t {
        var result: [*c]c.venus_object_t = null;
        try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, kind, parent, dispatchable, &result));
        return result;
    }
    fn retained(recording: *c.venus_object_t, target: *c.venus_object_t) bool {
        const index = resource_index(target);
        return resource_state(recording).buffer_references[index / 64] & (@as(u64, 1) << @as(u6, @intCast(index % 64))) != 0;
    }
};

test "image ownership transfer ACK publishes resources and allocations atomically" {
    // Every operation owns real memory records; failed ACKs cannot publish any refs.
    for (0..5) |operation| for (0..3) |mode| {
        var fixture = image_ownership_fixture_t{ .mode = @intCast(mode) };
        try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(image_ownership_fixture_t.exchange, &fixture));
        defer venus_icd_abandon();
        const device = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE, 0, 1);
        const pool = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_COMMAND_POOL, device.id, 0);
        const recording = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_COMMAND_BUFFER, pool.id, 1);
        const source_memory = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE_MEMORY, device.id, 0);
        const target_memory = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE_MEMORY, device.id, 0);
        const source = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_IMAGE, device.id, 0);
        const target = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_IMAGE, device.id, 0);
        const buffer = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_BUFFER, device.id, 0);
        resource_state(recording).command_state = .Recording;
        for ([_]*c.venus_object_t{ source, target }) |image| resource_state(image).* = .{ .id = image.id, .bound_memory = if (image == source) source_memory.handle else target_memory.handle, .image_usage = 3, .image_type = c.VK_IMAGE_TYPE_2D, .image_extent = .{ 8, 8, 1 }, .image_levels = 1, .image_layers = 1, .image_format = c.VK_FORMAT_R8G8B8A8_UNORM, .image_samples = 1, .requirements = .{ .size = 256 } };
        resource_state(buffer).* = .{ .id = buffer.id, .bound_memory = target_memory.handle, .buffer_size = 256, .buffer_usage = 3 };
        const cmd: c.VkCommandBuffer = @ptrFromInt(recording.handle);
        const source_handle: c.VkImage = @ptrFromInt(source.handle);
        const target_handle: c.VkImage = @ptrFromInt(target.handle);
        var copy = std.mem.zeroes(c.VkImageCopy);
        copy.srcSubresource = .{ .aspectMask = 1, .layerCount = 1 };
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = .{ .width = 8, .height = 8, .depth = 1 };
        var buffer_copy = std.mem.zeroes(c.VkBufferImageCopy);
        buffer_copy.imageSubresource = copy.srcSubresource;
        buffer_copy.imageExtent = copy.extent;
        var range = c.VkImageSubresourceRange{ .aspectMask = 1, .levelCount = 1, .layerCount = 1 };
        var clear_value = std.mem.zeroes(c.VkClearColorValue);
        switch (operation) {
            0 => copy_image(cmd, source_handle, c.VK_IMAGE_LAYOUT_GENERAL, target_handle, c.VK_IMAGE_LAYOUT_GENERAL, 1, &copy),
            1 => copy_buffer_to_image(cmd, @ptrFromInt(buffer.handle), source_handle, c.VK_IMAGE_LAYOUT_GENERAL, 1, &buffer_copy),
            2 => copy_image_to_buffer(cmd, source_handle, c.VK_IMAGE_LAYOUT_GENERAL, @ptrFromInt(buffer.handle), 1, &buffer_copy),
            3 => clear_color_image(cmd, source_handle, c.VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1, &range),
            4 => {
                resource_state(source).image_samples = 4;
                var resolve = c.VkImageResolve{ .srcSubresource = copy.srcSubresource, .dstSubresource = copy.dstSubresource, .extent = copy.extent };
                cmd_resolve_image(cmd, source_handle, c.VK_IMAGE_LAYOUT_GENERAL, target_handle, c.VK_IMAGE_LAYOUT_GENERAL, 1, &resolve);
            },
            else => unreachable,
        }
        try std.testing.expectEqual(@as(usize, 1), fixture.submissions);
        try std.testing.expectEqual(mode == 0, image_ownership_fixture_t.retained(recording, source));
        try std.testing.expectEqual(mode == 0, image_ownership_fixture_t.retained(recording, source_memory));
        if (operation != 3) {
            try std.testing.expectEqual(mode == 0, image_ownership_fixture_t.retained(recording, target_memory));
            try std.testing.expectEqual(mode == 0, image_ownership_fixture_t.retained(recording, if (operation == 1 or operation == 2) buffer else target));
        }
        try std.testing.expectEqual(mode == 0, lost == c.RingOk);
        // Connection failure keeps all private objects owned until explicit abandon.
        try std.testing.expectEqual(@as(usize, 8), objects.live_count);
    };
}

test "image ownership invalid geometry and foreign allocation never reach transport" {
    for (0..6) |case| {
        var fixture = image_ownership_fixture_t{};
        try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(image_ownership_fixture_t.exchange, &fixture));
        defer venus_icd_abandon();
        const device = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE, 0, 1);
        const other_device = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE, 0, 1);
        const pool = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_COMMAND_POOL, device.id, 0);
        const recording = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_COMMAND_BUFFER, pool.id, 1);
        const allocation = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE_MEMORY, if (case == 0) other_device.id else device.id, 0);
        const buffer_allocation = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE_MEMORY, device.id, 0);
        const image = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_IMAGE, device.id, 0);
        const buffer = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_BUFFER, device.id, 0);
        resource_state(recording).command_state = .Recording;
        resource_state(image).* = .{ .id = image.id, .bound_memory = allocation.handle, .image_usage = 3, .image_type = c.VK_IMAGE_TYPE_2D, .image_extent = .{ 8, 8, 1 }, .image_levels = 1, .image_layers = 1, .image_format = c.VK_FORMAT_R8G8B8A8_UNORM, .image_samples = 1, .requirements = .{ .size = 256 } };
        resource_state(buffer).* = .{ .id = buffer.id, .bound_memory = buffer_allocation.handle, .buffer_size = 256, .buffer_usage = 3 };
        var regions = [_]c.VkBufferImageCopy{std.mem.zeroes(c.VkBufferImageCopy)} ** 2;
        regions[0].imageSubresource = .{ .aspectMask = 1, .layerCount = 1 };
        regions[0].imageExtent = .{ .width = 8, .height = 8, .depth = 1 };
        regions[1] = regions[0];
        switch (case) {
            0 => {}, // Live allocation belongs to another device.
            1 => resource_state(image).bound_memory = 0,
            2 => regions[0].imageOffset.x = 1, // Exact extent overflows the mip.
            3 => regions[0].bufferOffset = 4, // Full image exceeds destination buffer.
            4 => resource_state(image).image_samples = 4,
            5 => {}, // Two writes overlap in destination storage.
            else => unreachable,
        }
        copy_image_to_buffer(@ptrFromInt(recording.handle), @ptrFromInt(image.handle), c.VK_IMAGE_LAYOUT_GENERAL, @ptrFromInt(buffer.handle), if (case == 5) 2 else 1, &regions);
        try std.testing.expectEqual(command_state_t.Invalid, resource_state(recording).command_state);
        try std.testing.expectEqual(@as(usize, 0), fixture.submissions);
        try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
        try std.testing.expectEqual([_]u64{0} ** 8, resource_state(recording).buffer_references);
    }
}

test "image ownership dynamic rendering scope and attachment refs publish only after ACK" {
    for (0..3) |mode| {
        var fixture = image_ownership_fixture_t{ .mode = @intCast(mode) };
        try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(image_ownership_fixture_t.exchange, &fixture));
        defer venus_icd_abandon();
        const device = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE, 0, 1);
        const pool = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_COMMAND_POOL, device.id, 0);
        const recording = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_COMMAND_BUFFER, pool.id, 1);
        const allocation = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE_MEMORY, device.id, 0);
        const image = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_IMAGE, device.id, 0);
        const view = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_IMAGE_VIEW, device.id, 0);
        device_caches[0] = .{ .handle = device.handle, .graphics_queue_ready = true, .graphics_queue_count = 1 };
        device_caches[0].graphics_queue_flags[0] = c.VK_QUEUE_GRAPHICS_BIT;
        resource_state(recording).command_state = .Recording;
        resource_state(recording).command_profile_index = try profiles.reserve_slot(&command_registry.commands, compute_state.command_profile_t{});
        resource_state(image).* = .{ .id = image.id, .bound_memory = allocation.handle, .image_usage = c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .image_type = c.VK_IMAGE_TYPE_2D, .image_extent = .{ 8, 8, 1 }, .image_levels = 1, .image_layers = 1, .image_format = c.VK_FORMAT_R8G8B8A8_UNORM, .image_samples = 1 };
        resource_state(view).* = .{ .id = view.id, .view_image = image.handle, .view_type = c.VK_IMAGE_VIEW_TYPE_2D, .view_range = .{ .aspectMask = 1, .levelCount = 1, .layerCount = 1 }, .image_format = c.VK_FORMAT_R8G8B8A8_UNORM, .image_usage = c.VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT };
        var attachment = c.VkRenderingAttachmentInfo{ .sType = c.VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = @ptrFromInt(view.handle), .imageLayout = c.VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = c.VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = c.VK_ATTACHMENT_STORE_OP_STORE };
        var info = c.VkRenderingInfo{ .sType = c.VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = .{ .extent = .{ .width = 8, .height = 8 } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment };
        const cmd: c.VkCommandBuffer = @ptrFromInt(recording.handle);
        begin_rendering(cmd, &info);
        try std.testing.expectEqual(@as(usize, 1), fixture.submissions);
        try std.testing.expectEqual(if (mode == 0) @as(u32, c.VK_FORMAT_R8G8B8A8_UNORM) else 0, graphics_recording(resource_state(recording)).active_format);
        for ([_]*c.venus_object_t{ allocation, image, view }) |target| try std.testing.expectEqual(mode == 0, image_ownership_fixture_t.retained(recording, target));
        if (mode == 0) {
            end_rendering(cmd);
            try std.testing.expectEqual(@as(usize, 2), fixture.submissions);
            try std.testing.expectEqual(@as(u32, 0), graphics_recording(resource_state(recording)).active_format);
            // Ending the scope does not release resources still captured by the command.
            for ([_]*c.venus_object_t{ allocation, image, view }) |target| try std.testing.expect(image_ownership_fixture_t.retained(recording, target));
            end_rendering(cmd);
            try std.testing.expectEqual(command_state_t.Invalid, resource_state(recording).command_state);
            try std.testing.expectEqual(@as(usize, 2), fixture.submissions);
        }
    }
}

test "image ownership public WSI preserves foreign chains and failed acquire indices" {
    for (0..2) |transport_loss| {
        var fixture = image_ownership_fixture_t{};
        try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(image_ownership_fixture_t.exchange, &fixture));
        defer venus_icd_abandon();
        const device = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE, 0, 1);
        const foreign = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE, 0, 1);
        const allocation = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_DEVICE_MEMORY, device.id, 0);
        const image = try image_ownership_fixture_t.reserve(c.VK_OBJECT_TYPE_IMAGE, device.id, 0);
        resource_state(image).* = .{ .id = image.id, .bound_memory = allocation.handle };
        device_caches[0] = .{ .handle = device.handle };
        device_caches[1] = .{ .handle = foreign.handle };
        // Registry publication models a completed constructor; all image/memory
        // handles below are real core owners, retired via the actual callbacks.
        wsi_state.swapchains[0] = .{ .id = 500, .device = device.handle, .count = 1 };
        wsi_state.swapchains[0].images[0] = .{ .image = image.handle, .memory = allocation.handle };
        const chain: c.VkSwapchainKHR = @ptrFromInt(500);
        var count: u32 = 7;
        try std.testing.expectEqual(@as(c_int, c.VK_ERROR_OUT_OF_DATE_KHR), swapchain_images(@ptrFromInt(foreign.handle), chain, &count, null));
        try std.testing.expectEqual(@as(u32, 7), count);
        try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), swapchain_images(@ptrFromInt(device.handle), chain, &count, null));
        try std.testing.expectEqual(@as(u32, 1), count);
        var native_image: c.VkImage = null;
        try std.testing.expectEqual(@as(c_int, c.VK_SUCCESS), swapchain_images(@ptrFromInt(device.handle), chain, &count, &native_image));
        try std.testing.expectEqual(image.handle, @intFromPtr(native_image.?));
        var index: u32 = 0xfeed;
        // No enabled queue: signaling fails; acquisition must roll back locally.
        try std.testing.expectEqual(@as(c_int, c.VK_ERROR_INITIALIZATION_FAILED), acquire_next_image(@ptrFromInt(device.handle), chain, 0, null, null, &index));
        try std.testing.expectEqual(@as(u32, 0xfeed), index);
        try std.testing.expect(!wsi_state.swapchains[0].images[0].acquired);
        destroy_swapchain(@ptrFromInt(foreign.handle), chain, null);
        try std.testing.expectEqual(@as(u64, 500), wsi_state.swapchains[0].id);
        try std.testing.expectEqual(@as(usize, 0), fixture.submissions);
        if (transport_loss != 0) _ = failure(c.RingClosed);
        destroy_swapchain(@ptrFromInt(device.handle), chain, null);
        try std.testing.expectEqual(@as(u64, 0), wsi_state.swapchains[0].id);
        // Failed transport transfers unresolved host ownership to core's retained
        // records. Successful destruction retires image before its allocation.
        try std.testing.expectEqual(if (transport_loss == 0) @as(usize, 2) else 4, objects.live_count);
        try std.testing.expectEqual(if (transport_loss == 0) @as(usize, 2) else 0, fixture.submissions);
    }
}

test "GPU mapping span union conservatively survives overlap fragmentation and resource retirement" {
    var state = resource_state_t{ .allocation_size = 4096 };
    retain_gpu_span(&state, 100, 20);
    retain_gpu_span(&state, 50, 10);
    retain_gpu_span(&state, 60, 40);
    try std.testing.expectEqual(@as(usize, 1), state.gpu_span_count);
    try std.testing.expectEqual(mapping_span_t{ .start = 50, .end = 120 }, state.gpu_spans[0]);
    retain_gpu_span(&state, 4090, 7);
    try std.testing.expectEqual(mapping_span_t{ .end = 4096 }, state.gpu_spans[0]);
    state = .{ .allocation_size = 4096 };
    for (0..17) |index| retain_gpu_span(&state, index * 100, 10);
    try std.testing.expectEqual(@as(usize, 1), state.gpu_span_count);
    try std.testing.expectEqual(mapping_span_t{ .end = 4096 }, state.gpu_spans[0]);
    state = .{ .allocation_size = 0 };
    retain_gpu_span(&state, 0, 0);
    try std.testing.expectEqual(@as(usize, 0), state.gpu_span_count);
}

test "coherent completion reads only retained GPU extents while preserving later CPU writes" {
    const fixture_t = struct {
        reads: usize = 0,
        fn exchange(context: ?*anyopaque, request: [*c]const c.venus_request_t, _: ?*const anyopaque, _: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
            const fixture: *@This() = @ptrCast(@alignCast(context.?));
            if (request.*.kind != c.RequestRead or request.*.argument_zero != 64 or request.*.argument_one != 64 or capacity != 64) return c.RingCorrupt;
            fixture.reads += 1;
            @memset(@as([*]u8, @ptrCast(output.?))[0..capacity], 0xaa);
            response.* = std.mem.zeroes(c.venus_request_t);
            response.*.kind = request.*.kind;
            response.*.direction = 1;
            response.*.payload_bytes = @intCast(capacity);
            return c.RingOk;
        }
    };
    var fixture = fixture_t{};
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &fixture));
    defer venus_icd_abandon();
    const bytes = try std.testing.allocator.alignedAlloc(u8, 4096, 4096);
    defer std.testing.allocator.free(bytes);
    const baseline = try std.testing.allocator.alloc(u8, 256);
    defer std.testing.allocator.free(baseline);
    @memset(bytes, 0);
    @memset(baseline, 0);
    var state = resource_state_t{ .allocation_size = 4096, .mapped_bytes = bytes, .mapped_baseline = baseline, .mapped_offset = 0, .mapped_size = baseline.len, .mapping_resource = 2 };
    try std.testing.expectEqual(@as(c_int, c.RingOk), synchronize_mapping(&state, false));
    try std.testing.expectEqual(@as(usize, 0), fixture.reads);
    retain_gpu_span(&state, 64, 64);
    retain_gpu_span(&state, 1024, 64);
    bytes[70] = 0xbb;
    try std.testing.expectEqual(@as(c_int, c.RingOk), synchronize_mapping(&state, false));
    try std.testing.expectEqual(@as(usize, 1), fixture.reads);
    try std.testing.expectEqual(@as(u8, 0xbb), bytes[70]);
    try std.testing.expectEqual(@as(u8, 0xaa), baseline[70]);
    try std.testing.expectEqual(@as(u8, 0xaa), bytes[64]);
    try std.testing.expectEqual(@as(u8, 0), bytes[0]);
    try std.testing.expectEqual(@as(u8, 0), bytes[128]);
    try std.testing.expectEqual(@as(usize, 2), state.gpu_span_count);
}

test "trusted resource reads use64KiB while legacy callbacks and writes stay4KiB" {
    const fixture_t = struct {
        reads: usize = 0,
        writes: usize = 0,
        maximum: usize,
        fn exchange(context: ?*anyopaque, request: [*c]const c.venus_request_t, input: ?*const anyopaque, length: usize, response: [*c]c.venus_request_t, output: ?*anyopaque, capacity: usize) callconv(.C) c_int {
            const fixture: *@This() = @ptrCast(@alignCast(context.?));
            const read = request.*.kind == c.RequestRead;
            if (!read and request.*.kind != c.RequestWrite) return c.RingCorrupt;
            if (request.*.argument_one != (if (read) capacity else length) or request.*.argument_one > (if (read) fixture.maximum else MappingChunkBytes)) return c.RingCorrupt;
            if (read) {
                fixture.reads += 1;
                @memset(@as([*]u8, @ptrCast(output.?))[0..capacity], 0xa6);
            } else {
                if (input == null or length == 0) return c.RingCorrupt;
                fixture.writes += 1;
            }
            response.* = std.mem.zeroes(c.venus_request_t);
            response.*.kind = request.*.kind;
            response.*.direction = 1;
            response.*.payload_bytes = @intCast(capacity);
            return c.RingOk;
        }
    };
    const bytes = try std.testing.allocator.alignedAlloc(u8, 4096, MappingReadChunkBytes * 2 + 7);
    defer std.testing.allocator.free(bytes);
    @memset(bytes, 0);
    for ([_]bool{ false, true }) |modern| {
        var fixture = fixture_t{ .maximum = if (modern) MappingReadChunkBytes else MappingChunkBytes };
        try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &fixture));
        defer venus_icd_abandon();
        reply_profile_ready = modern;
        var state = resource_state_t{ .allocation_size = bytes.len, .mapped_bytes = bytes, .mapping_resource = 2 };
        try std.testing.expectEqual(@as(c_int, c.RingOk), copy_mapping(&state, 0, bytes.len, false));
        try std.testing.expectEqual(@as(usize, if (modern) 3 else 33), fixture.reads);
        try std.testing.expectEqual(@as(u8, 0xa6), bytes[bytes.len - 1]);
        try std.testing.expectEqual(@as(c_int, c.RingOk), copy_mapping(&state, 0, bytes.len, true));
        try std.testing.expectEqual(@as(usize, 33), fixture.writes);
    }
}

test "submitted bound resource ranges remain allocation owned after guest resource retirement" {
    const fixture_t = struct {
        fn exchange(_: ?*anyopaque, _: [*c]const c.venus_request_t, _: ?*const anyopaque, _: usize, _: [*c]c.venus_request_t, _: ?*anyopaque, _: usize) callconv(.C) c_int { return c.RingInvalid; }
    };
    var sentinel: u8 = 0;
    try std.testing.expectEqual(@as(c_int, c.RingOk), venus_icd_bind(fixture_t.exchange, &sentinel));
    defer venus_icd_abandon();
    var device: [*c]c.venus_object_t = null;
    var memory_record: [*c]c.venus_object_t = null;
    var buffer: [*c]c.venus_object_t = null;
    var image: [*c]c.venus_object_t = null;
    var addressed_buffer: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE, 0, 1, &device));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE_MEMORY, device.*.id, 0, &memory_record));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_BUFFER, device.*.id, 0, &buffer));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_IMAGE, device.*.id, 0, &image));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_BUFFER, device.*.id, 0, &addressed_buffer));
    const allocation = resource_state(memory_record);
    allocation.* = .{ .id = memory_record.*.id, .allocation_size = 8192 };
    resource_state(buffer).* = .{ .id = buffer.*.id, .bound_memory = memory_record.*.handle, .memory_offset = 128, .buffer_size = 256 };
    resource_state(image).* = .{ .id = image.*.id, .bound_memory = memory_record.*.handle, .memory_offset = 1024, .requirements = .{ .size = 512 } };
    var ticket: submission_ticket_t = .{};
    _ = include_reference(&ticket, buffer);
    retain_submission_mapping_spans(device.*.id, ticket.references);
    try std.testing.expectEqual(@as(usize, 1), allocation.gpu_span_count);
    try std.testing.expectEqual(mapping_span_t{ .start = 128, .end = 384 }, allocation.gpu_spans[0]);
    _ = include_reference(&ticket, image);
    retain_submission_mapping_spans(device.*.id, ticket.references);
    try std.testing.expectEqual(@as(usize, 2), allocation.gpu_span_count);
    try std.testing.expectEqual(mapping_span_t{ .start = 1024, .end = 1536 }, allocation.gpu_spans[1]);
    resource_state(addressed_buffer).* = .{ .id = addressed_buffer.*.id, .bound_memory = memory_record.*.handle, .memory_offset = 2048, .buffer_size = 128, .address_exposed = true };
    retain_submission_mapping_spans(device.*.id, [_]u64{0} ** 8);
    try std.testing.expectEqual(@as(usize, 3), allocation.gpu_span_count);
    try std.testing.expectEqual(mapping_span_t{ .start = 2048, .end = 2176 }, allocation.gpu_spans[2]);
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_release(&objects, buffer.*.handle, c.VK_OBJECT_TYPE_BUFFER, 0));
    try std.testing.expectEqual(@as(usize, 3), allocation.gpu_span_count);
    try std.testing.expectEqual(mapping_span_t{ .start = 128, .end = 384 }, allocation.gpu_spans[0]);
}
