//! Bounded pinned-protocol fixture. No native Vulkan structs or reply casts.
const std = @import("std");
const BufferBytes: usize = 4096;
const MaxDevices: u32 = 16;
const MaxFamilies: u32 = 32;
const InstanceId: u64 = 1;
const DeviceId: u64 = 100;
const QueueId: u64 = 101;
const QueryPoolId: u64 = 102;
const CommandPoolId: u64 = 103;
const CommandBufferId: u64 = 104;
const ImageId: u64 = 105;
const MemoryId: u64 = 106;
const queue_family_t = struct { index: u32, timestamp_bits: u32 };
const venus_receiver_t = opaque {};
// Borrowed C ABI operations; ownership/status contract is venus_receiver.h.
extern fn venus_receiver_create(*?*venus_receiver_t, u32, u64) c_int;
extern fn venus_receiver_destroy(*?*venus_receiver_t) void;
extern fn venus_receiver_submit(*venus_receiver_t, [*]const u8, usize, *u64) c_int;
extern fn venus_receiver_poll(*const venus_receiver_t) c_int;
extern fn venus_receiver_reply(*const venus_receiver_t, u64, [*]u8, usize) c_int;
extern fn venus_receiver_health(*venus_receiver_t, ?*const u32) c_int;
extern fn venus_receiver_gpu_fence(*venus_receiver_t, u32, *u64) c_int;
extern fn venus_receiver_gpu_poll(*const venus_receiver_t, u32, u64) c_int;
extern fn venus_receiver_resource_create(*venus_receiver_t, u32, u64, u64, u32) c_int;
extern fn venus_receiver_resource_export(*venus_receiver_t, u32, u32, u64, *c_int) c_int;
extern fn venus_receiver_resource_free(*venus_receiver_t, u32) c_int;

const writer_t = struct {
    bytes: [BufferBytes]u8 align(64) = undefined,
    used: usize = 0,
    fn put(self: *writer_t, comptime integer_t: type, value: integer_t) !void {
        const extent = @sizeOf(integer_t);
        if (self.used > self.bytes.len - extent) return error.Bounds;
        std.mem.writeInt(integer_t, self.bytes[self.used..][0..extent], value, .little);
        self.used += extent;
    }
    fn words(self: *writer_t, values: []const u32) !void {
        for (values) |value| try self.put(u32, value);
    }
    fn begin(self: *writer_t, command: u32) !void {
        self.used = 0;
        // SetReplyCommandStreamMESA, no reply flag, blob 1, offset 0, size 4096.
        try self.words(&.{ 178, 0, 1, 0, 1, 0, 0, BufferBytes, 0, command, 1 });
    }
};
const reader_t = struct {
    bytes: []const u8,
    used: usize = 0,
    fn take(self: *reader_t, count: usize) ![]const u8 {
        if (self.used > self.bytes.len or count > self.bytes.len - self.used) return error.Bounds;
        const value = self.bytes[self.used..][0..count];
        self.used += count;
        return value;
    }
    fn get(self: *reader_t, comptime integer_t: type) !integer_t {
        const bytes = try self.take(@sizeOf(integer_t));
        return std.mem.readInt(integer_t, bytes[0..@sizeOf(integer_t)], .little);
    }
    fn expect(self: *reader_t, comptime integer_t: type, value: integer_t) !void {
        if (try self.get(integer_t) != value) return error.Protocol;
    }
};

