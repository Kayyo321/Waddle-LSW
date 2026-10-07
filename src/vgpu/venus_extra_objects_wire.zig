//! Bounded Vulkan sampler/view/query/cache/event packets. Host ownership and enabled
//! features remain with the ICD; every input is borrowed only for this call.
const std=@import("std");
const render=@import("venus_render_wire.zig");
const c=@cImport({@cInclude("vulkan/vulkan.h");});
/// Owned8192-byte initialized prefix; no borrowed pointers, allocation or shared state.
pub const writer_t=render.writer_t;
fn create_header(writer:*writer_t,opcode:u32,device:u64,tag:u32) !void {
    try writer.header(opcode,device);
    try writer.put(u64,1);try writer.put(u32,tag);try writer.put(u64,0);
}
fn finish(writer:*writer_t,id:u64) !void {
    if(id==0)return error.Invalid;
    try writer.put(u64,0);try writer.put(u64,1);try writer.put(u64,id);
}
fn valid_header(info:anytype,tag:u32) bool {return info.sType==tag and info.pNext==null;}
fn scalars(writer:*writer_t,info:anytype) !void {
    inline for(@typeInfo(@TypeOf(info)).Struct.fields) |field| {
        if(comptime !std.mem.eql(u8,field.name,"sType") and !std.mem.eql(u8,field.name,"pNext")) {
            if(field.type==f32) {
                const value=@field(info,field.name);
                if(comptime !std.mem.eql(u8,field.name,"maxAnisotropy")) {
                    if(!std.math.isFinite(value))return error.Invalid;
                } else if(info.anisotropyEnable!=0 and !std.math.isFinite(value))return error.Invalid;
                try writer.put(u32,@bitCast(value));
            } else try writer.put(u32,@field(info,field.name));
        }
    }
}
/// [in] info canonical core sampler, device/id resolved nonzero owners; native
/// allocator unsupported, no extension chain. Caller validates host limits and
/// enabled anisotropy/mirrorClamp features. [out] Exact create70 owned packet or
/// Invalid tags/flags/enums/Boolean/nonfinite values, Limit packet. No retention.
pub fn create_sampler(device:u64,id:u64,info:*const c.VkSamplerCreateInfo) !writer_t {
    if(!valid_header(info.*,c.VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO) or info.flags!=0 or
       info.magFilter>1 or info.minFilter>1 or info.mipmapMode>1 or info.addressModeU>4 or
       info.addressModeV>4 or info.addressModeW>4 or info.anisotropyEnable>1 or info.compareEnable>1 or
       info.unnormalizedCoordinates>1 or info.compareOp>7 or info.borderColor>5 or
       (info.anisotropyEnable!=0 and info.maxAnisotropy<1) or info.minLod>info.maxLod)return error.Invalid;
    var writer:writer_t=.{};try create_header(&writer,70,device,c.VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    try scalars(&writer,info.*);try finish(&writer,id);return writer;
}
/// [in] info borrowed no-chain view; buffer_id resolved host buffer, native handle
/// ignored after caller ownership validation. Caller checks format/alignment/range.
/// [out] Exact create52 packet or Invalid tag/flags/identity/format/zero range.
/// No allocation/retention; distinct owners safe concurrently.
pub fn create_buffer_view(device:u64,id:u64,buffer_id:u64,info:*const c.VkBufferViewCreateInfo) !writer_t {
    if(!valid_header(info.*,c.VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO) or info.flags!=0 or buffer_id==0 or info.format==0 or info.range==0)return error.Invalid;
    var writer:writer_t=.{};try create_header(&writer,52,device,c.VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
    try writer.put(u32,info.flags);try writer.put(u64,buffer_id);try writer.put(u32,info.format);try writer.put(u64,info.offset);try writer.put(u64,info.range);try finish(&writer,id);return writer;
}
/// [in] info core query type0..2/count1..4096 with known pipeline-statistics bits;
/// caller validates enabled occlusion/statistics/timestamp functionality.
/// [out] Owned create47 or Invalid topology/flags/type/count/statistics. No heap,
/// retained pointers or shared state; native/host object ownership remains caller.
pub fn create_query_pool(device:u64,id:u64,info:*const c.VkQueryPoolCreateInfo) !writer_t {
    if(!valid_header(info.*,c.VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO) or info.flags!=0 or info.queryType>2 or info.queryCount==0 or info.queryCount>4096 or
       info.pipelineStatistics & ~@as(u32,0x7ff)!=0 or (info.queryType!=c.VK_QUERY_TYPE_PIPELINE_STATISTICS and info.pipelineStatistics!=0))return error.Invalid;
    var writer:writer_t=.{};try create_header(&writer,47,device,c.VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);try scalars(&writer,info.*);try finish(&writer,id);return writer;
}
/// [in] info core immutable cache, initial bytes0..4096 accessible through return;
/// caller validates vendor cache compatibility, nonpersistent caller owner.
/// [out] Owned create61 or Invalid shape/flags/null data, Limit byte budget.
/// No allocation/retained pointers or shared mutation.
pub fn create_pipeline_cache(device:u64,id:u64,info:*const c.VkPipelineCacheCreateInfo) !writer_t {
    if(!valid_header(info.*,c.VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO) or info.flags & ~@as(u32,c.VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT)!=0 or (info.initialDataSize!=0 and info.pInitialData==null))return error.Invalid;
    if(info.initialDataSize>4096)return error.Limit;
    var writer:writer_t=.{};try create_header(&writer,61,device,c.VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO);
    try writer.put(u32,info.flags);try writer.put(u64,info.initialDataSize);try writer.put(u64,info.initialDataSize);
    if(info.pInitialData) |pointer| for(@as([*]const u8,@ptrCast(pointer))[0..info.initialDataSize]) |byte| try writer.put(u8,byte);
    while(writer.used%4!=0)try writer.put(u8,0);
    try finish(&writer,id);return writer;
}
/// [in] info core event flags0 or DEVICE_ONLY, live resolved device/new ID.
/// [out] Exact create42 packet or Invalid shape/unknown flags/ID. Caller validates
/// sync2 for device-only events. No allocation/retention; distinct calls safe.
pub fn create_event(device:u64,id:u64,info:*const c.VkEventCreateInfo) !writer_t {
    if(!valid_header(info.*,c.VK_STRUCTURE_TYPE_EVENT_CREATE_INFO) or info.flags & ~@as(u32,c.VK_EVENT_CREATE_DEVICE_ONLY_BIT)!=0)return error.Invalid;
    var writer:writer_t=.{};try create_header(&writer,42,device,c.VK_STRUCTURE_TYPE_EVENT_CREATE_INFO);try writer.put(u32,info.flags);try finish(&writer,id);return writer;
}
/// [in] device/id resolved live owners and opcode43event/48query/53view/62cache/71sampler.
/// [out] Exact destroy packet; Invalid zero ID/opcode. Caller retains ownership
/// until exact native completion ACK. No allocation or retained memory.
pub fn destroy(device:u64,id:u64,opcode:u32) !writer_t {
    if(id==0 or (opcode!=43 and opcode!=48 and opcode!=53 and opcode!=62 and opcode!=71))return error.Invalid;
    var writer:writer_t=.{};try writer.header(opcode,device);try writer.put(u64,id);try writer.put(u64,0);return writer;
}
/// [in] device/event resolved owners; opcode44status/45set/46reset. [out] Exact
/// synchronous event packet or Invalid IDs/opcode. Caller checks DEVICE_ONLY
/// restriction and GPU lifetime. No allocation/retention, disjoint calls safe.
pub fn event_operation(device:u64,event:u64,opcode:u32) !writer_t {
    if(event==0 or opcode<44 or opcode>46)return error.Invalid;
    var writer:writer_t=.{};try writer.header(opcode,device);try writer.put(u64,event);return writer;
}
/// [in] command/query resolved owners, first/count validated within actual pool;
/// flags core query control, stage64 for timestamp205. [out] Owned127begin,
///128end,129reset,130timestamp,205timestamp2 packet or Invalid IDs/opcode/overflow/control.
/// Caller validates executable GPU scope; no allocation, retention or locks.
pub fn query_command(command:u64,pool:u64,opcode:u32,first:u32,query_count:u32,flags:u32,stage:u64) !writer_t {
    if(pool==0 or (opcode!=127 and opcode!=128 and opcode!=129 and opcode!=130 and opcode!=205) or query_count>std.math.maxInt(u32)-first or flags & ~@as(u32,c.VK_QUERY_CONTROL_PRECISE_BIT)!=0)return error.Invalid;
    var writer:writer_t=.{};try writer.header(opcode,command);
    if(opcode==205)try writer.put(u64,stage);
    if(opcode==130){if(stage>std.math.maxInt(u32))return error.Invalid;try writer.put(u32,@intCast(stage));}
    try writer.put(u64,pool);try writer.put(u32,first);
    if(opcode==127)try writer.put(u32,flags);
    if(opcode==129)try writer.put(u32,query_count);
    return writer;
}
/// [in] device/query resolved owners, first/count within actual pool; data_size
/// accessible private output budget0..4096, stride/flags exact native values.
/// [out] Owned49 request; Invalid identity/overflow/flags, Limit data budget.
/// Native pointer token is presence only; no pointer retained or sent to host.
pub fn query_results(device:u64,pool:u64,first:u32,query_count:u32,data_size:usize,stride:u64,flags:u32) !writer_t {
    if(pool==0 or query_count>std.math.maxInt(u32)-first or flags & ~@as(u32,0x1f)!=0)return error.Invalid;
    if(data_size>4096)return error.Limit;
    var writer:writer_t=.{};try writer.header(49,device);try writer.put(u64,pool);try writer.put(u32,first);try writer.put(u32,query_count);try writer.put(u64,data_size);try writer.put(u64,data_size);try writer.put(u64,stride);try writer.put(u32,flags);return writer;
}
/// [in] reply completed immutable command49 bytes, output exclusive actual data
/// extent0..4096. [out] Native VkResult; Corrupt invalid prefix/array/payload/positive
/// result leaves output untouched. Native negative result also preserves output.
/// SUCCESS/NOT_READY copies bounded returned data; no allocation or pointer retention.
pub fn decode_query_results(reply:[]const u8,output:[]u8) !i32 {
    if(reply.len<16 or output.len>4096 or std.mem.readInt(u32,reply[0..4],.little)!=49)return error.Corrupt;
    const result=std.mem.readInt(i32,reply[4..8],.little);
    const bytes=std.mem.readInt(u64,reply[8..16],.little);
    if(bytes!=output.len or bytes>reply.len-16 or (result>0 and result!=c.VK_NOT_READY))return error.Corrupt;
    if(result>=0)@memcpy(output,reply[16..][0..output.len]);
    return result;
}
/// [in] device/cache resolved IDs, capacity bounded0..4096; fill distinguishes
/// count-only query from actual output storage. [out] Owned63 packet or Invalid
/// identity, Limit capacity. No allocation/retention or shared state.
pub fn cache_data(device:u64,cache:u64,capacity:usize,fill:bool) !writer_t {
    if(cache==0)return error.Invalid;
    if(capacity>4096)return error.Limit;
    var writer:writer_t=.{};try writer.header(63,device);try writer.put(u64,cache);try writer.put(u64,1);try writer.put(u64,capacity);try writer.put(u64,if(fill) capacity else 0);return writer;
}
/// [in] device/destination resolved IDs and sources borrowed resolved1..64 unique
/// host cache identities. [out] Owned64 packet, Invalid identity/duplicate/self,
/// Limit array. Caller serializes cache mutation; no allocation or retention.
pub fn merge_caches(device:u64,destination:u64,sources:[]const u64) !writer_t {
    if(destination==0 or sources.len==0)return error.Invalid;
    if(sources.len>64)return error.Limit;
    var writer:writer_t=.{};try writer.header(64,device);try writer.put(u64,destination);try writer.put(u32,@intCast(sources.len));try writer.put(u64,sources.len);
    for(sources,0..) |source,index| {if(source==0 or source==destination)return error.Invalid;for(sources[0..index]) |prior| if(prior==source)return error.Invalid;try writer.put(u64,source);}
    return writer;
}
/// Returned cache query metadata; owns scalar values, no pointers or allocation.
pub const cache_result_t=struct {
    /// Exact host VkResult including INCOMPLETE or negative failures.
    result:i32,
    /// Native size scalar; can exceed bounded fill budget in count-only mode.
    size:u64,
};
/// [in] reply complete immutable63 bytes; output nullable for count-only or actual
/// exclusive bounded byte owner. [out] Exact result/size; Corrupt invalid shape or
/// unsupported positive status leaves output unchanged. Negative native errors
/// preserve output; successful count mode copies no bytes. No allocation/retention.
pub fn decode_cache_data(reply:[]const u8,output:?[]u8) !cache_result_t {
    if(reply.len<32 or std.mem.readInt(u32,reply[0..4],.little)!=63 or std.mem.readInt(u64,reply[8..16],.little)!=1)return error.Corrupt;
    const result=std.mem.readInt(i32,reply[4..8],.little);
    const size=std.mem.readInt(u64,reply[16..24],.little);
    const bytes=std.mem.readInt(u64,reply[24..32],.little);
    if((result>0 and result!=c.VK_INCOMPLETE) or bytes>reply.len-32)return error.Corrupt;
    if(output) |target| {
        if(target.len>4096 or bytes!=size or size>target.len)return error.Corrupt;
        if(result>=0)@memcpy(target[0..@intCast(bytes)],reply[32..][0..@intCast(bytes)]);
    } else if(bytes!=0)return error.Corrupt;
    return .{.result=result,.size=size};
}

// Test-only fixtures.
extern fn venus_extra_objects_test_encode(u32,?*const anyopaque,[*]u8) usize;
fn compare(writer:writer_t,opcode:u32,info:?*const anyopaque) !void {
    var bytes:[8192]u8=undefined;
    const length=venus_extra_objects_test_encode(opcode,info,&bytes);
    try std.testing.expectEqual(length,writer.used);
    try std.testing.expectEqualSlices(u8,bytes[0..length],writer.bytes[0..writer.used]);
}
test "core extra objects and operations match immutable pinned encoder" {
    var sampler=c.VkSamplerCreateInfo{.sType=c.VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=1,.minFilter=1,.mipmapMode=1,.addressModeU=4,.maxAnisotropy=1,.maxLod=100};
    const view=c.VkBufferViewCreateInfo{.sType=c.VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,.buffer=@ptrFromInt(13),.format=37,.offset=256,.range=c.VK_WHOLE_SIZE};
    var query=c.VkQueryPoolCreateInfo{.sType=c.VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=c.VK_QUERY_TYPE_PIPELINE_STATISTICS,.queryCount=128,.pipelineStatistics=0x7ff};
    var event=c.VkEventCreateInfo{.sType=c.VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,.flags=c.VK_EVENT_CREATE_DEVICE_ONLY_BIT};
    const bytes=[_]u8{1,2,3,4,5};
    var cache=c.VkPipelineCacheCreateInfo{.sType=c.VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,.initialDataSize=bytes.len,.pInitialData=&bytes};
    try compare(try create_sampler(7,11,&sampler),70,&sampler);
    sampler.maxAnisotropy=0;try compare(try create_sampler(7,11,&sampler),70,&sampler);
    try compare(try create_buffer_view(7,11,13,&view),52,&view);
    try compare(try create_query_pool(7,11,&query),47,&query);
    try compare(try create_event(7,11,&event),42,&event);
    try compare(try create_pipeline_cache(7,11,&cache),61,&cache);
    for([_]u32{43,48,53,62,71}) |opcode| try compare(try destroy(7,11,opcode),opcode,null);
    for([_]u32{44,45,46}) |opcode| try compare(try event_operation(7,11,opcode),opcode,null);
    for([_]u32{127,128,129,130,205}) |opcode| try compare(try query_command(9,11,opcode,3,4,1,if(opcode==205) @as(u64,1)<<40 else 1),opcode,null);
    try compare(try query_results(7,11,3,4,32,8,1),49,null);
    try compare(try cache_data(7,11,32,true),63,null);
    try compare(try merge_caches(7,11,&.{13,15}),64,null);
    sampler.maxLod=std.math.nan(f32);try std.testing.expectError(error.Invalid,create_sampler(7,11,&sampler));
    sampler.maxLod=100;sampler.compareEnable=2;try std.testing.expectError(error.Invalid,create_sampler(7,11,&sampler));
    query.queryCount=0;try std.testing.expectError(error.Invalid,create_query_pool(7,11,&query));
    event.flags=4;try std.testing.expectError(error.Invalid,create_event(7,11,&event));
    cache.initialDataSize=4097;try std.testing.expectError(error.Limit,create_pipeline_cache(7,11,&cache));
    try std.testing.expectError(error.Invalid,destroy(7,0,43));
    try std.testing.expectError(error.Invalid,event_operation(7,11,42));
    try std.testing.expectError(error.Invalid,query_command(9,11,130,3,4,1,@as(u64,1)<<40));
    try std.testing.expectError(error.Invalid,merge_caches(7,11,&.{13,13}));
}
test "query/cache replies validate entire bounded output before publication" {
    var reply=[_]u8{0} ** 64;
    var target=[_]u8{0xaa} ** 32;
    std.mem.writeInt(u32,reply[0..4],49,.little);std.mem.writeInt(u64,reply[8..16],32,.little);
    @memset(reply[16..48],0x27);
    try std.testing.expectEqual(@as(i32,0),try decode_query_results(&reply,&target));
    try std.testing.expectEqualSlices(u8,&([_]u8{0x27} ** 32),&target);
    target=[_]u8{0xaa} ** 32;
    std.mem.writeInt(i32,reply[4..8],-4,.little);try std.testing.expectEqual(@as(i32,-4),try decode_query_results(&reply,&target));
    try std.testing.expectEqualSlices(u8,&([_]u8{0xaa} ** 32),&target);
    try std.testing.expectError(error.Corrupt,decode_query_results(reply[0..47],&target));
    @memset(&reply,0);std.mem.writeInt(u32,reply[0..4],63,.little);std.mem.writeInt(u64,reply[8..16],1,.little);std.mem.writeInt(u64,reply[16..24],32,.little);std.mem.writeInt(u64,reply[24..32],32,.little);@memset(reply[32..64],0x19);
    const result=try decode_cache_data(&reply,&target);try std.testing.expectEqual(@as(u64,32),result.size);
    try std.testing.expectEqualSlices(u8,&([_]u8{0x19} ** 32),&target);
    try std.testing.expectError(error.Corrupt,decode_cache_data(reply[0..63],&target));
    std.mem.writeInt(u64,reply[24..32],0,.little);std.mem.writeInt(u64,reply[16..24],1<<30,.little);const count_result=try decode_cache_data(&reply,null);try std.testing.expectEqual(@as(u64,1<<30),count_result.size);
}
