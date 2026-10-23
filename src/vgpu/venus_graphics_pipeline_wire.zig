//! Allocation-free canonical Vulkan 1.0 graphics pipeline encoder.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Owned8192-byte packet storage; no allocation, caller releases by scope exit.
pub const writer_t = render.writer_t;
fn put(writer: *writer_t, comptime word_t: type, value: word_t) void {
    writer.put(word_t, value) catch unreachable;
}
fn header_valid(value: anytype, expected: c.VkStructureType) bool {
    return value.sType == expected and value.pNext == null and value.flags == 0;
}
fn zeros(value: anytype, comptime names: anytype) bool {
    inline for (names) |name| {
        if (@field(value, name) != 0) return false;
    }
    return true;
}
fn state(writer: *writer_t, tag: u32) void {
    put(writer, u64, 1);
    put(writer, u32, tag);
    put(writer, u64, 0);
    put(writer, u32, 0);
}
fn float_word(writer: *writer_t, value: f32) void {
    put(writer, u32, @bitCast(value));
}
/// Encode core command65 for one canonical vertex/fragment triangle pipeline.
/// in: info and reachable records are nonnull accessible borrowed call-lifetime storage;
/// device/shader/layout/pass/output IDs are resolved nonzero host IDs, never guest handles.
/// out: returned writer exclusively owns bytes; no heap/global state. Invalid profiles
/// return error.Invalid before serialization; thread-safe on disjoint borrowed inputs.
pub fn create_graphics_pipeline(device_id: u64, info: *const c.VkGraphicsPipelineCreateInfo, vertex_id: u64, fragment_id: u64, layout_id: u64, pass_id: u64, output_id: u64) !writer_t {
    for ([_]u64{ device_id, vertex_id, fragment_id, layout_id, pass_id, output_id }) |id| if (id == 0) return error.Invalid;
    if (!header_valid(info.*, c.VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO) or info.stageCount != 2 or info.pStages == null or info.subpass != 0 or info.basePipelineHandle != null or (info.basePipelineIndex != -1 and info.basePipelineIndex != 0)) return error.Invalid;
    if (info.pVertexInputState == null or info.pInputAssemblyState == null or info.pViewportState == null or info.pRasterizationState == null or info.pMultisampleState == null or info.pColorBlendState == null or info.pTessellationState != null or info.pDepthStencilState != null or info.pDynamicState != null) return error.Invalid;
    for (0..2) |index| {
        const stage = info.pStages[index];
        if (!header_valid(stage, c.VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO) or stage.stage != (if (index == 0) @as(c.VkShaderStageFlagBits, c.VK_SHADER_STAGE_VERTEX_BIT) else c.VK_SHADER_STAGE_FRAGMENT_BIT) or stage.pName == null or stage.pSpecializationInfo != null) return error.Invalid;
        for ("main\x00", 0..) |byte, name_index| {
            if (stage.pName[name_index] != byte) return error.Invalid;
        }
    }
    const vertex = info.pVertexInputState[0];
    const assembly = info.pInputAssemblyState[0];
    const viewport_state = info.pViewportState[0];
    const raster = info.pRasterizationState[0];
    const multisample = info.pMultisampleState[0];
    const blend = info.pColorBlendState[0];
    if (!header_valid(vertex, c.VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO) or vertex.vertexBindingDescriptionCount != 0 or vertex.vertexAttributeDescriptionCount != 0 or vertex.pVertexBindingDescriptions != null or vertex.pVertexAttributeDescriptions != null) return error.Invalid;
    if (!header_valid(assembly, c.VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO) or assembly.topology != c.VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST or assembly.primitiveRestartEnable != 0) return error.Invalid;
    if (!header_valid(viewport_state, c.VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO) or viewport_state.viewportCount != 1 or viewport_state.scissorCount != 1 or viewport_state.pViewports == null or viewport_state.pScissors == null) return error.Invalid;
    const viewport = viewport_state.pViewports[0];
    inline for (.{ "x", "y", "width", "height", "minDepth", "maxDepth" }) |name| if (!std.math.isFinite(@field(viewport, name))) return error.Invalid;
    if (viewport.x < 0 or viewport.y < 0 or viewport.width < 1 or viewport.height < 1 or viewport.x + viewport.width > 4096 or viewport.y + viewport.height > 4096 or viewport.minDepth != 0 or viewport.maxDepth != 1) return error.Invalid;
    const scissor = viewport_state.pScissors[0];
    if (scissor.offset.x < 0 or scissor.offset.y < 0 or scissor.extent.width == 0 or scissor.extent.height == 0 or @as(u64, @intCast(scissor.offset.x)) + scissor.extent.width > 4096 or @as(u64, @intCast(scissor.offset.y)) + scissor.extent.height > 4096) return error.Invalid;
    if (!header_valid(raster, c.VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO) or !zeros(raster, .{ "depthClampEnable", "rasterizerDiscardEnable", "polygonMode", "depthBiasEnable", "depthBiasConstantFactor", "depthBiasClamp", "depthBiasSlopeFactor" }) or raster.cullMode > 3 or raster.frontFace > 1 or raster.lineWidth != 1) return error.Invalid;
    if (!header_valid(multisample, c.VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO) or multisample.rasterizationSamples != 1 or !zeros(multisample, .{ "sampleShadingEnable", "minSampleShading", "alphaToCoverageEnable", "alphaToOneEnable" }) or multisample.pSampleMask != null) return error.Invalid;
    if (!header_valid(blend, c.VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO) or blend.logicOpEnable != 0 or blend.logicOp != 0 or blend.attachmentCount != 1 or blend.pAttachments == null) return error.Invalid;
    const attachment = blend.pAttachments[0];
    if (!zeros(attachment, .{ "blendEnable", "srcColorBlendFactor", "dstColorBlendFactor", "colorBlendOp", "srcAlphaBlendFactor", "dstAlphaBlendFactor", "alphaBlendOp" }) or attachment.colorWriteMask != 15) return error.Invalid;
    for (blend.blendConstants) |value| if (value != 0) return error.Invalid;
    // Fixed packet contains no variable-length accepted arrays or strings. The
    // oracle asserts its exact size; this compile-time bound dominates all puts.
    comptime {
        std.debug.assert(632 <= render.MaxBytes);
    }
    var writer: writer_t = .{};
    put(&writer, u32, 65);
    put(&writer, u32, 1);
    put(&writer, u64, device_id);
    put(&writer, u64, 0);
    put(&writer, u32, 1);
    put(&writer, u64, 1);
    put(&writer, u32, 28);
    put(&writer, u64, 0);
    put(&writer, u32, 0);
    put(&writer, u32, 2);
    put(&writer, u64, 2);
    for ([_]u64{ vertex_id, fragment_id }, 0..) |shader_id, index| {
        put(&writer, u32, 18);
        put(&writer, u64, 0);
        put(&writer, u32, 0);
        put(&writer, u32, if (index == 0) 1 else 16);
        put(&writer, u64, shader_id);
        put(&writer, u64, 5);
        for ("main\x00\x00\x00\x00") |byte| put(&writer, u8, byte);
        put(&writer, u64, 0);
    }
    state(&writer, 19);
    put(&writer, u32, 0);
    put(&writer, u64, 0);
    put(&writer, u32, 0);
    put(&writer, u64, 0);
    state(&writer, 20);
    put(&writer, u32, 3);
    put(&writer, u32, 0);
    put(&writer, u64, 0);
    state(&writer, 22);
    put(&writer, u32, 1);
    put(&writer, u64, 1);
    inline for (.{ "x", "y", "width", "height", "minDepth", "maxDepth" }) |name| float_word(&writer, @field(viewport, name));
    put(&writer, u32, 1);
    put(&writer, u64, 1);
    put(&writer, i32, scissor.offset.x);
    put(&writer, i32, scissor.offset.y);
    put(&writer, u32, scissor.extent.width);
    put(&writer, u32, scissor.extent.height);
    state(&writer, 23);
    inline for (.{ "depthClampEnable", "rasterizerDiscardEnable", "polygonMode", "cullMode", "frontFace", "depthBiasEnable" }) |name| put(&writer, u32, @field(raster, name));
    inline for (.{ "depthBiasConstantFactor", "depthBiasClamp", "depthBiasSlopeFactor", "lineWidth" }) |name| float_word(&writer, @field(raster, name));
    state(&writer, 24);
    put(&writer, u32, 1);
    put(&writer, u32, 0);
    float_word(&writer, multisample.minSampleShading);
    put(&writer, u64, 0);
    put(&writer, u32, 0);
    put(&writer, u32, 0);
    put(&writer, u64, 0);
    state(&writer, 26);
    put(&writer, u32, 0);
    put(&writer, u32, 0);
    put(&writer, u32, 1);
    put(&writer, u64, 1);
    inline for (.{ "blendEnable", "srcColorBlendFactor", "dstColorBlendFactor", "colorBlendOp", "srcAlphaBlendFactor", "dstAlphaBlendFactor", "alphaBlendOp", "colorWriteMask" }) |name| put(&writer, u32, @field(attachment, name));
    put(&writer, u64, 4);
    for (blend.blendConstants) |value| float_word(&writer, value);
    put(&writer, u64, 0);
    put(&writer, u64, layout_id);
    put(&writer, u64, pass_id);
    put(&writer, u32, 0);
    put(&writer, u64, 0);
    put(&writer, i32, info.basePipelineIndex);
    put(&writer, u64, 0);
    put(&writer, u64, 1);
    put(&writer, u64, output_id);
    return writer;
}

