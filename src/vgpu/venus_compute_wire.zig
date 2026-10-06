//! Owned core command packets. Caller validates device capabilities, profiles and GPU lifetimes.
const std = @import("std");
const render_wire = @import("venus_render_wire.zig");
/// Owned 8192-byte packet, no allocation. Each writer is exclusively borrowed while encoding.
pub const writer_t = render_wire.writer_t;
/// Maximum descriptor sets per pipeline layout; no heap allocations.
pub const MaxSets: usize = 16;
/// Maximum dynamic offsets across the bounded 16-set, 64-element set profiles.
pub const MaxDynamicOffsets: usize = 1024;
fn append(writer: *writer_t, comptime scalar_t: type, value: scalar_t) void {
    writer.put(scalar_t, value) catch unreachable;
}
/// Encode pipeline binding. [in] translated command/pipeline IDs nonzero; bind_point core0/1.
/// Returns owned packet or Invalid; no ownership transfer, retained pointers or shared state.
/// Caller validates pipeline kind, same-device identity and recording state before submitting.
pub fn bind_pipeline(command_id: u64, pipeline_id: u64, bind_point: u32) !writer_t {
    if (command_id == 0 or pipeline_id == 0 or bind_point > 1) return error.Invalid;
    var writer = writer_t{};
    writer.header(93, command_id) catch unreachable;
    append(&writer, u32, bind_point);
    append(&writer, u64, pipeline_id);
    return writer;
}
/// Encode descriptor binding. [in] nonzero translated command/layout/set IDs, arrays borrowed for call.
/// [in] bind_point core0/1, first_set and arrays bounded by this module's fixed profile.
/// Returns owned packet or Invalid/Limit; complete capacity proof precedes writes; no allocations.
/// Caller validates structural compatibility and exact dynamic count/alignment before submission.
pub fn bind_descriptor_sets(command_id: u64, layout_id: u64, bind_point: u32, first_set: u32, set_ids: []const u64, dynamic_offsets: []const u32) !writer_t {
    if (command_id == 0 or layout_id == 0 or bind_point > 1 or first_set >= MaxSets) return error.Invalid;
    if (set_ids.len > MaxSets - first_set or dynamic_offsets.len > MaxDynamicOffsets) return error.Limit;
    if (set_ids.len == 0 and dynamic_offsets.len != 0) return error.Invalid;
    for (set_ids) |id| if (id == 0) return error.Invalid;
    // 56 + 16*8 + 1024*4 = 4280, strictly within the shared packet capacity.
    var writer = writer_t{};
    writer.header(103, command_id) catch unreachable;
    append(&writer, u32, bind_point);
    append(&writer, u64, layout_id);
    append(&writer, u32, first_set);
    append(&writer, u32, @intCast(set_ids.len));
    append(&writer, u64, set_ids.len);
    for (set_ids) |id| append(&writer, u64, id);
    append(&writer, u32, @intCast(dynamic_offsets.len));
    append(&writer, u64, dynamic_offsets.len);
    for (dynamic_offsets) |offset| append(&writer, u32, offset);
    return writer;
}
/// Encode push bytes. [in] nonzero translated command/layout IDs; values borrowed, never retained.
/// [in] stage_flags positive core mask; offset and byte length four-aligned within core128 bytes.
/// Returns owned packet or Invalid/Limit; no allocation, locking or shared mutable state.
/// Caller validates declared range coverage and compatibility with the eventual bound pipeline.
pub fn push_constants(command_id: u64, layout_id: u64, stage_flags: u32, offset: u32, values: []const u8) !writer_t {
    if (command_id == 0 or layout_id == 0 or stage_flags == 0 or stage_flags & ~@as(u32, 0x3f) != 0 or offset % 4 != 0 or values.len == 0 or values.len % 4 != 0) return error.Invalid;
    if (offset > 128 or values.len > 128 - offset) return error.Limit;
    var writer = writer_t{};
    writer.header(132, command_id) catch unreachable;
    append(&writer, u64, layout_id);
    append(&writer, u32, stage_flags);
    append(&writer, u32, offset);
    append(&writer, u32, @intCast(values.len));
    append(&writer, u64, values.len);
    @memcpy(writer.bytes[writer.used..][0..values.len], values);
    writer.used += values.len;
    return writer;
}
/// Encode dispatch groups including zero no-op groups. [in] translated command ID nonzero.
/// Returns owned packet or Invalid; no pointer retention/allocation, safe for distinct writers.
/// Caller validates counts against actual device limits and bound pipeline/descriptor state.
pub fn dispatch(command_id: u64, groups: [3]u32) !writer_t {
    if (command_id == 0) return error.Invalid;
    var writer = writer_t{};
    writer.header(110, command_id) catch unreachable;
    for (groups) |group| append(&writer, u32, group);
    return writer;
}

