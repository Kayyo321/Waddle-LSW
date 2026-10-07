const std = @import("std");
const resource_policy_t = @import("venus_receiver_bounds.zig");
const HeaderBytes: usize = 64;
const MaxPayload: u64 = 16777216;
const RequestMagic: u32 = 0x57565131;

/// Caller-owned C ABI host values; no pointers, allocations or wire padding.
const venus_request_t = extern struct {
    kind: u32,
    direction: u32,
    sequence: u64,
    payload_bytes: u32,
    status: u32,
    resource_id: u32,
    flags: u32,
    argument_zero: u64,
    argument_one: u64,
};

fn valid_fields(value: *const venus_request_t) bool {
    if (value.kind < 1 or value.kind > 13 or value.direction > 1 or value.sequence == 0 or value.payload_bytes > MaxPayload) return false;
    if (value.direction == 1) {
        if (value.status > 7 or value.resource_id != 0 or value.flags != 0 or value.argument_one != 0) return false;
        if (value.status != 0) return value.payload_bytes == 0 and value.argument_zero == 0;
        return switch (value.kind) {
            1 => value.payload_bytes == 160 and value.argument_zero == 0,
            13 => value.payload_bytes == 32 and value.argument_zero == 0,
            2, 9 => value.payload_bytes == 0 and value.argument_zero != 0,
            3, 6 => value.payload_bytes != 0 and value.argument_zero == 0,
            else => value.payload_bytes == 0 and value.argument_zero == 0,
        };
    }
    if (value.status != 0) return false;
    const plain = value.resource_id == 0 and value.flags == 0;
    const no_arguments = value.argument_zero == 0 and value.argument_one == 0;
    return switch (value.kind) {
        11 => plain and no_arguments and value.payload_bytes == 160,
        12 => plain and value.payload_bytes == 1216 and
            value.argument_zero >= 1 and value.argument_zero < 64 and value.argument_one != 0,
        13 => plain and value.payload_bytes == 0 and
            value.argument_zero != 0 and value.argument_one == 0,
        1, 8 => plain and no_arguments and value.payload_bytes == 0,
        2 => plain and no_arguments and value.payload_bytes >= 8 and value.payload_bytes & 3 == 0,
        3 => plain and value.payload_bytes == 0 and valid_range(value),
        4 => value.payload_bytes == 0 and resource_policy_t.venus_receiver_resource_request(value.resource_id, value.argument_zero, value.argument_one, value.flags) != 0,
        5 => resource_policy_t.venus_receiver_resource_slot(value.resource_id) != 0 and value.flags == 0 and no_arguments and value.payload_bytes == 0,
        7 => if (value.flags == 2) resource_policy_t.venus_receiver_resource_slot(value.resource_id) != 0 and
            value.argument_zero == 0 and value.argument_one == value.payload_bytes and
            value.payload_bytes >= 17 and value.payload_bytes <= 4096 else
            resource_policy_t.venus_receiver_resource_slot(value.resource_id) != 0 and value.flags == 0 and valid_range(value) and value.payload_bytes == value.argument_one,
        6 => resource_policy_t.venus_receiver_resource_slot(value.resource_id) != 0 and value.flags == 0 and valid_range(value) and
            value.payload_bytes == 0,
        9, 10 => plain and value.payload_bytes == 0 and value.argument_zero >= 1 and value.argument_zero < 64 and
            (if (value.kind == 9) value.argument_one == 0 else value.argument_one != 0),
        else => unreachable,
    };
}

fn valid_range(value: *const venus_request_t) bool {
    return value.argument_one != 0 and value.argument_one <= MaxPayload and value.argument_zero <= std.math.maxInt(u64) - value.argument_one;
}

