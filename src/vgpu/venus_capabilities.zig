const std = @import("std");
const CapabilitiesBytes: usize = 160;
const PinnedXmlVersion: u32 = (1 << 22) | (4 << 12) | 307;
/// Private C ABI host integers; caller-owned, no heap, handles or native wire view.
const venus_capabilities_t = extern struct {
    wire_format_version: u32,
    vk_xml_version: u32,
    vk_ext_command_serialization_spec_version: u32,
    vk_mesa_venus_protocol_spec_version: u32,
    supports_blob_id_0: u32,
    vk_extension_mask1: [32]u32,
    allow_vk_wait_syncs: u32,
    supports_multiple_timelines: u32,
    use_guest_vram: u32,
};
fn valid_shape(value: *const venus_capabilities_t) bool {
    return value.supports_blob_id_0 <= 1 and value.allow_vk_wait_syncs <= 1 and
        value.supports_multiple_timelines <= 1 and value.use_guest_vram <= 1;
}
/// out: nullable private record, zeroed on error, disjoint from input; in: nullable
/// immutable private bytes[length], exactly 160, borrowed only for call. Returns
/// 0 success, -1 null arguments, -2 malformed length/flags. Allocation-free,
/// thread-safe on disjoint outputs. Unsupported versions remain decoded values.
export fn venus_capabilities_decode(capabilities: ?*venus_capabilities_t, bytes: ?[*]const u8, length: usize) c_int {
    const output = capabilities orelse return -1;
    output.* = std.mem.zeroes(venus_capabilities_t);
    const source = bytes orelse return -1;
    if (length != CapabilitiesBytes) return -2;
    const input = source[0..CapabilitiesBytes];
    var value = venus_capabilities_t{
        .wire_format_version = std.mem.readInt(u32, input[0..4], .little),
        .vk_xml_version = std.mem.readInt(u32, input[4..8], .little),
        .vk_ext_command_serialization_spec_version = std.mem.readInt(u32, input[8..12], .little),
        .vk_mesa_venus_protocol_spec_version = std.mem.readInt(u32, input[12..16], .little),
        .supports_blob_id_0 = std.mem.readInt(u32, input[16..20], .little),
        .vk_extension_mask1 = undefined,
        .allow_vk_wait_syncs = std.mem.readInt(u32, input[148..152], .little),
        .supports_multiple_timelines = std.mem.readInt(u32, input[152..156], .little),
        .use_guest_vram = std.mem.readInt(u32, input[156..160], .little),
    };
    for (&value.vk_extension_mask1, 0..) |*word, index|
        word.* = std.mem.readInt(u32, input[20 + 4 * index ..][0..4], .little);
    if (!valid_shape(&value)) return -2;
    output.* = value;
    return 0;
}
/// in: nullable immutable private decoded snapshot. Returns 0 only for pinned
/// profile, -1 null/bad shape/unsupported profile. Pure/thread-safe, no allocation,
/// mutation, borrowed storage retention or host quota/extension authorization.
export fn venus_capabilities_compatible(capabilities: ?*const venus_capabilities_t) c_int {
    const value = capabilities orelse return -1;
    if (!valid_shape(value) or value.wire_format_version != 1 or
        value.vk_xml_version != PinnedXmlVersion or
        value.vk_ext_command_serialization_spec_version != 1 or
        value.vk_mesa_venus_protocol_spec_version != 3 or value.supports_blob_id_0 != 1 or
        value.supports_multiple_timelines != 1 or value.use_guest_vram != 0 or
        value.vk_extension_mask1[0] & 1 == 0 or value.vk_extension_mask1[12] & 3 != 3) return -1;
    return 0;
}
/// in: nullable immutable private snapshot, extension number 1..1023. Returns
/// one for explicit advertised support; zero invalid/null/absent/legacy-mask.
/// Pure/thread-safe, allocation-free; validates number before bounded indexing.
export fn venus_capabilities_extension(capabilities: ?*const venus_capabilities_t, number: u32) c_int {
    const value = capabilities orelse return 0;
    if (number == 0 or number >= 1024 or value.vk_extension_mask1[0] & 1 == 0) return 0;
    const mask = @as(u32, 1) << @as(u5, @intCast(number & 31));
    return if (value.vk_extension_mask1[number / 32] & mask != 0) 1 else 0;
}
fn fixture_value() venus_capabilities_t {
    var value = std.mem.zeroes(venus_capabilities_t);
    value.wire_format_version = 1;
    value.vk_xml_version = PinnedXmlVersion;
    value.vk_ext_command_serialization_spec_version = 1;
    value.vk_mesa_venus_protocol_spec_version = 3;
    value.supports_blob_id_0 = 1;
    value.supports_multiple_timelines = 1;
    value.vk_extension_mask1[0] = 1;
    value.vk_extension_mask1[12] = 3;
    return value;
}
fn fixture_bytes(value: *const venus_capabilities_t, bytes: []u8) void {
    const fields = [_]u32{ value.wire_format_version, value.vk_xml_version, value.vk_ext_command_serialization_spec_version, value.vk_mesa_venus_protocol_spec_version, value.supports_blob_id_0 };
    for (fields, 0..) |word, index| std.mem.writeInt(u32, bytes[index * 4 ..][0..4], word, .little);
    for (value.vk_extension_mask1, 0..) |word, index| std.mem.writeInt(u32, bytes[20 + index * 4 ..][0..4], word, .little);
    std.mem.writeInt(u32, bytes[148..152], value.allow_vk_wait_syncs, .little);
    std.mem.writeInt(u32, bytes[152..156], value.supports_multiple_timelines, .little);
    std.mem.writeInt(u32, bytes[156..160], value.use_guest_vram, .little);
}
test "exact private capability payload preserves every field and rejects shape" {
    const bytes = try std.testing.allocator.alloc(u8, CapabilitiesBytes);
    defer std.testing.allocator.free(bytes);
    var value = fixture_value();
    for ([_]u32{ 0, 1 }) |wait_syncs| {
        value.allow_vk_wait_syncs = wait_syncs;
        fixture_bytes(&value, bytes);
        var output: venus_capabilities_t = undefined;
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_capabilities_decode, .{ &output, bytes.ptr, bytes.len }));
        try std.testing.expectEqualDeep(value, output);
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_capabilities_compatible, .{&output}));
    }
    var output: venus_capabilities_t = undefined;
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_capabilities_decode, .{ null, bytes.ptr, bytes.len }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_capabilities_decode, .{ &output, null, bytes.len }));
    try std.testing.expectEqualDeep(std.mem.zeroes(venus_capabilities_t), output);
    for ([_]usize{ 0, 159, 161, std.math.maxInt(usize) }) |length| {
        try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_capabilities_decode, .{ &output, bytes.ptr, length }));
        try std.testing.expectEqualDeep(std.mem.zeroes(venus_capabilities_t), output);
    }
    for ([_]usize{ 16, 148, 152, 156 }) |offset| {
        for ([_]u32{ 2, std.math.maxInt(u32) }) |bad| {
            fixture_bytes(&value, bytes);
            std.mem.writeInt(u32, bytes[offset..][0..4], bad, .little);
            try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_capabilities_decode, .{ &output, bytes.ptr, bytes.len }));
            try std.testing.expectEqualDeep(std.mem.zeroes(venus_capabilities_t), output);
        }
    }
}
test "pinned profile rejects every mismatch without mutating local records" {
    const Good = fixture_value();
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_capabilities_compatible, .{null}));
    for (0..12) |mode| {
        var value = Good;
        switch (mode) {
            0 => value.wire_format_version = 0,
            1 => value.vk_xml_version = 0,
            2 => value.vk_ext_command_serialization_spec_version = 0,
            3 => value.vk_mesa_venus_protocol_spec_version = 0,
            4 => value.supports_blob_id_0 = 0,
            5 => value.supports_multiple_timelines = 0,
            6 => value.use_guest_vram = 1,
            7 => value.vk_extension_mask1[0] = 0,
            8 => value.vk_extension_mask1[12] = 1,
            9 => value.vk_extension_mask1[12] = 2,
            10 => value.allow_vk_wait_syncs = 2,
            else => value.supports_blob_id_0 = 2,
        }
        const Before = value;
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_capabilities_compatible, .{&value}));
        try std.testing.expectEqualDeep(Before, value);
    }
}
test "all explicit extension numbers remain bounded and distinguish presence" {
    var value = fixture_value();
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_capabilities_extension, .{ null, @as(u32, 1) }));
    for ([_]u32{ 0, 1024, std.math.maxInt(u32) }) |number|
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_capabilities_extension, .{ &value, number }));
    for (1..1024) |number| {
        @memset(&value.vk_extension_mask1, 0);
        value.vk_extension_mask1[0] = 1;
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_capabilities_extension, .{ &value, @as(u32, @intCast(number)) }));
        value.vk_extension_mask1[number / 32] |= @as(u32, 1) << @as(u5, @intCast(number & 31));
        try std.testing.expectEqual(@as(c_int, 1), @call(.never_inline, venus_capabilities_extension, .{ &value, @as(u32, @intCast(number)) }));
        value.vk_extension_mask1[0] &= ~@as(u32, 1);
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_capabilities_extension, .{ &value, @as(u32, @intCast(number)) }));
    }
}
