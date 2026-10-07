//! Bounded modern synchronization wire packets. Caller resolves every native handle
//! to a live host identity and owns all input arrays through the synchronous call.
const std = @import("std");
const render = @import("venus_render_wire.zig");
const c = @cImport({ @cInclude("vulkan/vulkan.h"); });
/// Owned8192-byte packet. No pointers retained; disjoint calls are thread safe.
pub const writer_t = render.writer_t;
/// Maximum records per borrowed native array, checked before dereference.
pub const MaxRecords = 64;
fn handle_bits(handle: anytype) u64 {
    return @intFromPtr(handle);
}
fn node(writer: *writer_t, tag: u32) !void {
    try writer.put(u32, tag);
    try writer.put(u64, 0);
}
fn array(comptime value_t: type, pointer: [*c]const value_t, length: u32) ![]const value_t {
    if (length > MaxRecords) return error.Limit;
    if (length == 0) return &.{};
    if (pointer == null or @intFromPtr(pointer) % @alignOf(value_t) != 0) return error.Invalid;
    return pointer[0..length];
}
fn count(writer: *writer_t, length: usize) !void {
    try writer.put(u32, @intCast(length));
    try writer.put(u64, length);
}
fn scopes(writer: *writer_t, barrier: anytype) !void {
    try writer.put(u64, barrier.srcStageMask);
    try writer.put(u64, barrier.srcAccessMask);
    try writer.put(u64, barrier.dstStageMask);
    try writer.put(u64, barrier.dstAccessMask);
}
/// [in] command_id nonzero resolved host ID; info borrowed canonical native
/// dependency with resolved buffer/image handles, null pNext and at most64 each.
/// [out] Owned exact command204 packet; Invalid malformed topology/zero identity,
/// Limit count or packet capacity. No allocation/retention; caller validates
/// recording state, enabled sync2, queue ownership, resource ranges/layouts.
pub fn pipeline_barrier2(command_id: u64, info: *const c.VkDependencyInfo) !writer_t {
    if (command_id == 0 or info.sType != c.VK_STRUCTURE_TYPE_DEPENDENCY_INFO or info.pNext != null) return error.Invalid;
    const memories = try array(c.VkMemoryBarrier2, info.pMemoryBarriers, info.memoryBarrierCount);
    const buffers = try array(c.VkBufferMemoryBarrier2, info.pBufferMemoryBarriers, info.bufferMemoryBarrierCount);
    const images = try array(c.VkImageMemoryBarrier2, info.pImageMemoryBarriers, info.imageMemoryBarrierCount);
    var writer: writer_t = .{};
    try writer.header(204, command_id);
    try writer.put(u64, 1);
    try node(&writer, c.VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    try writer.put(u32, info.dependencyFlags);
    try count(&writer, memories.len);
    for (memories) |value| {
        if (value.sType != c.VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 or value.pNext != null) return error.Invalid;
        try node(&writer, c.VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        try scopes(&writer, value);
    }
    try count(&writer, buffers.len);
    for (buffers) |value| {
        if (value.sType != c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 or value.pNext != null or value.buffer == null) return error.Invalid;
        try node(&writer, c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2);
        try scopes(&writer, value);
        try writer.put(u32, value.srcQueueFamilyIndex);
        try writer.put(u32, value.dstQueueFamilyIndex);
        try writer.put(u64, handle_bits(value.buffer));
        try writer.put(u64, value.offset);
        try writer.put(u64, value.size);
    }
    try count(&writer, images.len);
    for (images) |value| {
        if (value.sType != c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 or value.pNext != null or value.image == null) return error.Invalid;
        try node(&writer, c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
        try scopes(&writer, value);
        try writer.put(u32, value.oldLayout);
        try writer.put(u32, value.newLayout);
        try writer.put(u32, value.srcQueueFamilyIndex);
        try writer.put(u32, value.dstQueueFamilyIndex);
        try writer.put(u64, handle_bits(value.image));
        const range = value.subresourceRange;
        for ([_]u32{range.aspectMask, range.baseMipLevel, range.levelCount, range.baseArrayLayer, range.layerCount}) |word| try writer.put(u32, word);
    }
    return writer;
}
fn semaphore_submit(writer: *writer_t, values: []const c.VkSemaphoreSubmitInfo) !void {
    try count(writer, values.len);
    for (values) |value| {
        if (value.sType != c.VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO or value.pNext != null or value.semaphore == null or value.deviceIndex != 0) return error.Invalid;
        try node(writer, c.VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
        try writer.put(u64, handle_bits(value.semaphore));
        try writer.put(u64, value.value);
        try writer.put(u64, value.stageMask);
        try writer.put(u32, value.deviceIndex);
    }
}
/// [in] queue_id resolved live host queue; submits borrowed0..64 normalized
/// submit2 records with resolved commands/semaphores; fence_id zero or live host.
/// [out] Exact command206 owned packet. Invalid tags/chains/multidevice/zero
/// mandatory identities, Limit arrays/packet. Caller validates executable command
/// ownership and every semaphore type/value; no allocation or retained pointers.
pub fn queue_submit2(queue_id: u64, submits: []const c.VkSubmitInfo2, fence_id: u64) !writer_t {
    if (queue_id == 0) return error.Invalid;
    if (submits.len > MaxRecords) return error.Limit;
    var writer: writer_t = .{};
    try writer.header(206, queue_id);
    try count(&writer, submits.len);
    for (submits) |value| {
        if (value.sType != c.VK_STRUCTURE_TYPE_SUBMIT_INFO_2 or value.pNext != null or value.flags != 0) return error.Invalid;
        const waits = try array(c.VkSemaphoreSubmitInfo, value.pWaitSemaphoreInfos, value.waitSemaphoreInfoCount);
        const commands = try array(c.VkCommandBufferSubmitInfo, value.pCommandBufferInfos, value.commandBufferInfoCount);
        const signals = try array(c.VkSemaphoreSubmitInfo, value.pSignalSemaphoreInfos, value.signalSemaphoreInfoCount);
        try node(&writer, c.VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
        try writer.put(u32, value.flags);
        try semaphore_submit(&writer, waits);
        try count(&writer, commands.len);
        for (commands) |entry| {
            if (entry.sType != c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO or entry.pNext != null or entry.commandBuffer == null or entry.deviceMask != 1) return error.Invalid;
            try node(&writer, c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
            try writer.put(u64, handle_bits(entry.commandBuffer));
            try writer.put(u32, entry.deviceMask);
        }
        try semaphore_submit(&writer, signals);
    }
    try writer.put(u64, fence_id);
    return writer;
}
/// [in] device_id/semaphore_id resolved nonzero host identities. [out] Owned
/// counter-value command172 request. Invalid zero IDs; no allocation/retention.
/// Caller validates enabled timeline feature and semaphore ownership/type.
pub fn semaphore_counter(device_id: u64, semaphore_id: u64) !writer_t {
    if (semaphore_id == 0) return error.Invalid;
    var writer: writer_t = .{};
    try writer.header(172, device_id);
    try writer.put(u64, semaphore_id);
    try writer.put(u64, 1);
    return writer;
}
/// [in] device_id/semaphore_id resolved host owners; value exact timeline value.
/// [out] Owned command174 packet; Invalid zero IDs. Caller validates timeline
/// ownership/monotonic semantics. No allocation or retained memory; thread safe.
pub fn signal_semaphore(device_id: u64, semaphore_id: u64, value: u64) !writer_t {
    if (semaphore_id == 0) return error.Invalid;
    var writer: writer_t = .{};
    try writer.header(174, device_id);
    try writer.put(u64, 1);
    try node(&writer, c.VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO);
    try writer.put(u64, semaphore_id);
    try writer.put(u64, value);
    return writer;
}
/// [in] device_id nonzero; ids resolved timeline owners, values same extent1..64;
/// flags0 or WAIT_ANY, timeout exact nanoseconds including UINT64_MAX.
/// [out] Owned command173 request; Invalid IDs/shape/flags, Limit extent.
/// Caller bounds synchronous frontend waiting separately; no allocation/retention.
pub fn wait_semaphores(device_id: u64, ids: []const u64, values: []const u64, flags: u32, timeout: u64) !writer_t {
    if (ids.len == 0 or ids.len != values.len or flags & ~@as(u32, c.VK_SEMAPHORE_WAIT_ANY_BIT) != 0) return error.Invalid;
    if (ids.len > MaxRecords) return error.Limit;
    var writer: writer_t = .{};
    try writer.header(173, device_id);
    try writer.put(u64, 1);
    try node(&writer, c.VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
    try writer.put(u32, flags);
    try count(&writer, ids.len);
    for (ids) |id| {
        if (id == 0) return error.Invalid;
        try writer.put(u64, id);
    }
    try writer.put(u64, values.len);
    for (values) |value| try writer.put(u64, value);
    try writer.put(u64, timeout);
    return writer;
}
/// [in] device_id/buffer_id resolved nonzero host identities. [out] Exact command175
/// request; Invalid zero identity. Caller validates bound memory, ADDRESS usage
/// and enabled BDA feature. No allocation or retained memory; thread safe.
pub fn buffer_device_address(device_id: u64, buffer_id: u64) !writer_t {
    if (buffer_id == 0) return error.Invalid;
    var writer: writer_t = .{};
    try writer.header(175, device_id);
    try writer.put(u64, 1);
    try node(&writer, c.VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
    try writer.put(u64, buffer_id);
    return writer;
}
/// [in] device_id/query_id resolved host identities, first/count caller-validated
/// against actual pool capacity. [out] Owned command171, Invalid zero IDs or
/// arithmetic overflow. Host reset only after affected GPU queries retire.
/// Allocation-free, no ownership transfer or retained memory; thread safe.
pub fn reset_query_pool(device_id: u64, query_id: u64, first: u32, query_count: u32) !writer_t {
    if (query_id == 0 or query_count > std.math.maxInt(u32) - first) return error.Invalid;
    var writer: writer_t = .{};
    try writer.header(171, device_id);
    try writer.put(u64, query_id);
    try writer.put(u32, first);
    try writer.put(u32, query_count);
    return writer;
}
/// [in] reply borrowed completed bytes; opcode172 expects VkResult+present u64,
/// opcode175 expects direct address u64. [out] Exact owned value or Backend
/// negative native result, Corrupt shape/identity/unsupported positive result.
/// No allocation, retained pointer or output mutation; thread safe.
pub fn decode_value(reply: []const u8, opcode: u32) !u64 {
    if (reply.len < 12 or std.mem.readInt(u32, reply[0..4], .little) != opcode) return error.Corrupt;
    if (opcode == 175) return std.mem.readInt(u64, reply[4..12], .little);
    if (opcode != 172 or reply.len < 24) return error.Corrupt;
    const result = std.mem.readInt(i32, reply[4..8], .little);
    if (result < 0) return error.Backend;
    if (result != 0 or std.mem.readInt(u64, reply[8..16], .little) != 1) return error.Corrupt;
    return std.mem.readInt(u64, reply[16..24], .little);
}

// Test-only fixtures.
extern fn venus_modern_sync_test_encode(u32, ?*const anyopaque, u32, [*]u8) usize;
fn compare(writer: writer_t, opcode: u32, info: ?*const anyopaque, length: u32) !void {
    var bytes: [8192]u8 = undefined;
    const used = venus_modern_sync_test_encode(opcode, info, length, &bytes);
    try std.testing.expectEqual(used, writer.used);
    try std.testing.expectEqualSlices(u8, bytes[0..used], writer.bytes[0..writer.used]);
}
test "modern timeline and address requests match pinned native encoder and reject malformed bounds" {
    const ids = [_]u64{11,12};
    const values = [_]u64{15,std.math.maxInt(u64)};
    const semaphores = [_]c.VkSemaphore{@ptrFromInt(11),@ptrFromInt(12)};
    const wait = c.VkSemaphoreWaitInfo{.sType=c.VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,.flags=c.VK_SEMAPHORE_WAIT_ANY_BIT,.semaphoreCount=2,.pSemaphores=&semaphores,.pValues=&values};
    const signal = c.VkSemaphoreSignalInfo{.sType=c.VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,.semaphore=@ptrFromInt(11),.value=99};
    const address = c.VkBufferDeviceAddressInfo{.sType=c.VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=@ptrFromInt(11)};
    try compare(try semaphore_counter(7,11),172,null,0);
    try compare(try signal_semaphore(7,11,99),174,&signal,0);
    try compare(try wait_semaphores(7,&ids,&values,1,std.math.maxInt(u64)),173,&wait,0);
    try compare(try buffer_device_address(7,11),175,&address,0);
    try compare(try reset_query_pool(7,11,3,4),171,null,0);
    try std.testing.expectError(error.Invalid,semaphore_counter(0,11));
    try std.testing.expectError(error.Invalid,semaphore_counter(7,0));
    try std.testing.expectError(error.Invalid,signal_semaphore(7,0,99));
    try std.testing.expectError(error.Invalid,buffer_device_address(7,0));
    try std.testing.expectError(error.Invalid,reset_query_pool(7,11,std.math.maxInt(u32),1));
    try std.testing.expectError(error.Invalid,reset_query_pool(7,0,0,0));
    try std.testing.expectError(error.Invalid,wait_semaphores(7,&.{},&.{},0,0));
    try std.testing.expectError(error.Invalid,wait_semaphores(7,&ids,&.{0},0,0));
    try std.testing.expectError(error.Invalid,wait_semaphores(7,&ids,&values,2,0));
    try std.testing.expectError(error.Invalid,wait_semaphores(7,&.{0},&.{0},0,0));
    const many=[_]u64{1} ** 65;
    try std.testing.expectError(error.Limit,wait_semaphores(7,&many,&many,0,0));
}
test "sync2 mixed barriers and submit2 preserve64-bit scopes values and native byte order" {
    var memory=c.VkMemoryBarrier2{.sType=c.VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=1<<40,.srcAccessMask=1<<41,.dstStageMask=1<<42,.dstAccessMask=1<<43};
    var buffer=c.VkBufferMemoryBarrier2{.sType=c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,.srcStageMask=1<<40,.srcAccessMask=1<<41,.dstStageMask=1<<42,.dstAccessMask=1<<43,.srcQueueFamilyIndex=c.VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=c.VK_QUEUE_FAMILY_IGNORED,.buffer=@ptrFromInt(11),.offset=256,.size=c.VK_WHOLE_SIZE};
    var image=c.VkImageMemoryBarrier2{.sType=c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,.srcStageMask=1<<40,.srcAccessMask=1<<41,.dstStageMask=1<<42,.dstAccessMask=1<<43,.oldLayout=1,.newLayout=2,.srcQueueFamilyIndex=c.VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=c.VK_QUEUE_FAMILY_IGNORED,.image=@ptrFromInt(13),.subresourceRange=.{.aspectMask=1,.levelCount=3,.layerCount=4}};
    var dependency=c.VkDependencyInfo{.sType=c.VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&memory,.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&buffer,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&image};
    try compare(try pipeline_barrier2(9,&dependency),204,&dependency,0);
    var semaphore=c.VkSemaphoreSubmitInfo{.sType=c.VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,.semaphore=@ptrFromInt(11),.value=1<<40,.stageMask=1<<42};
    var command=c.VkCommandBufferSubmitInfo{.sType=c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,.commandBuffer=@ptrFromInt(15),.deviceMask=1};
    var submit=c.VkSubmitInfo2{.sType=c.VK_STRUCTURE_TYPE_SUBMIT_INFO_2,.waitSemaphoreInfoCount=1,.pWaitSemaphoreInfos=&semaphore,.commandBufferInfoCount=1,.pCommandBufferInfos=&command,.signalSemaphoreInfoCount=1,.pSignalSemaphoreInfos=&semaphore};
    try compare(try queue_submit2(9,(@as([*]c.VkSubmitInfo2,@ptrCast(&submit)))[0..1],13),206,&submit,1);
    try compare(try queue_submit2(9,&.{},13),206,null,0);
    try std.testing.expectError(error.Invalid,pipeline_barrier2(0,&dependency));
    dependency.sType=0;
    try std.testing.expectError(error.Invalid,pipeline_barrier2(9,&dependency));
    dependency.sType=c.VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount=65;
    try std.testing.expectError(error.Limit,pipeline_barrier2(9,&dependency));
    dependency.memoryBarrierCount=1;memory.pNext=@ptrFromInt(8);
    try std.testing.expectError(error.Invalid,pipeline_barrier2(9,&dependency));
    memory.pNext=null;buffer.buffer=null;
    try std.testing.expectError(error.Invalid,pipeline_barrier2(9,&dependency));
    buffer.buffer=@ptrFromInt(11);image.image=null;
    try std.testing.expectError(error.Invalid,pipeline_barrier2(9,&dependency));
    try std.testing.expectError(error.Invalid,queue_submit2(0,&.{},0));
    semaphore.deviceIndex=1;
    try std.testing.expectError(error.Invalid,queue_submit2(9,(@as([*]c.VkSubmitInfo2,@ptrCast(&submit)))[0..1],13));
    semaphore.deviceIndex=0;command.deviceMask=2;
    try std.testing.expectError(error.Invalid,queue_submit2(9,(@as([*]c.VkSubmitInfo2,@ptrCast(&submit)))[0..1],13));
}
test "modern completed value decoder validates identity shape and signed native errors" {
    var reply=[_]u8{0} ** 24;
    std.mem.writeInt(u32,reply[0..4],172,.little);
    std.mem.writeInt(u64,reply[8..16],1,.little);
    std.mem.writeInt(u64,reply[16..24],std.math.maxInt(u64),.little);
    try std.testing.expectEqual(std.math.maxInt(u64),try decode_value(&reply,172));
    try std.testing.expectError(error.Corrupt,decode_value(reply[0..12],172));
    std.mem.writeInt(i32,reply[4..8],-4,.little);
    try std.testing.expectError(error.Backend,decode_value(&reply,172));
    std.mem.writeInt(i32,reply[4..8],1,.little);
    try std.testing.expectError(error.Corrupt,decode_value(&reply,172));
    std.mem.writeInt(u32,reply[0..4],175,.little);
    std.mem.writeInt(u64,reply[4..12],1<<48,.little);
    try std.testing.expectEqual(@as(u64,1<<48),try decode_value(&reply,175));
    try std.testing.expectError(error.Corrupt,decode_value(reply[0..11],175));
    try std.testing.expectError(error.Corrupt,decode_value(&reply,172));
}
