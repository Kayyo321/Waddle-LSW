const std = @import("std");
const FrameBytes: usize = 64;
const ControlMagic: u32 = 0x57564331;

/// Private C ABI decoded value; caller-owned, no pointers/allocations. Integers
/// are host values, not a native wire layout. All fields must pass valid_fields.
const venus_control_t = extern struct {
    kind: u32,
    reason: u32,
    session_id: u64,
    mapping_bytes: u64,
    capacity: u32,
};

fn valid_fields(control: *const venus_control_t) bool {
    if (control.kind < 1 or control.kind > 4 or control.session_id == 0) return false;
    if (control.kind == 4) {
        if (control.reason < 1 or control.reason > 4) return false;
    } else if (control.reason != 0) return false;
    if (control.mapping_bytes < 4096 or control.mapping_bytes > 1073741824 or
        control.mapping_bytes & (control.mapping_bytes - 1) != 0) return false;
    if (control.capacity < 64 or control.capacity > 16777216 or
        control.capacity & (control.capacity - 1) != 0) return false;
    return 64 + 2 * (192 + @as(u64, control.capacity)) <= control.mapping_bytes;
}

/// out: nullable control, zeroed on error, disjoint from frame; in: nullable
/// private immutable frame[length], length exactly 64. Returns 0 success,
/// -1 null arguments, -2 corrupt bytes/length. Pure, allocation-free, thread-safe
/// on disjoint outputs; untrusted integer parsing uses bounds-checked slices.
export fn venus_control_decode(control: ?*venus_control_t, frame: ?[*]const u8, length: usize) c_int {
    const output = control orelse return -1;
    output.* = std.mem.zeroes(venus_control_t);
    const source = frame orelse return -1;
    if (length != FrameBytes) return -2;
    const bytes = source[0..FrameBytes];
    if (std.mem.readInt(u32, bytes[0..4], .little) != ControlMagic or
        std.mem.readInt(u32, bytes[4..8], .little) != 1) return -2;
    for (bytes[36..64]) |byte| if (byte != 0) return -2;
    const value = venus_control_t{
        .kind = std.mem.readInt(u32, bytes[8..12], .little),
        .reason = std.mem.readInt(u32, bytes[12..16], .little),
        .session_id = std.mem.readInt(u64, bytes[16..24], .little),
        .mapping_bytes = std.mem.readInt(u64, bytes[24..32], .little),
        .capacity = std.mem.readInt(u32, bytes[32..36], .little),
    };
    if (!valid_fields(&value)) return -2;
    output.* = value;
    return 0;
}

/// in: nullable immutable validated private control, disjoint from output;
/// out: nullable private frame[length], unchanged on error. Length exactly 64.
/// Returns 0 success, -1 null/invalid fields/length. Pure, allocation-free,
/// thread-safe on disjoint outputs; no native struct bytes transmitted.
export fn venus_control_encode(control: ?*const venus_control_t, frame: ?[*]u8, length: usize) c_int {
    const value = control orelse return -1;
    const output = frame orelse return -1;
    if (length != FrameBytes or !valid_fields(value)) return -1;
    const bytes = output[0..FrameBytes];
    @memset(bytes, 0);
    std.mem.writeInt(u32, bytes[0..4], ControlMagic, .little);
    std.mem.writeInt(u32, bytes[4..8], 1, .little);
    std.mem.writeInt(u32, bytes[8..12], value.kind, .little);
    std.mem.writeInt(u32, bytes[12..16], value.reason, .little);
    std.mem.writeInt(u64, bytes[16..24], value.session_id, .little);
    std.mem.writeInt(u64, bytes[24..32], value.mapping_bytes, .little);
    std.mem.writeInt(u32, bytes[32..36], value.capacity, .little);
    return 0;
}

