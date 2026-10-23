//! Native Vulkan ABI adapter; pinned fixed-structure wire parsing stays bounds checked.
const std = @import("std");
const c = @cImport({
    @cInclude("waddle/venus_values.h");
});
const MaxBytes: usize = 16777216;
const reader_t = struct {
    bytes: []const u8,
    used: usize = 0,
    fn take(self: *reader_t, count: usize) ![]const u8 {
        if (count > self.bytes.len - self.used) return error.Bounds;
        const bytes = self.bytes[self.used..][0..count];
        self.used += count;
        return bytes;
    }
    fn scalar(self: *reader_t, comptime scalar_t: type) !scalar_t {
        const native_bytes = @sizeOf(scalar_t);
        const bytes = try self.take(@max(native_bytes, 4));
        switch (@typeInfo(scalar_t)) {
            .Int => return std.mem.readInt(scalar_t, bytes[0..native_bytes], .little),
            .Float => {
                const bits_t = std.meta.Int(.unsigned, @bitSizeOf(scalar_t));
                const decoded: scalar_t = @bitCast(std.mem.readInt(
                    bits_t,
                    bytes[0..native_bytes],
                    .little,
                ));
                if (!std.math.isFinite(decoded)) return error.Value;
                return decoded;
            },
            else => @compileError("Unsupported Vulkan scalar wire type"),
        }
    }
    fn value(self: *reader_t, comptime value_t: type) !value_t {
        switch (@typeInfo(value_t)) {
            .Int, .Float => return self.scalar(value_t),
            .Struct => |info| {
                var result = std.mem.zeroes(value_t);
                inline for (info.fields) |field| {
                    @field(result, field.name) = try self.value(field.type);
                }
                return result;
            },
            .Array => |info| {
                if (try self.scalar(u64) != info.len) return error.Value;
                var result: value_t = undefined;
                if (@typeInfo(info.child) == .Int and @sizeOf(info.child) == 1) {
                    const bytes = try self.take((info.len + 3) & ~@as(usize, 3));
                    @memcpy(std.mem.asBytes(&result), bytes[0..info.len]);
                } else {
                    for (&result) |*item| item.* = try self.value(info.child);
                }
                return result;
            },
            else => @compileError("Variable/pointer Vulkan outputs require an explicit codec"),
        }
    }
};
fn valid_storage(output: usize, output_bytes: usize, bytes: usize, length: usize) bool {
    if (length > MaxBytes or length > std.math.maxInt(usize) - bytes or
        output_bytes > std.math.maxInt(usize) - output) return false;
    return !(output < bytes + length and bytes < output + output_bytes);
}
fn decode(
    comptime value_t: type,
    bytes: ?[*]const u8,
    length: usize,
    command: u32,
) !value_t {
    var reader = reader_t{ .bytes = bytes.?[0..length] };
    if (try reader.scalar(u32) != command or try reader.scalar(u64) != 1)
        return error.Value;
    return reader.value(value_t);
}
fn booleans(value: anytype) bool {
    inline for (@typeInfo(@TypeOf(value)).Struct.fields) |field| {
        if (@field(value, field.name) > 1) return false;
    }
    return true;
}

/// Core property reply conversion; header specifies ABI/bounds/nullability/ownership/errors.
export fn venus_values_properties_decode(
    output: ?*c.venus_vk_properties_t,
    bytes: ?[*]const u8,
    length: usize,
) c_int {
    if (output == null or bytes == null) return c.RingInvalid;
    if (!valid_storage(
        @intFromPtr(output.?),
        @sizeOf(c.venus_vk_properties_t),
        @intFromPtr(bytes.?),
        length,
    )) return c.RingInvalid;
    const value = decode(c.venus_vk_properties_t, bytes, length, 6) catch
        return c.RingCorrupt;
    if (value.apiVersion < (1 << 22) or value.apiVersion >> 29 != 0 or
        value.deviceType > 4 or !booleans(value.sparseProperties)) return c.RingCorrupt;
    const name = std.mem.asBytes(&value.deviceName);
    const end = std.mem.indexOfScalar(u8, name, 0) orelse return c.RingCorrupt;
    if (!std.unicode.utf8ValidateSlice(name[0..end])) return c.RingCorrupt;
    output.?.* = value;
    return c.RingOk;
}

/// Core feature reply conversion; header specifies preserved-output and strict boolean contract.
export fn venus_values_features_decode(
    output: ?*c.venus_vk_features_t,
    bytes: ?[*]const u8,
    length: usize,
) c_int {
    if (output == null or bytes == null) return c.RingInvalid;
    if (!valid_storage(
        @intFromPtr(output.?),
        @sizeOf(c.venus_vk_features_t),
        @intFromPtr(bytes.?),
        length,
    )) return c.RingInvalid;
    const value = decode(c.venus_vk_features_t, bytes, length, 3) catch
        return c.RingCorrupt;
    if (!booleans(value)) return c.RingCorrupt;
    output.?.* = value;
    return c.RingOk;
}

