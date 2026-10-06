//! Bounded production physical enumeration/query wire boundary; no native wire pointers.
const std = @import("std");
const c = @cImport({
    @cInclude("waddle/venus_query_wire.h");
});
const MaxBytes: usize = 16777216;
const MaxDevices: usize = 16;
const writer_t = struct {
    bytes: [164]u8 = undefined,
    used: usize = 0,
    fn put(self: *writer_t, comptime word_t: type, value: word_t) void {
        const count = @sizeOf(word_t);
        std.mem.writeInt(word_t, self.bytes[self.used..][0..count], value, .little);
        self.used += count;
    }
};
const reader_t = struct {
    bytes: []const u8,
    used: usize = 0,
    fn get(self: *reader_t, comptime word_t: type) !word_t {
        const count = @sizeOf(word_t);
        if (count > self.bytes.len - self.used) return error.Bounds;
        const value = std.mem.readInt(word_t, self.bytes[self.used..][0..count], .little);
        self.used += count;
        return value;
    }
};
fn overlap(a: usize, a_bytes: usize, b: usize, b_bytes: usize) bool {
    if (a_bytes > std.math.maxInt(usize) - a or b_bytes > std.math.maxInt(usize) - b)
        return true;
    return a < b + b_bytes and b < a + a_bytes;
}
fn output_valid(bytes: ?[*]u8, capacity: usize, written: ?*usize) bool {
    if (bytes == null or written == null or capacity < 8 or capacity > MaxBytes) return false;
    return !overlap(@intFromPtr(bytes.?), capacity, @intFromPtr(written.?), @sizeOf(usize));
}
fn ids_valid(ids: ?[*]const u64, count: u32) bool {
    if (count > MaxDevices or (ids == null) != (count == 0)) return false;
    if (ids) |values| {
        if (@intFromPtr(values) > std.math.maxInt(usize) - @as(usize, count) * @sizeOf(u64)) return false;
        for (values[0..count], 0..) |id, index| {
            if (id == 0) return false;
            for (values[0..index]) |previous| if (previous == id) return false;
        }
    }
    return true;
}
fn publish(writer: *const writer_t, bytes: [*]u8, capacity: usize, written: *usize) c_int {
    if (writer.used > capacity) return c.RingLimit;
    @memcpy(bytes[0..writer.used], writer.bytes[0..writer.used]);
    written.* = writer.used;
    return c.RingOk;
}
/// Count/fill encoder; C header specifies reservation/bounds/ownership/thread contract.
export fn venus_query_wire_enumerate(
    instance_id: u64,
    ids: ?[*]const u64,
    count: u32,
    bytes: ?[*]u8,
    capacity: usize,
    written: ?*usize,
) c_int {
    if (instance_id == 0 or !output_valid(bytes, capacity, written) or !ids_valid(ids, count))
        return c.RingInvalid;
    if (ids) |values| {
        if (overlap(@intFromPtr(values), count * 8, @intFromPtr(bytes.?), capacity) or
            overlap(@intFromPtr(values), count * 8, @intFromPtr(written.?), 8))
            return c.RingInvalid;
        for (values[0..count]) |id| if (id == instance_id) return c.RingInvalid;
    }
    var writer = writer_t{};
    writer.put(u32, 2);
    writer.put(u32, 1);
    writer.put(u64, instance_id);
    writer.put(u64, 1);
    writer.put(u32, count);
    writer.put(u64, count);
    if (ids) |values| for (values[0..count]) |id| writer.put(u64, id);
    return publish(&writer, bytes.?, capacity, written.?);
}
fn decode_enumeration(reader: *reader_t, ids: ?[*]const u64, capacity: u32) !struct {
    result: i32,
    count: u32,
} {
    if (try reader.get(u32) != 2) return error.Value;
    const result = try reader.get(i32);
    if (try reader.get(u64) != 1) return error.Value;
    const count = try reader.get(u32);
    const array_count = try reader.get(u64);
    if (result < 0) {
        if (count != 0 or array_count != 0) return error.Value;
    } else if (capacity == 0) {
        if (result != 0 or count > MaxDevices or array_count != 0) return error.Value;
    } else {
        if ((result != 0 and result != 5) or count > capacity or array_count != count)
            return error.Value;
        for (ids.?[0..count]) |id| if (try reader.get(u64) != id) return error.Value;
    }
    return .{ .result = result, .count = count };
}
/// Reply validation; C header specifies preserved outputs and exact reserved identity contract.
export fn venus_query_wire_enumerate_reply(
    result: ?*i32,
    count: ?*u32,
    ids: ?[*]const u64,
    capacity: u32,
    bytes: ?[*]const u8,
    length: usize,
) c_int {
    if (result == null or count == null or bytes == null or length > MaxBytes or
        !ids_valid(ids, capacity)) return c.RingInvalid;
    const result_address = @intFromPtr(result.?);
    const count_address = @intFromPtr(count.?);
    const bytes_address = @intFromPtr(bytes.?);
    if (overlap(result_address, 4, count_address, 4) or
        overlap(result_address, 4, bytes_address, length) or
        overlap(count_address, 4, bytes_address, length)) return c.RingInvalid;
    if (ids) |values| {
        if (overlap(@intFromPtr(values), capacity * 8, result_address, 4) or
            overlap(@intFromPtr(values), capacity * 8, count_address, 4) or
            overlap(@intFromPtr(values), capacity * 8, bytes_address, length))
            return c.RingInvalid;
    }
    var reader = reader_t{ .bytes = bytes.?[0..length] };
    const decoded = decode_enumeration(&reader, ids, capacity) catch return c.RingCorrupt;
    result.?.* = decoded.result;
    count.?.* = decoded.count;
    return c.RingOk;
}
/// Fixed-query encoder; C header specifies supported commands and exact partial wire tags.
export fn venus_query_wire_fixed(
    command: u32,
    physical_id: u64,
    bytes: ?[*]u8,
    capacity: usize,
    written: ?*usize,
) c_int {
    if (physical_id == 0 or (command != 3 and command != 6 and command != 8) or
        !output_valid(bytes, capacity, written)) return c.RingInvalid;
    var writer = writer_t{};
    writer.put(u32, command);
    writer.put(u32, 1);
    writer.put(u64, physical_id);
    writer.put(u64, 1);
    if (command == 8) {
        writer.put(u64, 32);
        writer.put(u64, 16);
    }
    return publish(&writer, bytes.?, capacity, written.?);
}

