//! Bounded general graphics pipeline serialization for core states and dynamic rendering.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const pipeline_helpers = @import("venus_pipeline_wire_helpers.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Owned bounded packet; partial packets never escape an error return.
pub const writer_t = render.writer_t;
// Bound every contribution before treating scalar appends as infallible: chain
// includes three tags/presence words, terminal null, Flags2, libraries64, and8formats.
// State terms are exact maxima for vertex32/32, assembly, tessellation, viewport16/16,
// rasterization, sample masks2, depth/stencil, blend8, dynamic32, and final handles.
const MaxPrivateBytes = 16384;
const MaxChainBytes = 3 * 12 + 8 + 8 + 12 + pipeline_helpers.MaxPipelineLibraries * 8 + 24 + 8 * 4;
const MaxStateBytes = (24 + 12 + 32 * 12 + 12 + 32 * 16) + 32 + 28 +
    (24 + 12 + 16 * 24 + 12 + 16 * 16) + 64 + (24 + 12 + 8 + 2 * 4 + 8) +
    (24 + 20 + 2 * 28 + 8) + (24 + 8 + 12 + 8 * 32 + 8 + 16) + (24 + 12 + 32 * 4);
const MaxBoundedBytes = 40 + MaxChainBytes + 16 +
    5 * (36 + 64 + pipeline_helpers.MaxSpecializationBytes) + MaxStateBytes + 56;
