//! Bounds-checked PE routing hint; never validates or executes an image.
const std = @import("std");

/// Maximum private prefix size; inputs beyond this ceiling are not examined.
pub const PrefixBytes: usize = 4096;
/// Pure routing classification with no ownership or external state.
pub const image_kind_t = enum(u32) {
    /// GUI fallback, including unknown and malformed content.
    Gui = 0,
    /// Valid bounded header with Windows console subsystem3.
    Console = 1,
};

/// Classify borrowed immutable prefix bytes; allocation-free and thread-safe.
/// Input beyond PrefixBytes is ignored. Returns Gui for every malformed or
/// truncated header; Console only for bounded PE32/PE32+ Subsystem3. No pointer
/// is retained and no I/O, signature validation or launch permission is implied.
pub fn classify(prefix: []const u8) image_kind_t {
    const bytes = prefix[0..@min(prefix.len, PrefixBytes)];
    if (bytes.len < 64 or !std.mem.eql(u8, bytes[0..2], "MZ")) return .Gui;
    const pe_offset: usize = std.mem.readInt(u32, bytes[0x3c..0x40], .little);
    if (pe_offset < 0x40 or pe_offset > PrefixBytes - 24) return .Gui;
    if (pe_offset > bytes.len or bytes.len - pe_offset < 24) return .Gui;
    if (!std.mem.eql(u8, bytes[pe_offset..][0..4], "PE\x00\x00")) return .Gui;
    const optional_bytes = std.mem.readInt(u16, bytes[pe_offset + 20 ..][0..2], .little);
    const optional_offset = pe_offset + 24;
    if (optional_bytes < 70 or bytes.len - optional_offset < 70) return .Gui;
    const magic = std.mem.readInt(u16, bytes[optional_offset..][0..2], .little);
    if (magic != 0x10b and magic != 0x20b) return .Gui;
    const subsystem = std.mem.readInt(u16, bytes[optional_offset + 68 ..][0..2], .little);
    return if (subsystem == 3) .Console else .Gui;
}

/// C ABI private-prefix classification, with no allocation or pointer retention.
/// @param[in] bytes Optional borrowed readable prefix, NULL only for length0.
/// @param[in] length Readable size0..4096.
/// @param[out] kind_out Nonnull aligned writable u32, disjoint from input.
/// @return 0 publishes routing kind;1 rejects API arguments, leaving output.
/// @note Caller guarantees accessibility/lifetimes. Independent storage is
/// thread-safe; malformed bytes yield successful Gui, not API failure.
export fn interop_pe_classify_prefix(bytes: ?[*]const u8, length: usize, kind_out: ?*align(1) u32) c_int {
    if (length > PrefixBytes) return 1;
    const output = kind_out orelse return 1;
    const output_start = @intFromPtr(output);
    if (output_start % @alignOf(u32) != 0 or output_start > std.math.maxInt(usize) - @sizeOf(u32)) return 1;
    var prefix: []const u8 = &.{};
    if (length != 0) {
        const input = bytes orelse return 1;
        const input_start = @intFromPtr(input);
        if (input_start > std.math.maxInt(usize) - length) return 1;
        if (input_start < output_start + @sizeOf(u32) and output_start < input_start + length) return 1;
        prefix = input[0..length];
    }
    output.* = @intFromEnum(classify(prefix));
    return 0;
}

// Test-only fixtures.
fn fixture(magic: u16, subsystem: u16, offset: usize) [PrefixBytes]u8 {
    var bytes = [_]u8{0} ** PrefixBytes;
    @memcpy(bytes[0..2], "MZ");
    std.mem.writeInt(u32, bytes[0x3c..0x40], @intCast(offset), .little);
    @memcpy(bytes[offset..][0..4], "PE\x00\x00");
    std.mem.writeInt(u16, bytes[offset + 20 ..][0..2], 240, .little);
    std.mem.writeInt(u16, bytes[offset + 24 ..][0..2], magic, .little);
    std.mem.writeInt(u16, bytes[offset + 24 + 68 ..][0..2], subsystem, .little);
    return bytes;
}

test "PE32 and PE32 plus accept only console subsystem with exact bounded fields" {
    for ([_]u16{ 0x10b, 0x20b }) |magic| {
        const bytes = fixture(magic, 3, 0x80);
        try std.testing.expectEqual(image_kind_t.Console, classify(&bytes));
        try std.testing.expectEqual(image_kind_t.Console, classify(bytes[0 .. 0x80 + 24 + 70]));
        for (0..0x80 + 24 + 70) |length|
            try std.testing.expectEqual(image_kind_t.Gui, classify(bytes[0..length]));
        for ([_]u16{ 0, 1, 2, 4, 7, 9, 10, 65535 }) |subsystem|
            try std.testing.expectEqual(image_kind_t.Gui, classify(&fixture(magic, subsystem, 64)));
    }
    const exact = fixture(0x20b, 3, PrefixBytes - 94);
    try std.testing.expectEqual(image_kind_t.Console, classify(&exact));
    var beyond: [PrefixBytes + 1]u8 = undefined;
    @memcpy(beyond[0..PrefixBytes], &exact);
    beyond[PrefixBytes] = 0xff;
    try std.testing.expectEqual(image_kind_t.Console, classify(&beyond));
}