// Test-only fixtures.
extern fn venus_query_test_encode(u32, u64, ?[*]const u64, u32, [*]u8, usize) usize;
const fixture_t = struct {
    fn enumerate(id: u64, ids: ?[*]const u64, count: u32, bytes: ?[*]u8, capacity: usize, written: ?*usize) c_int {
        var target: *const @TypeOf(venus_query_wire_enumerate) = venus_query_wire_enumerate;
        return @as(*volatile @TypeOf(target), &target).*(id, ids, count, bytes, capacity, written);
    }
    fn fixed(command: u32, id: u64, bytes: ?[*]u8, capacity: usize, written: ?*usize) c_int {
        var target: *const @TypeOf(venus_query_wire_fixed) = venus_query_wire_fixed;
        return @as(*volatile @TypeOf(target), &target).*(command, id, bytes, capacity, written);
    }
    fn reply(result: ?*i32, count: ?*u32, ids: ?[*]const u64, capacity: u32, bytes: ?[*]const u8, length: usize) c_int {
        var target: *const @TypeOf(venus_query_wire_enumerate_reply) =
            venus_query_wire_enumerate_reply;
        return @as(*volatile @TypeOf(target), &target).*(result, count, ids, capacity, bytes, length);
    }
    fn packet(result: i32, count: u32, array_count: u64, ids: []const u64) writer_t {
        var writer = writer_t{};
        writer.put(u32, 2);
        writer.put(i32, result);
        writer.put(u64, 1);
        writer.put(u32, count);
        writer.put(u64, array_count);
        for (ids) |id| writer.put(u64, id);
        return writer;
    }
};
test "requests match independent immutable guest generator" {
    const bytes = try std.testing.allocator.alignedAlloc(u8, 8, 4096);
    defer std.testing.allocator.free(bytes);
    const expected = try std.testing.allocator.alloc(u8, 4096);
    defer std.testing.allocator.free(expected);
    var ids: [MaxDevices]u64 = undefined;
    for (&ids, 0..) |*id, index| id.* = index + 2;
    var written: usize = 99;
    for (0..MaxDevices + 1) |count| {
        const values: ?[*]const u64 = if (count == 0) null else &ids;
        try std.testing.expectEqual(c.RingOk, fixture_t.enumerate(1, values, @intCast(count), bytes.ptr, bytes.len, &written));
        const size = venus_query_test_encode(2, 1, values, @intCast(count), expected.ptr, expected.len);
        try std.testing.expectEqual(size, written);
        try std.testing.expectEqualSlices(u8, expected[0..size], bytes[0..written]);
    }
    for ([_]u32{ 3, 6, 8 }) |command| {
        try std.testing.expectEqual(c.RingOk, fixture_t.fixed(command, 2, bytes.ptr, bytes.len, &written));
        const size = venus_query_test_encode(command, 2, null, 0, expected.ptr, expected.len);
        try std.testing.expectEqual(size, written);
        try std.testing.expectEqualSlices(u8, expected[0..size], bytes[0..written]);
    }
}
test "every reply prefix identity and malformed result preserves outputs" {
    const ids = [_]u64{ 2, 3 };
    var result: i32 = 99;
    var count: u32 = 99;
    for ([_]i32{ 0, 5, -1 }) |native_result| {
        const fill = native_result >= 0;
        var packet = fixture_t.packet(native_result, if (fill) 2 else 0, if (fill) 2 else 0, if (fill) &ids else &.{});
        for (0..packet.used) |length| {
            result = 99;
            count = 99;
            try std.testing.expectEqual(c.RingCorrupt, fixture_t.reply(&result, &count, &ids, 2, &packet.bytes, length));
            try std.testing.expectEqual(@as(i32, 99), result);
            try std.testing.expectEqual(@as(u32, 99), count);
        }
        try std.testing.expectEqual(c.RingOk, fixture_t.reply(&result, &count, &ids, 2, &packet.bytes, packet.used));
        try std.testing.expectEqual(native_result, result);
    }
    for ([_]u32{ 0, 16 }) |number| {
        var packet = fixture_t.packet(0, number, 0, &.{});
        try std.testing.expectEqual(c.RingOk, fixture_t.reply(&result, &count, null, 0, &packet.bytes, packet.used));
        try std.testing.expectEqual(number, count);
    }
    const mutations = [_]struct { offset: usize, value: u8, fill: bool }{
        .{ .offset = 0, .value = 3, .fill = true },    .{ .offset = 4, .value = 1, .fill = true },
        .{ .offset = 8, .value = 0, .fill = true },    .{ .offset = 16, .value = 3, .fill = true },
        .{ .offset = 20, .value = 1, .fill = true },   .{ .offset = 28, .value = 9, .fill = true },
        .{ .offset = 16, .value = 17, .fill = false }, .{ .offset = 20, .value = 1, .fill = false },
        .{ .offset = 4, .value = 5, .fill = false },
    };
    for (mutations) |mutation| {
        var packet = fixture_t.packet(0, if (mutation.fill) 2 else 0, if (mutation.fill) 2 else 0, if (mutation.fill) &ids else &.{});
        packet.bytes[mutation.offset] = mutation.value;
        result = 99;
        count = 99;
        try std.testing.expectEqual(c.RingCorrupt, fixture_t.reply(&result, &count, if (mutation.fill) &ids else null, if (mutation.fill) 2 else 0, &packet.bytes, packet.used));
        try std.testing.expectEqual(@as(i32, 99), result);
        try std.testing.expectEqual(@as(u32, 99), count);
    }
    for ([_]bool{ false, true }) |array| {
        var packet = fixture_t.packet(-1, if (array) 0 else 1, if (array) 1 else 0, &.{});
        try std.testing.expectEqual(c.RingCorrupt, fixture_t.reply(&result, &count, &ids, 2, &packet.bytes, packet.used));
    }
}
test "local invalid bounds identities capacity and aliases preserve storage" {
    var bytes: [256]u8 align(8) = undefined;
    @memset(&bytes, 0xa5);
    var written: usize = 99;
    var ids = [_]u64{ 2, 3 };
    const invalid = [_]struct { id: u64, ids: ?[*]const u64, count: u32, bytes: ?[*]u8, capacity: usize, written: ?*usize }{
        .{ .id = 0, .ids = null, .count = 0, .bytes = &bytes, .capacity = 256, .written = &written },
        .{ .id = 1, .ids = null, .count = 1, .bytes = &bytes, .capacity = 256, .written = &written },
        .{ .id = 1, .ids = &ids, .count = 0, .bytes = &bytes, .capacity = 256, .written = &written },
        .{ .id = 1, .ids = &ids, .count = 17, .bytes = &bytes, .capacity = 256, .written = &written },
        .{ .id = 2, .ids = &ids, .count = 2, .bytes = &bytes, .capacity = 256, .written = &written },
        .{ .id = 1, .ids = null, .count = 0, .bytes = null, .capacity = 256, .written = &written },
        .{ .id = 1, .ids = null, .count = 0, .bytes = &bytes, .capacity = 7, .written = &written },
        .{ .id = 1, .ids = null, .count = 0, .bytes = &bytes, .capacity = MaxBytes + 1, .written = &written },
        .{ .id = 1, .ids = null, .count = 0, .bytes = &bytes, .capacity = 256, .written = null },
        .{ .id = 1, .ids = null, .count = 0, .bytes = &bytes, .capacity = 256, .written = @ptrCast(&bytes) },
        .{ .id = 1, .ids = @ptrCast(&bytes), .count = 2, .bytes = &bytes, .capacity = 256, .written = &written },
        .{ .id = 1, .ids = @ptrCast(&written), .count = 1, .bytes = &bytes, .capacity = 256, .written = &written },
        .{ .id = 1, .ids = @ptrFromInt(std.math.maxInt(usize) - 7), .count = 2, .bytes = &bytes, .capacity = 256, .written = &written },
        .{ .id = 1, .ids = null, .count = 0, .bytes = @ptrFromInt(std.math.maxInt(usize) - 3), .capacity = 8, .written = &written },
        .{ .id = 1, .ids = null, .count = 0, .bytes = &bytes, .capacity = 256, .written = @ptrFromInt(std.math.maxInt(usize) - 7) },
    };
    for (invalid) |entry| {
        try std.testing.expectEqual(c.RingInvalid, fixture_t.enumerate(entry.id, entry.ids, entry.count, entry.bytes, entry.capacity, entry.written));
        try std.testing.expectEqual(@as(usize, 99), written);
        for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0xa5), byte);
    }
    for ([_]u64{ 0, 3 }) |bad| {
        ids[0] = bad;
        try std.testing.expectEqual(c.RingInvalid, fixture_t.enumerate(1, &ids, 2, &bytes, 256, &written));
    }
    ids[0] = 2;
    try std.testing.expectEqual(c.RingLimit, fixture_t.enumerate(1, &ids, 2, &bytes, 8, &written));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.fixed(2, 1, &bytes, 256, &written));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.fixed(3, 0, &bytes, 256, &written));
    try std.testing.expectEqual(c.RingLimit, fixture_t.fixed(8, 1, &bytes, 24, &written));
    try std.testing.expectEqual(@as(usize, 99), written);
    for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0xa5), byte);
    var result: i32 = 99;
    var count: u32 = 99;
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(null, &count, null, 0, &bytes, 28));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(&result, null, null, 0, &bytes, 28));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(&result, &count, null, 0, null, 28));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(&result, &count, null, 0, &bytes, MaxBytes + 1));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(&result, &count, null, 1, &bytes, 28));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(&result, @ptrCast(&result), null, 0, &bytes, 28));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(@ptrCast(&bytes), &count, null, 0, &bytes, 28));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(&result, @ptrCast(&bytes), null, 0, &bytes, 28));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(@ptrCast(&ids), &count, &ids, 1, &bytes, 28));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(&result, @ptrCast(&ids), &ids, 1, &bytes, 28));
    @memset(&bytes, 0);
    bytes[0] = 2;
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(&result, &count, @ptrCast(&bytes), 1, &bytes, 28));
}
