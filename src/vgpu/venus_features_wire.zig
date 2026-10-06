//! Typed allocation-free feature-query chain profile; API advertisement remains caller-owned.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Maximum unique owned feature nodes; no native pointers are retained.
pub const MaxNodes: usize = 4;
/// Largest recognized node's boolean extent, matching Vulkan11 features.
pub const MaxNodeFlags: usize = 12;
/// Pinned core Vulkan1.0 feature boolean count; native padding is not wire data.
pub const CoreFlags: usize = 55;
/// Owned typed flags; unused entries zero, no heap/pointer ownership, immutable sharing safe.
pub const node_t = struct { type_tag: u32 = 0, flag_count: u8 = 0, flags: [MaxNodeFlags]u32 = [_]u32{0} ** MaxNodeFlags };
/// Owned decoded query result; application chain conversion occurs only after successful return.
pub const result_t = struct { count: u8 = 0, nodes: [MaxNodes]node_t = [_]node_t{.{}} ** MaxNodes, core: [CoreFlags]u32 = [_]u32{0} ** CoreFlags };
fn flag_count(tag: u32) !u8 {
    return switch (tag) {
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES => 12,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES => 1,
        c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT => 2,
        else => error.Invalid,
    };
}
fn validate(tags: []const u32) !usize {
    if (tags.len > MaxNodes) return error.Invalid;
    var count: usize = 0;
    for (tags, 0..) |tag, index| {
        count += try flag_count(tag);
        for (tags[0..index]) |previous| if (previous == tag) return error.Invalid;
    }
    return count;
}
/// Encode partial feature query. [in] physical_id nonzero translated identity, tags borrowed for call.
/// Returns owned packet or Invalid for unsupported/duplicate/excess tags; no allocation or locks.
/// Caller validates negotiated receiver schema and application pointer conversion separately.
pub fn query(physical_id: u64, tags: []const u32) !render.writer_t {
    if (physical_id == 0) return error.Invalid;
    _ = try validate(tags);
    var writer = render.writer_t{};
    // Complete capacity proof:36+12*4=84 <=8192, before any checked scalar append.
    writer.header(147, physical_id) catch unreachable;
    writer.put(u64, 1) catch unreachable;
    writer.put(u32, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) catch unreachable;
    for (tags) |tag| {
        writer.put(u64, 1) catch unreachable;
        writer.put(u32, tag) catch unreachable;
    }
    writer.put(u64, 0) catch unreachable;
    std.debug.assert(writer.used == 36 + 12 * tags.len);
    return writer;
}
const reader_t = struct {
    bytes: []const u8,
    used: usize = 0,
    fn take(self: *reader_t, comptime scalar_t: type) scalar_t {
        // decode proves the complete expected prefix extent before any read.
        const value = std.mem.readInt(scalar_t, self.bytes[self.used..][0..@sizeOf(scalar_t)], .little);
        self.used += @sizeOf(scalar_t);
        return value;
    }
    fn boolean(self: *reader_t) !u32 {
        const value = self.take(u32);
        if (value > 1) return error.Corrupt;
        return value;
    }
};
/// Decode initialized reply prefix. [in] bytes/tags immutable accessible borrowed slices.
/// Returns owned normalized result; Bounds for truncation, Invalid for request tags,
/// Corrupt for shape/booleans. No allocation, pointer retention or shared state.
/// Extra bytes are unused receiver scratch, ignored; caller output publication follows success only.
pub fn decode(bytes: []const u8, tags: []const u32) !result_t {
    const total_flags = try validate(tags);
    const expected = 244 + 12 * tags.len + 4 * total_flags;
    if (bytes.len < expected) return error.Bounds;
    var reader = reader_t{ .bytes = bytes[0..expected] };
    if (reader.take(u32) != 147 or reader.take(u64) != 1 or
        reader.take(u32) != c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) return error.Corrupt;
    var result = result_t{ .count = @intCast(tags.len) };
    for (tags, 0..) |tag, index| {
        if (reader.take(u64) != 1 or reader.take(u32) != tag) return error.Corrupt;
        result.nodes[index].type_tag = tag;
        result.nodes[index].flag_count = flag_count(tag) catch unreachable;
    }
    if (reader.take(u64) != 0) return error.Corrupt;
    var remaining = tags.len;
    while (remaining != 0) {
        remaining -= 1;
        const node = &result.nodes[remaining];
        for (node.flags[0..node.flag_count]) |*flag| flag.* = try reader.boolean();
    }
    for (&result.core) |*flag| flag.* = try reader.boolean();
    std.debug.assert(reader.used == expected);
    return result;
}