fn wait_cpu(receiver: *venus_receiver_t) !void {
    var timer = try std.time.Timer.start();
    while (true) {
        if (venus_receiver_health(receiver, null) != 0) return error.Renderer;
        const status = venus_receiver_poll(receiver);
        if (status == 0) return;
        if (status != 1) return error.Renderer;
        if (timer.read() >= 5 * std.time.ns_per_s) return error.Deadline;
        std.time.sleep(std.time.ns_per_ms);
    }
}
fn exchange(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8, command: u32) !reader_t {
    var fence: u64 = 0;
    if (venus_receiver_submit(receiver, &writer.bytes, writer.used, &fence) != 0 or fence == 0) return error.Renderer;
    try wait_cpu(receiver);
    if (venus_receiver_reply(receiver, 0, reply, reply.len) != 0) return error.Renderer;
    var reader = reader_t{ .bytes = reply };
    try reader.expect(u32, command);
    return reader;
}
fn create_instance(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8) !void {
    try writer.begin(0);
    try writer.put(u64, 1); // pCreateInfo.
    try writer.put(u32, 1); // INSTANCE_CREATE_INFO.
    try writer.put(u64, 0); // pNext.
    try writer.put(u32, 0); // flags.
    try writer.put(u64, 1); // pApplicationInfo.
    try writer.put(u32, 0); // APPLICATION_INFO.
    try writer.put(u64, 0); // pNext.
    try writer.put(u64, 0); // application name array.
    try writer.put(u32, 0); // application version.
    try writer.put(u64, 0); // engine name array.
    try writer.words(&.{ 0, (1 << 22) | (1 << 12), 0 }); // engine, API 1.1, layer count.
    try writer.put(u64, 0); // layer array.
    try writer.put(u32, 0); // extension count.
    try writer.put(u64, 0); // extension array.
    try writer.put(u64, 0); // allocator.
    try writer.put(u64, 1); // output pointer.
    try writer.put(u64, InstanceId);
    var reader = try exchange(receiver, writer, reply, 0);
    try reader.expect(u32, 0); // VK_SUCCESS.
    try reader.expect(u64, 1);
    try reader.expect(u64, InstanceId);
}
fn enumerate_devices(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8) !u32 {
    try writer.begin(2);
    try writer.put(u64, InstanceId);
    try writer.put(u64, 1);
    try writer.put(u32, 0);
    try writer.put(u64, 0);
    var reader = try exchange(receiver, writer, reply, 2);
    try reader.expect(u32, 0);
    try reader.expect(u64, 1);
    const count = try reader.get(u32);
    if (count == 0 or count > MaxDevices) return error.NoDevice;
    try reader.expect(u64, 0);
    try writer.begin(2);
    try writer.put(u64, InstanceId);
    try writer.put(u64, 1);
    try writer.put(u32, count);
    try writer.put(u64, count);
    for (0..count) |index| try writer.put(u64, index + 2);
    reader = try exchange(receiver, writer, reply, 2);
    try reader.expect(u32, 0);
    try reader.expect(u64, 1);
    try reader.expect(u32, count);
    try reader.expect(u64, count);
    for (0..count) |index| try reader.expect(u64, index + 2);
    return count;
}
fn select_device(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8, count: u32, hardware: bool) !u64 {
    for (0..count) |index| {
        try writer.begin(6); // GetPhysicalDeviceProperties.
        try writer.put(u64, index + 2);
        try writer.put(u64, 1); // Partial properties need no scalar inputs.
        var reader = try exchange(receiver, writer, reply, 6);
        try reader.expect(u64, 1);
        const api = try reader.get(u32);
        _ = try reader.take(12); // driverVersion, vendorID, deviceID.
        const device_type = try reader.get(u32);
        if (device_type > 4) return error.Protocol;
        try reader.expect(u64, 256);
        const name = try reader.take(256);
        const terminator = std.mem.indexOfScalar(u8, name, 0) orelse return error.Protocol;
        if (hardware and device_type != 1 and device_type != 2) continue;
        std.debug.print("Venus queue device: type={d} API=0x{x} name={s}\n", .{ device_type, api, name[0..terminator] });
        return index + 2;
    }
    return error.NoDevice;
}
fn select_family(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8, physical: u64) !queue_family_t {
    try writer.begin(7);
    try writer.put(u64, physical);
    try writer.put(u64, 1);
    try writer.put(u32, 0);
    try writer.put(u64, 0);
    var reader = try exchange(receiver, writer, reply, 7);
    try reader.expect(u64, 1);
    const count = try reader.get(u32);
    if (count == 0 or count > MaxFamilies) return error.NoDevice;
    try reader.expect(u64, 0);
    try writer.begin(7);
    try writer.put(u64, physical);
    try writer.put(u64, 1);
    try writer.put(u32, count);
    try writer.put(u64, count); // Partial family properties contain no scalar input.
    reader = try exchange(receiver, writer, reply, 7);
    try reader.expect(u64, 1);
    try reader.expect(u32, count);
    try reader.expect(u64, count);
    var selected: ?queue_family_t = null;
    for (0..count) |index| {
        const flags = try reader.get(u32);
        const queues = try reader.get(u32);
        const valid_bits = try reader.get(u32);
        if (valid_bits > 64) return error.Protocol;
        _ = try reader.take(12); // transfer granularity.
        if (selected == null and flags & 3 != 0 and queues != 0 and valid_bits != 0)
            selected = .{ .index = @intCast(index), .timestamp_bits = valid_bits };
    }
    return selected orelse error.NoDevice;
}
fn create_device(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8, physical: u64, family: u32) !void {
    try writer.begin(11);
    try writer.put(u64, physical);
    try writer.put(u64, 1);
    try writer.put(u32, 3); // DEVICE_CREATE_INFO.
    try writer.put(u64, 0);
    try writer.words(&.{ 0, 1 }); // flags, queueCreateInfoCount.
    try writer.put(u64, 1);
    try writer.put(u32, 2); // DEVICE_QUEUE_CREATE_INFO.
    try writer.put(u64, 0);
    try writer.words(&.{ 0, family, 1 });
    try writer.put(u64, 1); // priorities array.
    try writer.words(&.{ 0x3f800000, 0 }); // float 1.0, layer count.
    try writer.put(u64, 0);
    try writer.put(u32, 0); // extension count.
    try writer.put(u64, 0);
    try writer.put(u64, 0); // enabled features.
    try writer.put(u64, 0); // allocator.
    try writer.put(u64, 1);
    try writer.put(u64, DeviceId);
    var reader = try exchange(receiver, writer, reply, 11);
    try reader.expect(u32, 0);
    try reader.expect(u64, 1);
    try reader.expect(u64, DeviceId);
    try writer.begin(155);
    try writer.put(u64, DeviceId);
    try writer.put(u64, 1);
    try writer.put(u32, 1000145003); // DEVICE_QUEUE_INFO_2.
    try writer.put(u64, 1); // timeline pNext.
    try writer.put(u32, 1000384005); // DEVICE_QUEUE_TIMELINE_INFO_MESA.
    try writer.put(u64, 0); // chained pNext.
    try writer.words(&.{ 1, 0, family, 0 }); // timeline 1, flags, family, queue index.
    try writer.put(u64, 1);
    try writer.put(u64, QueueId);
    reader = try exchange(receiver, writer, reply, 155);
    try reader.expect(u64, 1);
    try reader.expect(u64, QueueId);
}
fn create_workload(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8, family: u32, image: bool) !void {
    try writer.begin(47); // CreateQueryPool.
    try writer.put(u64, DeviceId);
    try writer.put(u64, 1);
    try writer.put(u32, 11); // QUERY_POOL_CREATE_INFO.
    try writer.put(u64, 0);
    try writer.words(&.{ 0, 2, 2, 0 }); // flags, timestamp query type, count, statistics.
    try writer.put(u64, 0); // allocator.
    try writer.put(u64, 1);
    try writer.put(u64, QueryPoolId);
    var reader = try exchange(receiver, writer, reply, 47);
    try reader.expect(u32, 0);
    try reader.expect(u64, 1);
    try reader.expect(u64, QueryPoolId);
    try writer.begin(85); // CreateCommandPool.
    try writer.put(u64, DeviceId);
    try writer.put(u64, 1);
    try writer.put(u32, 39); // COMMAND_POOL_CREATE_INFO.
    try writer.put(u64, 0);
    try writer.words(&.{ 0, family });
    try writer.put(u64, 0);
    try writer.put(u64, 1);
    try writer.put(u64, CommandPoolId);
    reader = try exchange(receiver, writer, reply, 85);
    try reader.expect(u32, 0);
    try reader.expect(u64, 1);
    try reader.expect(u64, CommandPoolId);
    try writer.begin(88); // AllocateCommandBuffers.
    try writer.put(u64, DeviceId);
    try writer.put(u64, 1);
    try writer.put(u32, 40); // COMMAND_BUFFER_ALLOCATE_INFO.
    try writer.put(u64, 0);
    try writer.put(u64, CommandPoolId);
    try writer.words(&.{ 0, 1 }); // primary level, count.
    try writer.put(u64, 1);
    try writer.put(u64, CommandBufferId);
    reader = try exchange(receiver, writer, reply, 88);
    try reader.expect(u32, 0);
    try reader.expect(u64, 1);
    try reader.expect(u64, CommandBufferId);
    try writer.begin(90); // BeginCommandBuffer.
    try writer.put(u64, CommandBufferId);
    try writer.put(u64, 1);
    try writer.put(u32, 42); // COMMAND_BUFFER_BEGIN_INFO.
    try writer.put(u64, 0);
    try writer.put(u32, 0);
    try writer.put(u64, 0); // no inheritance.
    reader = try exchange(receiver, writer, reply, 90);
    try reader.expect(u32, 0);
    if (image) try record_image(receiver, writer, reply, family);
    try writer.begin(129); // CmdResetQueryPool, executes again on every submission.
    try writer.put(u64, CommandBufferId);
    try writer.put(u64, QueryPoolId);
    try writer.words(&.{ 0, 2 });
    _ = try exchange(receiver, writer, reply, 129);
    for ([_]u32{ 1, 0x2000 }, 0..) |stage, query| {
        try writer.begin(130); // CmdWriteTimestamp, top then bottom of pipe.
        try writer.put(u64, CommandBufferId);
        try writer.put(u32, stage);
        try writer.put(u64, QueryPoolId);
        try writer.put(u32, @intCast(query));
        _ = try exchange(receiver, writer, reply, 130);
    }
    try writer.begin(91); // EndCommandBuffer.
    try writer.put(u64, CommandBufferId);
    reader = try exchange(receiver, writer, reply, 91);
    try reader.expect(u32, 0);
}
fn read_timestamps(reader: *reader_t, valid_bits: u32) !u64 {
    if (valid_bits == 0 or valid_bits > 64) return error.Protocol;
    try reader.expect(u32, 0); // VK_SUCCESS: no NOT_READY after completed GPU fence.
    try reader.expect(u64, 32);
    const first = try reader.get(u64);
    const first_available = try reader.get(u64);
    const last = try reader.get(u64);
    const last_available = try reader.get(u64);
    if (first_available == 0 or last_available == 0) return error.Protocol;
    const mask = @as(u64, std.math.maxInt(u64)) >> @as(u6, @intCast(64 - valid_bits));
    const delta = (last -% first) & mask;
    const half_range = @as(u64, 1) << @as(u6, @intCast(valid_bits - 1));
    if (delta >= half_range) return error.Protocol;
    return delta;
}
fn verify_workload(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8, valid_bits: u32) !void {
    try writer.begin(49); // GetQueryPoolResults, nonblocking after GPU retirement.
    try writer.put(u64, DeviceId);
    try writer.put(u64, QueryPoolId);
    try writer.words(&.{ 0, 2 }); // first query, count.
    try writer.put(u64, 32); // dataSize.
    try writer.put(u64, 32); // output blob extent, no request data follows.
    try writer.put(u64, 16); // timestamp + availability stride.
    try writer.put(u32, 5); // 64_BIT | WITH_AVAILABILITY, without WAIT.
    var reader = try exchange(receiver, writer, reply, 49);
    const delta = try read_timestamps(&reader, valid_bits);
    std.debug.print("GPU timestamp commands executed: valid_bits={d} delta={d}\n", .{ valid_bits, delta });
}
fn queue_roundtrip(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8, expected: u64) !void {
    try writer.begin(18);
    try writer.put(u64, QueueId);
    try writer.put(u32, 1); // One real command-buffer submission.
    try writer.put(u64, 1); // VkSubmitInfo array.
    try writer.put(u32, 4); // SUBMIT_INFO.
    try writer.put(u64, 0);
    try writer.put(u32, 0); // waitSemaphoreCount.
    try writer.put(u64, 0); // wait semaphore array.
    try writer.put(u64, 0); // wait stage array.
    try writer.put(u32, 1); // commandBufferCount.
    try writer.put(u64, 1);
    try writer.put(u64, CommandBufferId);
    try writer.put(u32, 0); // signalSemaphoreCount.
    try writer.put(u64, 0);
    try writer.put(u64, 0); // No Vulkan fence object.
    var reader = try exchange(receiver, writer, reply, 18);
    try reader.expect(u32, 0);
    var fence: u64 = 0;
    if (venus_receiver_gpu_fence(receiver, 1, &fence) != 0 or fence != expected) return error.Renderer;
    var timer = try std.time.Timer.start();
    while (true) {
        if (venus_receiver_health(receiver, null) != 0) return error.Renderer;
        const status = venus_receiver_gpu_poll(receiver, 1, fence);
        if (status == 0) break;
        if (status != 1) return error.Renderer;
        if (timer.read() >= 5 * std.time.ns_per_s) return error.Deadline;
        std.time.sleep(std.time.ns_per_ms);
    }
    std.debug.print("Real Venus GPU queue fence retired: timeline=1 id={d}\n", .{fence});
}
fn run_fixture(hardware: bool) !void {
    var owned: ?*venus_receiver_t = null;
    if (venus_receiver_create(&owned, BufferBytes, BufferBytes) != 0) return error.Renderer;
    defer venus_receiver_destroy(&owned);
    const receiver = owned orelse return error.Renderer;
    var writer = writer_t{};
    var reply: [BufferBytes]u8 = undefined;
    try create_instance(receiver, &writer, &reply);
    const count = try enumerate_devices(receiver, &writer, &reply);
    const physical = try select_device(receiver, &writer, &reply, count, hardware);
    const family = try select_family(receiver, &writer, &reply, physical);
    try create_device(receiver, &writer, &reply, physical, family.index);
    try create_workload(receiver, &writer, &reply, family.index, false);
    for (1..4) |fence| {
        try queue_roundtrip(receiver, &writer, &reply, fence);
        try verify_workload(receiver, &writer, &reply, family.timestamp_bits);
    }
    for ([_]u32{ 86, 48 }, [_]u64{ CommandPoolId, QueryPoolId }) |command, object| {
        try writer.begin(command);
        try writer.put(u64, DeviceId);
        try writer.put(u64, object);
        try writer.put(u64, 0);
        _ = try exchange(receiver, &writer, &reply, command);
    }
    for ([_]u32{ 12, 1 }, [_]u64{ DeviceId, InstanceId }) |command, object| {
        try writer.begin(command);
        try writer.put(u64, object);
        try writer.put(u64, 0);
        _ = try exchange(receiver, &writer, &reply, command);
    }
}
/// in: hardware nonzero requires physical integrated/discrete type; no borrowed
/// buffers. Returns 0 success, 1 protocol/renderer/deadline/device failure. Owns
/// receiver only for call, always destroyed; sole session thread, no heap buffers.
export fn venus_gpu_fixture_run(hardware: c_int) c_int {
    run_fixture(hardware != 0) catch |failure| {
        std.debug.print("Venus queue fixture failed: {s}\n", .{@errorName(failure)});
        return 1;
    };
    return 0;
}

