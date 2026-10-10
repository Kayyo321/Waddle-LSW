//! Bounded pinned-protocol fixture. No native Vulkan structs or reply casts.
const std = @import("std");
const c = @cImport({
    @cInclude("waddle/venus_request.h");
    @cInclude("waddle/venus_values.h");
});
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

const guest_exchange_t = *const fn (
    ?*anyopaque,
    *const c.venus_request_t,
    ?*const anyopaque,
    usize,
    *c.venus_request_t,
    ?*anyopaque,
    usize,
) callconv(.C) c_int;
// Private call-scoped transport; exactly one local receiver or remote binding.
const backend_t = struct {
    receiver: ?*venus_receiver_t = null,
    guest: ?*anyopaque = null,
    guest_exchange: ?guest_exchange_t = null,
    fn request(
        self: *backend_t,
        kind: u32,
        resource: u32,
        flags: u32,
        argument_zero: u64,
        argument_one: u64,
        input: ?[]const u8,
        output: ?[]u8,
        returned: ?*u64,
    ) c_int {
        var offered = std.mem.zeroes(c.venus_request_t);
        offered.kind = kind;
        offered.resource_id = resource;
        offered.flags = flags;
        offered.argument_zero = argument_zero;
        offered.argument_one = argument_one;
        offered.payload_bytes = if (input) |bytes| @intCast(bytes.len) else 0;
        var response = std.mem.zeroes(c.venus_request_t);
        const status = self.guest_exchange.?(
            self.guest,
            &offered,
            if (input) |bytes| bytes.ptr else null,
            offered.payload_bytes,
            &response,
            if (output) |bytes| bytes.ptr else null,
            if (output) |bytes| bytes.len else 0,
        );
        if (status == 0) {
            if (response.kind != kind or response.direction != 1 or response.status != 0 or
                response.payload_bytes != (if (output) |bytes| bytes.len else 0)) return -2;
            if (returned) |value| value.* = response.argument_zero;
        }
        return status;
    }
    fn health(self: *backend_t) c_int {
        // Remote health is checked by the production service on every RPC.
        return if (self.receiver) |receiver| venus_receiver_health(receiver, null) else 0;
    }
    fn poll(self: *backend_t) c_int {
        if (self.receiver) |receiver| return venus_receiver_poll(receiver);
        return self.request(c.RequestPoll, 0, 0, 0, 0, null, null, null);
    }
    fn submit(self: *backend_t, bytes: []const u8, fence: *u64) c_int {
        if (self.receiver) |receiver|
            return venus_receiver_submit(receiver, bytes.ptr, bytes.len, fence);
        return self.request(c.RequestSubmit, 0, 0, 0, 0, bytes, null, fence);
    }
    fn reply(self: *backend_t, bytes: []u8) c_int {
        if (self.receiver) |receiver|
            return venus_receiver_reply(receiver, 0, bytes.ptr, bytes.len);
        return self.request(c.RequestReply, 0, 0, 0, bytes.len, null, bytes, null);
    }
    fn gpu_fence(self: *backend_t, timeline: u32, fence: *u64) c_int {
        if (self.receiver) |receiver| return venus_receiver_gpu_fence(receiver, timeline, fence);
        return self.request(c.RequestGpuFence, 0, 0, timeline, 0, null, null, fence);
    }
    fn gpu_poll(self: *backend_t, timeline: u32, fence: u64) c_int {
        if (self.receiver) |receiver| return venus_receiver_gpu_poll(receiver, timeline, fence);
        return self.request(c.RequestGpuPoll, 0, 0, timeline, fence, null, null, null);
    }
    fn resource_create(self: *backend_t, resource: u32, blob: u64, size: u64, flags: u32) c_int {
        if (self.receiver) |receiver|
            return venus_receiver_resource_create(receiver, resource, blob, size, flags);
        return self.request(c.RequestCreate, resource, flags, blob, size, null, null, null);
    }
    fn resource_free(self: *backend_t, resource: u32) c_int {
        if (self.receiver) |receiver| return venus_receiver_resource_free(receiver, resource);
        return self.request(c.RequestFree, resource, 0, 0, 0, null, null, null);
    }
};

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

