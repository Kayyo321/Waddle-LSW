//! Experimental bounded Vulkan dispatch; full device API/DXVK support is separately gated.
const std = @import("std");
const c = @cImport({
    @cInclude("waddle/venus_icd.h");
    @cInclude("waddle/venus_objects.h");
    @cInclude("waddle/venus_query_wire.h");
});
const MaxInstances: usize = 16;
const MaxDevices: usize = 16;
const instance_cache_t = struct {
    handle: u64 = 0,
    ready: bool = false,
    count: u32 = 0,
    physical: [MaxDevices]u64 = [_]u64{0} ** MaxDevices,
};
const device_cache_t = struct {
    handle: u64 = 0,
    family_count: usize = 0,
    families: [16]u32 = [_]u32{0} ** 16,
    counts: [16]u32 = [_]u32{0} ** 16,
    queues: [64]u64 = [_]u64{0} ** 64,
    rings: [64]u32 = [_]u32{0} ** 64,
};
const command_state_t = enum { Initial, Recording, Executable, Invalid, Pending };
const resource_state_t = struct {
    id: u64 = 0,
    allocation_size: u64 = 0,
    type_index: u32 = 0,
    bound_memory: u64 = 0,
    buffer_size: u64 = 0,
    buffer_usage: u32 = 0,
    buffer_references: [8]u64 = [_]u64{0} ** 8,
    pool_family: u32 = 0,
    pool_flags: u32 = 0,
    command_state: command_state_t = .Initial,
    command_level: u32 = 0,
    command_flags: u32 = 0,
    requirements: c.VkMemoryRequirements = std.mem.zeroes(c.VkMemoryRequirements),
};
var resource_states = [_]resource_state_t{.{}} ** 512;
fn resource_index(record: [*c]const c.venus_object_t) usize {
    for (slots, 0..) |slot, index| if (slot.id == record.*.id) return index;
    unreachable;
}
fn resource_state(record: [*c]const c.venus_object_t) *resource_state_t {
    return &resource_states[resource_index(record)];
}
const QueueTimelineTag: u32 = 1000384005;
var ring_slots = [_]bool{false} ** 64;
var device_caches = [_]device_cache_t{.{}} ** 16;
var mutex = std.Thread.Mutex{};
var namespace_id: u32 = 1;
var command = std.mem.zeroes(c.venus_command_t);
var objects = std.mem.zeroes(c.venus_objects_t);
var slots: [512]c.venus_object_t = undefined;
var caches = [_]instance_cache_t{.{}} ** MaxInstances;
var tx: [8192]u8 = undefined;
var rx: [4096]u8 = undefined;
var lost: c_int = c.RingOk;
const exchange_t = *const fn (
    ?*anyopaque,
    [*c]const c.venus_request_t,
    ?*const anyopaque,
    usize,
    [*c]c.venus_request_t,
    ?*anyopaque,
    usize,
) callconv(.C) c_int;
fn clear() void {
    c.venus_command_free(&command);
    c.venus_objects_free(&objects);
    caches = [_]instance_cache_t{.{}} ** MaxInstances;
    device_caches = [_]device_cache_t{.{}} ** 16;
    ring_slots = [_]bool{false} ** 64;
    gpu_fences = [_]u64{0} ** 64;
    resource_states = [_]resource_state_t{.{}} ** 512;
    lost = c.RingOk;
}
/// Borrow one exclusive negotiated backend; public header defines ownership/deadlines/threads.
export fn venus_icd_bind(exchange: ?exchange_t, context: ?*anyopaque) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (exchange == null or context == null or command.exchange != null) return c.RingInvalid;
    if (namespace_id == std.math.maxInt(u32)) return c.RingLimit;
    const status = c.venus_objects_init(&objects, &slots, slots.len, namespace_id, context);
    std.debug.assert(status == c.RingOk);
    namespace_id += 1;
    const initialized = c.venus_command_init(&command, exchange, context, &tx, tx.len, &rx, rx.len);
    std.debug.assert(initialized == c.RingOk);
    return c.RingOk;
}
/// Release an empty binding; header specifies no frontend/resource teardown.
export fn venus_icd_unbind() c_int {
    mutex.lock();
    defer mutex.unlock();
    if (objects.live_count != 0 or command.state == c.CommandSubmitted or
        command.state == c.CommandReady) return c.RingAgain;
    clear();
    return c.RingOk;
}
/// Reset only after the caller retires the old receiver; header defines cancellation boundary.
export fn venus_icd_abandon() void {
    mutex.lock();
    defer mutex.unlock();
    clear();
}
/// External loader negotiation; accepts2..5, header defines preserved failure output.
export fn venus_icd_negotiate_loader(version: ?*u32) c_int {
    const value = version orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (value.* < 2) return c.VK_ERROR_INITIALIZATION_FAILED;
    value.* = @min(value.*, 5);
    return c.VK_SUCCESS;
}
fn object(handle: u64, kind: u32) ?*c.venus_object_t {
    var found: [*c]c.venus_object_t = null;
    if (c.venus_objects_lookup(&objects, handle, kind, 1, &found) != c.RingOk) return null;
    return @ptrCast(found);
}
fn cache(handle: u64) ?*instance_cache_t {
    if (object(handle, c.VK_OBJECT_TYPE_INSTANCE) == null) return null;
    for (&caches) |*entry| if (entry.handle == handle) return entry;
    return null;
}
fn failure(status: c_int) c_int {
    lost = status;
    return c.VK_ERROR_DEVICE_LOST;
}
fn transact(bytes: []const u8) ?[]const u8 {
    if (command.exchange == null or lost != c.RingOk) return null;
    const start = c.venus_command_start(&command, bytes.ptr, bytes.len);
    if (start != c.RingOk) {
        _ = failure(start);
        return null;
    }
    for (0..1000) |_| {
        const status = c.venus_command_poll(&command);
        if (status == c.RingOk) {
            var view: ?*const anyopaque = null;
            var length: usize = 0;
            const taken = c.venus_command_take(&command, &view, &length);
            std.debug.assert(taken == c.RingOk);
            return @as([*]const u8, @ptrCast(view.?))[0..length];
        }
        if (status != c.RingAgain) {
            _ = failure(status);
            return null;
        }
        std.time.sleep(std.time.ns_per_ms);
    }
    _ = failure(c.RingTimeout);
    return null;
}
/// Core instance creation; native inputs borrowed for call, output handle NULL on failure.
/// Mutex serialized; no allocations; reserved host identity published only after exact reply.
fn create_instance(
    info: [*c]const c.VkInstanceCreateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkInstance,
) callconv(.C) c_int {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (info == null or command.exchange == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (info.*.pApplicationInfo != null) {
        const version = info.*.pApplicationInfo.*.apiVersion;
        if (version != 0 and (version >> 29 != 0 or
            ((version >> 22) & 0x7f) != 1 or ((version >> 12) & 0x3ff) != 0))
            return c.VK_ERROR_INCOMPATIBLE_DRIVER;
    }
    var available: ?*instance_cache_t = null;
    for (&caches) |*entry| if (entry.handle == 0) {
        available = entry;
        break;
    };
    const entry = available orelse return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var instance: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_INSTANCE, 0, 1, &instance) != c.RingOk)
        return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    // The loader supplies private link/callback records in the instance chain.
    // Strip only its known ABI tag; application extension chains remain unsupported.
    var native_info = info.*;
    var next = native_info.pNext;
    var links: usize = 0;
    while (next != null) {
        const link: *const c.VkBaseInStructure = @ptrCast(@alignCast(next.?));
        if (links == 32 or link.sType != c.VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            _ = c.venus_objects_release(&objects, instance.*.handle, c.VK_OBJECT_TYPE_INSTANCE, 1);
            return c.VK_ERROR_INITIALIZATION_FAILED;
        }
        links += 1;
        next = link.pNext;
    }
    native_info.pNext = null;
    var encoded: [4096]u8 = undefined;
    var written: usize = 0;
    const status = c.venus_instance_wire_create(
        &native_info,
        instance.*.id,
        &encoded,
        encoded.len,
        &written,
    );
    if (status != c.RingOk) {
        _ = c.venus_objects_release(&objects, instance.*.handle, c.VK_OBJECT_TYPE_INSTANCE, 1);
        return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    const reply = transact(encoded[0..written]) orelse return c.VK_ERROR_DEVICE_LOST;
    var result: c.VkResult = c.VK_ERROR_UNKNOWN;
    if (c.venus_instance_wire_create_reply(
        &result,
        reply.ptr,
        reply.len,
        instance.*.id,
    ) != c.RingOk)
        return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, instance.*.handle, c.VK_OBJECT_TYPE_INSTANCE, 1);
        return result;
    }
    entry.* = .{ .handle = instance.*.handle };
    output.* = @ptrFromInt(instance.*.handle);
    return c.VK_SUCCESS;
}
/// Host instance destruction before local child/root retirement; no allocations.
/// Invalid handles ignored, failed transport poisons binding until caller abandonment.
fn destroy_instance(
    instance: c.VkInstance,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    const handle = if (instance) |value| @intFromPtr(value) else return;
    const entry = cache(handle) orelse return;
    const record = object(handle, c.VK_OBJECT_TYPE_INSTANCE).?;
    // Refuse invalid parent-before-child teardown before any host submission.
    for (slots) |child| {
        if (child.kind == c.VK_OBJECT_TYPE_DEVICE) {
            for (entry.physical[0..entry.count]) |physical| {
                if (object(physical, c.VK_OBJECT_TYPE_PHYSICAL_DEVICE).?.id == child.parent_id)
                    return;
            }
        }
    }
    var encoded: [24]u8 = undefined;
    var written: usize = 0;
    std.debug.assert(
        c.venus_instance_wire_destroy(record.id, &encoded, encoded.len, &written) == c.RingOk,
    );
    _ = transact(encoded[0..written]) orelse return;
    for (entry.physical[0..entry.count]) |physical| {
        if (c.venus_objects_release(
            &objects,
            physical,
            c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,
            1,
        ) != c.RingOk) {
            _ = failure(c.RingCorrupt);
            return;
        }
    }
    if (c.venus_objects_release(&objects, handle, c.VK_OBJECT_TYPE_INSTANCE, 1) != c.RingOk) {
        _ = failure(c.RingCorrupt);
        return;
    }
    entry.* = .{};
}
fn discover(entry: *instance_cache_t) c_int {
    if (entry.ready) return c.VK_SUCCESS;
    const instance = object(entry.handle, c.VK_OBJECT_TYPE_INSTANCE).?;
    var encoded: [164]u8 = undefined;
    var written: usize = 0;
    std.debug.assert(
        c.venus_query_wire_enumerate(
            instance.id,
            null,
            0,
            &encoded,
            encoded.len,
            &written,
        ) == c.RingOk,
    );
    var reply = transact(encoded[0..written]) orelse return c.VK_ERROR_DEVICE_LOST;
    var result: c.VkResult = c.VK_ERROR_UNKNOWN;
    var count: u32 = 0;
    if (c.venus_query_wire_enumerate_reply(
        &result,
        &count,
        null,
        0,
        reply.ptr,
        reply.len,
    ) != c.RingOk)
        return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) return result;
    if (count == 0) {
        entry.ready = true;
        return c.VK_SUCCESS;
    }
    var ids: [MaxDevices]u64 = undefined;
    for (0..count) |index| {
        var physical: [*c]c.venus_object_t = null;
        if (c.venus_objects_reserve(
            &objects,
            c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,
            instance.id,
            1,
            &physical,
        ) != c.RingOk) return failure(c.RingLimit);
        ids[index] = physical.*.id;
        entry.physical[index] = physical.*.handle;
        entry.count += 1;
    }
    std.debug.assert(
        c.venus_query_wire_enumerate(
            instance.id,
            &ids,
            count,
            &encoded,
            encoded.len,
            &written,
        ) == c.RingOk,
    );
    reply = transact(encoded[0..written]) orelse return c.VK_ERROR_DEVICE_LOST;
    var returned: u32 = 0;
    if (c.venus_query_wire_enumerate_reply(
        &result,
        &returned,
        &ids,
        count,
        reply.ptr,
        reply.len,
    ) != c.RingOk)
        return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS or returned != count) return failure(c.RingCorrupt);
    entry.ready = true;
    return c.VK_SUCCESS;
}
/// Enumerate stable borrowed physical handles; count-only/fill capacity follows Vulkan ABI.
/// No allocation; exact host IDs validated before publication, mutex serialized.
fn enumerate_physical(
    instance: c.VkInstance,
    count: [*c]u32,
    output: [*c]c.VkPhysicalDevice,
) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (instance == null or count == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const entry = cache(@intFromPtr(instance.?)) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const result = discover(entry);
    if (result != c.VK_SUCCESS) return result;
    if (output == null) {
        count.* = entry.count;
        return c.VK_SUCCESS;
    }
    const copied = @min(count.*, entry.count);
    for (0..copied) |index| output[index] = @ptrFromInt(entry.physical[index]);
    count.* = copied;
    return if (copied < entry.count) c.VK_INCOMPLETE else c.VK_SUCCESS;
}
fn query(physical: c.VkPhysicalDevice, command_id: u32) ?[]const u8 {
    if (physical == null) return null;
    const record = object(
        @intFromPtr(physical.?),
        c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,
    ) orelse return null;
    var encoded: [40]u8 = undefined;
    var written: usize = 0;
    std.debug.assert(
        c.venus_query_wire_fixed(
            command_id,
            record.id,
            &encoded,
            encoded.len,
            &written,
        ) == c.RingOk,
    );
    return transact(encoded[0..written]);
}
const reader_t = struct {
    bytes: []const u8,
    used: usize = 0,
    fn scalar(self: *reader_t, comptime value_t: type) !value_t {
        const size = @sizeOf(value_t);
        if (size > self.bytes.len - self.used) return error.Bounds;
        const word = std.mem.readInt(value_t, self.bytes[self.used..][0..size], .little);
        self.used += size;
        return word;
    }
    fn value(self: *reader_t, comptime value_t: type) !value_t {
        switch (@typeInfo(value_t)) {
            .Int => return self.scalar(value_t),
            .Struct => |info| {
                var result = std.mem.zeroes(value_t);
                inline for (info.fields) |field| {
                    @field(result, field.name) = try self.value(field.type);
                }
                return result;
            },
            else => @compileError("Only fixed integer physical query outputs supported"),
        }
    }
};
fn physical_request(
    physical: c.VkPhysicalDevice,
    command_id: u32,
    args: []const u32,
    capacity: ?u32,
) ?[]const u8 {
    if (physical == null) return null;
    const record = object(
        @intFromPtr(physical.?),
        c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,
    ) orelse return null;
    var encoded: [64]u8 = undefined;
    std.mem.writeInt(u32, encoded[0..4], command_id, .little);
    std.mem.writeInt(u32, encoded[4..8], 1, .little);
    std.mem.writeInt(u64, encoded[8..16], record.id, .little);
    var used: usize = 16;
    for (args) |arg| {
        std.mem.writeInt(u32, encoded[used..][0..4], arg, .little);
        used += 4;
    }
    std.mem.writeInt(u64, encoded[used..][0..8], 1, .little);
    used += 8;
    if (capacity) |count| {
        std.mem.writeInt(u32, encoded[used..][0..4], count, .little);
        used += 4;
        std.mem.writeInt(u64, encoded[used..][0..8], count, .little);
        used += 8;
    }
    return transact(encoded[0..used]);
}
fn fixed_value(comptime value_t: type, reader: *reader_t, command_id: u32) !value_t {
    if (try reader.scalar(u32) != command_id or try reader.scalar(u64) != 1) return error.Value;
    return reader.value(value_t);
}
/// Query host format flags; borrowed native output preserved on malformed reply, mutex serialized.
fn format_properties(
    physical: c.VkPhysicalDevice,
    format: c.VkFormat,
    output: [*c]c.VkFormatProperties,
) callconv(.C) void {
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return;
    const reply = physical_request(physical, 4, &.{format}, null) orelse return;
    var reader = reader_t{ .bytes = reply };
    const value = fixed_value(c.VkFormatProperties, &reader, 4) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    output.* = value;
}
fn image_value(reader: *reader_t, output: *c.VkImageFormatProperties) !i32 {
    if (try reader.scalar(u32) != 5) return error.Value;
    const result = try reader.scalar(i32);
    if (result > 0 or try reader.scalar(u64) != 1) return error.Value;
    const value = try reader.value(c.VkImageFormatProperties);
    if (result == 0) output.* = value;
    return result;
}
/// Query host image limits; returns native Vulkan error or sticky DeviceLost, no allocations.
fn image_properties(
    physical: c.VkPhysicalDevice,
    format: c.VkFormat,
    kind: c.VkImageType,
    tiling: c.VkImageTiling,
    usage: c.VkImageUsageFlags,
    flags: c.VkImageCreateFlags,
    output: [*c]c.VkImageFormatProperties,
) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const reply = physical_request(
        physical,
        5,
        &.{ format, kind, tiling, usage, flags },
        null,
    ) orelse return c.VK_ERROR_DEVICE_LOST;
    var reader = reader_t{ .bytes = reply };
    return image_value(&reader, @ptrCast(output)) catch return failure(c.RingCorrupt);
}
fn array_values(
    comptime value_t: type,
    reader: *reader_t,
    command_id: u32,
    capacity: u32,
    fill: bool,
    count: *u32,
    output: [*c]value_t,
) !void {
    if (try reader.scalar(u32) != command_id or try reader.scalar(u64) != 1) return error.Value;
    const returned = try reader.scalar(u32);
    const array_count = try reader.scalar(u64);
    if (returned > 64 or array_count != (if (fill) returned else @as(u32, 0)) or
        (fill and returned > capacity)) return error.Value;
    var values: [64]value_t = undefined;
    if (fill) {
        for (values[0..returned]) |*value| value.* = try reader.value(value_t);
    }
    if (fill) @memcpy(output[0..returned], values[0..returned]);
    count.* = returned;
}
/// Query actual host queue families into bounded caller capacity; malformed reply preserves output.
fn queue_properties(
    physical: c.VkPhysicalDevice,
    count: [*c]u32,
    output: [*c]c.VkQueueFamilyProperties,
) callconv(.C) void {
    mutex.lock();
    defer mutex.unlock();
    if (count == null) return;
    const fill = output != null;
    if (fill and count.* == 0) return;
    const capacity = if (fill) @min(count.*, 64) else @as(u32, 0);
    const reply = physical_request(physical, 7, &.{}, capacity) orelse return;
    var reader = reader_t{ .bytes = reply };
    array_values(
        c.VkQueueFamilyProperties,
        &reader,
        7,
        capacity,
        fill,
        @ptrCast(count),
        output,
    ) catch {
        _ = failure(c.RingCorrupt);
    };
}
/// Query host sparse image layout, at most64 entries; borrowed output, no allocation.
fn sparse_properties(
    physical: c.VkPhysicalDevice,
    format: c.VkFormat,
    kind: c.VkImageType,
    samples: c.VkSampleCountFlagBits,
    usage: c.VkImageUsageFlags,
    tiling: c.VkImageTiling,
    count: [*c]u32,
    output: [*c]c.VkSparseImageFormatProperties,
) callconv(.C) void {
    mutex.lock();
    defer mutex.unlock();
    if (count == null) return;
    const fill = output != null;
    if (fill and count.* == 0) return;
    const capacity = if (fill) @min(count.*, 64) else @as(u32, 0);
    const reply = physical_request(
        physical,
        33,
        &.{ format, kind, samples, usage, tiling },
        capacity,
    ) orelse return;
    var reader = reader_t{ .bytes = reply };
    array_values(
        c.VkSparseImageFormatProperties,
        &reader,
        33,
        capacity,
        fill,
        @ptrCast(count),
        output,
    ) catch {
        _ = failure(c.RingCorrupt);
    };
}
const writer_t = struct {
    bytes: [8192]u8 = undefined,
    used: usize = 0,
    fn put(self: *writer_t, comptime word_t: type, word: word_t) void {
        std.debug.assert(@sizeOf(word_t) <= self.bytes.len - self.used);
        std.mem.writeInt(word_t, self.bytes[self.used..][0..@sizeOf(word_t)], word, .little);
        self.used += @sizeOf(word_t);
    }
    fn header(self: *writer_t, command_id: u32, id: u64) void {
        self.put(u32, command_id);
        self.put(u32, 1);
        self.put(u64, id);
    }
};
fn device_cache(handle: u64) ?*device_cache_t {
    if (object(handle, c.VK_OBJECT_TYPE_DEVICE) == null) return null;
    for (&device_caches) |*entry| if (entry.handle == handle) return entry;
    return null;
}
fn encode_device(info: *const c.VkDeviceCreateInfo, physical_id: u64, id: u64) !writer_t {
    if (info.sType != c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO or info.flags != 0 or
        info.queueCreateInfoCount == 0 or info.queueCreateInfoCount > 16 or
        info.pQueueCreateInfos == null) return error.Invalid;
    if (info.enabledLayerCount != 0) return error.Layer;
    if (info.enabledExtensionCount != 0) return error.Extension;
    var next = info.pNext;
    var links: usize = 0;
    while (next != null) {
        const link: *const c.VkBaseInStructure = @ptrCast(@alignCast(next.?));
        if (links == 32 or link.sType != c.VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
            return error.Extension;
        links += 1;
        next = link.pNext;
    }
    var writer = writer_t{};
    writer.header(11, physical_id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    writer.put(u64, 0);
    writer.put(u32, 0);
    writer.put(u32, info.queueCreateInfoCount);
    writer.put(u64, info.queueCreateInfoCount);
    var total: u32 = 0;
    for (info.pQueueCreateInfos[0..info.queueCreateInfoCount], 0..) |queue, index| {
        if (queue.sType != c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO or
            queue.pNext != null or queue.flags != 0 or queue.queueCount == 0 or
            queue.queueCount > 16 or queue.pQueuePriorities == null) return error.Invalid;
        for (info.pQueueCreateInfos[0..index]) |previous| {
            if (previous.queueFamilyIndex == queue.queueFamilyIndex) return error.Invalid;
        }
        total += queue.queueCount;
        if (total > 64) return error.Invalid;
        writer.put(u32, c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        writer.put(u64, 0);
        writer.put(u32, 0);
        writer.put(u32, queue.queueFamilyIndex);
        writer.put(u32, queue.queueCount);
        writer.put(u64, queue.queueCount);
        for (queue.pQueuePriorities[0..queue.queueCount]) |priority| {
            if (!std.math.isFinite(priority) or priority < 0 or priority > 1) return error.Invalid;
            writer.put(u32, @bitCast(priority));
        }
    }
    writer.put(u32, 0);
    writer.put(u64, 0);
    writer.put(u32, 0);
    writer.put(u64, 0);
    writer.put(u64, @intFromBool(info.pEnabledFeatures != null));
    if (info.pEnabledFeatures != null) {
        inline for (@typeInfo(c.VkPhysicalDeviceFeatures).Struct.fields) |field| {
            const boolean = @field(info.pEnabledFeatures.*, field.name);
            if (boolean > 1) return error.Invalid;
            writer.put(u32, boolean);
        }
    }
    writer.put(u64, 0);
    writer.put(u64, 1);
    writer.put(u64, id);
    return writer;
}
fn identity_reply(bytes: []const u8, command_id: u32, id: u64, has_result: bool) !i32 {
    var reader = reader_t{ .bytes = bytes };
    if (try reader.scalar(u32) != command_id) return error.Value;
    const result = if (has_result) try reader.scalar(i32) else 0;
    if (try reader.scalar(u64) != 1 or try reader.scalar(u64) != id) return error.Value;
    return result;
}
/// Borrowed native input, allocation-free serialized reservation/publication; NULL output on error.
/// Canonical core queues/features only; unknown extension/layer chains explicitly rejected.
/// @param[in] physical Nonnull live private physical handle, validated without dereference.
/// @param[in] info Nonnull borrowed accessible native structs/priorities/features for this call.
/// @param[in] allocator Nullable borrowed callbacks; no allocations performed or retained.
/// @param[out] output Nonnull borrowed writable handle, NULL on any error.
/// @return Native host result, initialization/layer/extension/host-memory errors or device loss.
fn create_device(
    physical: c.VkPhysicalDevice,
    info: [*c]const c.VkDeviceCreateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkDevice,
) callconv(.C) c_int {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (physical == null or info == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(physical.?),
        c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var available: ?*device_cache_t = null;
    for (&device_caches) |*entry| if (entry.handle == 0) {
        available = entry;
        break;
    };
    const entry = available orelse return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_DEVICE, parent.id, 1, &record) !=
        c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    const encoded = encode_device(@ptrCast(info), parent.id, record.*.id) catch |err| {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_DEVICE, 1);
        return switch (err) {
            error.Layer => c.VK_ERROR_LAYER_NOT_PRESENT,
            error.Extension => c.VK_ERROR_EXTENSION_NOT_PRESENT,
            else => c.VK_ERROR_INITIALIZATION_FAILED,
        };
    };
    const reply = transact(encoded.bytes[0..encoded.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, 11, record.*.id, true) catch
        return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_DEVICE, 1);
        return result;
    }
    entry.* = .{ .handle = record.*.handle, .family_count = info.*.queueCreateInfoCount };
    for (info.*.pQueueCreateInfos[0..entry.family_count], 0..) |queue, index| {
        entry.families[index] = queue.queueFamilyIndex;
        entry.counts[index] = queue.queueCount;
    }
    output.* = @ptrFromInt(entry.handle);
    return c.VK_SUCCESS;
}
/// Borrowed output cleared for invalid/lost calls; stable queue identity lasts until device retire.
/// @param[in] device Nullable validated private device handle, not dereferenced.
/// @param[in] family Queue family requested during device creation.
/// @param[in] index Zero-based index below that family's requested count.
/// @param[out] output Nullable borrowed handle storage; NULL on invalid/lost/capacity failure.
/// Mutex serialized, no allocations; reply identity must match a private reservation.
fn get_device_queue(
    device: c.VkDevice,
    family: u32,
    index: u32,
    output: [*c]c.VkQueue,
) callconv(.C) void {
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return;
    output.* = null;
    if (device == null or lost != c.RingOk) return;
    const entry = device_cache(@intFromPtr(device.?)) orelse return;
    var offset: usize = 0;
    var position: ?usize = null;
    for (entry.families[0..entry.family_count], 0..) |number, family_index| {
        if (family == number and index < entry.counts[family_index]) position = offset + index;
        offset += entry.counts[family_index];
    }
    const queue_index = position orelse return;
    if (entry.queues[queue_index] != 0) {
        output.* = @ptrFromInt(entry.queues[queue_index]);
        return;
    }
    var available_ring: ?u32 = null;
    for (ring_slots[1..], 1..) |occupied, ring_index| if (!occupied) {
        available_ring = @intCast(ring_index);
        break;
    };
    const ring_index = available_ring orelse return;
    const parent = object(entry.handle, c.VK_OBJECT_TYPE_DEVICE).?;
    var queue: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_QUEUE, parent.id, 1, &queue) !=
        c.RingOk) return;
    var writer = writer_t{};
    ring_slots[ring_index] = true;
    writer.header(155, parent.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2);
    writer.put(u64, 1);
    writer.put(u32, QueueTimelineTag);
    writer.put(u64, 0);
    writer.put(u32, ring_index);
    writer.put(u32, 0);
    writer.put(u32, family);
    writer.put(u32, index);
    writer.put(u64, 1);
    writer.put(u64, queue.*.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    _ = identity_reply(reply, 155, queue.*.id, false) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    entry.queues[queue_index] = queue.*.handle;
    entry.rings[queue_index] = ring_index;
    output.* = @ptrFromInt(queue.*.handle);
}
/// Retire host device then private queues/device; loss retains reservations.
/// @param[in] device Nullable private handle, foreign/retired handles ignored without dereference.
/// @param[in] allocator Nullable borrowed callback input; no callbacks or allocations performed.
/// Allocation-free/mutex serialized; caller must first destroy any future nonqueue children.
fn destroy_device(
    device: c.VkDevice,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    if (device == null) return;
    const entry = device_cache(@intFromPtr(device.?)) orelse return;
    const record = object(entry.handle, c.VK_OBJECT_TYPE_DEVICE).?;
    for (slots) |child| {
        if (child.id != 0 and child.parent_id == record.id and child.kind != c.VK_OBJECT_TYPE_QUEUE)
            return;
    }
    var writer = writer_t{};
    writer.header(12, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const reply_command = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (reply_command != 12) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for (entry.queues) |handle| if (handle != 0) {
        std.debug.assert(c.venus_objects_release(&objects, handle, c.VK_OBJECT_TYPE_QUEUE, 1) ==
            c.RingOk);
    };
    std.debug.assert(c.venus_objects_release(&objects, entry.handle, c.VK_OBJECT_TYPE_DEVICE, 1) ==
        c.RingOk);
    for (entry.rings) |ring| if (ring != 0) {
        ring_slots[ring] = false;
    };
    entry.* = .{};
}
/// Empty supported device extension list; no allocation; invalid handles/layers rejected.
fn device_extensions(
    physical: c.VkPhysicalDevice,
    layer: [*c]const u8,
    count: [*c]u32,
    output: [*c]c.VkExtensionProperties,
) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (physical == null or object(
        @intFromPtr(physical.?),
        c.VK_OBJECT_TYPE_PHYSICAL_DEVICE,
    ) == null)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    return enumerate_extensions(layer, count, output);
}
/// Query actual host properties into caller storage only after bounded reply validation.
/// Borrowed output, no allocations; errors preserve output and poison transport binding.
fn properties(
    physical: c.VkPhysicalDevice,
    output: [*c]c.VkPhysicalDeviceProperties,
) callconv(.C) void {
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return;
    const reply = query(physical, 6) orelse return;
    var staged: c.VkPhysicalDeviceProperties = undefined;
    if (c.venus_values_properties_decode(&staged, reply.ptr, reply.len) != c.RingOk or
        staged.apiVersion >> 29 != 0 or ((staged.apiVersion >> 22) & 0x7f) != 1)
    {
        _ = failure(c.RingCorrupt);
        return;
    }
    staged.apiVersion = c.VK_API_VERSION_1_0;
    output.* = staged;
}
/// Query actual host core features; same serialized/preserved-output contract as properties.
fn features(
    physical: c.VkPhysicalDevice,
    output: [*c]c.VkPhysicalDeviceFeatures,
) callconv(.C) void {
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return;
    const reply = query(physical, 3) orelse return;
    if (c.venus_values_features_decode(output, reply.ptr, reply.len) != c.RingOk) {
        _ = failure(c.RingCorrupt);
    }
}
/// Query actual host memory layout; same serialized/preserved-output contract as properties.
fn memory(
    physical: c.VkPhysicalDevice,
    output: [*c]c.VkPhysicalDeviceMemoryProperties,
) callconv(.C) void {
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return;
    const reply = query(physical, 8) orelse return;
    if (c.venus_values_memory_decode(output, reply.ptr, reply.len) != c.RingOk) {
        _ = failure(c.RingCorrupt);
    }
}
/// Enumerate empty supported instance extensions; borrowed output, no allocation or transport.
fn enumerate_extensions(
    layer: [*c]const u8,
    count: [*c]u32,
    output: [*c]c.VkExtensionProperties,
) callconv(.C) c_int {
    _ = output;
    if (count == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (layer != null) return c.VK_ERROR_LAYER_NOT_PRESENT;
    count.* = 0;
    return c.VK_SUCCESS;
}
/// Report the implemented API ceiling; no ownership or transport, thread-safe.
fn enumerate_version(version: [*c]u32) callconv(.C) c_int {
    if (version == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    version.* = 1 << 22;
    return c.VK_SUCCESS;
}
fn child_object(handle: u64, kind: u32, parent_id: u64) ?*c.venus_object_t {
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_lookup(&objects, handle, kind, 0, &record) != c.RingOk or
        record.*.parent_id != parent_id) return null;
    return @ptrCast(record);
}
fn result_reply(bytes: []const u8, command_id: u32, pending: i32) c_int {
    var reader = reader_t{ .bytes = bytes };
    const received = reader.scalar(u32) catch return failure(c.RingCorrupt);
    if (received != command_id) return failure(c.RingCorrupt);
    const result = reader.scalar(i32) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0 and result != pending) return failure(c.RingCorrupt);
    return result;
}
/// Create one device-owned nondispatchable fence without allocation.
/// @param[in] device Nonnull live private device, validated without native dereference.
/// @param[in] info Nonnull borrowed canonical flags0/1/no-extension native input.
/// @param[in] allocator Nullable borrowed callbacks, not retained or invoked.
/// @param[out] output Nonnull native handle storage, NULL on any failure.
/// @return Host result, explicit local validation/memory errors or sticky device loss.
/// @note Mutex serialized; publish only a validated host reservation. Caller owns the fence.
fn create_fence(
    device: c.VkDevice,
    info: [*c]const c.VkFenceCreateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkFence,
) callconv(.C) c_int {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or info.*.sType != c.VK_STRUCTURE_TYPE_FENCE_CREATE_INFO or
        info.*.pNext != null or info.*.flags > 1) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(&objects, c.VK_OBJECT_TYPE_FENCE, parent.id, 0, &record) !=
        c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var writer = writer_t{};
    writer.header(35, parent.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    writer.put(u64, 0);
    writer.put(u32, info.*.flags);
    writer.put(u64, 0);
    writer.put(u64, 1);
    writer.put(u64, record.*.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, 35, record.*.id, true) catch
        return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0) return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_FENCE, 0);
        return result;
    }
    output.* = @ptrFromInt(record.*.handle);
    return c.VK_SUCCESS;
}
/// Destroy a quiescent device-owned fence; invalid/foreign handles ignored.
/// @param[in] device Nonnull live private parent; no native handle dereference.
/// @param[in] fence Nullable private token, borrowed until successful host destruction.
/// @param[in] allocator Nullable unused borrowed callbacks; no allocation or retained pointer.
/// @note Mutex serialized; caller retires GPU references first. Loss retains ownership.
fn destroy_fence(
    device: c.VkDevice,
    fence: c.VkFence,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    if (device == null or fence == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(
        @intFromPtr(fence.?),
        c.VK_OBJECT_TYPE_FENCE,
        parent.id,
    ) orelse return;
    var writer = writer_t{};
    writer.header(36, parent.id);
    writer.put(u64, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 36) {
        _ = failure(c.RingCorrupt);
        return;
    }
    std.debug.assert(c.venus_objects_release(&objects, record.handle, c.VK_OBJECT_TYPE_FENCE, 0) ==
        c.RingOk);
}
/// Create a device-owned core buffer after validating bounded native input.
/// @param[in] device Nonnull live private device; no caller handle dereference.
/// @param[in] info Nonnull accessible canonical info and optional2..16 family array.
/// @param[in] allocator Nullable unused callbacks, borrowed only for call.
/// @param[out] output Nonnull borrowed handle storage, NULL on failure.
/// @return Host result, initialization error, registry exhaustion or sticky device loss.
/// @note Allocation-free and mutex serialized; record owned until host destruction.
fn create_buffer(
    device: c.VkDevice,
    info: [*c]const c.VkBufferCreateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkBuffer,
) callconv(.C) c_int {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or
        info.*.sType != c.VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO or info.*.pNext != null or
        info.*.flags != 0 or info.*.size == 0 or info.*.usage == 0 or
        info.*.usage & ~@as(u32, 0x1ff) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var family_count: u32 = 0;
    if (info.*.sharingMode == c.VK_SHARING_MODE_CONCURRENT) {
        family_count = info.*.queueFamilyIndexCount;
        if (family_count < 2 or family_count > 16 or info.*.pQueueFamilyIndices == null)
            return c.VK_ERROR_INITIALIZATION_FAILED;
        const entry = device_cache(parent.handle).?;
        for (info.*.pQueueFamilyIndices[0..family_count], 0..) |family, index| {
            if (std.mem.indexOfScalar(
                u32,
                entry.families[0..entry.family_count],
                family,
            ) == null or std.mem.indexOfScalar(
                u32,
                info.*.pQueueFamilyIndices[0..index],
                family,
            ) != null) return c.VK_ERROR_INITIALIZATION_FAILED;
        }
    } else if (info.*.sharingMode != c.VK_SHARING_MODE_EXCLUSIVE)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_BUFFER,
        parent.id,
        0,
        &record,
    ) != c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var writer = writer_t{};
    writer.header(50, parent.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    writer.put(u64, 0);
    writer.put(u32, 0);
    writer.put(u64, info.*.size);
    writer.put(u32, info.*.usage);
    writer.put(u32, info.*.sharingMode);
    writer.put(u32, family_count);
    writer.put(u64, family_count);
    if (family_count != 0) for (info.*.pQueueFamilyIndices[0..family_count]) |family| {
        writer.put(u32, family);
    };
    writer.put(u64, 0);
    writer.put(u64, 1);
    writer.put(u64, record.*.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, 50, record.*.id, true) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0) return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_BUFFER, 0);
        return result;
    }
    resource_state(record).* = .{
        .id = record.*.id,
        .buffer_size = info.*.size,
        .buffer_usage = info.*.usage,
    };
    output.* = @ptrFromInt(record.*.handle);
    return c.VK_SUCCESS;
}
/// Destroy a quiescent buffer, ignoring NULL/stale/foreign handles.
/// @param[in] device Nullable private device parent, borrowed for call.
/// @param[in] buffer Nullable device-owned token, consumed only after host destruction.
/// @param[in] allocator Nullable unused callbacks, no pointer retained.
/// @note Allocation-free and mutex serialized; caller retires GPU uses first.
/// Host loss retains uncertain ownership until receiver retirement and abandonment.
fn destroy_buffer(
    device: c.VkDevice,
    buffer: c.VkBuffer,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    if (device == null or buffer == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_BUFFER,
        parent.id,
    ) orelse return;
    const index = resource_index(record);
    const bit = @as(u64, 1) << @as(u6, @intCast(index % 64));
    for (resource_states) |state| if (state.buffer_references[index / 64] & bit != 0 and
        state.command_state == .Pending) return;
    var writer = writer_t{};
    writer.header(51, parent.id);
    writer.put(u64, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 51) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for (&resource_states) |*state| if (state.buffer_references[index / 64] & bit != 0) {
        state.command_state = .Invalid;
        state.buffer_references = [_]u64{0} ** 8;
    };
    resource_state(record).* = .{};
    std.debug.assert(
        c.venus_objects_release(&objects, record.handle, c.VK_OBJECT_TYPE_BUFFER, 0) == c.RingOk,
    );
}
/// Query actual host memory requirements through a validated private buffer parent.
/// @param[in] device Nullable private parent handle, borrowed for call.
/// @param[in] buffer Nullable private device-owned token, no pointer dereference.
/// @param[out] output Nullable borrowed storage, preserved on all errors.
/// @note Allocation-free and mutex serialized; invalid handles ignored.
/// Peer/transport errors poison binding; staged size/alignment/type bits validated.
fn buffer_requirements(
    device: c.VkDevice,
    buffer: c.VkBuffer,
    output: [*c]c.VkMemoryRequirements,
) callconv(.C) void {
    mutex.lock();
    defer mutex.unlock();
    if (device == null or buffer == null or output == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_BUFFER,
        parent.id,
    ) orelse return;
    const value = query_buffer_requirements(parent.id, record.id) orelse return;
    resource_state(record).requirements = value;
    output.* = value;
}
fn query_buffer_requirements(device_id: u64, buffer_id: u64) ?c.VkMemoryRequirements {
    var writer = writer_t{};
    writer.header(30, device_id);
    writer.put(u64, buffer_id);
    writer.put(u64, 1);
    const reply = transact(writer.bytes[0..writer.used]) orelse return null;
    var reader = reader_t{ .bytes = reply };
    const value = fixed_value(c.VkMemoryRequirements, &reader, 30) catch {
        _ = failure(c.RingCorrupt);
        return null;
    };
    if (value.size == 0 or value.alignment == 0 or
        value.alignment & (value.alignment - 1) != 0 or value.memoryTypeBits == 0)
    {
        _ = failure(c.RingCorrupt);
        return null;
    }
    return value;
}
/// Allocate private device memory with exact host identity validation.
/// @param[in] device Nonnull private live parent, borrowed for call.
/// @param[in] info Nonnull canonical allocation info, borrowed; no pNext supported.
/// @param[in] allocator Nullable unused callbacks, no pointer retained.
/// @param[out] output Nonnull borrowed token storage; NULL on failure.
/// @return Host result, local initialization/exhaustion or sticky device loss.
/// @note Mutex serialized, no local allocation; owns host memory until validated free.
fn allocate_memory(
    device: c.VkDevice,
    info: [*c]const c.VkMemoryAllocateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkDeviceMemory,
) callconv(.C) c_int {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or
        info.*.sType != c.VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO or info.*.pNext != null or
        info.*.allocationSize == 0 or info.*.memoryTypeIndex >= 32) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_DEVICE_MEMORY,
        parent.id,
        0,
        &record,
    ) != c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var writer = writer_t{};
    writer.header(21, parent.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    writer.put(u64, 0);
    writer.put(u64, info.*.allocationSize);
    writer.put(u32, info.*.memoryTypeIndex);
    writer.put(u64, 0);
    writer.put(u64, 1);
    writer.put(u64, record.*.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, 21, record.*.id, true) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0) return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_DEVICE_MEMORY, 0);
        return result;
    }
    resource_state(record).* = .{
        .id = record.*.id,
        .allocation_size = info.*.allocationSize,
        .type_index = info.*.memoryTypeIndex,
    };
    output.* = @ptrFromInt(record.*.handle);
    return c.VK_SUCCESS;
}
/// Free memory after all private bound buffers and GPU uses are retired.
/// @param[in] device Nullable private parent, borrowed; invalid handle ignored.
/// @param[in] memory_handle Nullable private token; consumed only after exact host free.
/// @param[in] allocator Nullable unused callbacks; no pointer retained.
/// @note Mutex serialized, allocation-free. Loss retains ownership until abandonment.
fn free_memory(
    device: c.VkDevice,
    memory_handle: c.VkDeviceMemory,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    if (device == null or memory_handle == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(
        @intFromPtr(memory_handle.?),
        c.VK_OBJECT_TYPE_DEVICE_MEMORY,
        parent.id,
    ) orelse return;
    for (resource_states) |state| if (state.bound_memory == record.handle) return;
    var writer = writer_t{};
    writer.header(22, parent.id);
    writer.put(u64, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 22) {
        _ = failure(c.RingCorrupt);
        return;
    }
    resource_state(record).* = .{};
    std.debug.assert(
        c.venus_objects_release(
            &objects,
            record.handle,
            c.VK_OBJECT_TYPE_DEVICE_MEMORY,
            0,
        ) == c.RingOk,
    );
}
/// Bind a buffer to memory_handle using actual host requirements and overflow-safe bounds.
/// @param[in] device Nonnull private parent, borrowed for call.
/// @param[in] buffer Nonnull private unbound buffer of this device.
/// @param[in] memory_handle Nonnull private allocation of this device, retained by buffer.
/// @param[in] offset Byte offset aligned to requirements, within allocation extent.
/// @return Host result, local initialization error or sticky device loss.
/// @note Mutex serialized, allocation-free; publishes relationship only after success.
fn bind_buffer_memory(
    device: c.VkDevice,
    buffer: c.VkBuffer,
    memory_handle: c.VkDeviceMemory,
    offset: u64,
) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (device == null or buffer == null or memory_handle == null)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const buffer_record = child_object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_BUFFER,
        parent.id,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const memory_record = child_object(
        @intFromPtr(memory_handle.?),
        c.VK_OBJECT_TYPE_DEVICE_MEMORY,
        parent.id,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const buffer_state = resource_state(buffer_record);
    const memory_state = resource_state(memory_record);
    if (buffer_state.bound_memory != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (buffer_state.requirements.size == 0) {
        const value = query_buffer_requirements(parent.id, buffer_record.id) orelse
            return c.VK_ERROR_DEVICE_LOST;
        buffer_state.requirements = value;
    }
    const requirements = buffer_state.requirements;
    if (requirements.memoryTypeBits & (@as(u32, 1) << @as(
        u5,
        @intCast(memory_state.type_index),
    )) == 0 or offset % requirements.alignment != 0 or
        offset > memory_state.allocation_size or
        requirements.size > memory_state.allocation_size - offset) return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = writer_t{};
    writer.header(28, parent.id);
    writer.put(u64, buffer_record.id);
    writer.put(u64, memory_record.id);
    writer.put(u64, offset);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 28, 0);
    if (result == c.VK_SUCCESS) buffer_state.bound_memory = memory_record.handle;
    return result;
}
/// Create a core command pool with private device parent and configured queue family.
/// @param[in] device Nonnull live private parent, borrowed for call.
/// @param[in] info Nonnull canonical native info, no pNext; flags0..3 supported.
/// @param[in] allocator Nullable unused callbacks, no pointer retained.
/// @param[out] output Nonnull borrowed handle storage, NULL on failure.
/// @return Host result, initialization/exhaustion error or sticky device loss.
/// @note Allocation-free, mutex serialized; owned until validated host destruction.
fn create_command_pool(
    device: c.VkDevice,
    info: [*c]const c.VkCommandPoolCreateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkCommandPool,
) callconv(.C) c_int {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    if (output == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    output.* = null;
    if (device == null or info == null or
        info.*.sType != c.VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO or info.*.pNext != null or
        info.*.flags & ~@as(u32, 3) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const entry = device_cache(parent.handle).?;
    if (std.mem.indexOfScalar(
        u32,
        entry.families[0..entry.family_count],
        info.*.queueFamilyIndex,
    ) == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    var record: [*c]c.venus_object_t = null;
    if (c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        parent.id,
        0,
        &record,
    ) != c.RingOk) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    var writer = writer_t{};
    writer.header(85, parent.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    writer.put(u64, 0);
    writer.put(u32, info.*.flags);
    writer.put(u32, info.*.queueFamilyIndex);
    writer.put(u64, 0);
    writer.put(u64, 1);
    writer.put(u64, record.*.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = identity_reply(reply, 85, record.*.id, true) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result > 0) return failure(c.RingCorrupt);
    if (result != c.VK_SUCCESS) {
        _ = c.venus_objects_release(&objects, record.*.handle, c.VK_OBJECT_TYPE_COMMAND_POOL, 0);
        return result;
    }
    resource_state(record).* = .{
        .id = record.*.id,
        .pool_family = info.*.queueFamilyIndex,
        .pool_flags = info.*.flags,
    };
    output.* = @ptrFromInt(record.*.handle);
    return c.VK_SUCCESS;
}
/// Destroy a quiescent pool and its implicit command-buffer children.
/// @param[in] device Nullable private parent, borrowed; invalid handle ignored.
/// @param[in] pool Nullable private device-owned token; consumed after exact reply.
/// @param[in] allocator Nullable unused callbacks, no pointer retained.
/// @note Mutex serialized, allocation-free; caller retires pending GPU uses first.
/// Transport/peer loss retains all uncertain identities until receiver abandonment.
fn destroy_command_pool(
    device: c.VkDevice,
    pool: c.VkCommandPool,
    allocator: [*c]const c.VkAllocationCallbacks,
) callconv(.C) void {
    _ = allocator;
    mutex.lock();
    defer mutex.unlock();
    if (device == null or pool == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const record = child_object(
        @intFromPtr(pool.?),
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        parent.id,
    ) orelse return;
    for (slots, 0..) |child, index|
        if (child.parent_id == record.id and (child.kind != c.VK_OBJECT_TYPE_COMMAND_BUFFER or
            resource_states[index].command_state == .Pending)) return;
    var writer = writer_t{};
    writer.header(86, parent.id);
    writer.put(u64, record.id);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 86) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for (&slots, 0..) |*child, index| if (child.parent_id == record.id) {
        resource_states[index] = .{};
        std.debug.assert(
            c.venus_objects_release(
                &objects,
                child.handle,
                c.VK_OBJECT_TYPE_COMMAND_BUFFER,
                1,
            ) == c.RingOk,
        );
    };
    resource_state(record).* = .{};
    std.debug.assert(
        c.venus_objects_release(
            &objects,
            record.handle,
            c.VK_OBJECT_TYPE_COMMAND_POOL,
            0,
        ) == c.RingOk,
    );
}
/// Reset a quiescent pool after exact device-parent validation.
/// @param[in] device Nullable private parent, borrowed for call.
/// @param[in] pool Nullable private device-owned token, no ownership transfer.
/// @param[in] flags Core0/1 release-resources flags only.
/// @return Host result, local initialization error or sticky device loss.
/// @note Mutex serialized, allocation-free; caller ensures no child is pending GPU use.
fn reset_command_pool(device: c.VkDevice, pool: c.VkCommandPool, flags: u32) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (device == null or pool == null or flags > 1) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = child_object(
        @intFromPtr(pool.?),
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        parent.id,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    for (slots, 0..) |child, index|
        if (child.parent_id == record.id and resource_states[index].command_state == .Pending)
            return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = writer_t{};
    writer.header(87, parent.id);
    writer.put(u64, record.id);
    writer.put(u32, flags);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 87, 0);
    if (result == c.VK_SUCCESS) for (slots, 0..) |child, index| if (child.parent_id == record.id) {
        resource_states[index].command_state = .Initial;
        resource_states[index].command_flags = 0;
        resource_states[index].buffer_references = [_]u64{0} ** 8;
    };
    return result;
}
fn command_pool_for(record: *const c.venus_object_t) ?*c.venus_object_t {
    for (&slots) |*slot|
        if (slot.id == record.parent_id and slot.kind == c.VK_OBJECT_TYPE_COMMAND_POOL) return slot;
    return null;
}
fn command_buffers_reply(bytes: []const u8, ids: []const u64) !c_int {
    var reader = reader_t{ .bytes = bytes };
    if (try reader.scalar(u32) != 88) return error.Value;
    const result = try reader.scalar(i32);
    if (result > 0 or try reader.scalar(u64) != ids.len) return error.Value;
    for (ids) |id| {
        const received = try reader.scalar(u64);
        if (received != id and (result == 0 or received != 0)) return error.Value;
    }
    return result;
}
/// Allocate1..64 private command buffers transactionally under one pool.
/// @param[in] device Nonnull private live device parent, borrowed for call.
/// @param[in] info Nonnull canonical tag40/no pNext/level0..1 and private pool.
/// @param[out] output Borrowed accessible handles[count]; NULL on validated bounded failures.
/// @return Host result, local initialization/exhaustion or sticky device loss.
/// @note Allocation-free and mutex serialized; count>64 leaves output untouched.
/// Records are pool-owned, published only after all exact host IDs validate.
fn allocate_command_buffers(
    device: c.VkDevice,
    info: [*c]const c.VkCommandBufferAllocateInfo,
    output: [*c]c.VkCommandBuffer,
) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (info == null or output == null or info.*.commandBufferCount == 0)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    const native_info = info.*;
    const count = native_info.commandBufferCount;
    if (count > 64) return c.VK_ERROR_OUT_OF_HOST_MEMORY;
    @memset(output[0..count], null);
    if (device == null or native_info.sType != c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO or
        native_info.pNext != null or (native_info.level != 0 and native_info.level != 1) or
        native_info.commandPool == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const pool = child_object(
        @intFromPtr(native_info.commandPool.?),
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        parent.id,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var records: [64][*c]c.venus_object_t = undefined;
    var ids: [64]u64 = undefined;
    var reserved: usize = 0;
    while (reserved < count) : (reserved += 1) {
        var record: [*c]c.venus_object_t = null;
        if (c.venus_objects_reserve(
            &objects,
            c.VK_OBJECT_TYPE_COMMAND_BUFFER,
            pool.id,
            1,
            &record,
        ) != c.RingOk) {
            for (records[0..reserved]) |entry| _ = c.venus_objects_release(
                &objects,
                entry.*.handle,
                c.VK_OBJECT_TYPE_COMMAND_BUFFER,
                1,
            );
            return c.VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        records[reserved] = record;
        ids[reserved] = record.*.id;
    }
    var writer = writer_t{};
    writer.header(88, parent.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    writer.put(u64, 0);
    writer.put(u64, pool.id);
    writer.put(u32, native_info.level);
    writer.put(u32, count);
    writer.put(u64, count);
    for (ids[0..count]) |id| writer.put(u64, id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = command_buffers_reply(reply, ids[0..count]) catch return failure(c.RingCorrupt);
    if (result == c.VK_ERROR_DEVICE_LOST) return failure(c.RingClosed);
    if (result != c.VK_SUCCESS) {
        for (records[0..count]) |entry| _ = c.venus_objects_release(
            &objects,
            entry.*.handle,
            c.VK_OBJECT_TYPE_COMMAND_BUFFER,
            1,
        );
        return result;
    }
    for (records[0..count], 0..) |record, index| {
        resource_state(record).* = .{ .id = record.*.id, .command_level = native_info.level };
        output[index] = @ptrFromInt(record.*.handle);
    }
    return c.VK_SUCCESS;
}
/// Free a validated0..64 pool-owned command-buffer batch after GPU retirement.
/// @param[in] device Nullable private parent, borrowed; invalid handles ignored.
/// @param[in] pool Nullable private same-device pool.
/// @param[in] count0..64 accessible handle extent; zero is a no-op.
/// @param[in] buffers Nullable only for zero; borrowed and immutable for call.
/// @note Mutex serialized, allocation-free; duplicates/foreign/stale entries ignored.
/// Retire all private records only after host reply; loss retains uncertain ownership.
fn free_command_buffers(
    device: c.VkDevice,
    pool: c.VkCommandPool,
    count: u32,
    buffers: [*c]const c.VkCommandBuffer,
) callconv(.C) void {
    mutex.lock();
    defer mutex.unlock();
    if (count == 0 or count > 64 or device == null or pool == null or buffers == null) return;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return;
    const pool_record = child_object(
        @intFromPtr(pool.?),
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        parent.id,
    ) orelse return;
    var records: [64]*c.venus_object_t = undefined;
    for (buffers[0..count], 0..) |buffer, index| {
        if (buffer == null) return;
        const record = object(@intFromPtr(buffer.?), c.VK_OBJECT_TYPE_COMMAND_BUFFER) orelse return;
        if (record.parent_id != pool_record.id or resource_state(record).command_state == .Pending)
            return;
        for (records[0..index]) |prior| if (prior.id == record.id) return;
        records[index] = record;
    }
    var writer = writer_t{};
    writer.header(89, parent.id);
    writer.put(u64, pool_record.id);
    writer.put(u32, count);
    writer.put(u64, count);
    for (records[0..count]) |record| writer.put(u64, record.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 89) {
        _ = failure(c.RingCorrupt);
        return;
    }
    for (records[0..count]) |record| {
        resource_state(record).* = .{};
        std.debug.assert(
            c.venus_objects_release(
                &objects,
                record.handle,
                c.VK_OBJECT_TYPE_COMMAND_BUFFER,
                1,
            ) == c.RingOk,
        );
    }
}
/// Begin primary or bounded transfer/compute secondary recording.
/// @param[in] buffer Nonnull live private buffer, borrowed for call.
/// @param[in] info Nonnull canonical tag42/no pNext and flags confined to1|4.
/// Secondary inheritance tag41 is borrowed; render/query inheritance initially unsupported.
/// @return Host result, initialization error or sticky device loss.
/// @note Mutex serialized, allocation-free; primary inheritance ignored without dereference.
fn begin_command_buffer(
    buffer: c.VkCommandBuffer,
    info: [*c]const c.VkCommandBufferBeginInfo,
) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (buffer == null or info == null or
        info.*.sType != c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO or info.*.pNext != null or
        info.*.flags & ~@as(u32, 5) != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const pool = command_pool_for(record) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const state = resource_state(record);
    if (state.command_state == .Recording or state.command_state == .Pending)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    if (state.command_state != .Initial and resource_state(pool).pool_flags & 2 == 0)
        return c.VK_ERROR_INITIALIZATION_FAILED;
    const secondary = state.command_level == 1;
    if (!secondary and info.*.flags == 5) return c.VK_ERROR_INITIALIZATION_FAILED;
    if (secondary) {
        const inheritance = info.*.pInheritanceInfo;
        if (inheritance == null or
            inheritance.*.sType != c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO or
            inheritance.*.pNext != null or inheritance.*.renderPass != null or
            inheritance.*.framebuffer != null or inheritance.*.subpass != 0 or
            inheritance.*.occlusionQueryEnable != 0 or inheritance.*.queryFlags != 0 or
            inheritance.*.pipelineStatistics != 0) return c.VK_ERROR_INITIALIZATION_FAILED;
    }
    var writer = writer_t{};
    writer.header(90, record.id);
    writer.put(u64, 1);
    writer.put(u32, c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    writer.put(u64, 0);
    writer.put(u32, info.*.flags);
    writer.put(u64, if (secondary) 1 else 0);
    if (secondary) {
        writer.put(u32, c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO);
        writer.put(u64, 0);
        writer.put(u64, 0);
        writer.put(u32, 0);
        writer.put(u64, 0);
        writer.put(u32, 0);
        writer.put(u32, 0);
        writer.put(u32, 0);
    }
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 90, 0);
    if (result == c.VK_SUCCESS) {
        state.command_state = .Recording;
        state.command_flags = info.*.flags;
        state.buffer_references = [_]u64{0} ** 8;
    } else if (lost == c.RingOk) state.command_state = .Invalid;
    return result;
}
/// Finish a live recording; host success enters Executable, native error Invalid.
/// @param[in] buffer Nonnull private borrowed handle, never dereferenced as native pointer.
/// @return Host result, local initialization error or sticky device loss.
/// @note Mutex serialized, no allocation or ownership transfer.
fn end_command_buffer(buffer: c.VkCommandBuffer) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (buffer == null) return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const state = resource_state(record);
    if (state.command_state != .Recording) return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = writer_t{};
    writer.header(91, record.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 91, 0);
    if (lost == c.RingOk)
        state.command_state = if (result == c.VK_SUCCESS) .Executable else .Invalid;
    return result;
}
/// Record a bounded fill of a private device buffer; CPU acknowledgment only.
/// @param[in] command_buffer Nullable private borrowed handle; invalid states ignored.
/// @param[in] buffer Nullable same-device bound TRANSFER_DST token, no ownership transfer.
/// @param[in] offset Four-byte aligned offset below requested buffer size.
/// @param[in] size Positive four-byte multiple within bounds, or UINT64_MAX WHOLE_SIZE.
/// @param[in] data Repeated native word, serialized under the little-endian contract.
/// @return Void; local invalid Recording inputs invalidate recording, loss poisons binding.
/// @note Mutex serialized, allocation-free; caller satisfies native queue capabilities.
fn fill_buffer(
    command_buffer: c.VkCommandBuffer,
    buffer: c.VkBuffer,
    offset: u64,
    size: u64,
    data: u32,
) callconv(.C) void {
    mutex.lock();
    defer mutex.unlock();
    if (lost != c.RingOk or command_buffer == null) return;
    const record = object(
        @intFromPtr(command_buffer.?),
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
    ) orelse return;
    const state = resource_state(record);
    if (state.command_state != .Recording) return;
    const pool = command_pool_for(record) orelse return;
    const target = if (buffer != null) child_object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_BUFFER,
        pool.parent_id,
    ) else null;
    if (target == null) {
        state.command_state = .Invalid;
        return;
    }
    const destination = resource_state(target.?);
    if (destination.bound_memory == 0 or destination.buffer_usage & 2 == 0 or
        offset % 4 != 0 or offset >= destination.buffer_size or
        (size != std.math.maxInt(u64) and (size == 0 or size % 4 != 0 or
        size > destination.buffer_size - offset)))
    {
        state.command_state = .Invalid;
        return;
    }
    var writer = writer_t{};
    writer.header(118, record.id);
    writer.put(u64, target.?.id);
    writer.put(u64, offset);
    writer.put(u64, size);
    writer.put(u32, data);
    const reply = transact(writer.bytes[0..writer.used]) orelse return;
    var reader = reader_t{ .bytes = reply };
    const received = reader.scalar(u32) catch {
        _ = failure(c.RingCorrupt);
        return;
    };
    if (received != 118) {
        _ = failure(c.RingCorrupt);
        return;
    }
    const index = resource_index(target.?);
    state.buffer_references[index / 64] |= @as(u64, 1) << @as(u6, @intCast(index % 64));
}
/// Reset an individual nonpending buffer from a reset-capable private pool.
/// @param[in] buffer Nonnull private borrowed handle; no ownership transfer.
/// @param[in] flags0/1 release-resources only.
/// @return Host result, initialization error or sticky device loss.
/// @note Mutex serialized, allocation-free; changes state only after exact host success.
fn reset_command_buffer(buffer: c.VkCommandBuffer, flags: u32) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    if (buffer == null or flags > 1) return c.VK_ERROR_INITIALIZATION_FAILED;
    const record = object(
        @intFromPtr(buffer.?),
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
    ) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    const pool = command_pool_for(record) orelse return c.VK_ERROR_INITIALIZATION_FAILED;
    if (resource_state(pool).pool_flags & 2 == 0 or
        resource_state(record).command_state == .Pending) return c.VK_ERROR_INITIALIZATION_FAILED;
    var writer = writer_t{};
    writer.header(92, record.id);
    writer.put(u32, flags);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    const result = result_reply(reply, 92, 0);
    if (result == c.VK_SUCCESS) {
        resource_state(record).command_state = .Initial;
        resource_state(record).command_flags = 0;
        resource_state(record).buffer_references = [_]u64{0} ** 8;
    }
    return result;
}
fn encode_fences(command_id: u32, device: c.VkDevice, fences: []const c.VkFence) ?writer_t {
    if (device == null or fences.len == 0 or fences.len > 64) return null;
    const parent = object(@intFromPtr(device.?), c.VK_OBJECT_TYPE_DEVICE) orelse return null;
    var writer = writer_t{};
    writer.header(command_id, parent.id);
    writer.put(u32, @intCast(fences.len));
    writer.put(u64, fences.len);
    for (fences) |fence| {
        if (fence == null) return null;
        const record = child_object(
            @intFromPtr(fence.?),
            c.VK_OBJECT_TYPE_FENCE,
            parent.id,
        ) orelse return null;
        writer.put(u64, record.id);
    }
    return writer;
}
/// Reset1..64 device-owned fences after the caller retires their GPU work.
/// @param[in] device Nonnull live private parent.
/// @param[in] count Accessible native input count1..64; validated before slicing.
/// @param[in] fences Nonnull borrowed immutable handles[count], retained only for call.
/// @return Host result, local device error or sticky transport/peer device loss.
/// @note Mutex serialized, allocation-free; never resets a foreign device's object.
fn reset_fences(device: c.VkDevice, count: u32, fences: [*c]const c.VkFence) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (count == 0 or count > 64 or fences == null) return c.VK_ERROR_DEVICE_LOST;
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const writer = encode_fences(37, device, fences[0..count]) orelse return c.VK_ERROR_DEVICE_LOST;
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    return result_reply(reply, 37, 0);
}
/// Poll one native fence without blocking host execution.
/// @param[in] device Nonnull live private parent, not dereferenced as native pointer.
/// @param[in] fence Nonnull device-owned token; borrowed until call ends.
/// @return VK_SUCCESS, VK_NOT_READY, negative host result or sticky device loss.
/// @note Mutex serialized, no allocation; CPU completion does not imply signaled GPU fence.
fn get_fence_status(device: c.VkDevice, fence: c.VkFence) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (device == null or fence == null or lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const parent = object(
        @intFromPtr(device.?),
        c.VK_OBJECT_TYPE_DEVICE,
    ) orelse return c.VK_ERROR_DEVICE_LOST;
    const record = child_object(
        @intFromPtr(fence.?),
        c.VK_OBJECT_TYPE_FENCE,
        parent.id,
    ) orelse return c.VK_ERROR_DEVICE_LOST;
    var writer = writer_t{};
    writer.header(38, parent.id);
    writer.put(u64, record.id);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    return result_reply(reply, 38, c.VK_NOT_READY);
}
fn wait_round(device: c.VkDevice, fences: []const c.VkFence, all: u32) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    var writer = encode_fences(39, device, fences) orelse return c.VK_ERROR_DEVICE_LOST;
    writer.put(u32, all);
    writer.put(u64, 0);
    const reply = transact(writer.bytes[0..writer.used]) orelse return c.VK_ERROR_DEVICE_LOST;
    return result_reply(reply, 39, c.VK_TIMEOUT);
}
/// Wait with nonblocking host rounds and the caller's monotonic timeout.
/// @param[in] device Nonnull live parent, must remain live throughout call.
/// @param[in] count Input count1..64.
/// @param[in] fences Nonnull borrowed immutable handles[count]; snapshot privately, no retention.
/// @param[in] all Canonical0/1 any/all selection; waited fences must remain live.
/// @param[in] timeout Nanoseconds, zero still polls once; UINT64_MAX effectively indefinite.
/// @return VK_SUCCESS, VK_TIMEOUT or explicit negative host/local/device error.
/// @note No allocation. Mutex released between rounds so other threads can submit signaling work.
fn wait_fences(
    device: c.VkDevice,
    count: u32,
    fences: [*c]const c.VkFence,
    all: u32,
    timeout: u64,
) callconv(.C) c_int {
    if (count == 0 or count > 64 or fences == null or all > 1) return c.VK_ERROR_DEVICE_LOST;
    var snapshot: [64]c.VkFence = undefined;
    @memcpy(snapshot[0..count], fences[0..count]);
    var timer = std.time.Timer.start() catch {
        mutex.lock();
        defer mutex.unlock();
        return failure(c.RingInvalid);
    };
    while (true) {
        const result = wait_round(device, snapshot[0..count], all);
        if (result != c.VK_TIMEOUT) return result;
        const elapsed = timer.read();
        if (elapsed >= timeout) return c.VK_TIMEOUT;
        std.time.sleep(@min(std.time.ns_per_ms, timeout - elapsed));
    }
}
const IdleDeadlineNs: u64 = std.time.ns_per_s;
var gpu_fences = [_]u64{0} ** 64;
fn gpu_exchange(kind: u32, ring: u32, fence: u64, response: *c.venus_request_t) c_int {
    var request = std.mem.zeroes(c.venus_request_t);
    request.kind = kind;
    request.argument_zero = ring;
    request.argument_one = fence;
    response.* = std.mem.zeroes(c.venus_request_t);
    const status = command.exchange.?(command.context, &request, null, 0, response, null, 0);
    if (status != c.RingOk) return status;
    if (response.kind != kind or response.direction != 1 or response.status != 0 or
        response.resource_id != 0 or response.flags != 0 or response.payload_bytes != 0 or
        response.argument_one != 0 or (kind == c.RequestGpuPoll and response.argument_zero != 0))
        return c.RingCorrupt;
    return c.RingOk;
}
fn ring_idle(ring: u32, timer: *std.time.Timer) c_int {
    var fence: u64 = 0;
    while (true) {
        if (timer.read() >= IdleDeadlineNs) return failure(c.RingTimeout);
        var response: c.venus_request_t = undefined;
        const kind: u32 = if (fence == 0) c.RequestGpuFence else c.RequestGpuPoll;
        const status = gpu_exchange(kind, ring, fence, &response);
        if (timer.read() >= IdleDeadlineNs) return failure(c.RingTimeout);
        if (status == c.RingOk) {
            if (fence != 0) return c.VK_SUCCESS;
            if (response.argument_zero <= gpu_fences[ring]) return failure(c.RingCorrupt);
            fence = response.argument_zero;
            gpu_fences[ring] = fence;
        } else if (status != c.RingAgain) {
            return failure(status);
        }
        if (status == c.RingAgain) std.time.sleep(std.time.ns_per_ms);
    }
}
/// Wait for actual retirement of every initialized queue in the borrowed device.
/// @param[in] device Nullable private handle, validated without dereference.
/// @return VK_SUCCESS or sticky VK_ERROR_DEVICE_LOST on invalid/clock/deadline/peer errors.
/// @note Mutex serialized; no allocation; pending GPU loss requires receiver retirement/abandon.
fn device_wait_idle(device: c.VkDevice) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (device == null or lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const entry = device_cache(@intFromPtr(device.?)) orelse return c.VK_ERROR_DEVICE_LOST;
    var timer = std.time.Timer.start() catch return failure(c.RingInvalid);
    for (entry.rings) |ring| if (ring != 0) {
        const result = ring_idle(ring, &timer);
        if (result != c.VK_SUCCESS) return result;
    };
    return c.VK_SUCCESS;
}
/// Wait for actual GPU retirement of the queue using its receiver timeline.
/// @param[in] queue Nullable private handle, validated without dereference.
/// @return VK_SUCCESS or VK_ERROR_DEVICE_LOST; same deadline/lifetime contract as device idle.
/// @note No allocation; binding mutex held across issue/poll. CPU completion is separate.
fn queue_wait_idle(queue: c.VkQueue) callconv(.C) c_int {
    mutex.lock();
    defer mutex.unlock();
    if (queue == null or lost != c.RingOk) return c.VK_ERROR_DEVICE_LOST;
    const record = object(
        @intFromPtr(queue.?),
        c.VK_OBJECT_TYPE_QUEUE,
    ) orelse return c.VK_ERROR_DEVICE_LOST;
    for (&device_caches) |*entry| {
        if (entry.handle == 0) continue;
        for (entry.queues, 0..) |handle, index| if (handle == record.handle) {
            var timer = std.time.Timer.start() catch return failure(c.RingInvalid);
            return ring_idle(entry.rings[index], &timer);
        };
    }
    return failure(c.RingCorrupt);
}
fn device_proc(name: []const u8) c.PFN_vkVoidFunction {
    const Entries = .{
        .{ "vkGetDeviceProcAddr", &get_device_proc },
        .{ "vkDestroyDevice", &destroy_device },
        .{ "vkGetDeviceQueue", &get_device_queue },
        .{ "vkDeviceWaitIdle", &device_wait_idle },
        .{ "vkQueueWaitIdle", &queue_wait_idle },
        .{ "vkCreateFence", &create_fence },
        .{ "vkCreateBuffer", &create_buffer },
        .{ "vkCreateCommandPool", &create_command_pool },
        .{ "vkAllocateCommandBuffers", &allocate_command_buffers },
        .{ "vkFreeCommandBuffers", &free_command_buffers },
        .{ "vkBeginCommandBuffer", &begin_command_buffer },
        .{ "vkCmdFillBuffer", &fill_buffer },
        .{ "vkEndCommandBuffer", &end_command_buffer },
        .{ "vkResetCommandBuffer", &reset_command_buffer },
        .{ "vkDestroyCommandPool", &destroy_command_pool },
        .{ "vkResetCommandPool", &reset_command_pool },
        .{ "vkAllocateMemory", &allocate_memory },
        .{ "vkFreeMemory", &free_memory },
        .{ "vkBindBufferMemory", &bind_buffer_memory },
        .{ "vkDestroyBuffer", &destroy_buffer },
        .{ "vkGetBufferMemoryRequirements", &buffer_requirements },
        .{ "vkDestroyFence", &destroy_fence },
        .{ "vkResetFences", &reset_fences },
        .{ "vkGetFenceStatus", &get_fence_status },
        .{ "vkWaitForFences", &wait_fences },
    };
    inline for (Entries) |entry| if (std.mem.eql(u8, name, entry[0])) return @ptrCast(entry[1]);
    return null;
}
/// Resolve implemented device functions for a validated live device; no native pointer dereference.
/// Mutex serialized, static borrowed function pointers remain accessible for process lifetime.
/// @param[in] device Nullable handle; must validate as live to resolve any procedure.
/// @param[in] name Nullable accessible NUL-terminated native bytes, at most256 bytes.
/// @return Borrowed static function pointer or NULL for unsupported/invalid inputs.
fn get_device_proc(device: c.VkDevice, name: [*c]const u8) callconv(.C) c.PFN_vkVoidFunction {
    const valid_name = bounded_name(name) orelse return null;
    mutex.lock();
    defer mutex.unlock();
    if (device == null or device_cache(@intFromPtr(device.?)) == null) return null;
    return device_proc(valid_name);
}
fn bounded_name(name: [*c]const u8) ?[]const u8 {
    if (name == null) return null;
    for (0..256) |index| if (name[index] == 0) return name[0..index];
    return null;
}
fn physical_proc(name: []const u8) c.PFN_vkVoidFunction {
    const Entries = .{
        .{ "vkGetPhysicalDeviceProperties", &properties },
        .{ "vkGetPhysicalDeviceFeatures", &features },
        .{ "vkGetPhysicalDeviceMemoryProperties", &memory },
        .{ "vkGetPhysicalDeviceFormatProperties", &format_properties },
        .{ "vkGetPhysicalDeviceImageFormatProperties", &image_properties },
        .{ "vkGetPhysicalDeviceQueueFamilyProperties", &queue_properties },
        .{ "vkGetPhysicalDeviceSparseImageFormatProperties", &sparse_properties },
        .{ "vkEnumerateDeviceExtensionProperties", &device_extensions },
        .{ "vkCreateDevice", &create_device },
    };
    inline for (Entries) |entry| if (std.mem.eql(u8, name, entry[0])) return @ptrCast(entry[1]);
    return null;
}
/// Exact global/instance lookup; header defines names, lifetimes and no handle dereference.
export fn venus_icd_get_instance_proc_addr(
    instance: c.VkInstance,
    optional_name: [*c]const u8,
) c.PFN_vkVoidFunction {
    const name = bounded_name(optional_name) orelse return null;
    mutex.lock();
    defer mutex.unlock();
    if (instance != null and cache(@intFromPtr(instance.?)) == null) return null;
    const Globals = .{
        .{ "vkGetInstanceProcAddr", &venus_icd_get_instance_proc_addr },
        .{ "vkCreateInstance", &create_instance },
        .{ "vkEnumerateInstanceExtensionProperties", &enumerate_extensions },
        .{ "vkEnumerateInstanceVersion", &enumerate_version },
    };
    inline for (Globals) |entry| if (std.mem.eql(u8, name, entry[0])) return @ptrCast(entry[1]);
    if (instance == null) return null;
    const Entries = .{
        .{ "vkDestroyInstance", &destroy_instance },
        .{ "vkEnumeratePhysicalDevices", &enumerate_physical },
        .{ "vkGetDeviceProcAddr", &get_device_proc },
    };
    inline for (Entries) |entry| if (std.mem.eql(u8, name, entry[0])) return @ptrCast(entry[1]);
    return physical_proc(name) orelse device_proc(name);
}
/// Physical query lookup; header defines validated instance and supported procedure scope.
export fn venus_icd_get_physical_proc_addr(
    instance: c.VkInstance,
    optional_name: [*c]const u8,
) c.PFN_vkVoidFunction {
    const name = bounded_name(optional_name) orelse return null;
    mutex.lock();
    defer mutex.unlock();
    if (instance == null or cache(@intFromPtr(instance.?)) == null) return null;
    return physical_proc(name);
}
comptime {
    @export(venus_icd_negotiate_loader, .{ .name = "vk_icdNegotiateLoaderICDInterfaceVersion" });
    @export(venus_icd_get_instance_proc_addr, .{ .name = "vk_icdGetInstanceProcAddr" });
    @export(venus_icd_get_physical_proc_addr, .{ .name = "vk_icdGetPhysicalDeviceProcAddr" });
}

// Test-only fixtures.
extern fn venus_icd_native_fixture() c_int;
test "native ABI lifecycle churn routing and concurrent transport serialization" {
    try std.testing.expectEqual(@as(c_int, 0), venus_icd_native_fixture());
}
test "bounded fixed and array replies reject every truncation and invalid tags" {
    var bytes: [64]u8 = undefined;
    @memset(&bytes, 0);
    std.mem.writeInt(u32, bytes[0..4], 4, .little);
    std.mem.writeInt(u64, bytes[4..12], 1, .little);
    for (0..24) |length| {
        var reader = reader_t{ .bytes = bytes[0..length] };
        try std.testing.expectError(error.Bounds, fixed_value(c.VkFormatProperties, &reader, 4));
    }
    for ([_]usize{ 0, 4 }) |offset| {
        bytes[offset] = 9;
        var reader = reader_t{ .bytes = &bytes };
        try std.testing.expectError(error.Value, fixed_value(c.VkFormatProperties, &reader, 4));
        bytes[offset] = if (offset == 0) 4 else 1;
    }
    @memset(&bytes, 0);
    std.mem.writeInt(u32, bytes[0..4], 7, .little);
    std.mem.writeInt(u64, bytes[4..12], 1, .little);
    std.mem.writeInt(u32, bytes[12..16], 1, .little);
    std.mem.writeInt(u64, bytes[16..24], 1, .little);
    var count: u32 = 99;
    var queue: c.VkQueueFamilyProperties = std.mem.zeroes(c.VkQueueFamilyProperties);
    for (0..48) |length| {
        var reader = reader_t{ .bytes = bytes[0..length] };
        try std.testing.expectError(
            error.Bounds,
            array_values(c.VkQueueFamilyProperties, &reader, 7, 1, true, &count, &queue),
        );
        try std.testing.expectEqual(@as(u32, 99), count);
    }
    for ([_]usize{ 0, 4, 12, 16 }) |offset| {
        const before = bytes[offset];
        bytes[offset] = 99;
        var reader = reader_t{ .bytes = &bytes };
        try std.testing.expectError(
            error.Value,
            array_values(c.VkQueueFamilyProperties, &reader, 7, 1, true, &count, &queue),
        );
        bytes[offset] = before;
    }
    var reader = reader_t{ .bytes = &bytes };
    try std.testing.expectError(
        error.Value,
        array_values(c.VkQueueFamilyProperties, &reader, 7, 0, true, &count, &queue),
    );
    @memset(&bytes, 0);
    std.mem.writeInt(u32, bytes[0..4], 5, .little);
    std.mem.writeInt(u64, bytes[8..16], 1, .little);
    var image: c.VkImageFormatProperties = std.mem.zeroes(c.VkImageFormatProperties);
    for (0..48) |length| {
        reader = .{ .bytes = bytes[0..length] };
        try std.testing.expectError(error.Bounds, image_value(&reader, &image));
    }
    bytes[0] = 4;
    reader = .{ .bytes = &bytes };
    try std.testing.expectError(error.Value, image_value(&reader, &image));
    bytes[0] = 5;
    bytes[4] = 1;
    reader = .{ .bytes = &bytes };
    try std.testing.expectError(error.Value, image_value(&reader, &image));
    bytes[4] = 0;
    bytes[8] = 0;
    reader = .{ .bytes = &bytes };
    try std.testing.expectError(error.Value, image_value(&reader, &image));
    bytes[8] = 1;
    std.mem.writeInt(i32, bytes[4..8], -11, .little);
    reader = .{ .bytes = &bytes };
    try std.testing.expectEqual(@as(i32, -11), try image_value(&reader, &image));
}
test "bounded device input validation and identity reply truncations" {
    var priorities = [_]f32{ 0.25, 0.75 } ** 8;
    var queues = [_]c.VkDeviceQueueCreateInfo{.{
        .sType = c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext = null,
        .flags = 0,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priorities,
    }} ** 16;
    var feature = std.mem.zeroes(c.VkPhysicalDeviceFeatures);
    var info = c.VkDeviceCreateInfo{
        .sType = c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = null,
        .flags = 0,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queues,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = null,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = null,
        .pEnabledFeatures = &feature,
    };
    _ = try encode_device(&info, 1, 2);
    feature.robustBufferAccess = 2;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    feature.robustBufferAccess = 1;
    info.enabledLayerCount = 1;
    try std.testing.expectError(error.Layer, encode_device(&info, 1, 2));
    info.enabledLayerCount = 0;
    info.enabledExtensionCount = 1;
    try std.testing.expectError(error.Extension, encode_device(&info, 1, 2));
    info.enabledExtensionCount = 0;
    var link = c.VkBaseInStructure{
        .sType = c.VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO,
        .pNext = null,
    };
    info.pNext = &link;
    _ = try encode_device(&info, 1, 2);
    link.pNext = &link;
    try std.testing.expectError(error.Extension, encode_device(&info, 1, 2));
    link.pNext = null;
    link.sType = c.VK_STRUCTURE_TYPE_APPLICATION_INFO;
    try std.testing.expectError(error.Extension, encode_device(&info, 1, 2));
    info.pNext = null;
    info.queueCreateInfoCount = 2;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    for (&queues, 0..) |*queue, index| queue.queueFamilyIndex = @intCast(index);
    info.queueCreateInfoCount = 16;
    for (&queues) |*queue| queue.queueCount = 16;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    for (&queues) |*queue| queue.queueCount = 4;
    _ = try encode_device(&info, 1, 2);
    info.queueCreateInfoCount = 1;
    for ([_]f32{ -1, 2, std.math.inf(f32), std.math.nan(f32) }) |priority| {
        priorities[0] = priority;
        try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    }
    priorities[0] = 0.5;
    queues[0].pQueuePriorities = null;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    queues[0].pQueuePriorities = &priorities;
    queues[0].queueCount = 0;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    queues[0].queueCount = 17;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    queues[0].queueCount = 1;
    queues[0].flags = 1;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    queues[0].flags = 0;
    queues[0].pNext = &link;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    queues[0].pNext = null;
    queues[0].sType = 0;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    queues[0].sType = c.VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    info.sType = 0;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    info.sType = c.VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.flags = 1;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    info.flags = 0;
    info.queueCreateInfoCount = 0;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    info.queueCreateInfoCount = 17;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = null;
    try std.testing.expectError(error.Invalid, encode_device(&info, 1, 2));
    var bytes: [24]u8 = undefined;
    std.mem.writeInt(u32, bytes[0..4], 11, .little);
    std.mem.writeInt(i32, bytes[4..8], 0, .little);
    std.mem.writeInt(u64, bytes[8..16], 1, .little);
    std.mem.writeInt(u64, bytes[16..24], 2, .little);
    try std.testing.expectEqual(@as(i32, 0), try identity_reply(&bytes, 11, 2, true));
    for (0..24) |length| {
        if (identity_reply(bytes[0..length], 11, 2, true)) |_| {
            return error.AcceptedTruncation;
        } else |_| {}
    }
    for ([_]usize{ 0, 8, 16 }) |offset| {
        bytes[offset] ^= 1;
        try std.testing.expectError(error.Value, identity_reply(&bytes, 11, 2, true));
        bytes[offset] ^= 1;
    }
    std.mem.writeInt(u32, bytes[0..4], 155, .little);
    std.mem.writeInt(u64, bytes[4..12], 1, .little);
    std.mem.writeInt(u64, bytes[12..20], 2, .little);
    _ = try identity_reply(bytes[0..20], 155, 2, false);
    for (0..20) |length| {
        if (identity_reply(bytes[0..length], 155, 2, false)) |_| {
            return error.AcceptedTruncation;
        } else |_| {}
    }
}
test "fence result replies reject truncation malformed tags and unexpected positive statuses" {
    defer lost = c.RingOk;
    var bytes: [8]u8 = undefined;
    std.mem.writeInt(u32, bytes[0..4], 38, .little);
    std.mem.writeInt(i32, bytes[4..8], 0, .little);
    for (0..8) |length| {
        try std.testing.expectEqual(
            @as(c_int, c.VK_ERROR_DEVICE_LOST),
            @call(.never_inline, result_reply, .{ bytes[0..length], @as(u32, 38), @as(i32, 1) }),
        );
        try std.testing.expectEqual(@as(c_int, c.RingCorrupt), lost);
        lost = c.RingOk;
    }
    try std.testing.expectEqual(
        @as(c_int, 0),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
    std.mem.writeInt(i32, bytes[4..8], c.VK_NOT_READY, .little);
    try std.testing.expectEqual(
        @as(c_int, c.VK_NOT_READY),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
    std.mem.writeInt(i32, bytes[4..8], c.VK_ERROR_OUT_OF_HOST_MEMORY, .little);
    try std.testing.expectEqual(
        @as(c_int, c.VK_ERROR_OUT_OF_HOST_MEMORY),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
    std.mem.writeInt(i32, bytes[4..8], c.VK_ERROR_DEVICE_LOST, .little);
    try std.testing.expectEqual(
        @as(c_int, c.VK_ERROR_DEVICE_LOST),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
    try std.testing.expectEqual(@as(c_int, c.RingClosed), lost);
    lost = c.RingOk;
    std.mem.writeInt(i32, bytes[4..8], c.VK_INCOMPLETE, .little);
    try std.testing.expectEqual(
        @as(c_int, c.VK_ERROR_DEVICE_LOST),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
    std.mem.writeInt(i32, bytes[4..8], 0, .little);
    std.mem.writeInt(u32, bytes[0..4], 39, .little);
    try std.testing.expectEqual(
        @as(c_int, c.VK_ERROR_DEVICE_LOST),
        @call(.never_inline, result_reply, .{ &bytes, @as(u32, 38), @as(i32, 1) }),
    );
}

test "command buffer batch replies validate every truncation result count and identity" {
    const Ids = [_]u64{ 7, 9 };
    var writer = writer_t{};
    writer.put(u32, 88);
    writer.put(i32, 0);
    writer.put(u64, Ids.len);
    for (Ids) |id| writer.put(u64, id);
    for (0..writer.used) |length| {
        try std.testing.expectError(
            error.Bounds,
            @call(
                .never_inline,
                command_buffers_reply,
                .{ writer.bytes[0..length], &Ids },
            ),
        );
    }
    try std.testing.expectEqual(
        @as(c_int, c.VK_SUCCESS),
        try command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    writer.bytes[0] ^= 1;
    try std.testing.expectError(
        error.Value,
        command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    writer.bytes[0] ^= 1;
    std.mem.writeInt(u64, writer.bytes[8..16], 3, .little);
    try std.testing.expectError(
        error.Value,
        command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    std.mem.writeInt(u64, writer.bytes[8..16], Ids.len, .little);
    std.mem.writeInt(i32, writer.bytes[4..8], c.VK_NOT_READY, .little);
    try std.testing.expectError(
        error.Value,
        command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    std.mem.writeInt(i32, writer.bytes[4..8], 0, .little);
    std.mem.writeInt(u64, writer.bytes[24..32], 10, .little);
    try std.testing.expectError(
        error.Value,
        command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    std.mem.writeInt(u64, writer.bytes[16..24], 0, .little);
    std.mem.writeInt(u64, writer.bytes[24..32], 0, .little);
    try std.testing.expectError(
        error.Value,
        command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
    std.mem.writeInt(i32, writer.bytes[4..8], c.VK_ERROR_OUT_OF_HOST_MEMORY, .little);
    try std.testing.expectEqual(
        @as(
            c_int,
            c.VK_ERROR_OUT_OF_HOST_MEMORY,
        ),
        try command_buffers_reply(writer.bytes[0..writer.used], &Ids),
    );
}

test "pending buffer references prevent host destruction before GPU retirement" {
    const fixture_t = struct {
        fn exchange(
            _: ?*anyopaque,
            _: [*c]const c.venus_request_t,
            _: ?*const anyopaque,
            _: usize,
            _: [*c]c.venus_request_t,
            _: ?*anyopaque,
            _: usize,
        ) callconv(.C) c_int {
            return c.RingInvalid;
        }
    };
    var sentinel: u8 = 0;
    try std.testing.expectEqual(
        @as(c_int, c.RingOk),
        venus_icd_bind(fixture_t.exchange, &sentinel),
    );
    defer venus_icd_abandon();
    var device: [*c]c.venus_object_t = null;
    var buffer: [*c]c.venus_object_t = null;
    var recording: [*c]c.venus_object_t = null;
    var pool: [*c]c.venus_object_t = null;
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_DEVICE,
        0,
        1,
        &device,
    ));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_BUFFER,
        device.*.id,
        0,
        &buffer,
    ));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_COMMAND_POOL,
        device.*.id,
        0,
        &pool,
    ));
    try std.testing.expectEqual(@as(c_int, c.RingOk), c.venus_objects_reserve(
        &objects,
        c.VK_OBJECT_TYPE_COMMAND_BUFFER,
        pool.*.id,
        1,
        &recording,
    ));
    const index = resource_index(buffer);
    resource_state(recording).command_state = .Pending;
    resource_state(recording).buffer_references[index / 64] =
        @as(u64, 1) << @as(u6, @intCast(index % 64));
    destroy_buffer(@ptrFromInt(device.*.handle), @ptrFromInt(buffer.*.handle), null);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
    destroy_command_pool(@ptrFromInt(device.*.handle), @ptrFromInt(pool.*.handle), null);
    try std.testing.expectEqual(@as(c_int, c.RingOk), lost);
    try std.testing.expectEqual(@as(usize, 4), objects.live_count);
    try std.testing.expectEqual(command_state_t.Pending, resource_state(recording).command_state);
}