comptime {
    std.debug.assert(MaxBoundedBytes <= MaxPrivateBytes);
}
const packet_t = struct {
    bytes: [MaxPrivateBytes]u8 = undefined,
    used: usize = 0,
    /// Borrow exclusive private staging; validated quotas prove each scalar fits.
    /// No allocations/retention; invariant failure traps, helper-compatible success result.
    pub fn put(self: *packet_t, comptime value_t: type, value: value_t) !void {
        const count = @sizeOf(value_t);
        // Quotas checked before traversal prove this bound. Keep native safety checks
        // at each append; an invariant violation traps rather than publishing bytes.
        std.debug.assert(self.used <= MaxBoundedBytes and count <= MaxPrivateBytes - self.used);
        std.mem.writeInt(value_t, self.bytes[self.used..][0..count], value, .little);
        self.used += count;
    }
};
fn put(writer: *packet_t, comptime value_t: type, value: value_t) !void {
    try writer.put(value_t, value);
}
fn words(writer: *packet_t, value: anytype, comptime fields: anytype) !void {
    inline for (fields) |field| {
        const scalar = @field(value, field);
        if (@TypeOf(scalar) == f32) {
            try put(writer, u32, @bitCast(scalar));
        } else {
            try put(writer, u32, scalar);
        }
    }
}
fn checked_header(value: anytype, tag: u32) !void {
    if (value.sType != tag or value.pNext != null or value.flags != 0) return error.Invalid;
}
fn state_header(writer: *packet_t, value: anytype, tag: u32) !void {
    try checked_header(value, tag);
    try put(writer, u64, 1);
    try put(writer, u32, tag);
    try put(writer, u64, 0);
    try put(writer, u32, value.flags);
}
fn count_array(writer: *packet_t, count: u32, pointer: anytype, maximum: u32, required: bool) !u32 {
    if (count > maximum or (required and count != 0 and pointer == null)) return error.Invalid;
    try put(writer, u32, count);
    const actual: u32 = if (pointer == null) 0 else count;
    try put(writer, u64, actual);
    return actual;
}
fn elements(pointer: anytype, count: usize) []const @typeInfo(@TypeOf(pointer)).Pointer.child {
    if (count == 0) return &.{};
    return pointer[0..count];
}
fn blob(writer: *packet_t, bytes: []const u8) !void {
    for (bytes) |byte| try put(writer, u8, byte);
    var padding = (4 - bytes.len % 4) % 4;
    while (padding != 0) : (padding -= 1) try put(writer, u8, 0);
}
/// Encode one complete graphics pipeline using0..5 resolved host shader identities.
/// [in] info and reachable native inputs accessible immutable for call, ids match stage
/// order and actual live shader owners; layout/pass/output IDs resolved host identities.
/// pass may be0 only for a supplied PipelineRenderingCreateInfo. Caller validates enabled
/// features, all enum values, float constraints, host limits, shader interfaces and pipeline
/// state compatibility. Native object handles are never serialized without translation.
/// Supports vertex bindings/attributes, topology, viewport/scissors, rasterization,
/// multisampling, depth/stencil, blend, dynamic state and shader specialization.
/// Returns complete owned packet or Invalid/Limit. Validate bounded native topology first,
/// then admit exact encoded length against8192byte public packet capacity before copying.
/// Private16KiB staging follows the fixed quotas above; no partial output, allocations or retention.
pub fn create_graphics_pipeline(device: u64, info: *const c.VkGraphicsPipelineCreateInfo, shader_ids: []const u64, layout: u64, pass: u64, output: u64) !writer_t {
    return create_graphics_pipeline_cached(device, 0, info, shader_ids, layout, pass, output);
}
/// Encode a full general graphics pipeline with translated same-device cache ID or0.
/// Input validation, ownership/lifetime, return errors and thread safety match create_graphics_pipeline.
/// Caller retains cache ownership and verifies device ancestry before submission.
pub fn create_graphics_pipeline_cached(device: u64, cache: u64, info: *const c.VkGraphicsPipelineCreateInfo, shader_ids: []const u64, layout: u64, pass: u64, output: u64) !writer_t {
    if (device == 0 or layout == 0 or output == 0 or info.sType != c.VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO or
        info.stageCount > 5 or info.stageCount != shader_ids.len or (info.stageCount != 0 and info.pStages == null) or
        info.flags & ~@as(u32, 7 | c.VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0 or info.basePipelineHandle != null or info.basePipelineIndex != -1) return error.Invalid;
    const chain = try pipeline_helpers.collect_chain(info.pNext, true);
    if (info.stageCount == 0 and chain.library_count == 0) return error.Invalid;
    var writer: packet_t = .{};
    try put(&writer, u32, 65);
    try put(&writer, u32, 1);
    try put(&writer, u64, device);
    try put(&writer, u64, cache);
    try put(&writer, u32, 1);
    try put(&writer, u64, 1);
    try put(&writer, u32, info.sType);
    try pipeline_helpers.encode_chain(&writer, &chain);
    const dynamic_rendering = chain.rendering;
    if ((pass == 0 and !dynamic_rendering) or (pass != 0 and dynamic_rendering)) return error.Invalid;
    try put(&writer, u32, info.flags);
    try put(&writer, u32, info.stageCount);
    try put(&writer, u64, shader_ids.len);
    var stages: u32 = 0;
    for (shader_ids, 0..) |shader, index| {
        const stage = info.pStages[index];
        if (shader == 0 or stage.stage == 0 or stage.stage > 16 or stage.stage & (stage.stage - 1) != 0 or stage.stage & stages != 0 or stage.pName == null) return error.Invalid;
        try checked_header(stage, c.VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        stages |= stage.stage;
        try put(&writer, u32, stage.sType);
        try put(&writer, u64, 0);
        try put(&writer, u32, stage.flags);
        try put(&writer, u32, stage.stage);
        try put(&writer, u64, shader);
        var length: usize = 0;
        while (length < 64 and stage.pName[length] != 0) : (length += 1) {}
        if (length == 0 or length == 64) return error.Invalid;
        try put(&writer, u64, length + 1);
        try blob(&writer, stage.pName[0 .. length + 1]);
        try pipeline_helpers.encode_specialization(&writer, stage.pSpecializationInfo);
    }
    if (info.pVertexInputState == null) {
        try put(&writer, u64, 0);
    } else {
        const value = info.pVertexInputState[0];
        try state_header(&writer, value, c.VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        const bindings = try count_array(&writer, value.vertexBindingDescriptionCount, value.pVertexBindingDescriptions, 32, true);
        for (elements(value.pVertexBindingDescriptions, bindings)) |binding| try words(&writer, binding, .{ "binding", "stride", "inputRate" });
        const attributes = try count_array(&writer, value.vertexAttributeDescriptionCount, value.pVertexAttributeDescriptions, 32, true);
        for (elements(value.pVertexAttributeDescriptions, attributes)) |attribute| try words(&writer, attribute, .{ "location", "binding", "format", "offset" });
    }
    if (info.pInputAssemblyState == null) {
        try put(&writer, u64, 0);
    } else {
        const value = info.pInputAssemblyState[0];
        try state_header(&writer, value, c.VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        try words(&writer, value, .{ "topology", "primitiveRestartEnable" });
    }
    if (info.pTessellationState == null) {
        try put(&writer, u64, 0);
    } else {
        const value = info.pTessellationState[0];
        try state_header(&writer, value, c.VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO);
        try put(&writer, u32, value.patchControlPoints);
    }
    if (info.pViewportState == null) {
        try put(&writer, u64, 0);
    } else {
        const value = info.pViewportState[0];
        try state_header(&writer, value, c.VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        const viewports = try count_array(&writer, value.viewportCount, value.pViewports, 16, false);
        for (elements(value.pViewports, viewports)) |viewport| try words(&writer, viewport, .{ "x", "y", "width", "height", "minDepth", "maxDepth" });
        const scissors = try count_array(&writer, value.scissorCount, value.pScissors, 16, false);
        for (elements(value.pScissors, scissors)) |scissor| {
            try put(&writer, i32, scissor.offset.x);
            try put(&writer, i32, scissor.offset.y);
            try words(&writer, scissor.extent, .{ "width", "height" });
        }
    }
    if (info.pRasterizationState == null) {
        try put(&writer, u64, 0);
    } else {
        const value = info.pRasterizationState[0];
        try state_header(&writer, value, c.VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        try words(&writer, value, .{ "depthClampEnable", "rasterizerDiscardEnable", "polygonMode", "cullMode", "frontFace", "depthBiasEnable", "depthBiasConstantFactor", "depthBiasClamp", "depthBiasSlopeFactor", "lineWidth" });
    }
    if (info.pMultisampleState == null) {
        try put(&writer, u64, 0);
    } else {
        const value = info.pMultisampleState[0];
        try state_header(&writer, value, c.VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        if (value.rasterizationSamples == 0 or value.rasterizationSamples > 64 or value.rasterizationSamples & (value.rasterizationSamples - 1) != 0) return error.Invalid;
        try words(&writer, value, .{ "rasterizationSamples", "sampleShadingEnable", "minSampleShading" });
        const masks = if (value.pSampleMask != null) (value.rasterizationSamples + 31) / 32 else 0;
        try put(&writer, u64, masks);
        for (elements(value.pSampleMask, masks)) |mask| try put(&writer, u32, mask);
        try words(&writer, value, .{ "alphaToCoverageEnable", "alphaToOneEnable" });
    }
    if (info.pDepthStencilState == null) {
        try put(&writer, u64, 0);
    } else {
        const value = info.pDepthStencilState[0];
        try state_header(&writer, value, c.VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        try words(&writer, value, .{ "depthTestEnable", "depthWriteEnable", "depthCompareOp", "depthBoundsTestEnable", "stencilTestEnable" });
        inline for (.{ value.front, value.back }) |stencil| try words(&writer, stencil, .{ "failOp", "passOp", "depthFailOp", "compareOp", "compareMask", "writeMask", "reference" });
        try words(&writer, value, .{ "minDepthBounds", "maxDepthBounds" });
    }
    if (info.pColorBlendState == null) {
        try put(&writer, u64, 0);
    } else {
        const value = info.pColorBlendState[0];
        try state_header(&writer, value, c.VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        try words(&writer, value, .{ "logicOpEnable", "logicOp" });
        const count = try count_array(&writer, value.attachmentCount, value.pAttachments, 8, true);
        for (elements(value.pAttachments, count)) |attachment| try words(&writer, attachment, .{ "blendEnable", "srcColorBlendFactor", "dstColorBlendFactor", "colorBlendOp", "srcAlphaBlendFactor", "dstAlphaBlendFactor", "alphaBlendOp", "colorWriteMask" });
        try put(&writer, u64, 4);
        for (value.blendConstants) |scalar| try put(&writer, u32, @bitCast(scalar));
    }
    if (info.pDynamicState == null) {
        try put(&writer, u64, 0);
    } else {
        const value = info.pDynamicState[0];
        try state_header(&writer, value, c.VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        const count = try count_array(&writer, value.dynamicStateCount, value.pDynamicStates, 32, true);
        for (elements(value.pDynamicStates, count)) |dynamic| try put(&writer, u32, dynamic);
    }
    try put(&writer, u64, layout);
    try put(&writer, u64, pass);
    try put(&writer, u32, info.subpass);
    try put(&writer, u64, 0);
    try put(&writer, i32, info.basePipelineIndex);
    try put(&writer, u64, 0);
    try put(&writer, u64, 1);
    try put(&writer, u64, output);
    if (writer.used > render.MaxBytes) return error.Limit;
    var output_packet: writer_t = .{};
    @memcpy(output_packet.bytes[0..writer.used], writer.bytes[0..writer.used]);
    output_packet.used = writer.used;
    return output_packet;
}

// Test-only independent generated encoder, never linked into production runtime.
extern fn venus_graphics_general_test_create(*const c.VkGraphicsPipelineCreateInfo, [*]u8) usize;
const fixture_t = struct {
    stages: [3]c.VkPipelineShaderStageCreateInfo = .{
        .{ .sType = 18, .stage = 1, .module = @ptrFromInt(42), .pName = "main" },
        .{ .sType = 18, .stage = 8, .module = @ptrFromInt(43), .pName = "geometry_main" },
        .{ .sType = 18, .stage = 16, .module = @ptrFromInt(47), .pName = "fragment_main" },
    },
    entries: [2]c.VkSpecializationMapEntry = .{ .{ .constantID = 0, .offset = 0, .size = 4 }, .{ .constantID = 7, .offset = 4, .size = 1 } },
    data: [5]u8 = .{ 1, 2, 3, 4, 5 },
    specialization_info: c.VkSpecializationInfo = .{ .mapEntryCount = 2, .dataSize = 5 },
    bindings: [2]c.VkVertexInputBindingDescription = .{ .{ .binding = 0, .stride = 16 }, .{ .binding = 1, .stride = 4, .inputRate = 1 } },
    attributes: [2]c.VkVertexInputAttributeDescription = .{ .{ .location = 0, .format = c.VK_FORMAT_R32G32B32A32_SFLOAT }, .{ .location = 1, .binding = 1, .format = c.VK_FORMAT_R32_UINT } },
    vertex: c.VkPipelineVertexInputStateCreateInfo = .{ .sType = 19, .vertexBindingDescriptionCount = 2, .vertexAttributeDescriptionCount = 2 },
    assembly: c.VkPipelineInputAssemblyStateCreateInfo = .{ .sType = 20, .topology = c.VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, .primitiveRestartEnable = 1 },
    tessellation: c.VkPipelineTessellationStateCreateInfo = .{ .sType = 21, .patchControlPoints = 4 },
    viewports: [2]c.VkViewport = .{ .{ .width = 64, .height = 64, .maxDepth = 1 }, .{ .width = 32, .height = 32, .maxDepth = 1 } },
    scissors: [2]c.VkRect2D = .{ .{ .extent = .{ .width = 64, .height = 64 } }, .{ .offset = .{ .x = 16, .y = 16 }, .extent = .{ .width = 32, .height = 32 } } },
    viewport: c.VkPipelineViewportStateCreateInfo = .{ .sType = 22, .viewportCount = 2, .scissorCount = 2 },
    raster: c.VkPipelineRasterizationStateCreateInfo = .{ .sType = 23, .depthClampEnable = 1, .polygonMode = c.VK_POLYGON_MODE_LINE, .cullMode = 3, .frontFace = 1, .depthBiasEnable = 1, .depthBiasConstantFactor = 0.5, .depthBiasClamp = 0.25, .depthBiasSlopeFactor = 0.125, .lineWidth = 1 },
    masks: [2]u32 = .{ 0x12345678, 0x87654321 },
    multisample: c.VkPipelineMultisampleStateCreateInfo = .{ .sType = 24, .rasterizationSamples = 64, .sampleShadingEnable = 1, .minSampleShading = 0.5, .alphaToCoverageEnable = 1, .alphaToOneEnable = 1 },
    depth: c.VkPipelineDepthStencilStateCreateInfo = .{ .sType = 25, .depthTestEnable = 1, .depthWriteEnable = 1, .depthCompareOp = c.VK_COMPARE_OP_LESS, .depthBoundsTestEnable = 1, .stencilTestEnable = 1, .front = .{ .failOp = 1, .passOp = 2, .depthFailOp = 3, .compareOp = 4, .compareMask = 0xffffffff, .writeMask = 255, .reference = 17 }, .back = .{ .failOp = 4, .passOp = 3, .depthFailOp = 2, .compareOp = 1, .compareMask = 255, .writeMask = 0xffffffff, .reference = 23 }, .minDepthBounds = 0.25, .maxDepthBounds = 0.75 },
    attachments: [2]c.VkPipelineColorBlendAttachmentState = .{ .{ .blendEnable = 1, .srcColorBlendFactor = 1, .dstColorBlendFactor = 2, .colorBlendOp = 3, .srcAlphaBlendFactor = 4, .dstAlphaBlendFactor = 5, .alphaBlendOp = 4, .colorWriteMask = 15 }, .{ .blendEnable = 1, .srcColorBlendFactor = c.VK_BLEND_FACTOR_SRC1_COLOR, .dstColorBlendFactor = c.VK_BLEND_FACTOR_SRC1_ALPHA, .colorWriteMask = 7 } },
    blend: c.VkPipelineColorBlendStateCreateInfo = .{ .sType = 26, .logicOpEnable = 1, .logicOp = c.VK_LOGIC_OP_XOR, .attachmentCount = 2, .blendConstants = .{ 0.5, 0.25, 0.125, 1 } },
    dynamic_values: [3]c.VkDynamicState = .{ c.VK_DYNAMIC_STATE_VIEWPORT, c.VK_DYNAMIC_STATE_SCISSOR, c.VK_DYNAMIC_STATE_BLEND_CONSTANTS },
    dynamic: c.VkPipelineDynamicStateCreateInfo = .{ .sType = 27, .dynamicStateCount = 3 },
    formats: [2]c.VkFormat = .{ c.VK_FORMAT_B8G8R8A8_UNORM, c.VK_FORMAT_R8G8B8A8_UNORM },
    rendering: c.VkPipelineRenderingCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .viewMask = 3, .colorAttachmentCount = 2, .depthAttachmentFormat = c.VK_FORMAT_D32_SFLOAT_S8_UINT, .stencilAttachmentFormat = c.VK_FORMAT_D32_SFLOAT_S8_UINT },
    info: c.VkGraphicsPipelineCreateInfo = .{ .sType = 28, .stageCount = 3, .layout = @ptrFromInt(44), .renderPass = @ptrFromInt(45), .basePipelineIndex = -1 },
    fn link(self: *fixture_t) void {
        self.specialization_info.pMapEntries = &self.entries;
        self.specialization_info.pData = &self.data;
        self.stages[0].pSpecializationInfo = &self.specialization_info;
        self.vertex.pVertexBindingDescriptions = &self.bindings;
        self.vertex.pVertexAttributeDescriptions = &self.attributes;
        self.viewport.pViewports = &self.viewports;
        self.viewport.pScissors = &self.scissors;
        self.multisample.pSampleMask = &self.masks;
        self.blend.pAttachments = &self.attachments;
        self.dynamic.pDynamicStates = &self.dynamic_values;
        self.rendering.pColorAttachmentFormats = &self.formats;
        self.info.pStages = &self.stages;
        self.info.pVertexInputState = &self.vertex;
        self.info.pInputAssemblyState = &self.assembly;
        self.info.pTessellationState = &self.tessellation;
        self.info.pViewportState = &self.viewport;
        self.info.pRasterizationState = &self.raster;
        self.info.pMultisampleState = &self.multisample;
        self.info.pDepthStencilState = &self.depth;
        self.info.pColorBlendState = &self.blend;
        self.info.pDynamicState = &self.dynamic;
    }
};
fn compare_fixture(fixture: *fixture_t) !void {
    var expected: [8192]u8 = undefined;
    const writer = try create_graphics_pipeline(8, &fixture.info, &.{ 42, 43, 47 }, 44, if (fixture.info.renderPass) |handle| @intFromPtr(handle) else 0, 46);
    const used = venus_graphics_general_test_create(&fixture.info, &expected);
    try std.testing.expectEqual(used, writer.used);
    try std.testing.expectEqualSlices(u8, expected[0..used], writer.bytes[0..writer.used]);
}
test "general graphics every core state and specialization matches independent encoder" {
    var fixture: fixture_t = .{};
    fixture.link();
    try compare_fixture(&fixture);
    fixture.info.pNext = &fixture.rendering;
    fixture.info.renderPass = null;
    try compare_fixture(&fixture);
    fixture.viewport.pViewports = null;
    fixture.viewport.pScissors = null;
    try compare_fixture(&fixture);
    fixture.multisample.pSampleMask = null;
    try compare_fixture(&fixture);
    fixture.info.pVertexInputState = null;
    fixture.info.pInputAssemblyState = null;
    fixture.info.pTessellationState = null;
    fixture.info.pViewportState = null;
    fixture.info.pRasterizationState = null;
    fixture.info.pMultisampleState = null;
    fixture.info.pDepthStencilState = null;
    fixture.info.pColorBlendState = null;
    fixture.info.pDynamicState = null;
    fixture.stages[0].pSpecializationInfo = null;
    try compare_fixture(&fixture);
}
test "general graphics rejects untranslated identity and unsafe native arrays" {
    var fixture: fixture_t = .{};
    fixture.link();
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(0, &fixture.info, &.{ 42, 43, 47 }, 44, 45, 46));
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &.{ 42, 0, 47 }, 44, 45, 46));
    fixture.vertex.vertexBindingDescriptionCount = 33;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &.{ 42, 43, 47 }, 44, 45, 46));
    fixture.vertex.vertexBindingDescriptionCount = 2;
    fixture.entries[1].size = 2;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &.{ 42, 43, 47 }, 44, 45, 46));
    fixture.entries[1].size = 1;
    fixture.entries[1].constantID = 0;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &.{ 42, 43, 47 }, 44, 45, 46));
}