/// Core memory reply conversion; header specifies count/index checks and ownership/lifetime.
export fn venus_values_memory_decode(
    output: ?*c.venus_vk_memory_t,
    bytes: ?[*]const u8,
    length: usize,
) c_int {
    if (output == null or bytes == null) return c.RingInvalid;
    if (!valid_storage(
        @intFromPtr(output.?),
        @sizeOf(c.venus_vk_memory_t),
        @intFromPtr(bytes.?),
        length,
    )) return c.RingInvalid;
    const value = decode(c.venus_vk_memory_t, bytes, length, 8) catch
        return c.RingCorrupt;
    if (value.memoryTypeCount == 0 or value.memoryTypeCount > 32 or
        value.memoryHeapCount == 0 or value.memoryHeapCount > 16) return c.RingCorrupt;
    for (value.memoryTypes[0..value.memoryTypeCount]) |memory| {
        if (memory.heapIndex >= value.memoryHeapCount) return c.RingCorrupt;
    }
    output.?.* = value;
    return c.RingOk;
}

// Test-only fixtures.
extern fn venus_values_test_encode(u32, [*]u8, usize) usize;
const fixture_t = struct {
    fn expect_decode(
        comptime value_t: type,
        comptime function: anytype,
        output: *value_t,
        bytes: ?[*]const u8,
        length: usize,
        expected: c_int,
    ) !void {
        const before = output.*;
        try std.testing.expectEqual(expected, @call(.never_inline, function, .{
            output, bytes, length,
        }));
        if (expected != c.RingOk) try std.testing.expect(std.meta.eql(before, output.*));
    }
};

test "independent pinned renderer encoder, all truncated prefixes and local bounds" {
    const bytes = try std.testing.allocator.alignedAlloc(u8, 8, 4096);
    defer std.testing.allocator.free(bytes);
    const Cases = .{
        .{
            .record_t = c.venus_vk_properties_t,
            .kind = 6,
            .function = venus_values_properties_decode,
        },
        .{ .record_t = c.venus_vk_features_t, .kind = 3, .function = venus_values_features_decode },
        .{ .record_t = c.venus_vk_memory_t, .kind = 8, .function = venus_values_memory_decode },
    };
    inline for (Cases) |case| {
        var target: *const @TypeOf(case.function) = case.function;
        const opaque_target: *volatile @TypeOf(target) = &target;
        const function = opaque_target.*;
        const length = venus_values_test_encode(case.kind, bytes.ptr, bytes.len);
        try std.testing.expect(length > 12);
        var output = std.mem.zeroes(case.record_t);
        try fixture_t.expect_decode(
            case.record_t,
            case.function,
            &output,
            bytes.ptr,
            length,
            c.RingOk,
        );
        if (case.kind == 6) {
            try std.testing.expectEqual(@as(u32, 42), output.vendorID);
            try std.testing.expectEqual(@as(usize, 64), output.limits.minMemoryMapAlignment);
            try std.testing.expectEqual(@as(f32, 123.25), output.limits.timestampPeriod);
        } else if (case.kind == 8) {
            try std.testing.expectEqual(@as(u32, 1), output.memoryTypeCount);
            try std.testing.expectEqual(@as(u64, 1048576), output.memoryHeaps[0].size);
        } else {
            try std.testing.expectEqual(@as(u32, 1), output.robustBufferAccess);
            try std.testing.expectEqual(@as(u32, 1), output.tessellationShader);
        }
        for (0..length) |prefix| try fixture_t.expect_decode(
            case.record_t,
            case.function,
            &output,
            bytes.ptr,
            prefix,
            c.RingCorrupt,
        );
        try fixture_t.expect_decode(
            case.record_t,
            case.function,
            &output,
            bytes.ptr,
            length + 16,
            c.RingOk,
        );
        try fixture_t.expect_decode(
            case.record_t,
            case.function,
            &output,
            null,
            length,
            c.RingInvalid,
        );
        try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, function, .{
            null, bytes.ptr, length,
        }));
        try fixture_t.expect_decode(
            case.record_t,
            case.function,
            &output,
            bytes.ptr,
            MaxBytes + 1,
            c.RingInvalid,
        );
        const overflow: [*]const u8 = @ptrFromInt(std.math.maxInt(usize) - 7);
        try fixture_t.expect_decode(
            case.record_t,
            case.function,
            &output,
            overflow,
            16,
            c.RingInvalid,
        );
        const overlapping: *case.record_t = @ptrCast(@alignCast(bytes.ptr));
        try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, function, .{
            overlapping, bytes.ptr, length,
        }));
        const contained: [*]const u8 = @as([*]const u8, @ptrCast(&output)) + 8;
        try fixture_t.expect_decode(
            case.record_t,
            case.function,
            &output,
            contained,
            16,
            c.RingInvalid,
        );
        const bad_output: *case.record_t = @ptrFromInt(std.math.maxInt(usize) - 7);
        try std.testing.expectEqual(c.RingInvalid, function(bad_output, bytes.ptr, length));
        bytes[0] ^= 1;
        try fixture_t.expect_decode(
            case.record_t,
            case.function,
            &output,
            bytes.ptr,
            length,
            c.RingCorrupt,
        );
        bytes[0] ^= 1;
        bytes[4] = 0;
        try fixture_t.expect_decode(
            case.record_t,
            case.function,
            &output,
            bytes.ptr,
            length,
            c.RingCorrupt,
        );
    }
}