// Test-only fixtures.
extern fn venus_features_test_query([*]const u32, usize, [*]u8) usize;
extern fn venus_features_test_reply([*]const u32, usize, [*]u8) usize;
const FixtureTags = [_]u32{ c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, c.VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT };
test "partial queries and receiver flags match independent pinned encoders" {
    var expected: [4096]u8 = undefined;
    for (0..5) |count| {
        const tags = FixtureTags[0..count];
        const writer = try @call(.never_inline, query, .{ 7, tags });
        const size = venus_features_test_query(tags.ptr, count, &expected);
        try std.testing.expectEqual(@as(usize, 36 + 12 * count), writer.used);
        try std.testing.expectEqualSlices(u8, expected[0..size], writer.bytes[0..writer.used]);
        const reply_size = venus_features_test_reply(tags.ptr, count, &expected);
        const decoded = try decode(expected[0..reply_size], tags);
        try std.testing.expectEqual(@as(u8, @intCast(count)), decoded.count);
        for (decoded.core, 0..) |flag, index| try std.testing.expectEqual(@as(u32, @intCast(index % 2)), flag);
        for (decoded.nodes[0..count], tags) |node, tag| {
            try std.testing.expectEqual(tag, node.type_tag);
            for (node.flags[0..node.flag_count], 0..) |flag, index| try std.testing.expectEqual(@as(u32, @intCast(index % 2)), flag);
            for (node.flags[node.flag_count..]) |flag| try std.testing.expectEqual(@as(u32, 0), flag);
        }
        for (0..reply_size) |prefix| try std.testing.expectError(error.Bounds, decode(expected[0..prefix], tags));
    }
    const reversed = [_]u32{ FixtureTags[3], FixtureTags[2], FixtureTags[1], FixtureTags[0] };
    const reply_size = venus_features_test_reply(&reversed, reversed.len, &expected);
    _ = try decode(expected[0..reply_size], &reversed);
    @memset(expected[reply_size..], 0xaa);
    _ = try decode(&expected, &reversed);
}
test "invalid requests and every structural or boolean reply word reject" {
    try std.testing.expectError(error.Invalid, @call(.never_inline, query, .{ 0, &.{} }));
    for ([_][]const u32{ &.{0}, &.{ FixtureTags[0], FixtureTags[0] }, &.{ FixtureTags[0], FixtureTags[1], FixtureTags[2], FixtureTags[3], 0 } }) |tags| {
        try std.testing.expectError(error.Invalid, @call(.never_inline, query, .{ 7, tags }));
        try std.testing.expectError(error.Invalid, decode(&.{}, tags));
    }
    var bytes: [4096]u8 = undefined;
    const size = venus_features_test_reply(&FixtureTags, FixtureTags.len, &bytes);
    // Every four-byte initialized wire word is a tag, structure identity, or boolean.
    // Mutating the high halves of pointer tags is separately exercised below.
    for (0..size / 4) |index| {
        var malformed = bytes;
        std.mem.writeInt(u32, malformed[index * 4 ..][0..4], 99, .little);
        try std.testing.expectError(error.Corrupt, decode(malformed[0..size], &FixtureTags));
    }
}