test "bounded fixture writes and parses fixed little-endian scalars" {
    const bytes = try std.testing.allocator.alloc(u8, 12);
    defer std.testing.allocator.free(bytes);
    var writer = writer_t{};
    try writer.put(u32, 0x01020304);
    try writer.put(u64, 0x1020304050607080);
    @memcpy(bytes, writer.bytes[0..12]);
    try std.testing.expectEqual(@as(u8, 4), bytes[0]);
    var reader = reader_t{ .bytes = bytes };
    try reader.expect(u32, 0x01020304);
    try reader.expect(u64, 0x1020304050607080);
    try std.testing.expectError(error.Bounds, reader.get(u32));
    try std.testing.expectEqual(@as(usize, 12), reader.used);
    writer.used = BufferBytes - 3;
    try std.testing.expectError(error.Bounds, writer.put(u32, 1));
    reader = .{ .bytes = bytes };
    try std.testing.expectError(error.Protocol, reader.expect(u32, 0));
    reader.used = std.math.maxInt(usize);
    try std.testing.expectError(error.Bounds, reader.take(1));
}

test "timestamp reply requires complete available ordered GPU output" {
    const bytes = try std.testing.allocator.alloc(u8, 44);
    defer std.testing.allocator.free(bytes);
    var writer = writer_t{};
    try writer.put(u32, 0);
    try writer.put(u64, 32);
    try writer.put(u64, std.math.maxInt(u64) - 2);
    try writer.put(u64, 1);
    try writer.put(u64, 1);
    try writer.put(u64, 1);
    @memcpy(bytes, writer.bytes[0..44]);
    for ([_]u32{ 4, 32, 64 }) |valid_bits| {
        var reader = reader_t{ .bytes = bytes };
        try std.testing.expectEqual(@as(u64, 4), try read_timestamps(&reader, valid_bits));
        try std.testing.expectEqual(@as(usize, 44), reader.used);
    }
    for (0..44) |length| {
        var reader = reader_t{ .bytes = bytes[0..length] };
        try std.testing.expectError(error.Bounds, read_timestamps(&reader, 64));
    }
    for ([_]u32{ 0, 65, std.math.maxInt(u32) }) |valid_bits| {
        var reader = reader_t{ .bytes = bytes };
        try std.testing.expectError(error.Protocol, read_timestamps(&reader, valid_bits));
        try std.testing.expectEqual(@as(usize, 0), reader.used);
    }
    for ([_]usize{ 0, 4, 20, 36 }) |offset| {
        const saved = bytes[offset];
        bytes[offset] = if (offset == 0) 1 else 0;
        var reader = reader_t{ .bytes = bytes };
        try std.testing.expectError(error.Protocol, read_timestamps(&reader, 64));
        bytes[offset] = saved;
    }
    std.mem.writeInt(u64, bytes[12..20], 10, .little);
    std.mem.writeInt(u64, bytes[28..36], 9, .little);
    for ([_]u32{ 4, 64 }) |valid_bits| {
        var reader = reader_t{ .bytes = bytes };
        try std.testing.expectError(error.Protocol, read_timestamps(&reader, valid_bits));
    }
    // Equal timestamps can be legal at a coarse timestamp resolution.
    std.mem.writeInt(u64, bytes[28..36], 10, .little);
    var reader = reader_t{ .bytes = bytes };
    try std.testing.expectEqual(@as(u64, 0), try read_timestamps(&reader, 1));
}

