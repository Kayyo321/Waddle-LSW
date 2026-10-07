//! Shared bounded shader-specialization and graphics/compute pipeline-chain serialization.
const std = @import("std");
const c = @cImport({
    @cInclude("vulkan/vulkan.h");
});
/// Maximum specialization map records, checked before traversal; immutable/no ownership.
pub const MaxSpecializationEntries = 32;
/// Maximum copied opaque specialization bytes; immutable/no ownership.
pub const MaxSpecializationData = 1024;
/// Maximum specialization packet contribution, including pointer/count/size/blob padding.
pub const MaxSpecializationBytes = 8 + 4 + 8 + 16 * MaxSpecializationEntries + 8 + 8 + MaxSpecializationData;
/// Owned chain topology of at most2 recognized nodes; addresses borrow caller records
/// until immediate encoding, no heap allocation or independent native lifetime.
pub const chain_t = struct {
    /// Initialized forward count0..2.
    count: usize = 0,
    /// Borrowed accessible aligned SDK addresses, valid only during caller's synchronous call.
    addresses: [2]usize = undefined,
    /// Owned recognized tags in forward order.
    tags: [2]u32 = undefined,
    /// True when a dynamic rendering format signature is present.
    rendering: bool = false,
};
fn address(field: *const anyopaque) usize {
    return @as(*align(1) const usize, @ptrCast(field)).*;
}
fn elements(comptime value_t: type, field: *const anyopaque, count: usize) ![]const value_t {
    if (count == 0) return &.{};
    const bits = address(field);
    if (bits == 0 or bits % @alignOf(value_t) != 0) return error.Invalid;
    const pointer: [*]const value_t = @ptrFromInt(bits);
    return pointer[0..count];
}
/// Collect bounded optional pipeline chain, recognizing RenderingCreateInfo and Flags2.
/// [in] first nullable borrowed accessible SDK chain; allow_rendering false for compute.
/// Caller owns immutable full records/arrays until encoding. Invalid for unknown/duplicate/
/// over-quota/misaligned nodes or malformed format arrays. No mutation/allocations/locks;
/// returned addresses borrow only caller call-lifetime, independent calls thread-safe.
pub fn collect_chain(first: ?*const anyopaque, allow_rendering: bool) !chain_t {
    var result: chain_t = .{};
    var next: usize = if (first) |pointer| @intFromPtr(pointer) else 0;
    while (next != 0) {
        if (result.count == 2 or next % @alignOf(c.VkBaseInStructure) != 0) return error.Invalid;
        const header: *const c.VkBaseInStructure = @ptrFromInt(next);
        for (result.tags[0..result.count]) |tag| if (tag == header.sType) return error.Invalid;
        switch (header.sType) {
            c.VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO => {
                if (!allow_rendering) return error.Invalid;
                const info: *const c.VkPipelineRenderingCreateInfo = @ptrFromInt(next);
                if (info.colorAttachmentCount > 8) return error.Invalid;
                _ = try elements(c.VkFormat, @ptrCast(&info.pColorAttachmentFormats), info.colorAttachmentCount);
                result.rendering = true;
            },
            c.VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO => {},
            else => return error.Invalid,
        }
        result.addresses[result.count] = next;
        result.tags[result.count] = header.sType;
        result.count += 1;
        next = address(@ptrCast(&header.pNext));
    }
    return result;
}
/// Encode collected chain in pinned forward-header/reverse-payload order. [in,out] writer
/// borrowed exclusive bounded writer with put(type,value)!void; [in] chain and referenced
/// SDK records immutable borrowed through return. Full64-bit Flags2 preserved, never truncated.
/// Caller validates enabled maintenance5 and actual effective flag semantics. Returns Limit
/// for packet exhaustion, Invalid for changed input topology. No allocations/retained pointers.
pub fn encode_chain(writer: anytype, chain: *const chain_t) !void {
    if (chain.count > 2) return error.Invalid;
    for (chain.tags[0..chain.count], chain.addresses[0..chain.count], 0..) |tag, bits, index| {
        if (bits == 0 or bits % @alignOf(c.VkBaseInStructure) != 0) return error.Invalid;
        const header: *const c.VkBaseInStructure = @ptrFromInt(bits);
        const expected_next = if (index + 1 < chain.count) chain.addresses[index + 1] else 0;
        if (header.sType != tag or address(@ptrCast(&header.pNext)) != expected_next) return error.Invalid;
        try writer.put(u64, 1);
        try writer.put(u32, tag);
    }
    try writer.put(u64, 0);
    var remaining = chain.count;
    while (remaining != 0) {
        remaining -= 1;
        switch (chain.tags[remaining]) {
            c.VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO => {
                const info: *const c.VkPipelineCreateFlags2CreateInfo = @ptrFromInt(chain.addresses[remaining]);
                try writer.put(u64, info.flags);
            },
            c.VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO => {
                const info: *const c.VkPipelineRenderingCreateInfo = @ptrFromInt(chain.addresses[remaining]);
                if (info.colorAttachmentCount > 8) return error.Invalid;
                const formats = try elements(c.VkFormat, @ptrCast(&info.pColorAttachmentFormats), info.colorAttachmentCount);
                try writer.put(u32, info.viewMask);
                try writer.put(u32, info.colorAttachmentCount);
                try writer.put(u64, formats.len);
                for (formats) |format| try writer.put(u32, format);
                try writer.put(u32, info.depthAttachmentFormat);
                try writer.put(u32, info.stencilAttachmentFormat);
            },
            else => return error.Invalid,
        }
    }
}
/// Encode optional native shader specialization into exclusive bounded writer. Inputs borrow
/// full accessible immutable SDK record/map/data through return. Invalid for bounds/count,
/// duplicate constantIDs or malformed pointers; Limit for packet exhaustion. Copies raw bytes
/// with wire padding, never SDK padding; no allocations/retention/global state, thread-safe.
pub fn encode_specialization(writer: anytype, pointer: [*c]const c.VkSpecializationInfo) !void {
    try writer.put(u64, if (pointer != null) 1 else 0);
    if (pointer == null) return;
    const info = pointer[0];
    if (info.mapEntryCount > MaxSpecializationEntries or info.dataSize > MaxSpecializationData) return error.Invalid;
    const entries = try elements(c.VkSpecializationMapEntry, @ptrCast(&info.pMapEntries), info.mapEntryCount);
    const data = try elements(u8, @ptrCast(&info.pData), info.dataSize);
    for (entries, 0..) |entry, index| {
        if (entry.offset > info.dataSize or entry.size > info.dataSize - entry.offset) return error.Invalid;
        for (entries[0..index]) |previous| if (previous.constantID == entry.constantID) return error.Invalid;
    }
    try writer.put(u32, info.mapEntryCount);
    try writer.put(u64, entries.len);
    for (entries) |entry| {
        try writer.put(u32, entry.constantID);
        try writer.put(u32, entry.offset);
        try writer.put(u64, entry.size);
    }
    try writer.put(u64, info.dataSize);
    try writer.put(u64, data.len);
    for (data) |byte| try writer.put(u8, byte);
    var padding = (4 - data.len % 4) % 4;
    while (padding != 0) : (padding -= 1) try writer.put(u8, 0);
}
