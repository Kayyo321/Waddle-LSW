//! Bounded pinned command14 count/fill codecs; no transport or admission policy.
const std = @import("std");
/// Maximum actual list capacity; no storage/ownership, immutable/thread safe.
pub const MaxExtensions = 1024;
/// Maximum borrowed reply extent including unused owner capacity; immutable.
pub const MaxReplyBytes = 28 + MaxExtensions * 268;
/// Owned fixed request; no pointers/allocations, copied values are independent.
pub const request_t = struct {
    /// Entire initialized little-endian command14 request.
    bytes: [44]u8,
    /// Exact prefix extent; immutable after construction.
    used: usize = 44,
};
/// Owned normalized native-compatible extension record; caller owns its lifetime.
pub const extension_t = extern struct {
    /// NUL-terminated ASCII name with zero trailing storage.
    name: [256]u8,
    /// Exact copied native specVersion bits, no inferred range.
    version: u32,
};
/// Copied successful fill metadata; output ownership remains with caller.
pub const fill_result_t = struct {
    /// Number of output records initialized by this call.
    count: u32,
    /// True only for validated host VK_INCOMPLETE result.
    incomplete: bool,
};
const metadata_t = struct { count: u32, incomplete: bool };
comptime {
    std.debug.assert(@sizeOf(extension_t) == 260 and @alignOf(extension_t) == 4);
    std.debug.assert(@sizeOf(request_t) == 56 and @alignOf(request_t) == 8);
}
fn encode(physical_id: u64, capacity: u32) !request_t {
    if (physical_id == 0 or capacity > MaxExtensions) return error.Invalid;
    var result = request_t{ .bytes = [_]u8{0} ** 44 };
    std.mem.writeInt(u32, result.bytes[0..4], 14, .little);
    std.mem.writeInt(u32, result.bytes[4..8], 1, .little);
    std.mem.writeInt(u64, result.bytes[8..16], physical_id, .little);
    std.mem.writeInt(u64, result.bytes[24..32], 1, .little);
    std.mem.writeInt(u32, result.bytes[32..36], capacity, .little);
    std.mem.writeInt(u64, result.bytes[36..44], capacity, .little);
    return result;
}
/// [in] Nonzero translated physical identity, never dereferenced.
/// [out] Return independent owned44-byte count request; Invalid for zero ID.
/// No allocations, retained memory or shared state; thread safe.
pub fn encode_count(physical_id: u64) !request_t {
    return encode(physical_id, 0);
}
/// [in] Nonzero translated identity and actual fill capacity1..1024.
/// [out] Return owned44-byte fill request; Invalid for local ID/capacity.
/// No allocations, retention or shared state; thread safe.
pub fn encode_fill(physical_id: u64, capacity: u32) !request_t {
    if (capacity == 0) return error.Invalid;
    return encode(physical_id, capacity);
}
fn metadata(bytes: []const u8, capacity: ?usize) !metadata_t {
    if (bytes.len < 28 or bytes.len > MaxReplyBytes) return error.Corrupt;
    if (std.mem.readInt(u32, bytes[0..4], .little) != 14 or
        std.mem.readInt(u64, bytes[8..16], .little) != 1) return error.Corrupt;
    const result = std.mem.readInt(i32, bytes[4..8], .little);
    const count = std.mem.readInt(u32, bytes[16..20], .little);
    const array_count = std.mem.readInt(u64, bytes[20..28], .little);
    if (capacity) |limit| {
        if (count > limit or array_count != count or
            28 + @as(usize, count) * 268 > bytes.len) return error.Corrupt;
    } else {
        if (array_count != 0) return error.Corrupt;
        if (count > MaxExtensions) return error.Limit;
    }
    if (result < 0) return error.Backend;
    if (result != 0 and !(capacity != null and result == 5)) return error.Corrupt;
    return .{ .count = count, .incomplete = result == 5 };
}
fn name(entry: []const u8) ![]const u8 {
    if (std.mem.readInt(u64, entry[0..8], .little) != 256) return error.Corrupt;
    const storage = entry[8..264];
    const length = std.mem.indexOfScalar(u8, storage, 0) orelse return error.Corrupt;
    if (length < 4 or !std.mem.startsWith(u8, storage[0..length], "VK_")) return error.Corrupt;
    for (storage[0..length]) |value| {
        if (!std.ascii.isAlphanumeric(value) and value != '_') return error.Corrupt;
    }
    return storage[0..length];
}
/// [in] Borrow immutable accessible reply, length28..MaxReplyBytes; tail ignored.
/// [out] Return copied count0..1024; no pointer retained or output storage mutated.
/// Corrupt=wire/mode/result; Limit=valid count exceeds quota; Backend=host negative.
/// Allocation-free and thread safe for immutable inputs.
pub fn decode_count(bytes: []const u8) !u32 {
    return (try metadata(bytes, null)).count;
}
/// [in] Borrow immutable reply through both validation/copy passes; tail ignored.
/// [out] Borrow exclusive disjoint output[1..1024]; initialize only returned count.
/// Entire output stays byte-identical on Invalid(local capacity), Corrupt(wire/name/
/// duplicate/result) or Backend(host negative). Successful names own normalized bytes.
/// Return copied count/incomplete; no allocations/retention, disjoint calls thread safe.
pub fn decode_fill(bytes: []const u8, output: []extension_t) !fill_result_t {
    if (output.len == 0 or output.len > MaxExtensions) return error.Invalid;
    const decoded = try metadata(bytes, output.len);
    for (0..decoded.count) |index| {
        const start = 28 + index * 268;
        const current = try name(bytes[start..][0..268]);
        for (0..index) |previous| {
            const previous_start = 28 + previous * 268;
            if (std.mem.eql(u8, current, try name(bytes[previous_start..][0..268]))) return error.Corrupt;
        }
    }
    for (output[0..decoded.count], 0..) |*record, index| {
        const start = 28 + index * 268;
        const current = try name(bytes[start..][0..268]);
        record.* = .{ .name = [_]u8{0} ** 256, .version = std.mem.readInt(u32, bytes[start + 264 ..][0..4], .little) };
        @memcpy(record.name[0..current.len], current);
    }
    return .{ .count = decoded.count, .incomplete = decoded.incomplete };
}

