//! Bounded single-color render pass wire profile; caller owns native objects and GPU lifetimes.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Owned8192-byte scratch alias; thread-safe on distinct writers and allocation-free.
pub const writer_t = render.writer_t;
const MaxRenderExtent: u32 = 4096;
fn put(writer: *writer_t, comptime word_t: type, value: word_t) void {
    // All builders prove complete packet bounds before invoking this helper.
    writer.put(word_t, value) catch unreachable;
}
fn finish(writer: *writer_t, output_id: u64) void {
    put(writer, u64, 0);
    put(writer, u64, 1);
    put(writer, u64, output_id);
}
/// Encode a core single-color render pass. [in] info/arrays borrowed accessible for this call.
/// [in] device_id/pass_id nonzero translated host identities; no native handle is retained.
/// Returns owned packet or Invalid before publication; no allocation or shared mutable state.
/// Caller validates host format support and maintains compatible image/command lifetimes.
pub fn create_render_pass(info: *const c.VkRenderPassCreateInfo, device_id: u64, pass_id: u64) !writer_t {
    if (device_id == 0 or pass_id == 0 or info.sType != c.VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO or
        info.pNext != null or info.flags != 0 or info.attachmentCount != 1 or info.pAttachments == null or
        info.subpassCount != 1 or info.pSubpasses == null or info.dependencyCount > 16 or
        (info.dependencyCount != 0 and info.pDependencies == null)) return error.Invalid;
    const attachment = info.pAttachments[0];
    if (attachment.flags != 0 or (attachment.format != c.VK_FORMAT_R8G8B8A8_UNORM and attachment.format != c.VK_FORMAT_B8G8R8A8_UNORM) or
        attachment.samples != c.VK_SAMPLE_COUNT_1_BIT or attachment.loadOp > 2 or attachment.storeOp > 1 or
        attachment.stencilLoadOp > 2 or attachment.stencilStoreOp > 1 or
        (attachment.initialLayout != 0 and attachment.initialLayout != 1 and attachment.initialLayout != 2) or
        (attachment.finalLayout != 1 and attachment.finalLayout != 2 and attachment.finalLayout != 5)) return error.Invalid;
    const subpass = info.pSubpasses[0];
    if (subpass.flags != 0 or subpass.pipelineBindPoint != c.VK_PIPELINE_BIND_POINT_GRAPHICS or
        subpass.inputAttachmentCount != 0 or subpass.colorAttachmentCount != 1 or subpass.pColorAttachments == null or
        subpass.pResolveAttachments != null or subpass.pDepthStencilAttachment != null or subpass.preserveAttachmentCount != 0)
        return error.Invalid;
    if (subpass.pColorAttachments[0].attachment != 0 or subpass.pColorAttachments[0].layout != c.VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        return error.Invalid;
    if (info.dependencyCount != 0) for (info.pDependencies[0..info.dependencyCount]) |dependency| {
        if ((dependency.srcSubpass != 0 and dependency.srcSubpass != c.VK_SUBPASS_EXTERNAL) or
            (dependency.dstSubpass != 0 and dependency.dstSubpass != c.VK_SUBPASS_EXTERNAL) or
            (dependency.srcSubpass == c.VK_SUBPASS_EXTERNAL and dependency.dstSubpass == c.VK_SUBPASS_EXTERNAL) or
            dependency.srcStageMask == 0 or dependency.dstStageMask == 0 or
            (dependency.srcStageMask | dependency.dstStageMask) & ~@as(u32, 0x1ffff) != 0 or
            (dependency.srcAccessMask | dependency.dstAccessMask) & ~@as(u32, 0x1ffff) != 0 or dependency.dependencyFlags > 1)
            return error.Invalid;
    };
    // Exact bound:204+28*16=652 bytes, well within the owned scratch.
    var writer = writer_t{};
    writer.header(82, device_id) catch unreachable;
    put(&writer, u64, 1);
    put(&writer, u32, c.VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    put(&writer, u64, 0);
    put(&writer, u32, 0);
    put(&writer, u32, 1);
    put(&writer, u64, 1);
    for ([_]u32{ attachment.flags, @intCast(attachment.format), attachment.samples, attachment.loadOp, attachment.storeOp, attachment.stencilLoadOp, attachment.stencilStoreOp, attachment.initialLayout, attachment.finalLayout }) |word| put(&writer, u32, word);
    put(&writer, u32, 1);
    put(&writer, u64, 1);
    put(&writer, u32, 0);
    put(&writer, u32, 0);
    put(&writer, u32, 0);
    put(&writer, u64, 0);
    put(&writer, u32, 1);
    put(&writer, u64, 1);
    put(&writer, u32, 0);
    put(&writer, u32, c.VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    put(&writer, u64, 0);
    put(&writer, u64, 0);
    put(&writer, u32, 0);
    put(&writer, u64, 0);
    put(&writer, u32, info.dependencyCount);
    put(&writer, u64, info.dependencyCount);
    if (info.dependencyCount != 0) for (info.pDependencies[0..info.dependencyCount]) |dependency|
        for ([_]u32{ dependency.srcSubpass, dependency.dstSubpass, dependency.srcStageMask, dependency.dstStageMask, dependency.srcAccessMask, dependency.dstAccessMask, dependency.dependencyFlags }) |word| put(&writer, u32, word);
    finish(&writer, pass_id);
    std.debug.assert(writer.used == 204 + 28 * @as(usize, info.dependencyCount));
    return writer;
}
/// Encode one-view framebuffer. [in] info borrowed canonical native record; native handles ignored.
/// [in] IDs nonzero translated device/render-pass/view/output identities.
/// Returns owned104-byte packet or Invalid; no allocations, pointer retention or locks.
/// Caller verifies compatible view extent/format/usage and retains actual GPU dependencies.
pub fn create_framebuffer(info: *const c.VkFramebufferCreateInfo, device_id: u64, pass_id: u64, view_id: u64, framebuffer_id: u64) !writer_t {
    if (device_id == 0 or pass_id == 0 or view_id == 0 or framebuffer_id == 0 or
        info.sType != c.VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO or info.pNext != null or info.flags != 0 or
        info.attachmentCount != 1 or info.width == 0 or info.width > MaxRenderExtent or
        info.height == 0 or info.height > MaxRenderExtent or info.layers != 1) return error.Invalid;
    var writer = writer_t{};
    writer.header(80, device_id) catch unreachable;
    put(&writer, u64, 1);
    put(&writer, u32, c.VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    put(&writer, u64, 0);
    put(&writer, u32, 0);
    put(&writer, u64, pass_id);
    put(&writer, u32, 1);
    put(&writer, u64, 1);
    put(&writer, u64, view_id);
    put(&writer, u32, info.width);
    put(&writer, u32, info.height);
    put(&writer, u32, 1);
    finish(&writer, framebuffer_id);
    std.debug.assert(writer.used == 104);
    return writer;
}
/// Encode inline begin with one color clear. [in] info/clear value accessible borrowed for call.
/// [in] command_id/pass_id/framebuffer_id translated nonzero identities; contents must INLINE.
/// Returns owned116-byte packet or Invalid; allocation-free/thread-safe on distinct returned writers.
/// Caller validates area against actual framebuffer and recording/render-pass state, retaining dependencies.
pub fn begin_render_pass(info: *const c.VkRenderPassBeginInfo, command_id: u64, pass_id: u64, framebuffer_id: u64, contents: u32) !writer_t {
    if (command_id == 0 or pass_id == 0 or framebuffer_id == 0 or contents != c.VK_SUBPASS_CONTENTS_INLINE or
        info.sType != c.VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO or info.pNext != null or
        info.clearValueCount != 1 or info.pClearValues == null or info.renderArea.offset.x < 0 or info.renderArea.offset.y < 0 or
        info.renderArea.offset.x >= MaxRenderExtent or info.renderArea.offset.y >= MaxRenderExtent or
        info.renderArea.extent.width == 0 or info.renderArea.extent.height == 0 or
        info.renderArea.extent.width > MaxRenderExtent - @as(u32, @intCast(info.renderArea.offset.x)) or
        info.renderArea.extent.height > MaxRenderExtent - @as(u32, @intCast(info.renderArea.offset.y))) return error.Invalid;
    var writer = writer_t{};
    writer.header(133, command_id) catch unreachable;
    put(&writer, u64, 1);
    put(&writer, u32, c.VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    put(&writer, u64, 0);
    put(&writer, u64, pass_id);
    put(&writer, u64, framebuffer_id);
    put(&writer, i32, info.renderArea.offset.x);
    put(&writer, i32, info.renderArea.offset.y);
    put(&writer, u32, info.renderArea.extent.width);
    put(&writer, u32, info.renderArea.extent.height);
    put(&writer, u32, 1);
    put(&writer, u64, 1);
    put(&writer, u32, 0);
    put(&writer, u32, 2);
    put(&writer, u64, 4);
    for (info.pClearValues[0].color.uint32) |word| put(&writer, u32, word);
    put(&writer, u32, contents);
    std.debug.assert(writer.used == 116);
    return writer;
}

// Test-only fixtures.
extern fn venus_graphics_test_pass(*const c.VkRenderPassCreateInfo, [*]u8) usize;
extern fn venus_graphics_test_framebuffer(*const c.VkFramebufferCreateInfo, [*]u8) usize;
extern fn venus_graphics_test_begin(*const c.VkRenderPassBeginInfo, [*]u8) usize;
fn attachment_fixture() c.VkAttachmentDescription {
    return .{ .format = c.VK_FORMAT_R8G8B8A8_UNORM, .samples = 1, .loadOp = 1, .storeOp = 0, .stencilLoadOp = 2, .stencilStoreOp = 1, .initialLayout = 0, .finalLayout = 2 };
}
fn dependency_fixture() c.VkSubpassDependency {
    return .{ .srcSubpass = c.VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = 1, .dstStageMask = 1024, .dstAccessMask = 256, .dependencyFlags = 1 };
}
test "single color render pass maximum and empty dependencies match pinned oracle" {
    var attachment = attachment_fixture();
    var reference: c.VkAttachmentReference = .{ .attachment = 0, .layout = 2 };
    var subpass: c.VkSubpassDescription = .{ .colorAttachmentCount = 1, .pColorAttachments = &reference };
    var dependencies = [_]c.VkSubpassDependency{dependency_fixture()} ** 16;
    var info: c.VkRenderPassCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass };
    var expected: [8192]u8 = undefined;
    for ([_]u32{ 0, 1, 16 }) |count| {
        info.dependencyCount = count;
        info.pDependencies = if (count == 0) null else &dependencies;
        const writer = try create_render_pass(&info, 7, 42);
        const size = venus_graphics_test_pass(&info, &expected);
        try std.testing.expectEqual(@as(usize, 204 + 28 * count), writer.used);
        try std.testing.expectEqualSlices(u8, expected[0..size], writer.bytes[0..writer.used]);
    }
    attachment.format = c.VK_FORMAT_B8G8R8A8_UNORM;
    for ([_]u32{ 0, 1, 2 }) |layout| {
        attachment.initialLayout = layout;
        attachment.finalLayout = if (layout == 0) 5 else layout;
        _ = try create_render_pass(&info, 7, 42);
    }
}
test "render pass invalid headers attachment subpass and dependency guards" {
    var attachment = attachment_fixture();
    var reference: c.VkAttachmentReference = .{ .attachment = 0, .layout = 2 };
    var subpass: c.VkSubpassDescription = .{ .colorAttachmentCount = 1, .pColorAttachments = &reference };
    var dependency = dependency_fixture();
    const initial: c.VkRenderPassCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass, .dependencyCount = 1, .pDependencies = &dependency };
    var info = initial;
    try std.testing.expectError(error.Invalid, create_render_pass(&info, 0, 42));
    try std.testing.expectError(error.Invalid, create_render_pass(&info, 7, 0));
    inline for (.{ "sType", "flags", "attachmentCount", "subpassCount", "dependencyCount" }) |field| {
        info = initial;
        @field(info, field) = 99;
        try std.testing.expectError(error.Invalid, create_render_pass(&info, 7, 42));
    }
    inline for (.{ "pNext", "pAttachments", "pSubpasses", "pDependencies" }) |field| {
        info = initial;
        @field(info, field) = if (comptime std.mem.eql(u8, field, "pNext")) @ptrFromInt(1) else null;
        try std.testing.expectError(error.Invalid, create_render_pass(&info, 7, 42));
    }
    info = initial;
    inline for (.{ "flags", "format", "samples", "loadOp", "storeOp", "stencilLoadOp", "stencilStoreOp", "initialLayout", "finalLayout" }) |field| {
        attachment = attachment_fixture();
        @field(attachment, field) = 99;
        try std.testing.expectError(error.Invalid, create_render_pass(&info, 7, 42));
    }
    attachment = attachment_fixture();
    const initial_subpass = subpass;
    inline for (.{ "flags", "pipelineBindPoint", "inputAttachmentCount", "colorAttachmentCount", "preserveAttachmentCount" }) |field| {
        subpass = initial_subpass;
        @field(subpass, field) = 99;
        try std.testing.expectError(error.Invalid, create_render_pass(&info, 7, 42));
    }
    inline for (.{ "pColorAttachments", "pResolveAttachments", "pDepthStencilAttachment" }) |field| {
        subpass = initial_subpass;
        @field(subpass, field) = if (comptime std.mem.eql(u8, field, "pColorAttachments")) null else @ptrFromInt(8);
        try std.testing.expectError(error.Invalid, create_render_pass(&info, 7, 42));
    }
    subpass = initial_subpass;
    reference.attachment = 1;
    try std.testing.expectError(error.Invalid, create_render_pass(&info, 7, 42));
    reference.attachment = 0;
    reference.layout = 1;
    try std.testing.expectError(error.Invalid, create_render_pass(&info, 7, 42));
    reference.layout = 2;
    inline for (.{ "srcSubpass", "dstSubpass", "srcStageMask", "dstStageMask", "srcAccessMask", "dstAccessMask", "dependencyFlags" }) |field| {
        dependency = dependency_fixture();
        @field(dependency, field) = 0x20000;
        try std.testing.expectError(error.Invalid, create_render_pass(&info, 7, 42));
    }
    dependency = dependency_fixture();
    dependency.dstSubpass = c.VK_SUBPASS_EXTERNAL;
    try std.testing.expectError(error.Invalid, create_render_pass(&info, 7, 42));
    inline for (.{ "srcStageMask", "dstStageMask" }) |field| {
        dependency = dependency_fixture();
        @field(dependency, field) = 0;
        try std.testing.expectError(error.Invalid, create_render_pass(&info, 7, 42));
    }
    dependency = dependency_fixture();
    dependency.srcSubpass = 0;
    dependency.dstSubpass = c.VK_SUBPASS_EXTERNAL;
    _ = try create_render_pass(&info, 7, 42);
}
test "framebuffer bounds and independent translated identities" {
    const initial: c.VkFramebufferCreateInfo = .{ .sType = c.VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .attachmentCount = 1, .width = 64, .height = 32, .layers = 1 };
    var info = initial;
    var expected: [8192]u8 = undefined;
    for ([_]u32{ 1, 4096 }) |extent| {
        info.width = extent;
        info.height = extent;
        const writer = try create_framebuffer(&info, 7, 42, 43, 44);
        const size = venus_graphics_test_framebuffer(&info, &expected);
        try std.testing.expectEqual(@as(usize, 104), writer.used);
        try std.testing.expectEqualSlices(u8, expected[0..size], writer.bytes[0..writer.used]);
    }
    info = initial;
    for (0..4) |index| {
        var ids = [_]u64{ 7, 42, 43, 44 };
        ids[index] = 0;
        try std.testing.expectError(error.Invalid, create_framebuffer(&info, ids[0], ids[1], ids[2], ids[3]));
    }
    inline for (.{ "sType", "flags", "attachmentCount", "width", "height", "layers" }) |field| {
        info = initial;
        @field(info, field) = 9999;
        try std.testing.expectError(error.Invalid, create_framebuffer(&info, 7, 42, 43, 44));
    }
    inline for (.{ "width", "height" }) |field| {
        info = initial;
        @field(info, field) = 0;
        try std.testing.expectError(error.Invalid, create_framebuffer(&info, 7, 42, 43, 44));
    }
    info = initial;
    info.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, create_framebuffer(&info, 7, 42, 43, 44));
}
test "render pass begin clear union bytes and area bounds match pinned oracle" {
    var clear: c.VkClearValue = .{ .color = .{ .uint32 = .{ 0, 0x3f800000, 0x7fc00001, 0xffffffff } } };
    const initial: c.VkRenderPassBeginInfo = .{ .sType = c.VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .clearValueCount = 1, .pClearValues = &clear, .renderArea = .{ .extent = .{ .width = 64, .height = 64 } } };
    var info = initial;
    var expected: [8192]u8 = undefined;
    const writer = try begin_render_pass(&info, 8, 42, 44, 0);
    const size = venus_graphics_test_begin(&info, &expected);
    try std.testing.expectEqual(@as(usize, 116), writer.used);
    try std.testing.expectEqualSlices(u8, expected[0..size], writer.bytes[0..writer.used]);
    info.renderArea.offset = .{ .x = 4095, .y = 4095 };
    info.renderArea.extent = .{ .width = 1, .height = 1 };
    _ = try begin_render_pass(&info, 8, 42, 44, 0);
    info = initial;
    for (0..3) |index| {
        var ids = [_]u64{ 8, 42, 44 };
        ids[index] = 0;
        try std.testing.expectError(error.Invalid, begin_render_pass(&info, ids[0], ids[1], ids[2], 0));
    }
    try std.testing.expectError(error.Invalid, begin_render_pass(&info, 8, 42, 44, 1));
    inline for (.{ "sType", "clearValueCount" }) |field| {
        info = initial;
        @field(info, field) = 99;
        try std.testing.expectError(error.Invalid, begin_render_pass(&info, 8, 42, 44, 0));
    }
    info = initial;
    info.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, begin_render_pass(&info, 8, 42, 44, 0));
    info = initial;
    info.pClearValues = null;
    try std.testing.expectError(error.Invalid, begin_render_pass(&info, 8, 42, 44, 0));
    inline for (.{ "x", "y" }) |field| {
        for ([_]i32{ -1, 4096 }) |value| {
            info = initial;
            @field(info.renderArea.offset, field) = value;
            try std.testing.expectError(error.Invalid, begin_render_pass(&info, 8, 42, 44, 0));
        }
    }
    inline for (.{ "width", "height" }) |field| {
        for ([_]u32{ 0, 4097 }) |value| {
            info = initial;
            @field(info.renderArea.extent, field) = value;
            try std.testing.expectError(error.Invalid, begin_render_pass(&info, 8, 42, 44, 0));
        }
    }
}