test "all malformed offset signature optional size and magic states fall back" {
    var bytes = fixture(0x10b, 3, 64);
    for ([_]u32{ 0, 63, 4073, 4096, 0xffffffff }) |offset| {
        var changed = bytes;
        std.mem.writeInt(u32, changed[0x3c..0x40], offset, .little);
        try std.testing.expectEqual(image_kind_t.Gui, classify(&changed));
    }
    for ([_]u32{ 4072, 4050, 4003 }) |offset| {
        var changed = bytes;
        std.mem.writeInt(u32, changed[0x3c..0x40], offset, .little);
        @memcpy(changed[offset..][0..4], "PE\x00\x00");
        try std.testing.expectEqual(image_kind_t.Gui, classify(&changed));
    }
    for ([_]usize{ 0, 1, 64, 65, 66, 67 }) |offset| {
        var changed = bytes;
        changed[offset] ^= 0xff;
        try std.testing.expectEqual(image_kind_t.Gui, classify(&changed));
    }
    for ([_]u16{ 0, 1, 69 }) |size| {
        var changed = bytes;
        std.mem.writeInt(u16, changed[84..86], size, .little);
        try std.testing.expectEqual(image_kind_t.Gui, classify(&changed));
    }
    for ([_]u16{ 0, 0x107, 0x10a, 0x20c, 65535 }) |magic| {
        var changed = bytes;
        std.mem.writeInt(u16, changed[88..90], magic, .little);
        try std.testing.expectEqual(image_kind_t.Gui, classify(&changed));
    }
    std.mem.writeInt(u32, bytes[0x3c..0x40], 0x200, .little);
    try std.testing.expectEqual(image_kind_t.Gui, classify(bytes[0..100]));
}

test "C boundary validates ranges alignment aliases and preserves outputs" {
    var bytes = fixture(0x20b, 3, 64);
    var output: u32 = 77;
    try std.testing.expectEqual(@as(c_int, 0), interop_pe_classify_prefix(&bytes, bytes.len, &output));
    try std.testing.expectEqual(@as(u32, 1), output);
    output = 77;
    try std.testing.expectEqual(@as(c_int, 1), interop_pe_classify_prefix(null, 1, &output));
    try std.testing.expectEqual(@as(c_int, 1), interop_pe_classify_prefix(&bytes, PrefixBytes + 1, &output));
    try std.testing.expectEqual(@as(c_int, 1), interop_pe_classify_prefix(&bytes, bytes.len, null));
    try std.testing.expectEqual(@as(c_int, 1), interop_pe_classify_prefix(@ptrFromInt(std.math.maxInt(usize) - 1), 8, &output));
    try std.testing.expectEqual(@as(c_int, 1), interop_pe_classify_prefix(&bytes, bytes.len, @ptrFromInt(std.math.maxInt(usize) - 3)));
    try std.testing.expectEqual(@as(c_int, 1), interop_pe_classify_prefix(&bytes, bytes.len, @ptrFromInt(@intFromPtr(&output) + 1)));
    try std.testing.expectEqual(@as(c_int, 1), interop_pe_classify_prefix(&bytes, bytes.len, @ptrCast(&bytes)));
    try std.testing.expectEqual(@as(u32, 77), output);
    try std.testing.expectEqual(@as(c_int, 0), interop_pe_classify_prefix(null, 0, &output));
    try std.testing.expectEqual(@as(u32, 0), output);
    var unaligned: [PrefixBytes + 1]u8 = undefined;
    @memcpy(unaligned[1..], &bytes);
    try std.testing.expectEqual(image_kind_t.Console, classify(unaligned[1..]));
}

test "deterministic random prefixes and oversized tails stay within bound" {
    var random_state = std.Random.DefaultPrng.init(0x50452d626f756e64);
    const random = random_state.random();
    var bytes: [PrefixBytes + 32]u8 = undefined;
    for (0..20000) |_| {
        random.bytes(&bytes);
        const length = random.uintLessThan(usize, bytes.len + 1);
        const result = classify(bytes[0..length]);
        try std.testing.expect(result == .Gui or result == .Console);
    }
}

extern fn interop_pe_c_fixture_main() c_int;
test "independent C ABI consumer exercises runtime argument guards" {
    try std.testing.expectEqual(@as(c_int, 0), interop_pe_c_fixture_main());
}