extern fn venus_graphics_general_test_cached(*const c.VkGraphicsPipelineCreateInfo, u64, [*]u8) usize;
test "translated graphics pipeline cache matches generated encoder" {
    var fixture: fixture_t = .{};
    fixture.link();
    var expected: [8192]u8 = undefined;
    for ([_]u64{ 0, 52, 0xfedcba9876543210 }) |cache| {
        const writer = try create_graphics_pipeline_cached(8, cache, &fixture.info, &.{ 42, 43, 47 }, 44, 45, 46);
        const used = venus_graphics_general_test_cached(&fixture.info, cache, &expected);
        try std.testing.expectEqualSlices(u8, expected[0..used], writer.bytes[0..writer.used]);
    }
}

test "maintenance5 full width effective flags survive either dynamic rendering chain order" {
    var fixture: fixture_t = .{};
    fixture.link();
    fixture.info.renderPass = null;
    var flags2 = c.VkPipelineCreateFlags2CreateInfo{ .sType = c.VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO, .flags = 0xfedcba9876543210 };
    var expected: [8192]u8 = undefined;
    fixture.rendering.pNext = &flags2;
    fixture.info.pNext = &fixture.rendering;
    var writer = try create_graphics_pipeline_cached(8, 52, &fixture.info, &.{ 42, 43, 47 }, 44, 0, 46);
    var used = venus_graphics_general_test_cached(&fixture.info, 52, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..used], writer.bytes[0..writer.used]);
    fixture.rendering.pNext = null;
    flags2.pNext = &fixture.rendering;
    fixture.info.pNext = &flags2;
    writer = try create_graphics_pipeline_cached(8, 52, &fixture.info, &.{ 42, 43, 47 }, 44, 0, 46);
    used = venus_graphics_general_test_cached(&fixture.info, 52, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..used], writer.bytes[0..writer.used]);
    fixture.rendering.pNext = &flags2;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline_cached(8, 52, &fixture.info, &.{ 42, 43, 47 }, 44, 0, 46));
}

