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
fn node(writer: *writer_t, tag: u32) void {
    writer.put_proven(u32, tag);
    writer.put_proven(u64, 0);
}
noinline fn array(comptime value_t: type, pointer: [*c]align(1) const value_t, length: u32) ![]const value_t {
    if (length > MaxRecords) return error.Limit;
    if (length == 0) return &.{};
    if (pointer == null or @intFromPtr(pointer) % @alignOf(value_t) != 0) return error.Invalid;
    return @as([*c]const value_t, @alignCast(pointer))[0..length];
}
fn count(writer: *writer_t, length: usize) void {
    writer.put_proven(u32, @intCast(length));
    writer.put_proven(u64, length);
}
fn scopes(writer: *writer_t, barrier: anytype) void {
    writer.put_proven(u64, barrier.srcStageMask);
    writer.put_proven(u64, barrier.srcAccessMask);
    writer.put_proven(u64, barrier.dstStageMask);
    writer.put_proven(u64, barrier.dstAccessMask);
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
    // Validate every borrowed record before the exact aggregate capacity check.
    for (memories) |value| if (value.sType != c.VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 or value.pNext != null) return error.Invalid;
    for (buffers) |value| if (value.sType != c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 or value.pNext != null or value.buffer == null) return error.Invalid;
    for (images) |value| if (value.sType != c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 or value.pNext != null or value.image == null) return error.Invalid;
    var writer: writer_t = .{};
    try writer.require_capacity(76 + 44 * memories.len + 76 * buffers.len + 88 * images.len);
    writer.header(204, command_id) catch unreachable;
    writer.put_proven(u64, 1);
    node(&writer, c.VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    writer.put_proven(u32, info.dependencyFlags);
    count(&writer, memories.len);
    for (memories) |value| {
        node(&writer, c.VK_STRUCTURE_TYPE_MEMORY_BARRIER_2);
        scopes(&writer, value);
    }
    count(&writer, buffers.len);
    for (buffers) |value| {
        node(&writer, c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2);
        scopes(&writer, value);
        writer.put_proven(u32, value.srcQueueFamilyIndex);
        writer.put_proven(u32, value.dstQueueFamilyIndex);
        writer.put_proven(u64, handle_bits(value.buffer));
        writer.put_proven(u64, value.offset);
        writer.put_proven(u64, value.size);
    }
    count(&writer, images.len);
    for (images) |value| {
        node(&writer, c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
        scopes(&writer, value);
        writer.put_proven(u32, value.oldLayout);
        writer.put_proven(u32, value.newLayout);
        writer.put_proven(u32, value.srcQueueFamilyIndex);
        writer.put_proven(u32, value.dstQueueFamilyIndex);
        writer.put_proven(u64, handle_bits(value.image));
        const range = value.subresourceRange;
        for ([_]u32{range.aspectMask, range.baseMipLevel, range.levelCount, range.baseArrayLayer, range.layerCount}) |word| writer.put_proven(u32, word);
    }
    return writer;
}
fn append(writer:*writer_t,bytes:[]const u8) !void {
    if(writer.used>writer.bytes.len or bytes.len>writer.bytes.len-writer.used)return error.Limit;
    @memcpy(writer.bytes[writer.used..][0..bytes.len],bytes);writer.used+=bytes.len;
}
/// [in] resolved recording command/event owners and borrowed translated dependency.
/// [out] Exact201 packet; Invalid identities/topology, Limit records/packet. Caller
/// validates event ownership, enabledsync2 and recording-state/resource scopes.
/// No heap/native pointer retention; independent owned packet writers are safe.
pub fn set_event2(command_id:u64,event_id:u64,info:*const c.VkDependencyInfo) !writer_t {
    if(event_id==0)return error.Invalid;
    const dependency=try pipeline_barrier2(command_id,info);
    var writer:writer_t=.{};try writer.header(201,command_id);writer.put_proven(u64,event_id);try append(&writer,dependency.bytes[16..dependency.used]);return writer;
}
/// [in] resolved command/event and full64-bit stage mask; caller checks enabled
/// sync2/stage support and recording state. [out] Exact202 packet or Invalid IDs.
/// No heap/retained pointers or shared mutation.
pub fn reset_event2(command_id:u64,event_id:u64,stage:u64) !writer_t {
    if(event_id==0)return error.Invalid;
    var writer:writer_t=.{};try writer.header(202,command_id);writer.put_proven(u64,event_id);writer.put_proven(u64,stage);return writer;
}
/// [in] matched1..64 resolved event IDs and translated dependency records, borrowed.
/// [out] Exact203 packet or Invalid zero/unequal owner arrays/topology, Limit counts
/// or total packet bytes. Caller owns event/resource lifetimes and recording state;
/// no heap/retention and failed partial writers never escape to the peer.
pub fn wait_events2(command_id:u64,events:[]const u64,infos:[]const c.VkDependencyInfo) !writer_t {
    if(events.len==0 or events.len!=infos.len)return error.Invalid;
    if(events.len>MaxRecords)return error.Limit;
    var writer:writer_t=.{};try writer.header(203,command_id);count(&writer,events.len);
    for(events) |event| {if(event==0)return error.Invalid;writer.put_proven(u64,event);}
    writer.put_proven(u64,infos.len);
    for(infos) |*info| {const dependency=try pipeline_barrier2(command_id,info);try append(&writer,dependency.bytes[24..dependency.used]);}
    return writer;
}
fn semaphore_submit(writer: *writer_t, values: []const c.VkSemaphoreSubmitInfo) void {
    count(writer, values.len);
    for (values) |value| {
        node(writer, c.VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
        writer.put_proven(u64, handle_bits(value.semaphore));
        writer.put_proven(u64, value.value);
        writer.put_proven(u64, value.stageMask);
        writer.put_proven(u32, value.deviceIndex);
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
    var packet_bytes: usize = 36;
    for (submits) |value| {
        if (value.sType != c.VK_STRUCTURE_TYPE_SUBMIT_INFO_2 or value.pNext != null or value.flags != 0) return error.Invalid;
        const waits = try array(c.VkSemaphoreSubmitInfo, value.pWaitSemaphoreInfos, value.waitSemaphoreInfoCount);
        const commands = try array(c.VkCommandBufferSubmitInfo, value.pCommandBufferInfos, value.commandBufferInfoCount);
        const signals = try array(c.VkSemaphoreSubmitInfo, value.pSignalSemaphoreInfos, value.signalSemaphoreInfoCount);
        for (waits) |entry| if (entry.sType != c.VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO or entry.pNext != null or entry.semaphore == null or entry.deviceIndex != 0) return error.Invalid;
        for (signals) |entry| if (entry.sType != c.VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO or entry.pNext != null or entry.semaphore == null or entry.deviceIndex != 0) return error.Invalid;
        for (commands) |entry| if (entry.sType != c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO or entry.pNext != null or entry.commandBuffer == null or entry.deviceMask > 1) return error.Invalid;
        packet_bytes += 52 + 40 * (waits.len + signals.len) + 24 * commands.len;
    }
    var writer: writer_t = .{};
    try writer.require_capacity(packet_bytes);
    writer.header(206, queue_id) catch unreachable;
    count(&writer, submits.len);
    for (submits) |value| {
        const waits = array(c.VkSemaphoreSubmitInfo, value.pWaitSemaphoreInfos, value.waitSemaphoreInfoCount) catch unreachable;
        const commands = array(c.VkCommandBufferSubmitInfo, value.pCommandBufferInfos, value.commandBufferInfoCount) catch unreachable;
        const signals = array(c.VkSemaphoreSubmitInfo, value.pSignalSemaphoreInfos, value.signalSemaphoreInfoCount) catch unreachable;
        node(&writer, c.VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
        writer.put_proven(u32, value.flags);
        semaphore_submit(&writer, waits);
        count(&writer, commands.len);
        for (commands) |entry| {
            node(&writer, c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
            writer.put_proven(u64, handle_bits(entry.commandBuffer));
            writer.put_proven(u32, entry.deviceMask);
        }
        semaphore_submit(&writer, signals);
    }
    writer.put_proven(u64, fence_id);
    return writer;
}
/// [in] device_id/new semaphore_id resolved owners, semaphore_type binary0 or
/// timeline1 and initial exact timeline value (binary requires0). [out] Owned
/// create40 packet with canonical type node, Invalid shape/ID or Limit capacity.
/// Caller validates enabled timeline feature and host limits; no allocation,
/// retained pointers or shared state; ownership publishes only after exact ACK.
pub fn create_semaphore(device_id: u64, semaphore_id: u64, semaphore_type: u32, initial: u64) !writer_t {
    if (semaphore_id == 0 or semaphore_type > 1 or (semaphore_type == 0 and initial != 0)) return error.Invalid;
    var writer: writer_t = .{};
    try writer.header(40, device_id);
    writer.require_capacity(1092 - 16) catch unreachable;
    writer.put_proven(u64, 1);
    writer.put_proven(u32, c.VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    writer.put_proven(u64, 1);
    node(&writer, c.VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
    writer.put_proven(u32, semaphore_type);
    writer.put_proven(u64, initial);
    writer.put_proven(u32, 0);
    writer.put_proven(u64, 0);
    writer.put_proven(u64, 1);
    writer.put_proven(u64, semaphore_id);
    return writer;
}
/// [in] device_id/semaphore_id resolved nonzero host identities. [out] Owned
/// counter-value command172 request. Invalid zero IDs; no allocation/retention.
/// Caller validates enabled timeline feature and semaphore ownership/type.
pub fn semaphore_counter(device_id: u64, semaphore_id: u64) !writer_t {
    if (semaphore_id == 0) return error.Invalid;
    var writer: writer_t = .{};
    try writer.header(172, device_id);
    writer.require_capacity(1092 - 16) catch unreachable;
    writer.put_proven(u64, semaphore_id);
    writer.put_proven(u64, 1);
    return writer;
}
/// [in] device_id/semaphore_id resolved host owners; value exact timeline value.
/// [out] Owned command174 packet; Invalid zero IDs. Caller validates timeline
/// ownership/monotonic semantics. No allocation or retained memory; thread safe.
pub fn signal_semaphore(device_id: u64, semaphore_id: u64, value: u64) !writer_t {
    if (semaphore_id == 0) return error.Invalid;
    var writer: writer_t = .{};
    try writer.header(174, device_id);
    writer.require_capacity(1092 - 16) catch unreachable;
    writer.put_proven(u64, 1);
    node(&writer, c.VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO);
    writer.put_proven(u64, semaphore_id);
    writer.put_proven(u64, value);
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
    writer.require_capacity(1092 - 16) catch unreachable;
    writer.put_proven(u64, 1);
    node(&writer, c.VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
    writer.put_proven(u32, flags);
    count(&writer, ids.len);
    for (ids) |id| {
        if (id == 0) return error.Invalid;
        writer.put_proven(u64, id);
    }
    writer.put_proven(u64, values.len);
    for (values) |value| writer.put_proven(u64, value);
    writer.put_proven(u64, timeout);
    return writer;
}
/// [in] device_id/buffer_id resolved nonzero host identities. [out] Exact command175
/// request; Invalid zero identity. Caller validates bound memory, ADDRESS usage
/// and enabled BDA feature. No allocation or retained memory; thread safe.
pub fn buffer_device_address(device_id: u64, buffer_id: u64) !writer_t {
    if (buffer_id == 0) return error.Invalid;
    var writer: writer_t = .{};
    try writer.header(175, device_id);
    writer.require_capacity(1092 - 16) catch unreachable;
    writer.put_proven(u64, 1);
    node(&writer, c.VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
    writer.put_proven(u64, buffer_id);
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
    writer.require_capacity(1092 - 16) catch unreachable;
    writer.put_proven(u64, query_id);
    writer.put_proven(u32, first);
    writer.put_proven(u32, query_count);
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

/// Copied native timeline query result; no pointers or storage ownership.
pub const result_value_t = struct {
    /// Exact native VkResult; negative errors retain their original identity.
    result: i32,
    /// Actual timeline value; publish externally only when result is SUCCESS.
    value: u64,
};
/// [in] completed immutable command172 reply. [out] Exact signed native status
/// and copied value; Corrupt invalid prefix/shape/presence/positive result.
/// Negative native errors do not expose undefined value to application storage.
/// No allocations, pointer retention, shared state or output mutation.
pub fn decode_counter(reply: []const u8) !result_value_t {
    if (reply.len < 24 or std.mem.readInt(u32, reply[0..4], .little) != 172 or
        std.mem.readInt(u64, reply[8..16], .little) != 1) return error.Corrupt;
    const result = std.mem.readInt(i32, reply[4..8], .little);
    if (result > 0) return error.Corrupt;
    return .{ .result = result, .value = std.mem.readInt(u64, reply[16..24], .little) };
}

// Test-only fixtures.
// Keep public entrypoint validation independent of compile-time fixture values.
fn runtime_fn(comptime function: anytype) @TypeOf(&function) {
    var pointer = &function;
    return @as(*volatile @TypeOf(pointer), &pointer).*;
}

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
    try compare(try runtime_fn(semaphore_counter)(7,11),172,null,0);
    try compare(try runtime_fn(signal_semaphore)(7,11,99),174,&signal,0);
    try compare(try runtime_fn(wait_semaphores)(7,&ids,&values,1,std.math.maxInt(u64)),173,&wait,0);
    try compare(try runtime_fn(buffer_device_address)(7,11),175,&address,0);
    try compare(try runtime_fn(reset_query_pool)(7,11,3,4),171,null,0);
    try std.testing.expectError(error.Invalid,runtime_fn(semaphore_counter)(0,11));
    try std.testing.expectError(error.Invalid,runtime_fn(semaphore_counter)(7,0));
    try std.testing.expectError(error.Invalid,runtime_fn(signal_semaphore)(7,0,99));
    try std.testing.expectError(error.Invalid,runtime_fn(buffer_device_address)(7,0));
    try std.testing.expectError(error.Invalid,runtime_fn(reset_query_pool)(7,11,std.math.maxInt(u32),1));
    try std.testing.expectError(error.Invalid,runtime_fn(reset_query_pool)(7,0,0,0));
    try std.testing.expectError(error.Invalid,runtime_fn(wait_semaphores)(7,&.{},&.{},0,0));
    try std.testing.expectError(error.Invalid,runtime_fn(wait_semaphores)(7,&ids,&.{0},0,0));
    try std.testing.expectError(error.Invalid,runtime_fn(wait_semaphores)(7,&ids,&values,2,0));
    try std.testing.expectError(error.Invalid,runtime_fn(wait_semaphores)(7,&.{0},&.{0},0,0));
    const many=[_]u64{1} ** 65;
    try std.testing.expectError(error.Limit,runtime_fn(wait_semaphores)(7,&many,&many,0,0));
}
test "sync2 mixed barriers and submit2 preserve64-bit scopes values and native byte order" {
    var memory=c.VkMemoryBarrier2{.sType=c.VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=1<<40,.srcAccessMask=1<<41,.dstStageMask=1<<42,.dstAccessMask=1<<43};
    var buffer=c.VkBufferMemoryBarrier2{.sType=c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,.srcStageMask=1<<40,.srcAccessMask=1<<41,.dstStageMask=1<<42,.dstAccessMask=1<<43,.srcQueueFamilyIndex=c.VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=c.VK_QUEUE_FAMILY_IGNORED,.buffer=@ptrFromInt(11),.offset=256,.size=c.VK_WHOLE_SIZE};
    var image=c.VkImageMemoryBarrier2{.sType=c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,.srcStageMask=1<<40,.srcAccessMask=1<<41,.dstStageMask=1<<42,.dstAccessMask=1<<43,.oldLayout=1,.newLayout=2,.srcQueueFamilyIndex=c.VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=c.VK_QUEUE_FAMILY_IGNORED,.image=@ptrFromInt(13),.subresourceRange=.{.aspectMask=1,.levelCount=3,.layerCount=4}};
    var dependency=c.VkDependencyInfo{.sType=c.VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&memory,.bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&buffer,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&image};
    try compare(try runtime_fn(pipeline_barrier2)(9,&dependency),204,&dependency,0);
    var semaphore=c.VkSemaphoreSubmitInfo{.sType=c.VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,.semaphore=@ptrFromInt(11),.value=1<<40,.stageMask=1<<42};
    var command=c.VkCommandBufferSubmitInfo{.sType=c.VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,.commandBuffer=@ptrFromInt(15),.deviceMask=0};
    var submit=c.VkSubmitInfo2{.sType=c.VK_STRUCTURE_TYPE_SUBMIT_INFO_2,.waitSemaphoreInfoCount=1,.pWaitSemaphoreInfos=&semaphore,.commandBufferInfoCount=1,.pCommandBufferInfos=&command,.signalSemaphoreInfoCount=1,.pSignalSemaphoreInfos=&semaphore};
    try compare(try runtime_fn(queue_submit2)(9,(@as([*]c.VkSubmitInfo2,@ptrCast(&submit)))[0..1],13),206,&submit,1);
    try compare(try runtime_fn(queue_submit2)(9,&.{},13),206,null,0);
    try std.testing.expectError(error.Invalid,runtime_fn(pipeline_barrier2)(0,&dependency));
    dependency.sType=0;
    try std.testing.expectError(error.Invalid,runtime_fn(pipeline_barrier2)(9,&dependency));
    dependency.sType=c.VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount=65;
    try std.testing.expectError(error.Limit,runtime_fn(pipeline_barrier2)(9,&dependency));
    dependency.memoryBarrierCount=1;memory.pNext=@ptrFromInt(8);
    try std.testing.expectError(error.Invalid,runtime_fn(pipeline_barrier2)(9,&dependency));
    memory.pNext=null;buffer.buffer=null;
    try std.testing.expectError(error.Invalid,runtime_fn(pipeline_barrier2)(9,&dependency));
    buffer.buffer=@ptrFromInt(11);image.image=null;
    try std.testing.expectError(error.Invalid,runtime_fn(pipeline_barrier2)(9,&dependency));
    try std.testing.expectError(error.Invalid,runtime_fn(queue_submit2)(0,&.{},0));
    semaphore.deviceIndex=1;
    try std.testing.expectError(error.Invalid,runtime_fn(queue_submit2)(9,(@as([*]c.VkSubmitInfo2,@ptrCast(&submit)))[0..1],13));
    semaphore.deviceIndex=0;command.deviceMask=2;
    try std.testing.expectError(error.Invalid,runtime_fn(queue_submit2)(9,(@as([*]c.VkSubmitInfo2,@ptrCast(&submit)))[0..1],13));
}
test "modern completed value decoder validates identity shape and signed native errors" {
    var reply=[_]u8{0} ** 24;
    std.mem.writeInt(u32,reply[0..4],172,.little);
    std.mem.writeInt(u64,reply[8..16],1,.little);
    std.mem.writeInt(u64,reply[16..24],std.math.maxInt(u64),.little);
    try std.testing.expectEqual(std.math.maxInt(u64),try runtime_fn(decode_value)(&reply,172));
    try std.testing.expectError(error.Corrupt,runtime_fn(decode_value)(reply[0..12],172));
    std.mem.writeInt(i32,reply[4..8],-4,.little);
    try std.testing.expectError(error.Backend,runtime_fn(decode_value)(&reply,172));
    std.mem.writeInt(i32,reply[4..8],1,.little);
    try std.testing.expectError(error.Corrupt,runtime_fn(decode_value)(&reply,172));
    std.mem.writeInt(u32,reply[0..4],175,.little);
    std.mem.writeInt(u64,reply[4..12],1<<48,.little);
    try std.testing.expectEqual(@as(u64,1<<48),try runtime_fn(decode_value)(&reply,175));
    try std.testing.expectError(error.Corrupt,runtime_fn(decode_value)(reply[0..11],175));
    try std.testing.expectError(error.Corrupt,runtime_fn(decode_value)(&reply,172));
}

test "typed semaphore creation and exact counter status match pinned native ownership requests" {
    const kind = c.VkSemaphoreTypeCreateInfo{ .sType = c.VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = 1, .initialValue = 99 };
    const info = c.VkSemaphoreCreateInfo{ .sType = c.VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &kind };
    try compare(try runtime_fn(create_semaphore)(7,11,1,99),40,&info,0);
    try std.testing.expectError(error.Invalid,runtime_fn(create_semaphore)(7,11,0,99));
    try std.testing.expectError(error.Invalid,runtime_fn(create_semaphore)(7,11,2,0));
    var reply=[_]u8{0} ** 24;
    std.mem.writeInt(u32,reply[0..4],172,.little);std.mem.writeInt(u64,reply[8..16],1,.little);std.mem.writeInt(i32,reply[4..8],-1,.little);
    const result=try runtime_fn(decode_counter)(&reply);try std.testing.expectEqual(@as(i32,-1),result.result);
    try std.testing.expectError(error.Corrupt,runtime_fn(decode_counter)(reply[0..23]));
    std.mem.writeInt(i32,reply[4..8],1,.little);try std.testing.expectError(error.Corrupt,runtime_fn(decode_counter)(&reply));
}

test "synchronization2 event operations match pinned encoder and retain full stages" {
    const memory=c.VkMemoryBarrier2{.sType=c.VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=1<<40,.srcAccessMask=1<<41,.dstStageMask=1<<42,.dstAccessMask=1<<43};
    const info=c.VkDependencyInfo{.sType=c.VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&memory};
    try compare(try runtime_fn(set_event2)(9,11,&info),201,&info,0);
    const stage:u64=1<<40;try compare(try runtime_fn(reset_event2)(9,11,stage),202,&stage,0);
    const infos=[_]c.VkDependencyInfo{info,info};try compare(try runtime_fn(wait_events2)(9,&.{11,13},&infos),203,&infos,2);
    try std.testing.expectError(error.Invalid,runtime_fn(set_event2)(9,0,&info));try std.testing.expectError(error.Invalid,runtime_fn(reset_event2)(9,0,stage));
    try std.testing.expectError(error.Invalid,runtime_fn(wait_events2)(9,&.{},&.{}));try std.testing.expectError(error.Invalid,runtime_fn(wait_events2)(9,&.{11},&infos));try std.testing.expectError(error.Invalid,runtime_fn(wait_events2)(9,&.{11,0},&infos));
    const many=[_]c.VkDependencyInfo{info} ** 65;const events=[_]u64{11} ** 65;try std.testing.expectError(error.Limit,runtime_fn(wait_events2)(9,&events,&many));
}

test "complete sync2 packet preflight rejects malformed arrays records and real capacity exhaustion" {
    var dependency = c.VkDependencyInfo{ .sType = c.VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    dependency.pNext = @ptrFromInt(8);
    try std.testing.expectError(error.Invalid, runtime_fn(pipeline_barrier2)(9, &dependency));
    dependency.pNext = null;
    dependency.memoryBarrierCount = 1;
    try std.testing.expectError(error.Invalid, runtime_fn(pipeline_barrier2)(9, &dependency));
    std.mem.writeInt(usize, @as(*[@sizeOf(usize)]u8, @ptrCast(&dependency.pMemoryBarriers)), 1, if (@import("builtin").cpu.arch.endian() == .little) .little else .big);
    try std.testing.expectError(error.Invalid, runtime_fn(pipeline_barrier2)(9, &dependency));
    var memory = c.VkMemoryBarrier2{ .sType = c.VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    dependency.pMemoryBarriers = &memory;
    memory.sType = 0;
    try std.testing.expectError(error.Invalid, runtime_fn(pipeline_barrier2)(9, &dependency));
    memory.sType = c.VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    const memories = [_]c.VkMemoryBarrier2{memory} ** 64;
    const buffer = c.VkBufferMemoryBarrier2{ .sType = c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2, .buffer = @ptrFromInt(11) };
    const buffers = [_]c.VkBufferMemoryBarrier2{buffer} ** 64;
    dependency.memoryBarrierCount = 64; dependency.pMemoryBarriers = &memories;
    dependency.bufferMemoryBarrierCount = 64; dependency.pBufferMemoryBarriers = &buffers;
    const image = c.VkImageMemoryBarrier2{ .sType = c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .image = @ptrFromInt(13) };
    dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &image;
    // With one extra image record this still fits;64 images exceed the packet.
    const images = [_]c.VkImageMemoryBarrier2{image} ** 64;
    dependency.imageMemoryBarrierCount = 64; dependency.pImageMemoryBarriers = &images;
    try std.testing.expectError(error.Limit, runtime_fn(pipeline_barrier2)(9, &dependency));
    // Individually valid dependencies can still exceed the aggregate wait packet.
    dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &memory;
    dependency.bufferMemoryBarrierCount = 0; dependency.pBufferMemoryBarriers = null;
    dependency.imageMemoryBarrierCount = 0; dependency.pImageMemoryBarriers = null;
    const infos = [_]c.VkDependencyInfo{dependency} ** 64;
    const events = [_]u64{11} ** 64;
    _ = try runtime_fn(wait_events2)(9, &events, &infos);
    dependency.memoryBarrierCount = 3; dependency.pMemoryBarriers = &memories;
    const larger = [_]c.VkDependencyInfo{dependency} ** 64;
    try std.testing.expectError(error.Limit, runtime_fn(wait_events2)(9, &events, &larger));
    var submit = c.VkSubmitInfo2{ .sType = c.VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    const one = (@as([*]c.VkSubmitInfo2, @ptrCast(&submit)))[0..1];
    submit.sType = 0; try std.testing.expectError(error.Invalid, runtime_fn(queue_submit2)(9, one, 0));
    submit.sType = c.VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.pNext = @ptrFromInt(8); try std.testing.expectError(error.Invalid, runtime_fn(queue_submit2)(9, one, 0));
    submit.pNext = null; submit.flags = 1; try std.testing.expectError(error.Invalid, runtime_fn(queue_submit2)(9, one, 0));
    submit.flags = 0;
    var semaphore = c.VkSemaphoreSubmitInfo{ .sType = c.VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = @ptrFromInt(11) };
    submit.waitSemaphoreInfoCount = 1; submit.pWaitSemaphoreInfos = &semaphore;
    semaphore.sType = 0; try std.testing.expectError(error.Invalid, runtime_fn(queue_submit2)(9, one, 0));
    semaphore.sType = c.VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    semaphore.pNext = @ptrFromInt(8); try std.testing.expectError(error.Invalid, runtime_fn(queue_submit2)(9, one, 0));
    semaphore.pNext = null; semaphore.semaphore = null; try std.testing.expectError(error.Invalid, runtime_fn(queue_submit2)(9, one, 0));
    semaphore.semaphore = @ptrFromInt(11);
    const waits = [_]c.VkSemaphoreSubmitInfo{semaphore} ** 64;
    submit.waitSemaphoreInfoCount = 64; submit.pWaitSemaphoreInfos = &waits;
    submit.signalSemaphoreInfoCount = 64; submit.pSignalSemaphoreInfos = &waits;
    const submits = [_]c.VkSubmitInfo2{submit} ** 2;
    try std.testing.expectError(error.Limit, runtime_fn(queue_submit2)(9, &submits, 0));
    const too_many = [_]c.VkSubmitInfo2{submit} ** 65;
    try std.testing.expectError(error.Limit, runtime_fn(queue_submit2)(9, &too_many, 0));
    try std.testing.expectError(error.Invalid, runtime_fn(create_semaphore)(7, 0, 1, 0));
    try std.testing.expectError(error.Invalid, runtime_fn(create_semaphore)(0, 11, 1, 0));
    _ = try runtime_fn(create_semaphore)(7, 11, 0, 0);
    try std.testing.expectError(error.Invalid, runtime_fn(wait_semaphores)(0, &.{11}, &.{0}, 0, 0));
    try std.testing.expectError(error.Invalid, runtime_fn(buffer_device_address)(0, 11));
    try std.testing.expectError(error.Invalid, runtime_fn(signal_semaphore)(0, 11, 0));
    try std.testing.expectError(error.Invalid, runtime_fn(reset_query_pool)(0, 11, 0, 0));
    try std.testing.expectError(error.Invalid, runtime_fn(reset_event2)(0, 11, 0));
    try std.testing.expectError(error.Invalid, runtime_fn(wait_events2)(0, &.{11}, infos[0..1]));
    var reply = [_]u8{0} ** 24;
    std.mem.writeInt(u32, reply[0..4], 172, .little);
    std.mem.writeInt(u64, reply[8..16], 2, .little);
    try std.testing.expectError(error.Corrupt, runtime_fn(decode_value)(&reply, 172));
    try std.testing.expectError(error.Corrupt, runtime_fn(decode_counter)(&reply));
    std.mem.writeInt(u32, reply[0..4], 173, .little);
    try std.testing.expectError(error.Corrupt, runtime_fn(decode_value)(&reply, 173));
    try std.testing.expectError(error.Corrupt, runtime_fn(decode_counter)(&reply));
}
