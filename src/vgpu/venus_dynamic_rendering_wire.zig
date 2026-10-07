//! Bounded Vulkan dynamic rendering packets with translated host attachment identities.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Maximum simultaneous color attachments, caller checks actual host limit.
pub const MaxColors = 8;
/// Owned packet returned by value, no allocation or retained pointers.
pub const writer_t = render.writer_t;
/// Resolved view and resolve-view identities; zero denotes an unused native view.
pub const attachment_ids_t = struct { view: u64 = 0, resolve: u64 = 0 };
fn put(writer: *writer_t, comptime value_t: type, value: value_t) void {
    writer.put(value_t, value) catch unreachable;
}
fn attachment_valid(info: *const c.VkRenderingAttachmentInfo, ids: attachment_ids_t, depth: bool) bool {
    if (info.sType != c.VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO or info.pNext != null or
        (ids.view != 0 and (info.imageLayout == c.VK_IMAGE_LAYOUT_UNDEFINED or info.imageLayout == c.VK_IMAGE_LAYOUT_PREINITIALIZED)) or
        info.loadOp > c.VK_ATTACHMENT_LOAD_OP_DONT_CARE or
        (info.storeOp > c.VK_ATTACHMENT_STORE_OP_DONT_CARE and info.storeOp != c.VK_ATTACHMENT_STORE_OP_NONE) or
        (info.imageView == null) != (ids.view == 0) or (info.resolveImageView == null) != (ids.resolve == 0) or
        info.resolveMode > c.VK_RESOLVE_MODE_MAX_BIT or (info.resolveMode != 0 and info.resolveMode & (info.resolveMode - 1) != 0) or
        (info.resolveMode == 0 and ids.resolve != 0) or (info.resolveMode != 0 and ids.resolve == 0)) return false;
    if (depth and info.loadOp == c.VK_ATTACHMENT_LOAD_OP_CLEAR and
        (!std.math.isFinite(info.clearValue.depthStencil.depth) or info.clearValue.depthStencil.depth < 0 or info.clearValue.depthStencil.depth > 1)) return false;
    return true;
}
fn encode_attachment(writer: *writer_t, info: *const c.VkRenderingAttachmentInfo, ids: attachment_ids_t, depth: bool) void {
    put(writer, u32, c.VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    put(writer, u64, 0);
    put(writer, u64, ids.view);
    put(writer, u32, info.imageLayout);
    put(writer, u32, info.resolveMode);
    put(writer, u64, ids.resolve);
    put(writer, u32, info.resolveImageLayout);
    put(writer, u32, info.loadOp);
    put(writer, u32, info.storeOp);
    put(writer, u32, 0); // VkClearValue color representation carries identical lower depth/stencil bits.
    put(writer, u32, 2);
    put(writer, u64, 4);
    if (info.loadOp != c.VK_ATTACHMENT_LOAD_OP_CLEAR) {
        for (0..4) |_| put(writer, u32, 0);
    } else if (depth) {
        put(writer, u32, @bitCast(info.clearValue.depthStencil.depth));
        put(writer, u32, info.clearValue.depthStencil.stencil);
        put(writer, u32, 0);
        put(writer, u32, 0);
    } else for (info.clearValue.color.uint32) |word| put(writer, u32, word);
}
/// Encode VkRenderingInfo with no extension chain, 0..8 colors and optional depth/stencil.
/// [in] info/reachable records accessible borrowed call-lifetime; IDs already resolved to
/// matching live host image views. Caller verifies layouts, formats, sample compatibility,
/// multiview support, image bounds, usage, command recording state and lifetime retention.
/// [out] complete owned packet or Invalid, no allocation/global state, concurrent-safe.
/// Clear values unused by loadOp are canonical zeros; depth union padding is never read.
pub fn begin_rendering(command: u64, info: *const c.VkRenderingInfo, colors: []const attachment_ids_t, depth: attachment_ids_t, stencil: attachment_ids_t) !writer_t {
    if (command == 0 or info.sType != c.VK_STRUCTURE_TYPE_RENDERING_INFO or info.pNext != null or info.flags & ~@as(u32, 7) != 0 or
        info.colorAttachmentCount > MaxColors or info.colorAttachmentCount != colors.len or
        (colors.len != 0 and info.pColorAttachments == null) or info.renderArea.offset.x < 0 or info.renderArea.offset.y < 0 or
        info.renderArea.extent.width == 0 or info.renderArea.extent.height == 0 or (info.layerCount == 0 and info.viewMask == 0)) return error.Invalid;
    for (colors, 0..) |ids, index| if (!attachment_valid(@ptrCast(&info.pColorAttachments[index]), ids, false)) return error.Invalid;
    if (info.pDepthAttachment != null) {
        if (!attachment_valid(@ptrCast(info.pDepthAttachment), depth, true)) return error.Invalid;
    } else if (depth.view != 0 or depth.resolve != 0) return error.Invalid;
    if (info.pStencilAttachment != null) {
        if (!attachment_valid(@ptrCast(info.pStencilAttachment), stencil, false)) return error.Invalid;
    } else if (stencil.view != 0 or stencil.resolve != 0) return error.Invalid;
    var writer: writer_t = .{};
    put(&writer, u32, 213);
    put(&writer, u32, 1);
    put(&writer, u64, command);
    put(&writer, u64, 1);
    put(&writer, u32, c.VK_STRUCTURE_TYPE_RENDERING_INFO);
    put(&writer, u64, 0);
    put(&writer, u32, info.flags);
    put(&writer, i32, info.renderArea.offset.x);
    put(&writer, i32, info.renderArea.offset.y);
    put(&writer, u32, info.renderArea.extent.width);
    put(&writer, u32, info.renderArea.extent.height);
    put(&writer, u32, info.layerCount);
    put(&writer, u32, info.viewMask);
    put(&writer, u32, info.colorAttachmentCount);
    put(&writer, u64, colors.len);
    for (colors, 0..) |ids, index| encode_attachment(&writer, @ptrCast(&info.pColorAttachments[index]), ids, false);
    put(&writer, u64, if (info.pDepthAttachment != null) 1 else 0);
    if (info.pDepthAttachment != null) encode_attachment(&writer, @ptrCast(info.pDepthAttachment), depth, true);
    put(&writer, u64, if (info.pStencilAttachment != null) 1 else 0);
    if (info.pStencilAttachment != null) encode_attachment(&writer, @ptrCast(info.pStencilAttachment), stencil, true);
    return writer;
}
/// End dynamic rendering for a nonzero translated host command ID. Caller ensures a
/// live rendering scope; returns owned16-byte packet or Invalid. Allocation-free, thread-safe.
pub fn end_rendering(command: u64) !writer_t {
    if (command == 0) return error.Invalid;
    var writer: writer_t = .{};
    put(&writer, u32, 214);
    put(&writer, u32, 1);
    put(&writer, u64, command);
    return writer;
}

// Test-only generated encoder oracle.
extern fn venus_dynamic_rendering_test_begin(*const c.VkRenderingInfo, [*]u8) usize;
extern fn venus_dynamic_rendering_test_end([*]u8) usize;
fn compare(writer: writer_t, expected: []const u8) !void {
    try std.testing.expectEqual(expected.len, writer.used);
    try std.testing.expectEqualSlices(u8, expected, writer.bytes[0..writer.used]);
}
test "empty and full rendering scopes match generated encoder including resolve and depth" {
    var colors: [MaxColors]c.VkRenderingAttachmentInfo = undefined;
    var ids: [MaxColors]attachment_ids_t = undefined;
    for (&colors, &ids, 0..) |*attachment, *resolved, index| {
        resolved.* = .{ .view = 42 + index, .resolve = 52 + index };
        attachment.* = .{ .sType = c.VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = @ptrFromInt(resolved.view), .imageLayout = c.VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .resolveMode = c.VK_RESOLVE_MODE_AVERAGE_BIT, .resolveImageView = @ptrFromInt(resolved.resolve), .resolveImageLayout = c.VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = c.VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = c.VK_ATTACHMENT_STORE_OP_STORE, .clearValue = .{ .color = .{ .uint32 = .{ 0x3f800000, 0, 0, 0x3f800000 } } } };
    }
    var depth_attachment = c.VkRenderingAttachmentInfo{ .sType = c.VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = @ptrFromInt(64), .imageLayout = c.VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .loadOp = c.VK_ATTACHMENT_LOAD_OP_CLEAR, .clearValue = .{ .color = .{ .uint32 = .{ 0x3f800000, 17, 0, 0 } } } };
    var info = c.VkRenderingInfo{ .sType = c.VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = .{ .extent = .{ .width = 64, .height = 64 } }, .layerCount = 1 };
    var expected: [8192]u8 = undefined;
    try compare(try begin_rendering(8, &info, &.{}, .{}, .{}), expected[0..venus_dynamic_rendering_test_begin(&info, &expected)]);
    info.colorAttachmentCount = MaxColors;
    info.pColorAttachments = &colors;
    info.pDepthAttachment = &depth_attachment;
    info.pStencilAttachment = &depth_attachment;
    try compare(try begin_rendering(8, &info, &ids, .{ .view = 64 }, .{ .view = 64 }), expected[0..venus_dynamic_rendering_test_begin(&info, &expected)]);
    try compare(try end_rendering(8), expected[0..venus_dynamic_rendering_test_end(&expected)]);
}
test "rendering structural errors preserve borrowed native input" {
    const info = c.VkRenderingInfo{ .sType = c.VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = .{ .extent = .{ .width = 64, .height = 64 } }, .layerCount = 1 };
    try std.testing.expectError(error.Invalid, begin_rendering(0, &info, &.{}, .{}, .{}));
    try std.testing.expectError(error.Invalid, end_rendering(0));
    inline for (.{ "sType", "layerCount" }) |field| {
        var invalid = info;
        @field(invalid, field) = 0;
        try std.testing.expectError(error.Invalid, begin_rendering(8, &invalid, &.{}, .{}, .{}));
    }
    var invalid = info;
    invalid.colorAttachmentCount = 1;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &invalid, &.{}, .{}, .{}));
    invalid = info;
    invalid.renderArea.offset.x = -1;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &invalid, &.{}, .{}, .{}));
    try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{ .view = 1 }, .{}));
}

