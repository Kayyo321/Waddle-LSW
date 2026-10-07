//! Actual host memory-requirement and format-property extension queries. No
//! hardware requirements are synthesized from legacy queries or compile-time values.
const std=@import("std");
const render=@import("venus_render_wire.zig");
const c=@cImport({@cInclude("vulkan/vulkan.h");});
/// Owned bounded query packet; no retained input pointers or heap ownership.
pub const writer_t=render.writer_t;
const reader_t=struct {
    bytes:[]const u8,
    offset:usize=0,
    fn read(self:*reader_t,comptime value_t:type) !value_t {
        const size=@sizeOf(value_t);
        if(self.offset>self.bytes.len or size>self.bytes.len-self.offset)return error.Corrupt;
        const value=std.mem.readInt(value_t,self.bytes[self.offset..][0..size],.little);self.offset+=size;return value;
    }
    fn expect(self:*reader_t,comptime value_t:type,value:value_t) !void {if(try self.read(value_t)!=value)return error.Corrupt;}
};
/// Copied actual native host requirements and optional dedicated-allocation flags.
/// Caller owns values; no output-chain pointers or shared storage survive decoding.
pub const memory_result_t=struct {
    /// Actual byte size/alignment/type bits, validated canonical nonempty values.
    requirements:c.VkMemoryRequirements,
    /// Actual host preference, zero only when node was not requested.
    preferred:u32=0,
    /// Actual host mandatory dedicated flag, zero only when node was not requested.
    required:u32=0,
};
/// [in] device/resource resolved live host identities; image selects144 versus145;
/// dedicated requests actual dedicated node. [out] Owned exact query or Invalid
/// zero IDs/Limit capacity. No allocation/retention; caller owns resource lifetime.
pub fn memory_requirements2(device:u64,resource:u64,image:bool,dedicated:bool) !writer_t {
    if(resource==0)return error.Invalid;
    var writer:writer_t=.{};try writer.header(if(image) 144 else 145,device);
    try writer.put(u64,1);try writer.put(u32,if(image)c.VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 else c.VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2);try writer.put(u64,0);try writer.put(u64,resource);
    try writer.put(u64,1);try writer.put(u32,c.VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2);
    try writer.put(u64,if(dedicated) 1 else 0);
    if(dedicated){try writer.put(u32,c.VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);try writer.put(u64,0);}
    return writer;
}
/// [in] completed immutable host reply, matching image/dedicated request shape.
/// [out] Copied actual requirements/flags or Corrupt identity/topology/truncation/
/// noncanonical Boolean/zero or nonpower-of-two alignment/empty type bits.
/// No allocation/retention or caller mutation; thread-safe on independent inputs.
pub fn decode_memory(reply:[]const u8,image:bool,dedicated:bool) !memory_result_t {
    var reader=reader_t{.bytes=reply};try reader.expect(u32,if(image)144 else 145);try reader.expect(u64,1);try reader.expect(u32,c.VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2);try reader.expect(u64,if(dedicated)1 else 0);
    var value=memory_result_t{.requirements=std.mem.zeroes(c.VkMemoryRequirements)};
    if(dedicated){try reader.expect(u32,c.VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS);try reader.expect(u64,0);value.preferred=try reader.read(u32);value.required=try reader.read(u32);if(value.preferred>1 or value.required>1)return error.Corrupt;}
    value.requirements.size=try reader.read(u64);value.requirements.alignment=try reader.read(u64);value.requirements.memoryTypeBits=try reader.read(u32);
    if(value.requirements.size==0 or value.requirements.alignment==0 or value.requirements.alignment & (value.requirements.alignment-1)!=0 or value.requirements.memoryTypeBits==0)return error.Corrupt;
    return value;
}
/// Actual host format feature records, copied values with no output pointers.
pub const format_result_t=struct {
    /// Original32-bit Vulkan format support bits.
    core:c.VkFormatProperties,
    /// Actual64-bit feature values, initialized only when requested.
    extended:[3]u64=[_]u64{0} ** 3,
};
/// [in] physical resolved nonzero host ID, format exact native enumeration;
/// extended requests actual VkFormatProperties3. [out] Exact149 query packet or
/// Invalid identity/Limit capacity. No synthesized flags, allocation or retention.
pub fn format_properties2(physical:u64,format:u32,extended:bool) !writer_t {
    var writer:writer_t=.{};try writer.header(149,physical);try writer.put(u32,format);try writer.put(u64,1);try writer.put(u32,c.VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2);try writer.put(u64,if(extended)1 else 0);
    if(extended){try writer.put(u32,c.VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3);try writer.put(u64,0);}
    return writer;
}
/// [in] immutable complete149 reply and matching extended flag. [out] Actual
///32/64-bit host support or Corrupt identity/topology/truncation. All flag bits
/// preserved as provided by host; caller filters unsupported guest operations.
/// Allocation-free, no retained pointers or output writes; independent calls safe.
pub fn decode_format(reply:[]const u8,extended:bool) !format_result_t {
    var reader=reader_t{.bytes=reply};try reader.expect(u32,149);try reader.expect(u64,1);try reader.expect(u32,c.VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2);try reader.expect(u64,if(extended)1 else 0);
    var result=format_result_t{.core=std.mem.zeroes(c.VkFormatProperties)};
    if(extended){try reader.expect(u32,c.VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3);try reader.expect(u64,0);for(&result.extended) |*value| value.*=try reader.read(u64);}
    result.core.linearTilingFeatures=try reader.read(u32);result.core.optimalTilingFeatures=try reader.read(u32);result.core.bufferFeatures=try reader.read(u32);return result;
}

