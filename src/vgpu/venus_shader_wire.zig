//! Bounded full-size shader packets for real DXVK builtin and application shaders.
const std = @import("std");
const c = @cImport({ @cInclude("vulkan/vulkan.h"); });
/// Maximum SPIR-V byte extent; fixed caller-owned storage, no allocation.
pub const MaxCodeBytes: usize = 65536;
/// Maximum complete create packet, including native command and owner fields.
pub const MaxPacketBytes: usize = MaxCodeBytes + 80;
/// Owned complete packet; borrowed source words are never retained. Independent
/// writers are thread safe. Caller owns host module lifetime after exact ACK.
pub const writer_t = struct {
    /// Initialized byte prefix and unused bounded scratch, valid for writer lifetime.
    bytes: [MaxPacketBytes]u8 = undefined,
    /// Exact initialized length, <=MaxPacketBytes.
    used: usize = 0,
    fn put(self: *writer_t, comptime value_t: type, value: value_t) void {
        std.debug.assert(self.used <= self.bytes.len and @sizeOf(value_t) <= self.bytes.len - self.used);
        std.mem.writeInt(value_t, self.bytes[self.used..][0..@sizeOf(value_t)], value, .little);
        self.used += @sizeOf(value_t);
    }
};
/// [in] borrowed accessible canonical native info and code words, IDs nonzero
/// resolved host owners. [out] Exact owned create59 packet; Invalid native shape,
/// SPIR-V header or instruction bounds, Limit code>65536 checked before traversal.
/// Host validates semantic shader capabilities against enabled device features.
/// No heap, pointers retained, shared state or partial packet publication.
pub fn create_shader(info: *const c.VkShaderModuleCreateInfo, device: u64, module: u64) !writer_t {
    if (device == 0 or module == 0 or info.sType != c.VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO or
        info.pNext != null or info.flags != 0 or info.pCode == null or info.codeSize < 20 or info.codeSize % 4 != 0) return error.Invalid;
    if (info.codeSize > MaxCodeBytes) return error.Limit;
    const code_address = @as(*align(1) const usize, @ptrCast(&info.pCode)).*;
    if (code_address == 0 or code_address % @alignOf(u32) != 0) return error.Invalid;
    const code: [*]const u32 = @ptrFromInt(code_address);
    const words = code[0..info.codeSize / 4];
    if (words[0] != 0x07230203 or words[1] & 0xff0000ff != 0 or
        (words[1] >> 16) & 0xff != 1 or (words[1] >> 8) & 0xff > 6 or
        words[3] == 0 or words[4] != 0) return error.Invalid;
    var cursor: usize = 5;
    while (cursor < words.len) {
        const count = words[cursor] >> 16;
        if (count == 0 or count > words.len - cursor) return error.Invalid;
        cursor += count;
    }
    var writer: writer_t = .{};
    writer.put(u32, 59); writer.put(u32, 1); writer.put(u64, device);
    writer.put(u64, 1); writer.put(u32, c.VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    writer.put(u64, 0); writer.put(u32, 0); writer.put(u64, info.codeSize); writer.put(u64, words.len);
    for (words) |word| writer.put(u32, word);
    writer.put(u64, 0); writer.put(u64, 1); writer.put(u64, module);
    return writer;
}
// Test-only fixtures.
extern fn venus_shader_test_encode(*const c.VkShaderModuleCreateInfo, [*]u8, usize) usize;
test "maximum and ordinary full-size shader packets match pinned encoder and retain no source pointer" {
    var words = [_]u32{0x00010000} ** (MaxCodeBytes / 4);
    words[0] = 0x07230203; words[1] = 0x00010600; words[2] = 0; words[3] = 1; words[4] = 0;
    var info = c.VkShaderModuleCreateInfo{.sType=c.VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=words.len*4,.pCode=&words};
    var expected: [MaxPacketBytes]u8 = undefined;
    for ([_]usize{20,8192,MaxCodeBytes}) |size| {
        info.codeSize=size;
        const packet=try create_shader(&info,7,11);
        const length=venus_shader_test_encode(&info,&expected,expected.len);
        try std.testing.expectEqual(length,packet.used);try std.testing.expectEqualSlices(u8,expected[0..length],packet.bytes[0..packet.used]);
    }
    info.codeSize=MaxCodeBytes+4;info.pCode=@ptrFromInt(4);
    try std.testing.expectError(error.Limit,create_shader(&info,7,11));
}
test "full-size shader malformed headers instructions owners and alignment reject before encoding" {
    var words=[_]u32{0x07230203,0x00010000,0,1,0,0x00010000};
    var info=c.VkShaderModuleCreateInfo{.sType=c.VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=24,.pCode=&words};
    try std.testing.expectError(error.Invalid,create_shader(&info,0,11));try std.testing.expectError(error.Invalid,create_shader(&info,7,0));
    info.flags=1;try std.testing.expectError(error.Invalid,create_shader(&info,7,11));info.flags=0;
    words[0]=0;try std.testing.expectError(error.Invalid,create_shader(&info,7,11));words[0]=0x07230203;
    words[1]=0x00010700;try std.testing.expectError(error.Invalid,create_shader(&info,7,11));words[1]=0x00010000;
    words[3]=0;try std.testing.expectError(error.Invalid,create_shader(&info,7,11));words[3]=1;
    words[4]=1;try std.testing.expectError(error.Invalid,create_shader(&info,7,11));words[4]=0;
    words[5]=0;try std.testing.expectError(error.Invalid,create_shader(&info,7,11));words[5]=0x00020000;try std.testing.expectError(error.Invalid,create_shader(&info,7,11));
    info.codeSize=19;try std.testing.expectError(error.Invalid,create_shader(&info,7,11));info.codeSize=24;
    info.pCode=null;try std.testing.expectError(error.Invalid,create_shader(&info,7,11));
    @as(*align(1) usize,@ptrCast(&info.pCode)).*=5;try std.testing.expectError(error.Invalid,create_shader(&info,7,11));
}