// Test-only fixtures.
extern fn venus_graphics_pipeline_test_create(info: *const c.VkGraphicsPipelineCreateInfo, output: [*]u8) usize;
const fixture_t = struct {
    stages: [2]c.VkPipelineShaderStageCreateInfo = .{
        .{ .sType = 18, .stage = 1, .pName = "main" },
        .{ .sType = 18, .stage = 16, .pName = "main" },
    },
    vertex: c.VkPipelineVertexInputStateCreateInfo = .{ .sType = 19 },
    assembly: c.VkPipelineInputAssemblyStateCreateInfo = .{ .sType = 20, .topology = 3 },
    viewport: c.VkViewport = .{ .width = 64, .height = 64, .maxDepth = 1 },
    scissor: c.VkRect2D = .{ .extent = .{ .width = 64, .height = 64 } },
    viewport_state: c.VkPipelineViewportStateCreateInfo = .{ .sType = 22, .viewportCount = 1, .scissorCount = 1 },
    raster: c.VkPipelineRasterizationStateCreateInfo = .{ .sType = 23, .lineWidth = 1 },
    multisample: c.VkPipelineMultisampleStateCreateInfo = .{ .sType = 24, .rasterizationSamples = 1 },
    attachment: c.VkPipelineColorBlendAttachmentState = .{ .colorWriteMask = 15 },
    blend: c.VkPipelineColorBlendStateCreateInfo = .{ .sType = 26, .attachmentCount = 1 },
    info: c.VkGraphicsPipelineCreateInfo = .{ .sType = 28, .stageCount = 2, .basePipelineIndex = -1 },
    fn link(self: *fixture_t) void {
        self.info.pStages = &self.stages;
        self.info.pVertexInputState = &self.vertex;
        self.info.pInputAssemblyState = &self.assembly;
        self.info.pViewportState = &self.viewport_state;
        self.info.pRasterizationState = &self.raster;
        self.info.pMultisampleState = &self.multisample;
        self.info.pColorBlendState = &self.blend;
        self.viewport_state.pViewports = &self.viewport;
        self.viewport_state.pScissors = &self.scissor;
        self.blend.pAttachments = &self.attachment;
    }
    fn encode(self: *fixture_t) !writer_t {
        return create_graphics_pipeline(7, &self.info, 42, 43, 44, 45, 46);
    }
};
test "graphics pipeline packet agrees with pinned generated serializer" {
    var fixture: fixture_t = .{};
    fixture.link();
    for (0..4) |cull| {
        fixture.raster.cullMode = @intCast(cull);
        fixture.raster.frontFace = @intCast(cull % 2);
        fixture.info.basePipelineIndex = if (cull % 2 == 0) -1 else 0;
        const writer = try fixture.encode();
        var oracle: [8192]u8 = undefined;
        const used = venus_graphics_pipeline_test_create(&fixture.info, &oracle);
        try std.testing.expectEqual(@as(usize, 632), writer.used);
        try std.testing.expectEqual(used, writer.used);
        try std.testing.expectEqualSlices(u8, oracle[0..used], writer.bytes[0..writer.used]);
    }
}
test "graphics pipeline rejects independently malformed scalar fields" {
    inline for (.{ "info", "stages", "vertex", "assembly", "viewport_state", "raster", "multisample", "blend", "attachment", "viewport" }) |record_name| {
        const record_t = if (comptime std.mem.eql(u8, record_name, "stages")) c.VkPipelineShaderStageCreateInfo else @TypeOf(@field(@as(fixture_t, undefined), record_name));
        inline for (@typeInfo(record_t).Struct.fields) |field| {
            const field_info = @typeInfo(field.type);
            if (field_info == .Int or field_info == .Float) {
                var fixture: fixture_t = .{};
                fixture.link();
                const record = if (comptime std.mem.eql(u8, record_name, "stages")) &fixture.stages[0] else &@field(fixture, record_name);
                const before = @field(record, field.name);
                @field(record, field.name) = if (field_info == .Float) std.math.nan(f32) else if (field_info.Int.signedness == .signed) 123 else 123;
                // Cull/front-face and count fields are deliberately outside their range.
                _ = before;
                try std.testing.expectError(error.Invalid, fixture.encode());
            }
        }
    }
}
test "graphics pipeline rejects null required pointers and extension chains" {
    inline for (.{ "info", "vertex", "assembly", "viewport_state", "raster", "multisample", "blend" }) |record_name| {
        inline for (@typeInfo(@TypeOf(@field(@as(fixture_t, undefined), record_name))).Struct.fields) |field| {
            if (comptime @typeInfo(field.type) == .Pointer or @typeInfo(field.type) == .Optional) {
                if (comptime std.mem.eql(u8, field.name, "layout") or std.mem.eql(u8, field.name, "renderPass")) continue;
                var fixture: fixture_t = .{};
                fixture.link();
                const record = &@field(fixture, record_name);
                if (@field(record, field.name) == null) {
                    @field(record, field.name) = @ptrFromInt(4096);
                } else {
                    @field(record, field.name) = null;
                }
                try std.testing.expectError(error.Invalid, fixture.encode());
            }
        }
    }
    inline for (0..2) |index| {
        inline for (.{ "pNext", "pName", "pSpecializationInfo" }) |field_name| {
            var fixture: fixture_t = .{};
            fixture.link();
            if (comptime std.mem.eql(u8, field_name, "pName")) fixture.stages[index].pName = null else @field(fixture.stages[index], field_name) = @ptrFromInt(4096);
            try std.testing.expectError(error.Invalid, fixture.encode());
        }
        var fixture: fixture_t = .{};
        fixture.link();
        fixture.stages[index].pName = "other";
        try std.testing.expectError(error.Invalid, fixture.encode());
    }
}
test "graphics pipeline validates every translated object identifier" {
    var fixture: fixture_t = .{};
    fixture.link();
    for (0..6) |index| {
        var ids = [_]u64{ 7, 42, 43, 44, 45, 46 };
        ids[index] = 0;
        try std.testing.expectError(error.Invalid, create_graphics_pipeline(ids[0], &fixture.info, ids[1], ids[2], ids[3], ids[4], ids[5]));
    }
}