// Test-only fixtures.
extern fn venus_requirements2_test_query(u32,u32,[*]u8) usize;
extern fn venus_requirements2_test_reply(u32,u32,[*]u8) usize;
test "requirements2 exact partial queries and actual mandatory dedicated results match pinned sources" {
    for([_]bool{false,true}) |image| for([_]bool{false,true}) |dedicated| {
        const opcode:@TypeOf(@as(u32,0))=if(image)144 else 145;
        var expected:[4096]u8=undefined;
        const request=try memory_requirements2(7,11,image,dedicated);
        const query_bytes=venus_requirements2_test_query(opcode,@intFromBool(dedicated),&expected);
        try std.testing.expectEqual(query_bytes,request.used);try std.testing.expectEqualSlices(u8,expected[0..query_bytes],request.bytes[0..request.used]);
        const reply_bytes=venus_requirements2_test_reply(opcode,@intFromBool(dedicated),&expected);
        const decoded=try decode_memory(expected[0..reply_bytes],image,dedicated);
        try std.testing.expectEqual(@as(u64,4096),decoded.requirements.size);try std.testing.expectEqual(@as(u64,256),decoded.requirements.alignment);try std.testing.expectEqual(@as(u32,31),decoded.requirements.memoryTypeBits);try std.testing.expectEqual(@as(u32,@intFromBool(dedicated)),decoded.required);
        for(0..reply_bytes) |length| try std.testing.expectError(error.Corrupt,decode_memory(expected[0..length],image,dedicated));
        try std.testing.expectError(error.Corrupt,decode_memory(expected[0..reply_bytes],!image,dedicated));
        std.mem.writeInt(u64,expected[reply_bytes-12..][0..8],3,.little);try std.testing.expectError(error.Corrupt,decode_memory(expected[0..reply_bytes],image,dedicated));
    };
    try std.testing.expectError(error.Invalid,memory_requirements2(0,11,false,false));try std.testing.expectError(error.Invalid,memory_requirements2(7,0,false,false));
}
test "format2 and format3 preserve actual high64-bit flags and reject every truncated topology" {
    for([_]bool{false,true}) |extended| {
        var expected:[4096]u8=undefined;
        const query=try format_properties2(7,37,extended);
        const query_bytes=venus_requirements2_test_query(149,@intFromBool(extended),&expected);try std.testing.expectEqual(query_bytes,query.used);try std.testing.expectEqualSlices(u8,expected[0..query_bytes],query.bytes[0..query.used]);
        const bytes=venus_requirements2_test_reply(149,@intFromBool(extended),&expected);
        const result=try decode_format(expected[0..bytes],extended);
        try std.testing.expectEqual(@as(u32,1),result.core.linearTilingFeatures);try std.testing.expectEqual(@as(u32,2),result.core.optimalTilingFeatures);try std.testing.expectEqual(@as(u32,4),result.core.bufferFeatures);
        if(extended)try std.testing.expectEqualSlices(u64,&.{@as(u64,1)<<40,@as(u64,1)<<41,@as(u64,1)<<42},&result.extended);
        for(0..bytes) |length| try std.testing.expectError(error.Corrupt,decode_format(expected[0..length],extended));
        try std.testing.expectError(error.Corrupt,decode_format(expected[0..bytes],!extended));
    }
}