test "graphics pipeline libraries preserve all three chain orders and zero-stage linking" {
    var fixture: fixture_t = .{};
    fixture.link();
    fixture.info.renderPass = null;
    const pipelines = [_]c.VkPipeline{ @ptrFromInt(51), @ptrFromInt(53) };
    var library = c.VkPipelineLibraryCreateInfoKHR{ .sType = c.VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 2, .pLibraries = &pipelines };
    var flags = c.VkPipelineCreateFlags2CreateInfo{ .sType = c.VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO, .flags = 0x800 };
    const orders = [_][3]u32{ .{ 0, 1, 2 }, .{ 0, 2, 1 }, .{ 1, 0, 2 }, .{ 1, 2, 0 }, .{ 2, 0, 1 }, .{ 2, 1, 0 } };
    const pointers = [_]usize{ @intFromPtr(&fixture.rendering), @intFromPtr(&flags), @intFromPtr(&library) };
    for (orders) |order| {
        for (order, 0..) |index, position| {
            const header: *c.VkBaseOutStructure = @ptrFromInt(pointers[index]);
            header.pNext = if (position == 2) null else @ptrFromInt(pointers[order[position + 1]]);
        }
        fixture.info.pNext = @ptrFromInt(pointers[order[0]]);
        try compare_fixture(&fixture);
    }
    fixture.rendering.pNext = &library;
    library.pNext = null;
    fixture.info.pNext = &fixture.rendering;
    fixture.info.stageCount = 0;
    fixture.info.pStages = null;
    var expected: [8192]u8 = undefined;
    const packet = try create_graphics_pipeline(8, &fixture.info, &.{}, 44, 0, 46);
    const used = venus_graphics_general_test_create(&fixture.info, &expected);
    try std.testing.expectEqualSlices(u8, expected[0..used], packet.bytes[0..packet.used]);
    library.libraryCount = 65;
    library.pLibraries = @ptrFromInt(8);
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &.{}, 44, 0, 46));
    library.libraryCount = 1;
    library.pLibraries = null;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &.{}, 44, 0, 46));
    library.libraryCount = 0;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &.{}, 44, 0, 46));
}