test "rendering attachment topology resolve modes depth values and empty clear payloads" {
    const initial: c.VkRenderingAttachmentInfo = .{ .sType = c.VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = @ptrFromInt(42), .imageLayout = c.VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = c.VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = c.VK_ATTACHMENT_STORE_OP_STORE };
    var attachment = initial;
    const ids = [_]attachment_ids_t{.{ .view = 42 }};
    const initial_info: c.VkRenderingInfo = .{ .sType = c.VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = .{ .extent = .{ .width = 64, .height = 64 } }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment };
    inline for (.{ "sType", "loadOp", "storeOp", "resolveMode", "imageLayout" }) |field| {
        attachment = initial;
        @field(attachment, field) = if (comptime std.mem.eql(u8, field, "sType")) 0 else if (comptime std.mem.eql(u8, field, "imageLayout")) c.VK_IMAGE_LAYOUT_PREINITIALIZED else if (comptime std.mem.eql(u8, field, "resolveMode")) 16 else 9;
        try std.testing.expectError(error.Invalid, begin_rendering(8, &initial_info, &ids, .{}, .{}));
    }
    attachment = initial;
    attachment.imageLayout = c.VK_IMAGE_LAYOUT_UNDEFINED;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &initial_info, &ids, .{}, .{}));
    attachment = initial;
    attachment.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, begin_rendering(8, &initial_info, &ids, .{}, .{}));
    attachment = initial;
    attachment.imageView = null;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &initial_info, &ids, .{}, .{}));
    attachment = initial;
    attachment.resolveImageView = @ptrFromInt(43);
    try std.testing.expectError(error.Invalid, begin_rendering(8, &initial_info, &ids, .{}, .{}));
    try std.testing.expectError(error.Invalid, begin_rendering(8, &initial_info, &.{.{ .view = 42, .resolve = 43 }}, .{}, .{}));
    attachment.resolveMode = 3;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &initial_info, &.{.{ .view = 42, .resolve = 43 }}, .{}, .{}));
    attachment.resolveImageView = null;
    attachment.resolveMode = 1;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &initial_info, &ids, .{}, .{}));
    attachment = initial;
    attachment.storeOp = c.VK_ATTACHMENT_STORE_OP_NONE;
    attachment.loadOp = c.VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.clearValue = undefined;
    _ = try begin_rendering(8, &initial_info, &ids, .{}, .{});
    var info = initial_info;
    info.colorAttachmentCount = 0;
    info.pColorAttachments = null;
    info.pDepthAttachment = &attachment;
    for ([_]f32{ -1, 2, std.math.nan(f32) }) |depth_value| {
        attachment = initial;
        attachment.clearValue.depthStencil = .{ .depth = depth_value, .stencil = 0 };
        try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{ .view = 42 }, .{}));
    }
    attachment = initial;
    attachment.sType = 0;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{ .view = 42 }, .{}));
    info.pDepthAttachment = null;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{ .resolve = 43 }, .{}));
    try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{}, .{ .view = 42 }));
    try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{}, .{ .resolve = 43 }));
    info.pStencilAttachment = &attachment;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{}, .{ .view = 42 }));
}

test "rendering scope offsets extents masks flags and missing color arrays" {
    const initial: c.VkRenderingInfo = .{ .sType = c.VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = .{ .extent = .{ .width = 64, .height = 64 } }, .layerCount = 1 };
    var info = initial;
    info.pNext = @ptrFromInt(1);
    try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{}, .{}));
    info = initial;
    info.flags = 8;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{}, .{}));
    info = initial;
    info.colorAttachmentCount = 9;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{}, .{}));
    info.colorAttachmentCount = 1;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{.{}}, .{}, .{}));
    info = initial;
    info.renderArea.offset.y = -1;
    try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{}, .{}));
    inline for (.{ "width", "height" }) |field| {
        info = initial;
        @field(info.renderArea.extent, field) = 0;
        try std.testing.expectError(error.Invalid, begin_rendering(8, &info, &.{}, .{}, .{}));
    }
    info = initial;
    info.layerCount = 0;
    info.viewMask = 1;
    _ = try begin_rendering(8, &info, &.{}, .{}, .{});
}