/// out: nullable decoded record, zeroed on error; in: nullable private immutable
/// frame[length], exactly 64, disjoint from output. Returns 0 success, -1 null,
/// -2 malformed wire. Allocation-free/thread-safe; bounded parsing before lengths
/// escape to C; payload, live extents and session identity checked by caller.
export fn venus_request_decode(request: ?*venus_request_t, frame: ?[*]const u8, length: usize) c_int {
    const output = request orelse return -1;
    output.* = std.mem.zeroes(venus_request_t);
    const source = frame orelse return -1;
    if (length != HeaderBytes) return -2;
    const bytes = source[0..HeaderBytes];
    if (std.mem.readInt(u32, bytes[0..4], .little) != RequestMagic or std.mem.readInt(u32, bytes[4..8], .little) != 1) return -2;
    for (bytes[56..64]) |byte| if (byte != 0) return -2;
    const value = venus_request_t{
        .kind = std.mem.readInt(u32, bytes[8..12], .little),
        .direction = std.mem.readInt(u32, bytes[12..16], .little),
        .sequence = std.mem.readInt(u64, bytes[16..24], .little),
        .payload_bytes = std.mem.readInt(u32, bytes[24..28], .little),
        .status = std.mem.readInt(u32, bytes[28..32], .little),
        .resource_id = std.mem.readInt(u32, bytes[32..36], .little),
        .flags = std.mem.readInt(u32, bytes[36..40], .little),
        .argument_zero = std.mem.readInt(u64, bytes[40..48], .little),
        .argument_one = std.mem.readInt(u64, bytes[48..56], .little),
    };
    if (!valid_fields(&value)) return -2;
    output.* = value;
    return 0;
}

/// in: nullable immutable decoded host values; out: nullable disjoint private
/// frame[length], exactly 64, unchanged on error. Returns 0 success, -1 local
/// null/fields/length error; allocation-free/thread-safe, writes no payload.
export fn venus_request_encode(request: ?*const venus_request_t, frame: ?[*]u8, length: usize) c_int {
    const value = request orelse return -1;
    const destination = frame orelse return -1;
    if (length != HeaderBytes or !valid_fields(value)) return -1;
    const bytes = destination[0..HeaderBytes];
    @memset(bytes, 0);
    std.mem.writeInt(u32, bytes[0..4], RequestMagic, .little);
    std.mem.writeInt(u32, bytes[4..8], 1, .little);
    std.mem.writeInt(u32, bytes[8..12], value.kind, .little);
    std.mem.writeInt(u32, bytes[12..16], value.direction, .little);
    std.mem.writeInt(u64, bytes[16..24], value.sequence, .little);
    std.mem.writeInt(u32, bytes[24..28], value.payload_bytes, .little);
    std.mem.writeInt(u32, bytes[28..32], value.status, .little);
    std.mem.writeInt(u32, bytes[32..36], value.resource_id, .little);
    std.mem.writeInt(u32, bytes[36..40], value.flags, .little);
    std.mem.writeInt(u64, bytes[40..48], value.argument_zero, .little);
    std.mem.writeInt(u64, bytes[48..56], value.argument_one, .little);
    return 0;
}

fn good_request(kind: u32) venus_request_t {
    var value = std.mem.zeroes(venus_request_t);
    value.kind = kind;
    value.sequence = 0x1020304050607080;
    switch (kind) {
        2 => value.payload_bytes = 8,
        11 => value.payload_bytes = 160,
        12 => {
            value.payload_bytes = 1216;
            value.argument_zero = 1;
            value.argument_one = 1;
        },
        13 => value.argument_zero = 1,
        3 => value.argument_one = 4,
        4 => {
            value.resource_id = 2;
            value.flags = 1;
            value.argument_one = 4096;
        },
        5 => value.resource_id = 2,
        6, 7 => {
            value.resource_id = 65;
            value.argument_one = 4;
            if (kind == 7) value.payload_bytes = 4;
        },
        9, 10 => {
            value.argument_zero = 1;
            if (kind == 10) value.argument_one = 1;
        },
        else => {},
    }
    return value;
}

fn good_response(kind: u32, status: u32) venus_request_t {
    var value = std.mem.zeroes(venus_request_t);
    value.kind = kind;
    value.direction = 1;
    value.sequence = 1;
    value.status = status;
    if (status == 0) switch (kind) {
        1 => value.payload_bytes = 160,
        13 => value.payload_bytes = 32,
        2, 9 => value.argument_zero = 1,
        3, 6 => value.payload_bytes = 4,
        else => {},
    };
    return value;
}