fn wait_cpu(receiver: *backend_t) !void {
    var timer = try std.time.Timer.start();
    while (true) {
        if (receiver.health() != 0) return error.Renderer;
        const status = receiver.poll();
        if (status == 0) return;
        if (status != 1) return error.Renderer;
        if (timer.read() >= 5 * std.time.ns_per_s) return error.Deadline;
        std.time.sleep(std.time.ns_per_ms);
    }
}
fn exchange(
    receiver: *backend_t,
    writer: *writer_t,
    reply: *[BufferBytes]u8,
    command: u32,
) !reader_t {
    var fence: u64 = 0;
    if (receiver.submit(writer.bytes[0..writer.used], &fence) != 0 or
        fence == 0) return error.Renderer;
    try wait_cpu(receiver);
    if (receiver.reply(reply) != 0) return error.Renderer;
    var reader = reader_t{ .bytes = reply };
    try reader.expect(u32, command);
    return reader;
}
fn create_instance(receiver: *backend_t, writer: *writer_t, reply: *[BufferBytes]u8) !void {
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
fn enumerate_devices(receiver: *backend_t, writer: *writer_t, reply: *[BufferBytes]u8) !u32 {
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
fn select_device(
    receiver: *backend_t,
    writer: *writer_t,
    reply: *[BufferBytes]u8,
    count: u32,
    hardware: bool,
) !u64 {
    for (0..count) |index| {
        try writer.begin(6); // GetPhysicalDeviceProperties.
        try writer.put(u64, index + 2);
        try writer.put(u64, 1); // Partial properties need no scalar inputs.
        _ = try exchange(receiver, writer, reply, 6);
        var properties: c.venus_vk_properties_t = undefined;
        if (c.venus_values_properties_decode(&properties, reply, reply.len) != c.RingOk)
            return error.Protocol;
        const device_type = properties.deviceType;
        const name = std.mem.asBytes(&properties.deviceName);
        const terminator = std.mem.indexOfScalar(u8, name, 0) orelse return error.Protocol;
        if (hardware and device_type != 1 and device_type != 2) continue;
        try writer.begin(3); // GetPhysicalDeviceFeatures.
        try writer.put(u64, index + 2);
        try writer.put(u64, 1);
        _ = try exchange(receiver, writer, reply, 3);
        var features: c.venus_vk_features_t = undefined;
        if (c.venus_values_features_decode(&features, reply, reply.len) != c.RingOk)
            return error.Protocol;
        try writer.begin(8); // GetPhysicalDeviceMemoryProperties.
        try writer.put(u64, index + 2);
        try writer.put(u64, 1);
        try writer.put(u64, 32); // Fixed partial memory type array.
        try writer.put(u64, 16); // Fixed partial memory heap array.
        _ = try exchange(receiver, writer, reply, 8);
        var memory: c.venus_vk_memory_t = undefined;
        if (c.venus_values_memory_decode(&memory, reply, reply.len) != c.RingOk)
            return error.Protocol;
        std.debug.print(
            "Venus queue device: type={d} API=0x{x} name={s} heaps={d}\n",
            .{ device_type, properties.apiVersion, name[0..terminator], memory.memoryHeapCount },
        );
        return index + 2;
    }
    return error.NoDevice;
}
fn select_family(
    receiver: *backend_t,
    writer: *writer_t,
    reply: *[BufferBytes]u8,
    physical: u64,
) !queue_family_t {
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
fn create_device(
    receiver: *backend_t,
    writer: *writer_t,
    reply: *[BufferBytes]u8,
    physical: u64,
    family: u32,
) !void {
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
fn create_workload(
    receiver: *backend_t,
    writer: *writer_t,
    reply: *[BufferBytes]u8,
    family: u32,
    image: bool,
) !void {
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
fn verify_workload(
    receiver: *backend_t,
    writer: *writer_t,
    reply: *[BufferBytes]u8,
    valid_bits: u32,
) !void {
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
    std.debug.print(
        "GPU timestamp commands executed: valid_bits={d} delta={d}\n",
        .{ valid_bits, delta },
    );
}
fn queue_roundtrip(
    receiver: *backend_t,
    writer: *writer_t,
    reply: *[BufferBytes]u8,
    expected: u64,
) !void {
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
    if (receiver.gpu_fence(1, &fence) != 0 or fence != expected) return error.Renderer;
    var timer = try std.time.Timer.start();
    while (true) {
        if (receiver.health() != 0) return error.Renderer;
        const status = receiver.gpu_poll(1, fence);
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
    var backend = backend_t{ .receiver = owned orelse return error.Renderer };
    const receiver = &backend;
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
fn allocate_image(
    receiver: *backend_t,
    writer: *writer_t,
    reply: *[BufferBytes]u8,
    physical: u64,
) !image_layout_t {
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
    if (size == 0 or
        size > 1073737728 or
        alignment == 0 or
        alignment & (alignment - 1) != 0 or
        types == 0) return error.Protocol;
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
        if (index < count and
            types & (@as(u32, 1) << @as(u5, @intCast(index))) != 0 and
            flags & 2 != 0 and
            selected == null)
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
    if (offset > extent or
        image_size > extent - offset or
        stride < 128 or
        stride > 2147483647 or
        image_size < stride * 15 + 128) return error.Protocol;
    return .{ .offset = offset, .stride = stride, .size = image_size, .extent = extent };
}
fn image_barrier(
    receiver: *backend_t,
    writer: *writer_t,
    reply: *[BufferBytes]u8,
    old_layout: u32,
    new_layout: u32,
    source_stage: u32,
    destination_stage: u32,
    source_access: u32,
    destination_access: u32,
    source_family: u32,
    destination_family: u32,
) !void {
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
    try writer.words(&.{
        source_access,
        destination_access,
        old_layout,
        new_layout,
        source_family,
        destination_family,
    });
    try writer.put(u64, ImageId);
    try writer.words(&.{ 1, 0, 1, 0, 1 });
    _ = try exchange(receiver, writer, reply, 126);
}
fn record_image(
    receiver: *backend_t,
    writer: *writer_t,
    reply: *[BufferBytes]u8,
    family: u32,
) !void {
    try image_barrier(
        receiver,
        writer,
        reply,
        0,
        7,
        1,
        0x1000,
        0,
        0x1000,
        std.math.maxInt(u32),
        std.math.maxInt(u32),
    );
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
    try image_barrier(
        receiver,
        writer,
        reply,
        7,
        1,
        0x1000,
        0x2000,
        0x1000,
        0,
        family,
        std.math.maxInt(u32) - 1,
    );
}
const present_image_t = *const fn (c_int, u64, u64, u64, u64, ?*anyopaque) callconv(.C) c_int;
fn run_image_fixture(present: present_image_t, context: ?*anyopaque) !void {
    var owned: ?*venus_receiver_t = null;
    if (venus_receiver_create(&owned, BufferBytes, BufferBytes) != 0) return error.Renderer;
    defer venus_receiver_destroy(&owned);
    var backend = backend_t{ .receiver = owned orelse return error.Renderer };
    const receiver = &backend;
    var writer = writer_t{};
    var reply: [BufferBytes]u8 = undefined;
    const layout = try prepare_image(receiver, &writer, &reply);
    var fd: c_int = -1;
    const export_status = venus_receiver_resource_export(receiver.receiver.?, 2, 1, 1, &fd);
    if (export_status != 0 or fd < 0) return error.Renderer;
    defer std.posix.close(fd);
    const present_status = present(
        fd,
        layout.offset,
        layout.stride,
        layout.size,
        layout.extent,
        context,
    );
    if (present_status != 0) return error.Presentation;
    if (receiver.resource_free(2) != 0) return error.Renderer;
    try destroy_image(receiver, &writer, &reply);
}
fn prepare_image(receiver: *backend_t, writer: *writer_t, reply: *[BufferBytes]u8) !image_layout_t {
    try create_instance(receiver, writer, reply);
    const count = try enumerate_devices(receiver, writer, reply);
    const physical = try select_device(receiver, writer, reply, count, true);
    const family = try select_family(receiver, writer, reply, physical);
    try create_device(receiver, writer, reply, physical, family.index);
    const layout = try allocate_image(receiver, writer, reply, physical);
    try create_workload(receiver, writer, reply, family.index, true);
    try queue_roundtrip(receiver, writer, reply, 1);
    try verify_workload(receiver, writer, reply, family.timestamp_bits);
    if (receiver.resource_create(2, MemoryId, layout.extent, 6) != 0) return error.Renderer;
    return layout;
}
fn destroy_image(receiver: *backend_t, writer: *writer_t, reply: *[BufferBytes]u8) !void {
    const Commands = [_]u32{ 86, 48, 55, 22 };
    const Objects = [_]u64{ CommandPoolId, QueryPoolId, ImageId, MemoryId };
    for (Commands, Objects) |command, object| {
        try writer.begin(command);
        try writer.put(u64, DeviceId);
        try writer.put(u64, object);
        try writer.put(u64, 0);
        _ = try exchange(receiver, writer, reply, command);
    }
    for ([_]u32{ 12, 1 }, [_]u64{ DeviceId, InstanceId }) |command, object| {
        try writer.begin(command);
        try writer.put(u64, object);
        try writer.put(u64, 0);
        _ = try exchange(receiver, writer, reply, command);
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

const remote_present_t = *const fn (u64, u64, u64, u64, ?*anyopaque) callconv(.C) c_int;
fn run_remote_image(
    guest_exchange: guest_exchange_t,
    guest: *anyopaque,
    present: remote_present_t,
    context: ?*anyopaque,
) !void {
    var backend = backend_t{ .guest = guest, .guest_exchange = guest_exchange };
    var writer = writer_t{};
    var reply: [BufferBytes]u8 = undefined;
    const layout = try prepare_image(&backend, &writer, &reply);
    if (present(layout.offset, layout.stride, layout.size, layout.extent, context) != 0)
        return error.Presentation;
    if (backend.resource_free(2) != 0) return error.Renderer;
    try destroy_image(&backend, &writer, &reply);
}
/// in: nullable negotiated guest callback/context and image callback/context.
/// Borrows all for call; callback receives queried metadata only and must finish
/// release consumption before returning. Returns0 success/1 failure. Owns no host
/// receiver/FD; on failure caller destroys old worker/session. Sole guest thread.
export fn venus_gpu_remote_image_fixture_run(
    guest_exchange: ?guest_exchange_t,
    guest: ?*anyopaque,
    present: ?remote_present_t,
    context: ?*anyopaque,
) c_int {
    run_remote_image(
        guest_exchange orelse return 1,
        guest orelse return 1,
        present orelse return 1,
        context,
    ) catch |failure| {
        std.debug.print("Mapped Venus image fixture failed: {s}\n", .{@errorName(failure)});
        return 1;
    };
    return 0;
}

const request_fixture_t = struct {
    status: c_int = 0,
    corruption: u32 = 4,
    seen: c.venus_request_t = std.mem.zeroes(c.venus_request_t),
};
fn test_guest_exchange(
    context: ?*anyopaque,
    request: *const c.venus_request_t,
    input: ?*const anyopaque,
    length: usize,
    response: *c.venus_request_t,
    output: ?*anyopaque,
    capacity: usize,
) callconv(.C) c_int {
    const fixture: *request_fixture_t = @ptrCast(@alignCast(context.?));
    fixture.seen = request.*;
    std.debug.assert(request.sequence == 0 and request.direction == 0 and request.status == 0);
    std.debug.assert(length == request.payload_bytes and (length == 0 or input != null));
    if (fixture.status != 0) return fixture.status;
    response.* = std.mem.zeroes(c.venus_request_t);
    response.kind = request.kind;
    response.direction = 1;
    response.payload_bytes = @intCast(capacity);
    if (request.kind == c.RequestSubmit or request.kind == c.RequestGpuFence)
        response.argument_zero = 17;
    if (output) |bytes| {
        const destination: [*]u8 = @ptrCast(bytes);
        @memset(destination[0..capacity], 0x5a);
    }
    switch (fixture.corruption) {
        0 => response.kind = 0,
        1 => response.direction = 0,
        2 => response.status = 1,
        3 => response.payload_bytes += 1,
        else => {},
    }
    return 0;
}
fn test_remote_present(_: u64, _: u64, _: u64, _: u64, _: ?*anyopaque) callconv(.C) c_int {
    return 1;
}
test "guest GPU backend emits exact operation fields and propagates malformed responses" {
    var fixture = request_fixture_t{};
    var backend = backend_t{ .guest = &fixture, .guest_exchange = test_guest_exchange };
    const bytes = try std.testing.allocator.alloc(u8, BufferBytes);
    defer std.testing.allocator.free(bytes);
    @memset(bytes, 0);
    var fence: u64 = 0;
    try std.testing.expectEqual(@as(c_int, 0), backend.health());
    try std.testing.expectEqual(@as(c_int, 0), backend.submit(bytes[0..8], &fence));
    try std.testing.expectEqual(@as(u64, 17), fence);
    try std.testing.expectEqual(@as(u32, c.RequestSubmit), fixture.seen.kind);
    try std.testing.expectEqual(@as(u32, 8), fixture.seen.payload_bytes);
    try std.testing.expectEqual(@as(c_int, 0), backend.poll());
    try std.testing.expectEqual(@as(u32, c.RequestPoll), fixture.seen.kind);
    try std.testing.expectEqual(@as(c_int, 0), backend.reply(bytes));
    try std.testing.expectEqual(@as(u32, c.RequestReply), fixture.seen.kind);
    try std.testing.expectEqual(@as(u64, BufferBytes), fixture.seen.argument_one);
    for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0x5a), byte);
    try std.testing.expectEqual(@as(c_int, 0), backend.gpu_fence(2, &fence));
    try std.testing.expectEqual(@as(u32, c.RequestGpuFence), fixture.seen.kind);
    try std.testing.expectEqual(@as(u64, 2), fixture.seen.argument_zero);
    try std.testing.expectEqual(@as(u64, 0), fixture.seen.argument_one);
    try std.testing.expectEqual(@as(c_int, 0), backend.gpu_poll(2, 17));
    try std.testing.expectEqual(@as(u32, c.RequestGpuPoll), fixture.seen.kind);
    try std.testing.expectEqual(@as(u64, 17), fixture.seen.argument_one);
    try std.testing.expectEqual(@as(c_int, 0), backend.resource_create(2, 106, 4096, 6));
    try std.testing.expectEqual(@as(u32, c.RequestCreate), fixture.seen.kind);
    try std.testing.expectEqual(@as(u32, 2), fixture.seen.resource_id);
    try std.testing.expectEqual(@as(u32, 6), fixture.seen.flags);
    try std.testing.expectEqual(@as(u64, 106), fixture.seen.argument_zero);
    try std.testing.expectEqual(@as(u64, 4096), fixture.seen.argument_one);
    try std.testing.expectEqual(@as(c_int, 0), backend.resource_free(2));
    try std.testing.expectEqual(@as(u32, c.RequestFree), fixture.seen.kind);
    try std.testing.expectEqual(@as(u32, 0), fixture.seen.flags);
    try std.testing.expectEqual(@as(u64, 0), fixture.seen.argument_zero);
    try std.testing.expectEqual(@as(u64, 0), fixture.seen.argument_one);
    for (0..4) |corruption| {
        fixture.corruption = @intCast(corruption);
        try std.testing.expectEqual(@as(c_int, -2), backend.poll());
    }
    fixture.corruption = 4;
    for ([_]c_int{ 1, -1, -2, -3, -4, -5, -6 }) |status| {
        fixture.status = status;
        try std.testing.expectEqual(status, backend.poll());
    }
    try std.testing.expectEqual(@as(c_int, 1), venus_gpu_remote_image_fixture_run(
        null,
        &fixture,
        test_remote_present,
        null,
    ));
    try std.testing.expectEqual(@as(c_int, 1), venus_gpu_remote_image_fixture_run(
        test_guest_exchange,
        null,
        test_remote_present,
        null,
    ));
    try std.testing.expectEqual(@as(c_int, 1), venus_gpu_remote_image_fixture_run(
        test_guest_exchange,
        &fixture,
        null,
        null,
    ));
    fixture.status = -1;
    try std.testing.expectEqual(@as(c_int, 1), venus_gpu_remote_image_fixture_run(
        test_guest_exchange,
        &fixture,
        test_remote_present,
        null,
    ));
}