// Test-only fixtures.
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
extern fn venus_extensions_test_request(u64, u32, [*]u8) usize;
extern fn venus_extensions_test_reply(i32, u32, ?[*]const c.VkExtensionProperties, [*]u8, usize) usize;

fn properties_fixture(properties: []c.VkExtensionProperties) void {
    for (properties, 0..) |*property, index| {
        property.* = std.mem.zeroes(c.VkExtensionProperties);
        const label = std.fmt.bufPrint(property.extensionName[0..], "VK_test_{d:0>4}", .{index}) catch unreachable;
        property.extensionName[label.len] = 0;
        @memset(property.extensionName[label.len + 1 ..], 0xa5);
        property.specVersion = std.math.maxInt(u32) - @as(u32, @intCast(index));
    }
}

test "pinned count and fill requests match exact native generator boundaries" {
    var expected: [44]u8 = undefined;
    for ([_]u64{ 1, 7, std.math.maxInt(u64) }) |id| {
        const count = try encode_count(id);
        try std.testing.expectEqual(@as(usize, 44), venus_extensions_test_request(id, 0, &expected));
        try std.testing.expectEqualSlices(u8, &expected, &count.bytes);
        for ([_]u32{ 1, 17, 1024 }) |capacity| {
            const fill = try encode_fill(id, capacity);
            try std.testing.expectEqual(@as(usize, 44), venus_extensions_test_request(id, capacity, &expected));
            try std.testing.expectEqualSlices(u8, &expected, &fill.bytes);
            try std.testing.expectEqual(@as(usize, 44), fill.used);
        }
    }
    try std.testing.expectError(error.Invalid, encode_count(0));
    try std.testing.expectError(error.Invalid, encode_fill(0, 1));
    try std.testing.expectError(error.Invalid, encode_fill(1, 0));
    try std.testing.expectError(error.Invalid, encode_fill(1, 1025));
}