test "all operation and status envelopes round trip without native padding" {
    const bytes = try std.testing.allocator.alloc(u8, HeaderBytes);
    defer std.testing.allocator.free(bytes);
    var decoded: venus_request_t = undefined;
    for (1..14) |kind| {
        const request = good_request(@intCast(kind));
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_request_encode, .{ &request, bytes.ptr, bytes.len }));
        try std.testing.expectEqual(@as(u8, 0x80), bytes[16]);
        try std.testing.expectEqual(@as(u8, 0x10), bytes[23]);
        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_request_decode, .{ &decoded, bytes.ptr, bytes.len }));
        try std.testing.expectEqualDeep(request, decoded);
        for (0..8) |status| {
            const response = good_response(@intCast(kind), @intCast(status));
            try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_request_encode, .{ &response, bytes.ptr, bytes.len }));
            try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_request_decode, .{ &decoded, bytes.ptr, bytes.len }));
            try std.testing.expectEqualDeep(response, decoded);
        }
    }
}

test "invalid fields and wire mutations preserve all output guarantees" {
    const bytes = try std.testing.allocator.alloc(u8, HeaderBytes);
    defer std.testing.allocator.free(bytes);
    var decoded: venus_request_t = undefined;
    const good = good_request(1);
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_request_decode, .{ null, bytes.ptr, bytes.len }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_request_decode, .{ &decoded, null, bytes.len }));
    try std.testing.expectEqualDeep(std.mem.zeroes(venus_request_t), decoded);
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_request_encode, .{ null, bytes.ptr, bytes.len }));
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_request_encode, .{ &good, null, bytes.len }));
    for ([_]usize{ 0, 63, 65, std.math.maxInt(usize) }) |length| {
        @memset(bytes, 0x5a);
        try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_request_encode, .{ &good, bytes.ptr, length }));
        for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0x5a), byte);
        try std.testing.expectEqual(@as(c_int, -2), @call(.never_inline, venus_request_decode, .{ &decoded, bytes.ptr, length }));
    }
    for (1..14) |kind| {
        for (0..2) |direction| {
            const base = if (direction == 0) good_request(@intCast(kind)) else good_response(@intCast(kind), 0);
            inline for (std.meta.fields(venus_request_t)) |field| {
                for ([_]u64{ 0, 1, 2, 3, 4, 7, 8, 9, 65, 66, 160, 4096, MaxPayload, MaxPayload + 1, std.math.maxInt(u32), std.math.maxInt(u64) }) |mutation| {
                    var value = base;
                    @field(value, field.name) = @truncate(mutation);
                    @memset(bytes, 0x5a);
                    const result = @call(.never_inline, venus_request_encode, .{ &value, bytes.ptr, bytes.len });
                    if (result == 0) {
                        try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_request_decode, .{ &decoded, bytes.ptr, bytes.len }));
                        try std.testing.expectEqualDeep(value, decoded);
                    } else {
                        try std.testing.expectEqual(@as(c_int, -1), result);
                        for (bytes) |byte| try std.testing.expectEqual(@as(u8, 0x5a), byte);
                    }
                }
            }
            try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_request_encode, .{ &base, bytes.ptr, bytes.len }));
            // Every byte is changed independently. Valid alternate values are
            // decoded faithfully; malformed values must zero the output.
            for (0..HeaderBytes) |offset| {
                bytes[offset] ^= 0x80;
                const result = @call(.never_inline, venus_request_decode, .{ &decoded, bytes.ptr, bytes.len });
                if (result != 0) {
                    try std.testing.expectEqual(@as(c_int, -2), result);
                    try std.testing.expectEqualDeep(std.mem.zeroes(venus_request_t), decoded);
                }
                bytes[offset] ^= 0x80;
            }
        }
    }
    var value = good_request(7);
    value.argument_zero = std.math.maxInt(u64) - 3;
    try std.testing.expectEqual(@as(c_int, -1), @call(.never_inline, venus_request_encode, .{ &value, bytes.ptr, bytes.len }));
    value.argument_zero -= 1;
    try std.testing.expectEqual(@as(c_int, 0), @call(.never_inline, venus_request_encode, .{ &value, bytes.ptr, bytes.len }));
}