const image_layout_t = struct { offset: u64, stride: u64, size: u64, extent: u64 };
fn allocate_image(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8, physical: u64) !image_layout_t {
    try writer.begin(54); // CreateImage, linear BGRA8 with explicit DMA-BUF handle support.
    try writer.put(u64, DeviceId);
    try writer.put(u64, 1);
    try writer.put(u32, 14); // IMAGE_CREATE_INFO.
    try writer.put(u64, 1);
    try writer.put(u32, 1000072001); // EXTERNAL_MEMORY_IMAGE_CREATE_INFO.
    try writer.put(u64, 0);
    try writer.words(&.{ 0x200, 0, 1, 44, 32, 16, 1, 1, 1, 1, 1, 2, 0, 0 });
    try writer.put(u64, 0); // queue-family array.
    try writer.put(u32, 0); // UNDEFINED initial layout.
    try writer.put(u64, 0);
    try writer.put(u64, 1);
    try writer.put(u64, ImageId);
    var reader = try exchange(receiver, writer, reply, 54);
    try reader.expect(u32, 0);
    try reader.expect(u64, 1);
    try reader.expect(u64, ImageId);
    try writer.begin(31); // GetImageMemoryRequirements.
    try writer.put(u64, DeviceId);
    try writer.put(u64, ImageId);
    try writer.put(u64, 1);
    reader = try exchange(receiver, writer, reply, 31);
    try reader.expect(u64, 1);
    const size = try reader.get(u64);
    const alignment = try reader.get(u64);
    const types = try reader.get(u32);
    if (size == 0 or size > 1073737728 or alignment == 0 or alignment & (alignment - 1) != 0 or types == 0) return error.Protocol;
    const extent = (size + 4095) & ~@as(u64, 4095);
    try writer.begin(8); // GetPhysicalDeviceMemoryProperties.
    try writer.put(u64, physical);
    try writer.put(u64, 1);
    try writer.put(u64, 32); // Partial fixed array sizes, no scalar values.
    try writer.put(u64, 16);
    reader = try exchange(receiver, writer, reply, 8);
    try reader.expect(u64, 1);
    const count = try reader.get(u32);
    if (count == 0 or count > 32) return error.Protocol;
    try reader.expect(u64, 32);
    var selected: ?u32 = null;
    for (0..32) |index| {
        const flags = try reader.get(u32);
        const heap = try reader.get(u32);
        if (index < count and heap >= 16) return error.Protocol;
        if (index < count and types & (@as(u32, 1) << @as(u5, @intCast(index))) != 0 and flags & 2 != 0 and selected == null)
            selected = @intCast(index);
    }
    const heaps = try reader.get(u32);
    if (heaps == 0 or heaps > 16) return error.Protocol;
    try reader.expect(u64, 16);
    _ = try reader.take(16 * 12);
    try writer.begin(21); // AllocateMemory with explicit DMA-BUF export chain.
    try writer.put(u64, DeviceId);
    try writer.put(u64, 1);
    try writer.put(u32, 5); // MEMORY_ALLOCATE_INFO.
    try writer.put(u64, 1);
    try writer.put(u32, 1000072002); // EXPORT_MEMORY_ALLOCATE_INFO.
    try writer.put(u64, 0);
    try writer.put(u32, 0x200);
    try writer.put(u64, extent);
    try writer.put(u32, selected orelse return error.NoDevice);
    try writer.put(u64, 0);
    try writer.put(u64, 1);
    try writer.put(u64, MemoryId);
    reader = try exchange(receiver, writer, reply, 21);
    try reader.expect(u32, 0);
    try reader.expect(u64, 1);
    try reader.expect(u64, MemoryId);
    try writer.begin(29); // BindImageMemory, offset zero.
    try writer.put(u64, DeviceId);
    try writer.put(u64, ImageId);
    try writer.put(u64, MemoryId);
    try writer.put(u64, 0);
    reader = try exchange(receiver, writer, reply, 29);
    try reader.expect(u32, 0);
    try writer.begin(56); // Query authoritative linear image layout.
    try writer.put(u64, DeviceId);
    try writer.put(u64, ImageId);
    try writer.put(u64, 1);
    try writer.words(&.{ 1, 0, 0 });
    try writer.put(u64, 1);
    reader = try exchange(receiver, writer, reply, 56);
    try reader.expect(u64, 1);
    const offset = try reader.get(u64);
    const image_size = try reader.get(u64);
    const stride = try reader.get(u64);
    _ = try reader.take(16);
    if (offset > extent or image_size > extent - offset or stride < 128 or stride > 2147483647 or image_size < stride * 15 + 128) return error.Protocol;
    return .{ .offset = offset, .stride = stride, .size = image_size, .extent = extent };
}
fn image_barrier(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8, old_layout: u32, new_layout: u32, source_stage: u32, destination_stage: u32, source_access: u32, destination_access: u32, source_family: u32, destination_family: u32) !void {
    try writer.begin(126);
    try writer.put(u64, CommandBufferId);
    try writer.words(&.{ source_stage, destination_stage, 0, 0 });
    try writer.put(u64, 0);
    try writer.put(u32, 0);
    try writer.put(u64, 0);
    try writer.put(u32, 1);
    try writer.put(u64, 1);
    try writer.put(u32, 45); // IMAGE_MEMORY_BARRIER.
    try writer.put(u64, 0);
    try writer.words(&.{ source_access, destination_access, old_layout, new_layout, source_family, destination_family });
    try writer.put(u64, ImageId);
    try writer.words(&.{ 1, 0, 1, 0, 1 });
    _ = try exchange(receiver, writer, reply, 126);
}
fn record_image(receiver: *venus_receiver_t, writer: *writer_t, reply: *[BufferBytes]u8, family: u32) !void {
    try image_barrier(receiver, writer, reply, 0, 7, 1, 0x1000, 0, 0x1000, std.math.maxInt(u32), std.math.maxInt(u32));
    try writer.begin(119);
    try writer.put(u64, CommandBufferId);
    try writer.put(u64, ImageId);
    try writer.put(u32, 7); // TRANSFER_DST_OPTIMAL.
    try writer.put(u64, 1);
    try writer.put(u32, 2); // Clear union tag: raw four uint32 float bit patterns.
    try writer.put(u64, 4);
    try writer.words(&.{ 0x3f000000, 0x3e800000, 0x3f400000, 0x3f800000, 1 });
    try writer.put(u64, 1);
    try writer.words(&.{ 1, 0, 1, 0, 1 });
    _ = try exchange(receiver, writer, reply, 119);
    try image_barrier(receiver, writer, reply, 7, 1, 0x1000, 0x2000, 0x1000, 0, family, std.math.maxInt(u32) - 1);
}
const present_image_t = *const fn (c_int, u64, u64, u64, u64, ?*anyopaque) callconv(.C) c_int;
fn run_image_fixture(present: present_image_t, context: ?*anyopaque) !void {
    var owned: ?*venus_receiver_t = null;
    if (venus_receiver_create(&owned, BufferBytes, BufferBytes) != 0) return error.Renderer;
    defer venus_receiver_destroy(&owned);
    const receiver = owned orelse return error.Renderer;
    var writer = writer_t{};
    var reply: [BufferBytes]u8 = undefined;
    try create_instance(receiver, &writer, &reply);
    const count = try enumerate_devices(receiver, &writer, &reply);
    const physical = try select_device(receiver, &writer, &reply, count, true);
    const family = try select_family(receiver, &writer, &reply, physical);
    try create_device(receiver, &writer, &reply, physical, family.index);
    const layout = try allocate_image(receiver, &writer, &reply, physical);
    try create_workload(receiver, &writer, &reply, family.index, true);
    try queue_roundtrip(receiver, &writer, &reply, 1);
    try verify_workload(receiver, &writer, &reply, family.timestamp_bits);
    if (venus_receiver_resource_create(receiver, 2, MemoryId, layout.extent, 6) != 0) return error.Renderer;
    var fd: c_int = -1;
    if (venus_receiver_resource_export(receiver, 2, 1, 1, &fd) != 0 or fd < 0) return error.Renderer;
    defer std.posix.close(fd);
    if (present(fd, layout.offset, layout.stride, layout.size, layout.extent, context) != 0) return error.Presentation;
    if (venus_receiver_resource_free(receiver, 2) != 0) return error.Renderer;
    for ([_]u32{ 86, 48, 55, 22 }, [_]u64{ CommandPoolId, QueryPoolId, ImageId, MemoryId }) |command, object| {
        try writer.begin(command);
        try writer.put(u64, DeviceId);
        try writer.put(u64, object);
        try writer.put(u64, 0);
        _ = try exchange(receiver, &writer, &reply, command);
    }
    for ([_]u32{ 12, 1 }, [_]u64{ DeviceId, InstanceId }) |command, object| {
        try writer.begin(command);
        try writer.put(u64, object);
        try writer.put(u64, 0);
        _ = try exchange(receiver, &writer, &reply, command);
    }
}
/// in: nullable callback and nullable borrowed context. Callback borrows real
/// DMA-BUF plus authoritative linear layout until return; must finish compositor
/// releases before returning. Returns 0 success/1 failure; owns receiver/FD for
/// call and destroys both on every path. Sole session thread, hardware required.
export fn venus_gpu_image_fixture_run(present: ?present_image_t, context: ?*anyopaque) c_int {
    run_image_fixture(present orelse return 1, context) catch |failure| {
        std.debug.print("Venus hardware image fixture failed: {s}\n", .{@errorName(failure)});
        return 1;
    };
    return 0;
}