test "codec handles every valid kind and stop reason without native padding" {
    const bytes = try std.testing.allocator.alloc(u8, FrameBytes);
    defer std.testing.allocator.free(bytes);
    var value = venus_control_t{ .kind = 1, .reason = 0, .session_id = 0x1020304050607080, .mapping_bytes = 4096, .capacity = 64 };
    var decoded: venus_control_t = undefined;
    for (1..5) |kind| {
        value.kind = @intCast(kind);
        value.reason = if (kind == 4) 1 else 0;
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_control_encode, .{ &value, bytes.ptr, bytes.len }));
        try std.testing.expectEqual(@as(u8, 0x80), bytes[16]);
        try std.testing.expectEqual(@as(u8, 0x10), bytes[23]);
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_control_decode, .{ &decoded, bytes.ptr, bytes.len }));
        try std.testing.expectEqualDeep(value, decoded);
    }
    for (1..5) |reason| {
        value.reason = @intCast(reason);
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_control_encode, .{ &value, bytes.ptr, bytes.len }));
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_control_decode, .{ &decoded, bytes.ptr, bytes.len }));
        try std.testing.expectEqualDeep(value, decoded);
    }
}

test "every field error preserves output and rejects truncated or corrupt frames" {
    const bytes = try std.testing.allocator.alloc(u8, FrameBytes + 1);
    defer std.testing.allocator.free(bytes);
    const Good = venus_control_t{ .kind = 1, .reason = 0, .session_id = 1, .mapping_bytes = 4096, .capacity = 64 };
    var decoded: venus_control_t = undefined;
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_control_decode, .{ null, bytes.ptr, FrameBytes }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_control_decode, .{ &decoded, null, FrameBytes }));
    try std.testing.expectEqualDeep(std.mem.zeroes(venus_control_t), decoded);
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_control_encode, .{ null, bytes.ptr, FrameBytes }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_control_encode, .{ &Good, null, FrameBytes }));
    for ([_]usize{ 0, 63, 65, std.math.maxInt(usize) }) |length| {
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_control_encode, .{ &Good, bytes.ptr, length }));
        try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_control_decode, .{ &decoded, bytes.ptr, length }));
    }
    const Bad = [_]venus_control_t{
        .{ .kind = 0, .reason = 0, .session_id = 1, .mapping_bytes = 4096, .capacity = 64 },
        .{ .kind = 5, .reason = 0, .session_id = 1, .mapping_bytes = 4096, .capacity = 64 },
        .{ .kind = 1, .reason = 0, .session_id = 0, .mapping_bytes = 4096, .capacity = 64 },
        .{ .kind = 1, .reason = 1, .session_id = 1, .mapping_bytes = 4096, .capacity = 64 },
        .{ .kind = 4, .reason = 0, .session_id = 1, .mapping_bytes = 4096, .capacity = 64 },
        .{ .kind = 4, .reason = 5, .session_id = 1, .mapping_bytes = 4096, .capacity = 64 },
        .{ .kind = 1, .reason = 0, .session_id = 1, .mapping_bytes = 2147483648, .capacity = 64 },
        .{ .kind = 1, .reason = 0, .session_id = 1, .mapping_bytes = 0, .capacity = 64 },
        .{ .kind = 1, .reason = 0, .session_id = 1, .mapping_bytes = 4096, .capacity = 65 },
    };
    for (Bad) |bad| {
        @memset(bytes, 0x5a);
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_control_encode, .{ &bad, bytes.ptr, FrameBytes }));
        for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0x5a), byte);
    }
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_control_encode, .{ &Good, bytes.ptr, FrameBytes }));
    for (0..FrameBytes) |offset| {
        if (offset >= 16 and offset < 24) continue; // Other nonzero IDs remain valid codec values.
        bytes[offset] ^= 0x80;
        try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_control_decode, .{ &decoded, bytes.ptr, FrameBytes }));
        try std.testing.expectEqualDeep(std.mem.zeroes(venus_control_t), decoded);
        bytes[offset] ^= 0x80;
    }
    // Explicitly exercise the lower bounds, stop reason, and zero ID in decoding.
    for ([_]usize{ 8, 16, 25, 32 }) |offset| {
        const saved = bytes[offset];
        bytes[offset] = 0;
        try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_control_decode, .{ &decoded, bytes.ptr, FrameBytes }));
        bytes[offset] = saved;
    }
}