test "graphics native headers stage names topology state counts and multisample bounds" {
    var fixture: fixture_t = .{};
    fixture.link();
    const ids = [_]u64{ 42, 43, 47 };
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 0, 45, 46));
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 0));
    inline for (.{ "sType", "stageCount", "flags", "basePipelineIndex" }) |field| {
        fixture = .{};
        fixture.link();
        @field(fixture.info, field) = if (comptime std.mem.eql(u8, field, "sType")) 0 else if (comptime std.mem.eql(u8, field, "stageCount")) 6 else if (comptime std.mem.eql(u8, field, "flags")) 8 else 0;
        try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    }
    fixture = .{};
    fixture.link();
    fixture.info.pStages = null;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    fixture.link();
    fixture.info.basePipelineHandle = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    fixture.info.basePipelineHandle = null;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 0, 46));
    fixture.info.pNext = &fixture.rendering;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    for ([_]u32{ 0, 3, 32, 1 }) |stage| {
        fixture = .{};
        fixture.link();
        fixture.stages[1].stage = stage;
        try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    }
    fixture = .{};
    fixture.link();
    fixture.stages[0].pName = null;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    fixture.stages[0].pName = "";
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    const long_name = [_:0]u8{'a'} ** 64;
    fixture.stages[0].pName = &long_name;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    inline for (.{ "stages", "vertex", "assembly", "tessellation", "viewport", "raster", "multisample", "depth", "blend", "dynamic" }) |field| {
        fixture = .{};
        fixture.link();
        if (comptime std.mem.eql(u8, field, "stages")) fixture.stages[0].sType = 0 else @field(fixture, field).sType = 0;
        try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    }
    fixture = .{};
    fixture.link();
    fixture.vertex.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    fixture.vertex.pNext = null;
    fixture.vertex.flags = 1;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    for ([_]u32{ 0, 3, 128 }) |samples| {
        fixture = .{};
        fixture.link();
        fixture.multisample.rasterizationSamples = samples;
        try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
    }
    fixture = .{};
    fixture.link();
    fixture.vertex.pVertexAttributeDescriptions = null;
    try std.testing.expectError(error.Invalid, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
}

