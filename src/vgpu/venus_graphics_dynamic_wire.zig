//! Bounded dynamic graphics state, vertex/index bindings and indexed/indirect draws.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Owned bounded packet, no allocation or retained pointers.
pub const writer_t = render.writer_t;
/// Fixed scalar dynamic commands with pinned Venus opcodes.
pub const scalar_command_t = enum(u32) {
    CullMode = 215,
    FrontFace = 216,
    PrimitiveTopology = 217,
    DepthTestEnable = 221,
    DepthWriteEnable = 222,
    DepthCompareOp = 223,
    DepthBoundsTestEnable = 224,
    StencilTestEnable = 225,
    RasterizerDiscardEnable = 227,
    DepthBiasEnable = 228,
    PrimitiveRestartEnable = 229,
};
/// Stencil mask/reference selectors, no mutable storage or ownership.
pub const stencil_command_t = enum(u32) { CompareMask = 100, WriteMask = 101, Reference = 102 };
fn put(writer: *writer_t, comptime value_t: type, value: value_t) void {
    writer.put(value_t, value) catch unreachable;
}
fn header(opcode: u32, command: u64) !writer_t {
    if (command == 0) return error.Invalid;
    var writer: writer_t = .{};
    put(&writer, u32, opcode);
    put(&writer, u32, 1);
    put(&writer, u64, command);
    return writer;
}
/// Encode finite dynamic viewports,1..16 records. [in] command host ID, first viewport;
/// values borrowed, counts bounded; with_count chooses Vulkan1.3 count-setting operation.
/// Caller checks feature support, actual viewport dimensions/limits and recording state.
/// Owned result or Invalid; no allocation/retention, disjoint calls thread-safe.
pub fn viewports(command: u64, first: u32, values: []const c.VkViewport, with_count: bool) !writer_t {
    if (values.len == 0 or values.len > 16 or first > 16 - values.len or (with_count and first != 0)) return error.Invalid;
    for (values) |value| {
        inline for (.{ "x", "y", "width", "height", "minDepth", "maxDepth" }) |field| if (!std.math.isFinite(@field(value, field))) return error.Invalid;
        if (value.width <= 0 or value.height == 0 or value.minDepth < 0 or value.minDepth > 1 or value.maxDepth < 0 or value.maxDepth > 1) return error.Invalid;
    }
    var writer = try header(if (with_count) 218 else 94, command);
    if (!with_count) put(&writer, u32, first);
    put(&writer, u32, @intCast(values.len));
    put(&writer, u64, values.len);
    for (values) |value| inline for (.{ "x", "y", "width", "height", "minDepth", "maxDepth" }) |field| put(&writer, u32, @bitCast(@field(value, field)));
    return writer;
}
/// Encode borrowed1..16 dynamic scissors with nonnegative offsets. Exact host extent
/// limits and recording state belong to caller. with_count selects count-setting operation.
/// Owned packet or Invalid; no allocation, retained pointer or shared state.
pub fn scissors(command: u64, first: u32, values: []const c.VkRect2D, with_count: bool) !writer_t {
    if (values.len == 0 or values.len > 16 or first > 16 - values.len or (with_count and first != 0)) return error.Invalid;
    for (values) |value| if (value.offset.x < 0 or value.offset.y < 0 or @as(u64, @intCast(value.offset.x)) + value.extent.width > std.math.maxInt(i32) or @as(u64, @intCast(value.offset.y)) + value.extent.height > std.math.maxInt(i32)) return error.Invalid;
    var writer = try header(if (with_count) 219 else 95, command);
    if (!with_count) put(&writer, u32, first);
    put(&writer, u32, @intCast(values.len));
    put(&writer, u64, values.len);
    for (values) |value| {
        put(&writer, i32, value.offset.x);
        put(&writer, i32, value.offset.y);
        put(&writer, u32, value.extent.width);
        put(&writer, u32, value.extent.height);
    }
    return writer;
}
/// Encode dynamic blend constants from owned four-float value. Finite values required;
/// caller checks recording state. Owned packet or Invalid, allocation-free/thread-safe.
pub fn blend_constants(command: u64, values: [4]f32) !writer_t {
    for (values) |value| if (!std.math.isFinite(value)) return error.Invalid;
    var writer = try header(98, command);
    put(&writer, u64, 4);
    for (values) |value| put(&writer, u32, @bitCast(value));
    return writer;
}
/// Encode finite depth bias constant/clamp/slope. Caller validates depthBiasClamp enablement.
/// Owned packet or Invalid, no allocation/retention, thread-safe on independent calls.
pub fn depth_bias(command: u64, values: [3]f32) !writer_t {
    for (values) |value| if (!std.math.isFinite(value)) return error.Invalid;
    var writer = try header(97, command);
    for (values) |value| put(&writer, u32, @bitCast(value));
    return writer;
}
/// Encode normalized finite minimum/maximum depth bounds; caller checks enabled feature.
/// Owned packet or Invalid, no allocations/global state/pointer retention.
pub fn depth_bounds(command: u64, minimum: f32, maximum: f32) !writer_t {
    if (!std.math.isFinite(minimum) or !std.math.isFinite(maximum) or minimum < 0 or maximum > 1 or minimum > maximum) return error.Invalid;
    var writer = try header(99, command);
    put(&writer, u32, @bitCast(minimum));
    put(&writer, u32, @bitCast(maximum));
    return writer;
}
/// Encode typed core1.3 scalar dynamic state. enum/Boolean values checked locally;
/// caller validates enabled feature and recording state. Owned packet or Invalid; no heap.
pub fn scalar(command: u64, operation: scalar_command_t, value: u32) !writer_t {
    const maximum: u32 = switch (operation) {
        .CullMode => 3,
        .FrontFace => 1,
        .PrimitiveTopology => 10,
        .DepthCompareOp => 7,
        else => 1,
    };
    if (value > maximum) return error.Invalid;
    var writer = try header(@intFromEnum(operation), command);
    put(&writer, u32, value);
    return writer;
}
/// Encode stencil front/back mask or reference with full32-bit value. face_mask1..3;
/// caller validates recording state. Owned packet or Invalid; allocation-free/thread-safe.
pub fn stencil_mask(command: u64, operation: stencil_command_t, face_mask: u32, value: u32) !writer_t {
    if (face_mask == 0 or face_mask > 3) return error.Invalid;
    var writer = try header(@intFromEnum(operation), command);
    put(&writer, u32, face_mask);
    put(&writer, u32, value);
    return writer;
}
/// Encode stencil fail/pass/depth-fail ops and compare function for faces1..3.
/// Caller checks recording state and feature enablement. Owned packet or Invalid, no heap.
pub fn stencil_ops(command: u64, face_mask: u32, values: [4]u32) !writer_t {
    if (face_mask == 0 or face_mask > 3) return error.Invalid;
    for (values) |value| if (value > 7) return error.Invalid;
    var writer = try header(226, command);
    put(&writer, u32, face_mask);
    for (values) |value| put(&writer, u32, value);
    return writer;
}
/// Encode real translated vertex-buffer bindings1..32. Borrowed buffers/offsets same
/// length; optional sizes/strides same length when present. Zero buffer requires actual
/// nullDescriptor support checked by caller. Caller checks owner/bounds/usages/retention.
/// v2 chooses Vulkan1.3 binding sizes/strides; v1 rejects supplied sizes/strides.
/// Owned packet or Invalid, no allocation/retention/global state.
pub fn bind_vertex_buffers(command: u64, first: u32, buffers: []const u64, offsets: []const u64, sizes: ?[]const u64, strides: ?[]const u64, v2: bool) !writer_t {
    if (buffers.len == 0 or buffers.len > 32 or first > 32 - buffers.len or offsets.len != buffers.len or (!v2 and (sizes != null or strides != null))) return error.Invalid;
    if (sizes) |values| if (values.len != buffers.len) return error.Invalid;
    if (strides) |values| if (values.len != buffers.len) return error.Invalid;
    var writer = try header(if (v2) 220 else 105, command);
    put(&writer, u32, first);
    put(&writer, u32, @intCast(buffers.len));
    put(&writer, u64, buffers.len);
    for (buffers) |buffer| put(&writer, u64, buffer);
    put(&writer, u64, offsets.len);
    for (offsets) |offset| put(&writer, u64, offset);
    if (v2) {
        put(&writer, u64, if (sizes) |values| values.len else 0);
        if (sizes) |values| for (values) |value| put(&writer, u64, value);
        put(&writer, u64, if (strides) |values| values.len else 0);
        if (strides) |values| for (values) |value| put(&writer, u64, value);
    }
    return writer;
}
/// Encode uint16/uint32 index-buffer binding. Offset aligned to actual index size;
/// caller validates owner/buffer bounds/usages and memory binding; nonzero translated ID.
/// Optional size selects maintenance5 BindIndexBuffer2, real feature checked by caller.
/// Owned packet or Invalid, allocation-free/thread-safe.
pub fn bind_index_buffer(command: u64, buffer: u64, offset: u64, index_type: u32, size: ?u64) !writer_t {
    if (buffer == 0 or index_type > 1 or offset % (if (index_type == 0) @as(u64, 2) else 4) != 0) return error.Invalid;
    var writer = try header(if (size != null) 279 else 104, command);
    put(&writer, u64, buffer);
    put(&writer, u64, offset);
    if (size) |value| put(&writer, u64, value);
    put(&writer, u32, index_type);
    return writer;
}
/// Encode indexed draw scalars, preserving signed vertex offset. Caller validates active
/// compatible pipeline, index/vertex buffer bounds and command scope. Nonzero host command.
/// Owned packet or Invalid; no allocation/retention, thread-safe.
pub fn draw_indexed(command: u64, count: u32, instances: u32, first: u32, vertex_offset: i32, first_instance: u32) !writer_t {
    var writer = try header(107, command);
    put(&writer, u32, count);
    put(&writer, u32, instances);
    put(&writer, u32, first);
    put(&writer, i32, vertex_offset);
    put(&writer, u32, first_instance);
    return writer;
}
/// Encode direct/ indexed indirect draw. Offset4-aligned; count may0, stride multiple4
/// and >=16/20 when count>1. Caller checks actual buffer span, usages, ownership, enabled
/// multiDrawIndirect, bound pipeline/index/vertex state and resource retention.
/// Owned packet or Invalid, no allocation/retention/shared state.
pub fn draw_indirect(command: u64, buffer: u64, offset: u64, count: u32, stride: u32, indexed: bool) !writer_t {
    if (buffer == 0 or offset % 4 != 0 or (count > 1 and (stride % 4 != 0 or stride < (if (indexed) @as(u32, 20) else 16)))) return error.Invalid;
    var writer = try header(if (indexed) 109 else 108, command);
    put(&writer, u64, buffer);
    put(&writer, u64, offset);
    put(&writer, u32, count);
    put(&writer, u32, stride);
    return writer;
}