test "graphics viewport and scissor finite extent boundaries" {
    inline for (.{ "x", "y", "width", "height", "minDepth", "maxDepth" }) |name| {
        for ([_]f32{ -1, 0, 4097, std.math.inf(f32), -std.math.inf(f32) }) |value| {
            var fixture: fixture_t = .{};
            fixture.link();
            @field(fixture.viewport, name) = value;
            const valid = (comptime (std.mem.eql(u8, name, "x") or std.mem.eql(u8, name, "y") or std.mem.eql(u8, name, "minDepth"))) and value == 0;
            if (valid) {
                _ = try fixture.encode();
            } else try std.testing.expectError(error.Invalid, fixture.encode());
        }
    }
    inline for (.{ "x", "y" }) |name| {
        for ([_]i32{ -1, 4096, std.math.maxInt(i32) }) |value| {
            var fixture: fixture_t = .{};
            fixture.link();
            @field(fixture.scissor.offset, name) = value;
            try std.testing.expectError(error.Invalid, fixture.encode());
        }
    }
    inline for (.{ "width", "height" }) |name| {
        for ([_]u32{ 0, 4097, std.math.maxInt(u32) }) |value| {
            var fixture: fixture_t = .{};
            fixture.link();
            @field(fixture.scissor.extent, name) = value;
            try std.testing.expectError(error.Invalid, fixture.encode());
        }
    }
    var fixture: fixture_t = .{};
    fixture.link();
    fixture.viewport.width = 4096;
    fixture.viewport.height = 4096;
    fixture.scissor.extent = .{ .width = 4096, .height = 4096 };
    const writer = try fixture.encode();
    var oracle: [8192]u8 = undefined;
    const used = venus_graphics_pipeline_test_create(&fixture.info, &oracle);
    try std.testing.expectEqualSlices(u8, oracle[0..used], writer.bytes[0..writer.used]);
    fixture.stages[0].pName = "";
    try std.testing.expectError(error.Invalid, fixture.encode());
    fixture.stages[0].pName = "m";
    try std.testing.expectError(error.Invalid, fixture.encode());
    fixture.stages[0].pName = "ma";
    try std.testing.expectError(error.Invalid, fixture.encode());
    fixture.stages[0].pName = "mai";
    try std.testing.expectError(error.Invalid, fixture.encode());
    fixture.stages[0].pName = "mainx";
    try std.testing.expectError(error.Invalid, fixture.encode());
    inline for (0..4) |index| {
        fixture.stages[0].pName = "main";
        fixture.blend.blendConstants[index] = 1;
        try std.testing.expectError(error.Invalid, fixture.encode());
        fixture.blend.blendConstants[index] = 0;
    }
}