// Test-only fixtures.
extern fn venus_compute_test_bind_pipeline(u32, [*]u8) usize;
extern fn venus_compute_test_bind_sets(u32, u32, usize, [*]const u64, usize, [*]const u32, [*]u8) usize;
extern fn venus_compute_test_push(u32, u32, usize, [*]const u8, [*]u8) usize;
extern fn venus_compute_test_dispatch([*]const u32, [*]u8) usize;
fn compare(writer: writer_t, bytes: []const u8) !void {
    try std.testing.expectEqual(bytes.len, writer.used);
    try std.testing.expectEqualSlices(u8, bytes, writer.bytes[0..writer.used]);
}
test "generated oracle command packets ordinary empty and maximum" {
    var bytes: [8192]u8 = undefined;
    for ([_]u32{ 0, 1 }) |point| {
        const count = venus_compute_test_bind_pipeline(point, &bytes);
        try compare(try bind_pipeline(7, 42, point), bytes[0..count]);
    }
    var ids: [16]u64 = undefined;
    for (&ids, 0..) |*id, index| id.* = 42 + index;
    var offsets: [1024]u32 = undefined;
    for (&offsets, 0..) |*offset, index| offset.* = @intCast(index * 256);
    for ([_]usize{ 0, 1, 16 }) |count| {
        const dynamic_count: usize = if (count == 16) 1024 else count;
        const size = venus_compute_test_bind_sets(1, 0, count, &ids, dynamic_count, &offsets, &bytes);
        try compare(try bind_descriptor_sets(7, 43, 1, 0, ids[0..count], offsets[0..dynamic_count]), bytes[0..size]);
    }
    var values: [128]u8 = undefined;
    for (&values, 0..) |*value, index| value.* = @intCast(index);
    for ([_]usize{ 4, 128 }) |count| {
        const size = venus_compute_test_push(0x20, 0, count, &values, &bytes);
        try compare(try push_constants(7, 43, 0x20, 0, values[0..count]), bytes[0..size]);
    }
    for ([_][3]u32{ .{ 0, 0, 0 }, .{ 64, 1, 1 }, .{ 0xffffffff, 0xffffffff, 0xffffffff } }) |groups| {
        const size = venus_compute_test_dispatch(&groups, &bytes);
        try compare(try dispatch(7, groups), bytes[0..size]);
    }
}
test "invalid identities enums counts and push ranges fail before encoding" {
    try std.testing.expectError(error.Invalid, bind_pipeline(0, 42, 0));
    try std.testing.expectError(error.Invalid, bind_pipeline(7, 0, 0));
    try std.testing.expectError(error.Invalid, bind_pipeline(7, 42, 2));
    const ids = [_]u64{42} ** 17;
    const offsets = [_]u32{0} ** 1025;
    try std.testing.expectError(error.Invalid, bind_descriptor_sets(0, 43, 1, 0, &.{}, &.{}));
    try std.testing.expectError(error.Invalid, bind_descriptor_sets(7, 0, 1, 0, &.{}, &.{}));
    try std.testing.expectError(error.Invalid, bind_descriptor_sets(7, 43, 2, 0, &.{}, &.{}));
    try std.testing.expectError(error.Invalid, bind_descriptor_sets(7, 43, 1, 16, &.{}, &.{}));
    try std.testing.expectError(error.Limit, bind_descriptor_sets(7, 43, 1, 0, &ids, &.{}));
    try std.testing.expectError(error.Limit, bind_descriptor_sets(7, 43, 1, 0, ids[0..1], &offsets));
    try std.testing.expectError(error.Invalid, bind_descriptor_sets(7, 43, 1, 0, &.{}, offsets[0..1]));
    try std.testing.expectError(error.Invalid, bind_descriptor_sets(7, 43, 1, 0, &.{0}, &.{}));
    const values = [_]u8{0} ** 132;
    try std.testing.expectError(error.Invalid, push_constants(0, 43, 1, 0, values[0..4]));
    try std.testing.expectError(error.Invalid, push_constants(7, 0, 1, 0, values[0..4]));
    try std.testing.expectError(error.Invalid, push_constants(7, 43, 0, 0, values[0..4]));
    try std.testing.expectError(error.Invalid, push_constants(7, 43, 64, 0, values[0..4]));
    try std.testing.expectError(error.Invalid, push_constants(7, 43, 1, 1, values[0..4]));
    try std.testing.expectError(error.Invalid, push_constants(7, 43, 1, 0, values[0..0]));
    try std.testing.expectError(error.Invalid, push_constants(7, 43, 1, 0, values[0..3]));
    try std.testing.expectError(error.Limit, push_constants(7, 43, 1, 132, values[0..4]));
    try std.testing.expectError(error.Limit, push_constants(7, 43, 1, 0, &values));
    try std.testing.expectError(error.Invalid, dispatch(0, .{ 1, 1, 1 }));
}