test "properties reject malformed tags, names, API, kinds, sparse bools and NaN" {
    const bytes = try std.testing.allocator.alignedAlloc(u8, 8, 4096);
    defer std.testing.allocator.free(bytes);
    var output = std.mem.zeroes(c.venus_vk_properties_t);
    for (0..8) |mutation| {
        const length = venus_values_test_encode(6, bytes.ptr, bytes.len);
        switch (mutation) {
            0 => std.mem.writeInt(u64, bytes[32..40], 255, .little),
            1 => @memset(bytes[40..296], 'a'),
            2 => bytes[40] = 0xff,
            3 => std.mem.writeInt(u32, bytes[12..16], 0, .little),
            4 => std.mem.writeInt(u32, bytes[12..16], 0x20400000, .little),
            5 => std.mem.writeInt(u32, bytes[28..32], 5, .little),
            6 => std.mem.writeInt(u32, bytes[length - 4 ..][0..4], 2, .little),
            7 => {
                var found = false;
                var offset: usize = 0;
                while (offset + 4 <= length) : (offset += 4) {
                    if (std.mem.readInt(u32, bytes[offset..][0..4], .little) == 0x42f68000) {
                        std.mem.writeInt(u32, bytes[offset..][0..4], 0x7fc00000, .little);
                        found = true;
                        break;
                    }
                }
                try std.testing.expect(found);
            },
            else => unreachable,
        }
        try fixture_t.expect_decode(
            c.venus_vk_properties_t,
            venus_values_properties_decode,
            &output,
            bytes.ptr,
            length,
            c.RingCorrupt,
        );
    }
}

test "features and memory reject unsafe counts, boolean values and heap indices" {
    const bytes = try std.testing.allocator.alignedAlloc(u8, 8, 4096);
    defer std.testing.allocator.free(bytes);
    var features = std.mem.zeroes(c.venus_vk_features_t);
    const feature_bytes = venus_values_test_encode(3, bytes.ptr, bytes.len);
    std.mem.writeInt(u32, bytes[12..16], 2, .little);
    try fixture_t.expect_decode(
        c.venus_vk_features_t,
        venus_values_features_decode,
        &features,
        bytes.ptr,
        feature_bytes,
        c.RingCorrupt,
    );
    var memory = std.mem.zeroes(c.venus_vk_memory_t);
    const Mutations = [_][2]usize{
        .{ 12, 0 }, .{ 12, 33 }, .{ 280, 0 },  .{ 280, 17 },
        .{ 28, 1 }, .{ 16, 31 }, .{ 284, 15 },
    };
    for (Mutations) |mutation| {
        const length = venus_values_test_encode(8, bytes.ptr, bytes.len);
        std.mem.writeInt(u32, bytes[mutation[0]..][0..4], @intCast(mutation[1]), .little);
        try fixture_t.expect_decode(
            c.venus_vk_memory_t,
            venus_values_memory_decode,
            &memory,
            bytes.ptr,
            length,
            c.RingCorrupt,
        );
    }
}

test "multiple live memory types validate every heap index" {
    const bytes = try std.testing.allocator.alignedAlloc(u8, 8, 4096);
    defer std.testing.allocator.free(bytes);
    const length = venus_values_test_encode(8, bytes.ptr, bytes.len);
    std.mem.writeInt(u32, bytes[12..16], 2, .little);
    var memory: c.venus_vk_memory_t = undefined;
    try std.testing.expectEqual(c.RingOk, venus_values_memory_decode(&memory, bytes.ptr, length));
    std.mem.writeInt(u32, bytes[36..40], 1, .little);
    try fixture_t.expect_decode(
        c.venus_vk_memory_t,
        venus_values_memory_decode,
        &memory,
        bytes.ptr,
        length,
        c.RingCorrupt,
    );
}
