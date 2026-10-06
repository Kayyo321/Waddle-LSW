//! Experimental bounded Vulkan instance dispatch; device/DXVK support is separately gated.
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
/// Device creation fails explicitly while device APIs are pending; output NULL, no transport.
fn create_device(
    physical: c.VkPhysicalDevice,
    info: [*c]const c.VkDeviceCreateInfo,
    allocator: [*c]const c.VkAllocationCallbacks,
    output: [*c]c.VkDevice,
) callconv(.C) c_int {
    _ = physical;
    _ = info;
    _ = allocator;
    if (output != null) output.* = null;
    return c.VK_ERROR_FEATURE_NOT_PRESENT;
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
    if (c.venus_values_properties_decode(output, reply.ptr, reply.len) != c.RingOk) {
        _ = failure(c.RingCorrupt);
    }
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
/// Device functions remain unsupported until separately implemented; no ownership/state.
fn get_device_proc(device: c.VkDevice, name: [*c]const u8) callconv(.C) c.PFN_vkVoidFunction {
    _ = device;
    _ = name;
    return null;
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
    return physical_proc(name);
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
