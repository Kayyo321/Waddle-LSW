//! Initial core instance serializer and transaction decoder; no wire pointer dereference.
const std = @import("std");
const c = @cImport({
    @cInclude("waddle/venus_instance_wire.h");
});
const MaxBytes: usize = 16777216;
const MaxNameBytes: usize = 1024;
const writer_t = struct {
    bytes: [4096]u8 = undefined,
    used: usize = 0,
    fn put(self: *writer_t, comptime word_t: type, value: word_t) void {
        const count = @sizeOf(word_t);
        std.debug.assert(count <= self.bytes.len - self.used);
        std.mem.writeInt(word_t, self.bytes[self.used..][0..count], value, .little);
        self.used += count;
    }
    fn name(self: *writer_t, value: ?[*:0]const u8) !void {
        if (value == null) {
            self.put(u64, 0);
            return;
        }
        var count: usize = 0;
        while (count < MaxNameBytes) : (count += 1) {
            if (value.?[count] == 0) break;
        }
        if (count == MaxNameBytes) return error.Limit;
        if (!std.unicode.utf8ValidateSlice(value.?[0..count])) return error.Invalid;
        count += 1;
        self.put(u64, count);
        const padded = (count + 3) & ~@as(usize, 3);
        std.debug.assert(padded <= self.bytes.len - self.used);
        @memcpy(self.bytes[self.used..][0..count], value.?[0..count]);
        @memset(self.bytes[self.used + count ..][0 .. padded - count], 0);
        self.used += padded;
    }
};
fn overlap(first: usize, first_bytes: usize, second: usize, second_bytes: usize) bool {
    if (first_bytes > std.math.maxInt(usize) - first or
        second_bytes > std.math.maxInt(usize) - second) return true;
    return first < second + second_bytes and second < first + first_bytes;
}
fn output_valid(bytes: ?[*]u8, capacity: usize, written: ?*usize) bool {
    if (bytes == null or written == null or capacity < 8 or capacity > MaxBytes) return false;
    return !overlap(@intFromPtr(bytes.?), capacity, @intFromPtr(written.?), @sizeOf(usize));
}
fn encode_info(writer: *writer_t, info: *const c.venus_vk_instance_info_t, host_id: u64) !void {
    if (info.sType != 1 or info.pNext != null or info.flags != 0 or
        info.enabledLayerCount != 0 or info.enabledExtensionCount != 0) return error.Invalid;
    writer.put(u32, 0);
    writer.put(u32, 1);
    writer.put(u64, 1);
    writer.put(u32, 1);
    writer.put(u64, 0);
    writer.put(u32, 0);
    if (info.pApplicationInfo != null) {
        const app = &info.pApplicationInfo[0];
        if (app.sType != 0 or app.pNext != null) return error.Invalid;
        const api = if (app.apiVersion == 0) @as(u32, 1 << 22) else app.apiVersion;
        if (api >> 29 != 0 or api >> 22 != 1 or ((api >> 12) & 1023) > 1)
            return error.Invalid;
        writer.put(u64, 1);
        writer.put(u32, 0);
        writer.put(u64, 0);
        try writer.name(@ptrCast(app.pApplicationName));
        writer.put(u32, app.applicationVersion);
        try writer.name(@ptrCast(app.pEngineName));
        writer.put(u32, app.engineVersion);
        writer.put(u32, api);
    } else writer.put(u64, 0);
    writer.put(u32, 0);
    writer.put(u64, 0);
    writer.put(u32, 0);
    writer.put(u64, 0);
    writer.put(u64, 0);
    writer.put(u64, 1);
    writer.put(u64, host_id);
}
fn status(failure: anyerror) c_int {
    return if (failure == error.Limit) c.RingLimit else c.RingInvalid;
}
/// Initial core encoder; header defines borrowed input, preservation, bounds and errors.
export fn venus_instance_wire_create(
    info: ?*const c.venus_vk_instance_info_t,
    host_id: u64,
    bytes: ?[*]u8,
    capacity: usize,
    written: ?*usize,
) c_int {
    if (info == null or host_id == 0 or !output_valid(bytes, capacity, written))
        return c.RingInvalid;
    if (overlap(
        @intFromPtr(info.?),
        @sizeOf(c.venus_vk_instance_info_t),
        @intFromPtr(bytes.?),
        capacity,
    ) or
        overlap(
        @intFromPtr(info.?),
        @sizeOf(c.venus_vk_instance_info_t),
        @intFromPtr(written.?),
        @sizeOf(usize),
    )) return c.RingInvalid;
    if (info.?.pApplicationInfo != null) {
        const app = &info.?.pApplicationInfo[0];
        if (overlap(
            @intFromPtr(app),
            @sizeOf(c.VkApplicationInfo),
            @intFromPtr(bytes.?),
            capacity,
        ) or
            overlap(
            @intFromPtr(app),
            @sizeOf(c.VkApplicationInfo),
            @intFromPtr(written.?),
            @sizeOf(usize),
        )) return c.RingInvalid;
    }
    var writer = writer_t{};
    encode_info(&writer, info.?, host_id) catch |failure| return status(failure);
    if (writer.used > capacity) return c.RingLimit;
    @memcpy(bytes.?[0..writer.used], writer.bytes[0..writer.used]);
    written.?.* = writer.used;
    return c.RingOk;
}
/// Initial create transaction decode; header specifies reserved identity and failure preservation.
export fn venus_instance_wire_create_reply(
    result: ?*c.venus_vk_result_t,
    bytes: ?[*]const u8,
    length: usize,
    host_id: u64,
) c_int {
    if (result == null or bytes == null or host_id == 0 or length > MaxBytes)
        return c.RingInvalid;
    if (overlap(
        @intFromPtr(result.?),
        @sizeOf(c.venus_vk_result_t),
        @intFromPtr(bytes.?),
        length,
    )) return c.RingInvalid;
    if (length < 24) return c.RingCorrupt;
    const value = bytes.?[0..24];
    if (std.mem.readInt(u32, value[0..4], .little) != 0 or
        std.mem.readInt(u64, value[8..16], .little) != 1) return c.RingCorrupt;
    const decoded = std.mem.readInt(i32, value[4..8], .little);
    const id = std.mem.readInt(u64, value[16..24], .little);
    if (decoded > 0 or (decoded == 0 and id != host_id) or
        (decoded < 0 and id != 0 and id != host_id)) return c.RingCorrupt;
    result.?.* = decoded;
    return c.RingOk;
}
/// Destroy encoder; header specifies existing identity and host-before-local retirement.
export fn venus_instance_wire_destroy(
    host_id: u64,
    bytes: ?[*]u8,
    capacity: usize,
    written: ?*usize,
) c_int {
    if (host_id == 0 or !output_valid(bytes, capacity, written)) return c.RingInvalid;
    if (capacity < 24) return c.RingLimit;
    var writer = writer_t{};
    writer.put(u32, 1);
    writer.put(u32, 1);
    writer.put(u64, host_id);
    writer.put(u64, 0);
    @memcpy(bytes.?[0..writer.used], writer.bytes[0..writer.used]);
    written.?.* = writer.used;
    return c.RingOk;
}