// Test-only pinned generated oracles.
extern fn venus_graphics_dynamic_test_encode(u32, ?*const anyopaque, u32, [*]u8) usize;
fn compare(writer: writer_t, opcode: u32, records: ?*const anyopaque, count: u32) !void {
    var expected: [8192]u8 = undefined;
    const used = venus_graphics_dynamic_test_encode(opcode, records, count, &expected);
    try std.testing.expectEqual(used, writer.used);
    try std.testing.expectEqualSlices(u8, expected[0..used], writer.bytes[0..writer.used]);
}
test "all dynamic graphics state binding and draw commands match generated encoder" {
    const views = [_]c.VkViewport{ .{ .width = 64, .height = -64, .maxDepth = 1 }, .{ .x = 16, .y = 32, .width = 32, .height = 16, .minDepth = 0.25, .maxDepth = 0.75 } };
    const rectangles = [_]c.VkRect2D{ .{ .offset = .{ .x = 3, .y = 7 }, .extent = .{ .width = 64, .height = 32 } }, .{ .extent = .{ .width = 16, .height = 8 } } };
    try compare(try viewports(8, 2, &views, false), 94, &views, 2);
    try compare(try viewports(8, 0, &views, true), 218, &views, 2);
    try compare(try scissors(8, 2, &rectangles, false), 95, &rectangles, 2);
    try compare(try scissors(8, 0, &rectangles, true), 219, &rectangles, 2);
    try compare(try depth_bias(8, .{ 0.5, 0.25, 0.125 }), 97, null, 0);
    try compare(try blend_constants(8, .{ 0.5, 0.25, 0.125, 1 }), 98, null, 0);
    try compare(try depth_bounds(8, 0.25, 0.75), 99, null, 0);
    inline for (std.meta.fields(scalar_command_t)) |field| try compare(try scalar(8, @enumFromInt(field.value), 1), field.value, null, 0);
    inline for (std.meta.fields(stencil_command_t)) |field| try compare(try stencil_mask(8, @enumFromInt(field.value), 3, 0x87654321), field.value, null, 0);
    try compare(try stencil_ops(8, 3, .{ 1, 2, 3, 4 }), 226, null, 0);
    try compare(try bind_vertex_buffers(8, 2, &.{ 42, 43 }, &.{ 16, 32 }, null, null, false), 105, null, 0);
    try compare(try bind_vertex_buffers(8, 2, &.{ 42, 43 }, &.{ 16, 32 }, &.{ 64, 128 }, &.{ 4, 16 }, true), 220, null, 0);
    try compare(try bind_index_buffer(8, 42, 16, 1, null), 104, null, 0);
    try compare(try bind_index_buffer(8, 42, 16, 1, 128), 279, null, 0);
    try compare(try draw_indexed(8, 6, 2, 3, -17, 19), 107, null, 0);
    try compare(try draw_indirect(8, 42, 16, 2, 32, false), 108, null, 0);
    try compare(try draw_indirect(8, 42, 16, 2, 32, true), 109, null, 0);
}
test "dynamic malformed arrays floats and alignment reject without allocation" {
    const view = c.VkViewport{ .width = 64, .height = 64, .maxDepth = 1 };
    try std.testing.expectError(error.Invalid, viewports(0, 0, &.{view}, false));
    try std.testing.expectError(error.Invalid, viewports(8, 1, &.{view}, true));
    try std.testing.expectError(error.Invalid, viewports(8, 0, &.{}, false));
    var bad_view = view;
    bad_view.height = 0;
    try std.testing.expectError(error.Invalid, viewports(8, 0, &.{bad_view}, false));
    bad_view = view;
    bad_view.width = std.math.inf(f32);
    try std.testing.expectError(error.Invalid, viewports(8, 0, &.{bad_view}, false));
    try std.testing.expectError(error.Invalid, blend_constants(8, .{ 0, 0, 0, std.math.nan(f32) }));
    try std.testing.expectError(error.Invalid, depth_bounds(8, 0.75, 0.25));
    try std.testing.expectError(error.Invalid, scalar(8, .DepthTestEnable, 2));
    try std.testing.expectError(error.Invalid, stencil_mask(8, .Reference, 0, 7));
    try std.testing.expectError(error.Invalid, bind_vertex_buffers(8, 0, &.{42}, &.{}, null, null, true));
    try std.testing.expectError(error.Invalid, bind_vertex_buffers(8, 0, &.{42}, &.{0}, &.{1}, null, false));
    try std.testing.expectError(error.Invalid, bind_vertex_buffers(8, 0, &.{42}, &.{0}, &.{}, null, true));
    try std.testing.expectError(error.Invalid, bind_index_buffer(8, 42, 1, 1, null));
    try std.testing.expectError(error.Invalid, draw_indirect(8, 42, 2, 1, 0, false));
    try std.testing.expectError(error.Invalid, draw_indirect(8, 42, 0, 2, 16, true));
}