test "complete bounded graphics staging rejects public packet overflow without publication" {
    var fixture: fixture_t = .{};
    fixture.link();
    var entries: [32]c.VkSpecializationMapEntry = undefined;
    for (&entries, 0..) |*entry, index| entry.* = .{ .constantID = @intCast(index), .offset = @intCast(index * 4), .size = 4 };
    const data = [_]u8{0} ** 1024;
    const specialization: c.VkSpecializationInfo = .{ .mapEntryCount = 32, .pMapEntries = &entries, .dataSize = data.len, .pData = &data };
    var stages: [5]c.VkPipelineShaderStageCreateInfo = undefined;
    const ids = [_]u64{ 42, 43, 44, 45, 46 };
    for (&stages, ids, 0..) |*stage, id, index| stage.* = .{ .sType = c.VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = @as(u32, 1) << @as(u5, @intCast(index)), .module = @ptrFromInt(id), .pName = "main", .pSpecializationInfo = &specialization };
    fixture.info.stageCount = 5;
    fixture.info.pStages = &stages;
    try std.testing.expectError(error.Limit, create_graphics_pipeline(8, &fixture.info, &ids, 44, 45, 46));
}

test "pipeline helper borrowed topology validation and every encoded prefix capacity" {
    var formats = [_]c.VkFormat{37};
    var rendering: c.VkPipelineRenderingCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &formats };
    var native_libraries = [_]c.VkPipeline{ @ptrFromInt(52), @ptrFromInt(53) };
    var library: c.VkPipelineLibraryCreateInfoKHR = .{ .sType = c.VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .pNext = &rendering, .libraryCount = 2, .pLibraries = &native_libraries };
    var flags: c.VkPipelineCreateFlags2CreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO, .pNext = &library, .flags = 1 };
    const initial = try pipeline_helpers.collect_chain(&flags, true);
    var writer: render.writer_t = .{};
    try pipeline_helpers.encode_chain(&writer, &initial);
    const encoded_bytes = writer.used;
    for (0..encoded_bytes) |available| {
        writer = .{ .used = render.MaxBytes - available };
        try std.testing.expectError(error.Limit, pipeline_helpers.encode_chain(&writer, &initial));
        try std.testing.expect(writer.used <= render.MaxBytes);
    }
    try std.testing.expectError(error.Invalid, pipeline_helpers.collect_chain(&rendering, false));
    try std.testing.expectError(error.Invalid, pipeline_helpers.collect_chain(&library, false));
    try std.testing.expectError(error.Invalid, pipeline_helpers.collect_chain(@ptrFromInt(3), true));
    var duplicate: c.VkPipelineCreateFlags2CreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO };
    flags.pNext = &duplicate;
    try std.testing.expectError(error.Invalid, pipeline_helpers.collect_chain(&flags, true));
    flags.pNext = &library;
    rendering.colorAttachmentCount = 9;
    try std.testing.expectError(error.Invalid, pipeline_helpers.collect_chain(&flags, true));
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = null;
    try std.testing.expectError(error.Invalid, pipeline_helpers.collect_chain(&flags, true));
    rendering.pColorAttachmentFormats = &formats;
    library.libraryCount = 65;
    try std.testing.expectError(error.Invalid, pipeline_helpers.collect_chain(&flags, true));
    library.libraryCount = 2;
    native_libraries[1] = null;
    try std.testing.expectError(error.Invalid, pipeline_helpers.collect_chain(&flags, true));
    writer = .{};
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_chain(&writer, &initial));
    native_libraries[1] = @ptrFromInt(53);
    var changed = initial;
    changed.count = 4;
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_chain(&writer, &changed));
    changed = initial;
    changed.addresses[0] = 0;
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_chain(&writer, &changed));
    changed.addresses[0] = 3;
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_chain(&writer, &changed));
    flags.sType = 0;
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_chain(&writer, &initial));
    changed = .{ .count = 1 };
    changed.addresses[0] = @intFromPtr(&flags);
    changed.tags[0] = 0;
    flags.pNext = null;
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_chain(&writer, &changed));
    flags.sType = c.VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_chain(&writer, &initial));
    flags.pNext = &library;
    library.libraryCount = 1;
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_chain(&writer, &initial));
    library.libraryCount = 65;
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_chain(&writer, &initial));
    library.libraryCount = 2;
    rendering.colorAttachmentCount = 9;
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_chain(&writer, &initial));
}

test "specialization helper arbitrary writer prefixes data quota and map bounds" {
    const data = [_]u8{ 0, 1, 2, 3, 4 };
    var entries = [_]c.VkSpecializationMapEntry{ .{ .constantID = 0, .offset = 0, .size = 4 }, .{ .constantID = 1, .offset = 4, .size = 1 } };
    var info: c.VkSpecializationInfo = .{ .mapEntryCount = 2, .pMapEntries = &entries, .dataSize = 5, .pData = &data };
    var writer: render.writer_t = .{};
    try pipeline_helpers.encode_specialization(&writer, &info);
    const encoded_bytes = writer.used;
    for (0..encoded_bytes) |available| {
        writer = .{ .used = render.MaxBytes - available };
        try std.testing.expectError(error.Limit, pipeline_helpers.encode_specialization(&writer, &info));
        try std.testing.expect(writer.used <= render.MaxBytes);
    }
    writer = .{};
    info.dataSize = pipeline_helpers.MaxSpecializationData + 1;
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_specialization(&writer, &info));
    info.dataSize = 5;
    entries[0].offset = 6;
    try std.testing.expectError(error.Invalid, pipeline_helpers.encode_specialization(&writer, &info));
}