test "count metadata rejects truncation quota mode command presence and host errors" {
    var bytes: [32]u8 = [_]u8{0xa5} ** 32;
    for ([_]u32{ 0, 1, 1024 }) |count| {
        try std.testing.expectEqual(@as(usize, 28), venus_extensions_test_reply(0, count, null, &bytes, bytes.len));
        try std.testing.expectEqual(count, try @call(.never_inline, decode_count, .{&bytes}));
        for (0..28) |cut| try std.testing.expectError(error.Corrupt, @call(.never_inline, decode_count, .{bytes[0..cut]}));
    }
    const original = bytes;
    for ([_]usize{ 0, 8, 20 }) |offset| {
        bytes = original;
        bytes[offset] ^= 1;
        try std.testing.expectError(error.Corrupt, @call(.never_inline, decode_count, .{&bytes}));
    }
    bytes = original;
    std.mem.writeInt(u32, bytes[16..20], 1025, .little);
    try std.testing.expectError(error.Limit, @call(.never_inline, decode_count, .{&bytes}));
    for ([_]i32{ -1, std.math.minInt(i32), 1, 5, std.math.maxInt(i32) }) |result| {
        _ = venus_extensions_test_reply(result, 0, null, &bytes, bytes.len);
        try std.testing.expectError(if (result < 0) error.Backend else error.Corrupt, @call(.never_inline, decode_count, .{&bytes}));
    }
}

test "actual renderer fills normalize names preserve versions and untouched capacity" {
    var properties: [2]c.VkExtensionProperties = undefined;
    properties_fixture(&properties);
    var bytes: [28 + 2 * 268 + 16]u8 = [_]u8{0xcc} ** (28 + 2 * 268 + 16);
    var output: [3]extension_t = undefined;
    for ([_]i32{ 0, 5 }) |result| {
        @memset(std.mem.asBytes(&output), 0xaa);
        const tail = output[2];
        try std.testing.expectEqual(@as(usize, 564), venus_extensions_test_reply(result, 2, &properties, &bytes, bytes.len));
        const filled = try @call(.never_inline, decode_fill, .{ &bytes, &output });
        try std.testing.expectEqual(@as(u32, 2), filled.count);
        try std.testing.expectEqual(result == 5, filled.incomplete);
        for (output[0..2], 0..) |record, index| {
            try std.testing.expectEqual(properties[index].specVersion, record.version);
            try std.testing.expectEqualSlices(u8, properties[index].extensionName[0..12], record.name[0..12]);
            try std.testing.expect(std.mem.allEqual(u8, record.name[12..], 0));
        }
        try std.testing.expectEqualSlices(u8, std.mem.asBytes(&tail), std.mem.asBytes(&output[2]));
        _ = venus_extensions_test_reply(result, 0, null, &bytes, bytes.len);
        const before = output;
        try std.testing.expectEqual(@as(u32, 0), (try @call(.never_inline, decode_fill, .{ &bytes, &output })).count);
        try std.testing.expectEqualSlices(u8, std.mem.asBytes(&before), std.mem.asBytes(&output));
    }
}