// Test-only fixtures.
extern fn venus_instance_test_encode(*const c.venus_vk_instance_info_t, u64, [*]u8, usize) usize;
const fixture_t = struct {
    fn create(
        info_value: ?*const c.venus_vk_instance_info_t,
        id: u64,
        bytes: ?[*]u8,
        capacity: usize,
        written: ?*usize,
    ) c_int {
        var target: *const @TypeOf(venus_instance_wire_create) = venus_instance_wire_create;
        const function = @as(*volatile @TypeOf(target), &target).*;
        return function(info_value, id, bytes, capacity, written);
    }
    fn reply(result: ?*c.venus_vk_result_t, bytes: ?[*]const u8, length: usize, id: u64) c_int {
        const reply_function_t = *const @TypeOf(venus_instance_wire_create_reply);
        var target: reply_function_t = venus_instance_wire_create_reply;
        const function = @as(*volatile @TypeOf(target), &target).*;
        return function(result, bytes, length, id);
    }
    fn destroy(id: u64, bytes: ?[*]u8, capacity: usize, written: ?*usize) c_int {
        var target: *const @TypeOf(venus_instance_wire_destroy) = venus_instance_wire_destroy;
        const function = @as(*volatile @TypeOf(target), &target).*;
        return function(id, bytes, capacity, written);
    }

    fn info() c.venus_vk_instance_info_t {
        return std.mem.zeroInit(c.venus_vk_instance_info_t, .{ .sType = 1 });
    }
    fn expect_create(
        info_value: ?*const c.venus_vk_instance_info_t,
        host_id: u64,
        bytes: []u8,
        expected: c_int,
    ) !void {
        @memset(bytes, 0xa5);
        var written: usize = 99;
        const result = fixture_t.create(
            info_value,
            host_id,
            bytes.ptr,
            bytes.len,
            &written,
        );
        try std.testing.expectEqual(expected, result);
        if (expected != c.RingOk) {
            try std.testing.expectEqual(@as(usize, 99), written);
            for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0xa5), byte);
        }
    }
};
test "create packets match independent pinned guest serializer" {
    const bytes = try std.testing.allocator.alloc(u8, 4096);
    defer std.testing.allocator.free(bytes);
    const expected = try std.testing.allocator.alloc(u8, 4096);
    defer std.testing.allocator.free(expected);
    var name_bytes: [MaxNameBytes:0]u8 = undefined;
    @memset(&name_bytes, 'a');
    name_bytes[MaxNameBytes - 1] = 0;
    var info_value = fixture_t.info();
    var app = std.mem.zeroInit(c.VkApplicationInfo, .{});
    for (0..5) |mode| {
        if (mode > 0) info_value.pApplicationInfo = &app;
        if (mode == 2) {
            app.pApplicationName = "app";
            app.pEngineName = "engine";
            app.apiVersion = (1 << 22) | (1 << 12);
            app.applicationVersion = 123;
            app.engineVersion = 456;
        }
        if (mode == 3) {
            app.pApplicationName = &name_bytes;
            app.pEngineName = &name_bytes;
        }
        if (mode == 4) {
            app.pApplicationName = "";
            app.pEngineName = "";
        }
        var written: usize = 0;
        try std.testing.expectEqual(c.RingOk, fixture_t.create(
            &info_value,
            42,
            bytes.ptr,
            bytes.len,
            &written,
        ));
        // API zero has core1.0 semantics; compare its normalized wire representation.
        if (mode == 1) app.apiVersion = 1 << 22;
        const count = venus_instance_test_encode(&info_value, 42, expected.ptr, expected.len);
        try std.testing.expectEqual(count, written);
        try std.testing.expectEqualSlices(u8, expected[0..count], bytes[0..written]);
    }
}
test "create rejects unsupported native inputs and preserves failed output" {
    const bytes = try std.testing.allocator.alloc(u8, 4096);
    defer std.testing.allocator.free(bytes);
    var info_value = fixture_t.info();
    var app = std.mem.zeroInit(c.VkApplicationInfo, .{});
    var bad_name: [MaxNameBytes]u8 = undefined;
    @memset(&bad_name, 'a');
    for (0..13) |mode| {
        info_value = fixture_t.info();
        app = std.mem.zeroInit(c.VkApplicationInfo, .{});
        info_value.pApplicationInfo = &app;
        var expected: c_int = c.RingInvalid;
        switch (mode) {
            0 => info_value.sType = 2,
            1 => info_value.flags = 1,
            2 => info_value.pNext = &app,
            3 => info_value.enabledLayerCount = 1,
            4 => info_value.enabledExtensionCount = 1,
            5 => app.sType = 1,
            6 => app.pNext = &info_value,
            7 => app.apiVersion = 0x20400000,
            8 => app.apiVersion = 2 << 22,
            9 => app.apiVersion = (1 << 22) | (2 << 12),
            10 => app.pApplicationName = "\xff",
            11 => {
                app.pApplicationName = &bad_name;
                expected = c.RingLimit;
            },
            12 => app.pEngineName = "\xff",
            else => unreachable,
        }
        try fixture_t.expect_create(&info_value, 42, bytes, expected);
    }
    info_value = fixture_t.info();
    try fixture_t.expect_create(null, 42, bytes, c.RingInvalid);
    try fixture_t.expect_create(&info_value, 0, bytes, c.RingInvalid);
    try fixture_t.expect_create(&info_value, 42, bytes[0..8], c.RingLimit);
    var written: usize = 99;
    var target: *const @TypeOf(venus_instance_wire_create) = venus_instance_wire_create;
    const function = @as(*volatile @TypeOf(target), &target).*;
    try std.testing.expectEqual(c.RingInvalid, function(&info_value, 42, null, 4096, &written));
    try std.testing.expectEqual(c.RingInvalid, function(&info_value, 42, bytes.ptr, 4096, null));
    try std.testing.expectEqual(c.RingInvalid, function(&info_value, 42, bytes.ptr, 7, &written));
    try std.testing.expectEqual(c.RingInvalid, function(
        &info_value,
        42,
        bytes.ptr,
        MaxBytes + 1,
        &written,
    ));
    const overflow: [*]u8 = @ptrFromInt(std.math.maxInt(usize) - 7);
    try std.testing.expectEqual(c.RingInvalid, function(&info_value, 42, overflow, 16, &written));
    try std.testing.expectEqual(c.RingInvalid, function(
        &info_value,
        42,
        @ptrCast(&info_value),
        @sizeOf(@TypeOf(info_value)),
        &written,
    ));
    info_value.pApplicationInfo = &app;
    try std.testing.expectEqual(c.RingInvalid, function(
        &info_value,
        42,
        @ptrCast(&app),
        @sizeOf(@TypeOf(app)),
        &written,
    ));
    const alias_info_written: *usize = @ptrCast(@alignCast(&info_value));
    try std.testing.expectEqual(c.RingInvalid, function(
        &info_value,
        42,
        bytes.ptr,
        bytes.len,
        alias_info_written,
    ));
    const alias_app_written: *usize = @ptrCast(@alignCast(&app));
    try std.testing.expectEqual(c.RingInvalid, function(
        &info_value,
        42,
        bytes.ptr,
        bytes.len,
        alias_app_written,
    ));
}
test "create replies preserve outputs on every truncated and corrupted transaction" {
    const bytes = try std.testing.allocator.alignedAlloc(u8, 8, 64);
    defer std.testing.allocator.free(bytes);
    var result: c.venus_vk_result_t = -1;
    @memset(bytes, 0);
    std.mem.writeInt(u64, bytes[8..16], 1, .little);
    std.mem.writeInt(u64, bytes[16..24], 42, .little);
    for (0..24) |prefix| {
        try std.testing.expectEqual(c.RingCorrupt, fixture_t.reply(
            &result,
            bytes.ptr,
            prefix,
            42,
        ));
        try std.testing.expectEqual(@as(c_int, -1), result);
    }
    try std.testing.expectEqual(c.RingOk, fixture_t.reply(
        &result,
        bytes.ptr,
        bytes.len,
        42,
    ));
    try std.testing.expectEqual(@as(c_int, 0), result);
    const Mutations = [_][2]u32{ .{ 0, 1 }, .{ 8, 0 }, .{ 16, 43 }, .{ 4, 1 } };
    for (Mutations) |mutation| {
        std.mem.writeInt(u32, bytes[mutation[0]..][0..4], mutation[1], .little);
        try std.testing.expectEqual(c.RingCorrupt, fixture_t.reply(
            &result,
            bytes.ptr,
            bytes.len,
            42,
        ));
        try std.testing.expectEqual(@as(c_int, 0), result);
        std.mem.writeInt(u32, bytes[mutation[0]..][0..4], switch (mutation[0]) {
            8 => 1,
            16 => 42,
            else => 0,
        }, .little);
    }
    std.mem.writeInt(i32, bytes[4..8], -2, .little);
    for ([_]u64{ 0, 42, 43 }) |id| {
        std.mem.writeInt(u64, bytes[16..24], id, .little);
        try std.testing.expectEqual(
            if (id == 43) c.RingCorrupt else c.RingOk,
            fixture_t.reply(&result, bytes.ptr, bytes.len, 42),
        );
        try std.testing.expectEqual(@as(c_int, -2), result);
    }
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(
        &result,
        null,
        bytes.len,
        42,
    ));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(
        &result,
        bytes.ptr,
        bytes.len,
        0,
    ));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(
        &result,
        bytes.ptr,
        MaxBytes + 1,
        42,
    ));
    const alias_result: *c.venus_vk_result_t = @ptrCast(@alignCast(bytes.ptr));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.reply(
        alias_result,
        bytes.ptr,
        bytes.len,
        42,
    ));
    const reply_function_t = *const @TypeOf(venus_instance_wire_create_reply);
    var target: reply_function_t = venus_instance_wire_create_reply;
    const function = @as(*volatile @TypeOf(target), &target).*;
    try std.testing.expectEqual(c.RingInvalid, function(null, bytes.ptr, bytes.len, 42));
    const overflow: [*]const u8 = @ptrFromInt(std.math.maxInt(usize) - 7);
    try std.testing.expectEqual(c.RingInvalid, function(&result, overflow, 16, 42));
}
test "destroy serializes exact identity and validates bounded output" {
    const bytes = try std.testing.allocator.alignedAlloc(u8, 8, 64);
    defer std.testing.allocator.free(bytes);
    var written: usize = 99;
    for (8..24) |capacity| {
        try std.testing.expectEqual(c.RingLimit, fixture_t.destroy(
            42,
            bytes.ptr,
            capacity,
            &written,
        ));
        try std.testing.expectEqual(@as(usize, 99), written);
    }
    try std.testing.expectEqual(c.RingOk, fixture_t.destroy(
        42,
        bytes.ptr,
        bytes.len,
        &written,
    ));
    try std.testing.expectEqual(@as(usize, 24), written);
    try std.testing.expectEqual(@as(u32, 1), std.mem.readInt(u32, bytes[0..4], .little));
    try std.testing.expectEqual(@as(u64, 42), std.mem.readInt(u64, bytes[8..16], .little));
    try std.testing.expectEqual(@as(u64, 0), std.mem.readInt(u64, bytes[16..24], .little));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.destroy(
        0,
        bytes.ptr,
        bytes.len,
        &written,
    ));
    const alias_written: *usize = @ptrCast(@alignCast(bytes.ptr));
    try std.testing.expectEqual(c.RingInvalid, fixture_t.destroy(
        42,
        bytes.ptr,
        bytes.len,
        alias_written,
    ));
}

test "private overlap boundary and output nullability" {
    try std.testing.expect(overlap(1, std.math.maxInt(usize), 10, 20));
    try std.testing.expect(overlap(10, 20, 1, std.math.maxInt(usize)));
    try std.testing.expect(!overlap(10, 20, 100, 20));
    try std.testing.expect(!overlap(100, 20, 10, 20));
    var bytes: [64]u8 = undefined;
    var written: usize = 0;
    var target: *const @TypeOf(venus_instance_wire_destroy) = venus_instance_wire_destroy;
    const function = @as(*volatile @TypeOf(target), &target).*;
    try std.testing.expectEqual(c.RingInvalid, function(42, null, bytes.len, &written));
    try std.testing.expectEqual(c.RingInvalid, function(42, &bytes, bytes.len, null));
    try std.testing.expectEqual(c.RingInvalid, function(42, &bytes, 0, &written));
    try std.testing.expectEqual(c.RingInvalid, function(42, &bytes, MaxBytes + 1, &written));
}