test "complete malformed fills preserve every output byte before publication" {
    var properties: [2]c.VkExtensionProperties = undefined;
    properties_fixture(&properties);
    var bytes: [564]u8 = undefined;
    _ = venus_extensions_test_reply(0, 2, &properties, &bytes, bytes.len);
    const original = bytes;
    var output: [2]extension_t = undefined;
    @memset(std.mem.asBytes(&output), 0xaa);
    const before = output;
    for (0..bytes.len) |cut| {
        try std.testing.expectError(error.Corrupt, @call(.never_inline, decode_fill, .{ bytes[0..cut], &output }));
        try std.testing.expectEqualSlices(u8, std.mem.asBytes(&before), std.mem.asBytes(&output));
    }
    for ([_]usize{ 0, 8, 16, 20, 28, 28 + 268 }) |offset| {
        bytes = original;
        bytes[offset] ^= 1;
        try std.testing.expectError(error.Corrupt, @call(.never_inline, decode_fill, .{ &bytes, &output }));
        try std.testing.expectEqualSlices(u8, std.mem.asBytes(&before), std.mem.asBytes(&output));
    }
    for ([_]u8{ 0, 'X', ' ', 0xff }) |value| {
        bytes = original;
        bytes[36] = value;
        try std.testing.expectError(error.Corrupt, @call(.never_inline, decode_fill, .{ &bytes, &output }));
    }
    for ([_]u8{ ' ', 0xff, '-' }) |value| {
        bytes = original;
        bytes[40] = value;
        try std.testing.expectError(error.Corrupt, @call(.never_inline, decode_fill, .{ &bytes, &output }));
    }
    bytes = original;
    @memset(bytes[36..292], 'A');
    try std.testing.expectError(error.Corrupt, @call(.never_inline, decode_fill, .{ &bytes, &output }));
    bytes = original;
    @memcpy(bytes[304..560], bytes[36..292]);
    try std.testing.expectError(error.Corrupt, @call(.never_inline, decode_fill, .{ &bytes, &output }));
    for ([_]i32{ -4, 1, 6 }) |result| {
        _ = venus_extensions_test_reply(result, 2, &properties, &bytes, bytes.len);
        try std.testing.expectError(if (result < 0) error.Backend else error.Corrupt, @call(.never_inline, decode_fill, .{ &bytes, &output }));
    }
    try std.testing.expectError(error.Invalid, @call(.never_inline, decode_fill, .{ &bytes, output[0..0] }));
    try std.testing.expectEqualSlices(u8, std.mem.asBytes(&before), std.mem.asBytes(&output));
}

test "maximum reply exact quota long names last duplicate and oversized buffers" {
    const allocator = std.testing.allocator;
    const properties = try allocator.alloc(c.VkExtensionProperties, MaxExtensions);
    defer allocator.free(properties);
    const bytes = try allocator.alloc(u8, MaxReplyBytes + 1);
    defer allocator.free(bytes);
    const output = try allocator.alloc(extension_t, MaxExtensions + 1);
    defer allocator.free(output);
    properties_fixture(properties);
    @memset(std.mem.sliceAsBytes(output), 0xaa);
    try std.testing.expectEqual(@as(usize, MaxReplyBytes), venus_extensions_test_reply(0, MaxExtensions, properties.ptr, bytes.ptr, MaxReplyBytes));
    const filled = try @call(.never_inline, decode_fill, .{ bytes[0..MaxReplyBytes], output[0..MaxExtensions] });
    try std.testing.expectEqual(@as(u32, MaxExtensions), filled.count);
    try std.testing.expectEqual(properties[1023].specVersion, output[1023].version);
    try std.testing.expectError(error.Corrupt, @call(.never_inline, decode_count, .{bytes}));
    try std.testing.expectError(error.Corrupt, @call(.never_inline, decode_fill, .{ bytes, output[0..MaxExtensions] }));
    try std.testing.expectError(error.Invalid, @call(.never_inline, decode_fill, .{ bytes[0..MaxReplyBytes], output }));
    properties[1023] = properties[0];
    _ = venus_extensions_test_reply(0, MaxExtensions, properties.ptr, bytes.ptr, MaxReplyBytes);
    const preserved = output[1023];
    try std.testing.expectError(error.Corrupt, @call(.never_inline, decode_fill, .{ bytes[0..MaxReplyBytes], output[0..MaxExtensions] }));
    try std.testing.expectEqualSlices(u8, std.mem.asBytes(&preserved), std.mem.asBytes(&output[1023]));
    @memset(properties[0].extensionName[0..], 'a');
    @memcpy(properties[0].extensionName[0..3], "VK_");
    properties[0].extensionName[255] = 0;
    properties[0].specVersion = 0;
    _ = venus_extensions_test_reply(0, 1, properties.ptr, bytes.ptr, bytes.len);
    try std.testing.expectEqual(@as(u32, 1), (try @call(.never_inline, decode_fill, .{ bytes[0..296], output[0..1] })).count);
    try std.testing.expectEqual(@as(u8, 0), output[0].name[255]);
    try std.testing.expectEqual(@as(u32, 0), output[0].version);
}
