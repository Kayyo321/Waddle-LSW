//! Fixed owned command compatibility snapshots, independent of public pipeline-layout lifetimes.
const std = @import("std");
const profiles = @import("venus_icd_profiles.zig");
/// Maximum live command metadata owners. Caller reserves before native command creation.
pub const MaxCommands: usize = 64;
/// Copied push definition for one stage, independent of the creating layout token.
pub const push_profile_t = struct {
    /// Initialized normalized range count0..32.
    count: usize = 0,
    /// Owned complete range definition used for this stage's most recent writes.
    ranges: [profiles.MaxPushRanges]profiles.push_range_t = [_]profiles.push_range_t{.{}} ** profiles.MaxPushRanges,
    /// Initialized byte bitmap for bounded256 push bytes, no pointers or allocation.
    initialized: [4]u64 = .{ 0, 0, 0, 0 },
};
/// Owned command bindings, scrubbed after successful reset or native command retirement.
pub const command_profile_t = struct {
    /// Private bound pipeline token; caller publishes a lifetime reference after acknowledgment.
    pipeline: u64 = 0,
    /// Whether descriptor_layout contains a successfully recorded binding definition.
    descriptor_layout_ready: bool = false,
    /// Owned latest descriptor bind definition; compatible older bindings survive layout changes.
    descriptor_layout: profiles.pipeline_layout_t = .{},
    /// Private set tokens; zero denotes unbound/disturbed, lifetime validated by caller.
    sets: [profiles.MaxSets]u64 = [_]u64{0} ** profiles.MaxSets,
    /// Per-stage owned push-range definitions and initialized bytes, core stage bit order.
    pushes: [6]push_profile_t = [_]push_profile_t{.{}} ** 6,
};
/// One fixed owner slot; callers use profiles.reserve_slot/get_profile/release_slot under their mutex.
pub const slot_t = struct {
    /// Reserved owner, retained across uncertain transport failure until abandonment.
    occupied: bool = false,
    /// Owned command metadata, scrubbed deterministically at retirement.
    profile: command_profile_t = .{},
};
/// Fixed command registry with no heap or shared global mutation.
pub const registry_t = struct {
    ///64 owner slots; the registry's caller serializes mutation and retirement.
    commands: [MaxCommands]slot_t = [_]slot_t{.{}} ** MaxCommands,
};
/// Compare normalized complete push definitions. [in] profiles borrowed nonnull for call.
/// Returns structural equality; no mutation/allocation, safe for independent concurrent readers.
pub fn pushes_compatible(left: *const profiles.pipeline_layout_t, right: *const profiles.pipeline_layout_t) bool {
    if (left.push_count != right.push_count) return false;
    for (left.pushes[0..left.push_count], right.pushes[0..right.push_count]) |a, b| if (!std.meta.eql(a, b)) return false;
    return true;
}
/// Compare compatibility for a descriptor set index including all lower sets and push definitions.
/// [in] normalized profiles borrowed for call; last_set0..15. False for absent definitions.
/// No allocation, retained input pointer or mutation; caller ensures normalized bounded profiles.
pub fn layouts_compatible(left: *const profiles.pipeline_layout_t, right: *const profiles.pipeline_layout_t, last_set: usize) bool {
    if (last_set >= profiles.MaxSets or left.set_count <= last_set or right.set_count <= last_set or !pushes_compatible(left, right)) return false;
    for (left.sets[0 .. last_set + 1], right.sets[0 .. last_set + 1]) |a, b| if (!std.meta.eql(a, b)) return false;
    return true;
}
/// Apply an acknowledged descriptor bind and Vulkan layout disturbance rules.
/// [in,out] state exclusive owned command metadata; [in] layout normalized copied definition.
/// [in] first/set_tokens validated extent; no pointers retained. Returns Invalid before mutation.
/// Existing lower/upper sets survive only if their required compatibility relation holds.
pub fn bind_sets(state: *command_profile_t, layout: *const profiles.pipeline_layout_t, first: usize, set_tokens: []const u64) !void {
    if (first >= profiles.MaxSets or set_tokens.len > profiles.MaxSets - first or first + set_tokens.len > layout.set_count) return error.Invalid;
    for (set_tokens) |token| if (token == 0) return error.Invalid;
    if (set_tokens.len == 0) return;
    const last = first + set_tokens.len - 1;
    if (state.descriptor_layout_ready) {
        for (&state.sets, 0..) |*token, index| {
            if (index >= first and index <= last) continue;
            const required = if (index < first) index else last;
            if (!layouts_compatible(&state.descriptor_layout, layout, required)) token.* = 0;
        }
    }
    for (set_tokens, 0..) |token, index| state.sets[first + index] = token;
    var snapshot = layout.*;
    // Surviving upper sets retain their original unbound prefix definitions too.
    // Compatibility through last permits replacing only the lower bound prefix.
    for (state.sets[last + 1 ..], last + 1..) |token, index| if (token != 0) {
        snapshot.set_count = @max(snapshot.set_count, index + 1);
        for (last + 1..index + 1) |prefix| snapshot.sets[prefix] = state.descriptor_layout.sets[prefix];
    };
    state.descriptor_layout = snapshot;
    state.descriptor_layout_ready = true;
}
/// Record acknowledged push writes independently of pipeline binding order.
/// [in,out] state exclusive; [in] layout normalized borrowed definition copied by value.
/// [in] stages positive core mask, offset/size aligned and inside256 bytes.
/// Returns Invalid before mutation if any requested stage lacks whole range coverage.
/// A changed full push definition clears that stage's old initialized-byte bitmap.
pub fn push_bytes(state: *command_profile_t, layout: *const profiles.pipeline_layout_t, stages: u32, offset: u32, size: u32) !void {
    if (stages == 0 or stages & ~@as(u32, 0x3f) != 0 or offset % 4 != 0 or size == 0 or size % 4 != 0 or offset > profiles.MaxPushBytes or size > profiles.MaxPushBytes - offset) return error.Invalid;
    var covered: u32 = 0;
    for (layout.pushes[0..layout.push_count]) |range| if (offset >= range.offset and size <= range.size and offset - range.offset <= range.size - size) {
        covered |= range.stage_flags;
    };
    if (stages & ~covered != 0) return error.Invalid;
    for (&state.pushes, 0..) |*push, index| {
        if (stages & (@as(u32, 1) << @as(u5, @intCast(index))) == 0) continue;
        var compatible = push.count == layout.push_count;
        if (compatible) for (push.ranges[0..push.count], layout.pushes[0..layout.push_count]) |a, b| if (!std.meta.eql(a, b)) {
            compatible = false;
            break;
        };
        if (!compatible) push.initialized = .{ 0, 0, 0, 0 };
        push.count = layout.push_count;
        push.ranges = layout.pushes;
        for (offset..offset + size) |byte| push.initialized[byte / 64] |= @as(u64, 1) << @as(u6, @intCast(byte % 64));
    }
}

// Test-only fixtures.
const fixture_t = struct {
    fn test_bind_sets(state: *command_profile_t, definition: *const profiles.pipeline_layout_t, first: usize, tokens: []const u64) !void {
        return @call(.never_inline, bind_sets, .{ state, definition, first, tokens });
    }
    fn test_push_bytes(state: *command_profile_t, definition: *const profiles.pipeline_layout_t, stages: u32, offset: u32, size: u32) !void {
        return @call(.never_inline, push_bytes, .{ state, definition, stages, offset, size });
    }
    fn test_layouts_compatible(left: *const profiles.pipeline_layout_t, right: *const profiles.pipeline_layout_t, last: usize) bool {
        return @call(.never_inline, layouts_compatible, .{ left, right, last });
    }
    fn test_pushes_compatible(left: *const profiles.pipeline_layout_t, right: *const profiles.pipeline_layout_t) bool {
        return @call(.never_inline, pushes_compatible, .{ left, right });
    }
};
fn fixture_layout(binding: u32, stages: u32) !profiles.pipeline_layout_t {
    const definition = try profiles.normalize_bindings(&.{.{ .binding = binding, .descriptor_type = 7, .descriptor_count = 1, .stage_flags = stages }});
    return profiles.normalize_pipeline(&.{ definition, definition, definition }, &.{.{ .stage_flags = stages, .offset = 0, .size = 128 }});
}
test "command owners exhaust reuse and scrub without heap lifetime" {
    var registry = registry_t{};
    for (0..MaxCommands) |index| try std.testing.expectEqual(@as(u8, @intCast(index + 1)), try profiles.reserve_slot(&registry.commands, command_profile_t{ .pipeline = 42 }));
    try std.testing.expectError(error.Exhausted, profiles.reserve_slot(&registry.commands, command_profile_t{}));
    try std.testing.expect(profiles.release_slot(&registry.commands, 32));
    try std.testing.expectEqual(@as(u8, 32), try profiles.reserve_slot(&registry.commands, command_profile_t{}));
    for (1..MaxCommands + 1) |index| try std.testing.expect(profiles.release_slot(&registry.commands, @intCast(index)));
}
test "descriptor snapshots support bindings before pipelines and exact disturbance" {
    var state = command_profile_t{};
    var first = try fixture_layout(0, 32);
    var different = try fixture_layout(1, 32);
    try fixture_t.test_bind_sets(&state, &first, 0, &.{ 42, 43, 44 });
    first.sets[2] = different.sets[2];
    try fixture_t.test_bind_sets(&state, &first, 1, &.{45});
    try std.testing.expectEqualSlices(u64, &.{ 42, 45, 44 }, state.sets[0..3]);
    try std.testing.expect(!fixture_t.test_layouts_compatible(&state.descriptor_layout, &different, 0));
    try std.testing.expect(!std.meta.eql(state.descriptor_layout.sets[2], first.sets[2]));
    try fixture_t.test_bind_sets(&state, &different, 1, &.{46});
    try std.testing.expectEqualSlices(u64, &.{ 0, 46, 0 }, state.sets[0..3]);
    try std.testing.expect(fixture_t.test_layouts_compatible(&different, &different, 2));
    try std.testing.expect(!fixture_t.test_layouts_compatible(&different, &different, 3));
    try std.testing.expect(!fixture_t.test_layouts_compatible(&different, &different, 16));
    try fixture_t.test_bind_sets(&state, &first, 0, &.{});
    try std.testing.expectEqual(@as(u64, 46), state.sets[1]);
    try std.testing.expectError(error.Invalid, fixture_t.test_bind_sets(&state, &first, 16, &.{}));
    try std.testing.expectError(error.Invalid, fixture_t.test_bind_sets(&state, &first, 2, &.{ 42, 43 }));
    try std.testing.expectError(error.Invalid, fixture_t.test_bind_sets(&state, &first, 0, &.{0}));
    different.pushes[0].size = 64;
    try std.testing.expect(!fixture_t.test_pushes_compatible(&first, &different));
    different.push_count = 0;
    try std.testing.expect(!fixture_t.test_pushes_compatible(&first, &different));
}
test "push stage snapshots persist partial writes and reset incompatible ranges" {
    var state = command_profile_t{};
    var definition = try fixture_layout(0, 33);
    try fixture_t.test_push_bytes(&state, &definition, 33, 60, 8);
    try std.testing.expectEqual(@as(u64, 0xf000000000000000), state.pushes[5].initialized[0]);
    try std.testing.expectEqual(@as(u64, 15), state.pushes[5].initialized[1]);
    try fixture_t.test_push_bytes(&state, &definition, 32, 0, 4);
    try std.testing.expectEqual(@as(u64, 0xf00000000000000f), state.pushes[5].initialized[0]);
    definition.pushes[0].size = 64;
    try fixture_t.test_push_bytes(&state, &definition, 32, 4, 4);
    try std.testing.expectEqual(@as(u64, 240), state.pushes[5].initialized[0]);
    try std.testing.expectEqual(@as(u64, 0), state.pushes[5].initialized[1]);
    try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, 2, 0, 4));
    for ([_]u32{ 0, 64 }) |stages| try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, stages, 0, 4));
    try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, 32, 1, 4));
    try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, 32, 0, 0));
    try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, 32, 0, 3));
    try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, 32, 132, 4));
    try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, 32, 128, 4));
}

test "all profile extent and push coverage boundary directions remain checked" {
    var state = command_profile_t{};
    var definition = try fixture_layout(0, 32);
    var smaller = definition;
    smaller.set_count = 1;
    try std.testing.expect(!fixture_t.test_layouts_compatible(&definition, &smaller, 1));
    try std.testing.expect(!fixture_t.test_layouts_compatible(&smaller, &definition, 1));
    smaller.pushes[0].size = 64;
    try std.testing.expect(!fixture_t.test_layouts_compatible(&definition, &smaller, 0));
    smaller.sets[0].bindings[0].binding = 1;
    smaller.pushes = definition.pushes;
    try std.testing.expect(!fixture_t.test_layouts_compatible(&definition, &smaller, 0));
    const tokens = [_]u64{42} ** 17;
    try std.testing.expectError(error.Invalid, fixture_t.test_bind_sets(&state, &definition, 0, &tokens));
    definition.pushes[0].offset = 4;
    definition.pushes[0].size = 8;
    try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, 32, 0, 4));
    try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, 32, 4, 12));
    try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, 32, 12, 4));
    definition.push_count = 0;
    try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, 32, 0, 4));
}

test "rebinding higher incompatible set preserves compatible lower set" {
    var state = command_profile_t{};
    const original = try fixture_layout(0, 32);
    var changed = original;
    changed.sets[1] = (try fixture_layout(1, 32)).sets[1];
    try fixture_t.test_bind_sets(&state, &original, 0, &.{ 42, 43 });
    try fixture_t.test_bind_sets(&state, &changed, 1, &.{44});
    try std.testing.expectEqualSlices(u64, &.{ 42, 44 }, state.sets[0..2]);
    try std.testing.expect(fixture_t.test_layouts_compatible(&original, &state.descriptor_layout, 0));
    try std.testing.expect(!fixture_t.test_layouts_compatible(&original, &state.descriptor_layout, 1));
}

const origin_fixture_t = struct {
    token: u64 = 0,
    ready: bool = false,
    prefix: [3]u32 = .{ 0, 0, 0 },
    push_size: u32 = 0,
};
const origin_oracle_t = struct {
    extent: usize = 0,
    slots: [3]origin_fixture_t = [_]origin_fixture_t{.{}} ** 3,
    fn compatible(origin: origin_fixture_t, layout: *const profiles.pipeline_layout_t, index: usize) bool {
        if (!origin.ready or origin.push_size != layout.pushes[0].size) return false;
        for (0..index + 1) |prefix| if (origin.prefix[prefix] != layout.sets[prefix].bindings[0].binding) return false;
        return true;
    }
    fn snapshot(layout: *const profiles.pipeline_layout_t, token: u64) origin_fixture_t {
        var origin = origin_fixture_t{ .token = token, .ready = true, .push_size = layout.pushes[0].size };
        for (0..layout.set_count) |index| origin.prefix[index] = layout.sets[index].bindings[0].binding;
        return origin;
    }
    fn bind(self: *origin_oracle_t, layout: *const profiles.pipeline_layout_t, first: usize, tokens: []const u64) void {
        const end = first + tokens.len;
        if (end < self.extent and !compatible(self.slots[end - 1], layout, end - 1)) {
            for (self.slots[end..self.extent]) |*origin| origin.* = .{};
            self.extent = end;
        }
        self.extent = @max(self.extent, end);
        for (0..first) |index| if (!compatible(self.slots[index], layout, index)) {
            self.slots[index] = snapshot(layout, 0);
        };
        for (tokens, 0..) |token, index| self.slots[first + index] = snapshot(layout, token);
    }
};
fn compare_origin_oracle(state: *const command_profile_t, oracle: *const origin_oracle_t, layouts: []const profiles.pipeline_layout_t) !void {
    for (0..3) |index| {
        try std.testing.expectEqual(oracle.slots[index].token, state.sets[index]);
        if (state.sets[index] == 0) continue;
        for (layouts) |*layout| if (layout.set_count > index) {
            try std.testing.expectEqual(origin_oracle_t.compatible(oracle.slots[index], layout, index), layouts_compatible(&state.descriptor_layout, layout, index));
        };
    }
}
test "independent origin oracle validates compact merge and initial upper gaps" {
    var layouts: [48]profiles.pipeline_layout_t = undefined;
    for (&layouts, 0..) |*layout, ordinal| {
        const pattern = ordinal / 3;
        layout.* = try fixture_layout(0, 32);
        for (0..3) |index| if (pattern & (@as(usize, 1) << @intCast(index)) != 0) {
            layout.sets[index].bindings[0].binding = 1;
        };
        if (pattern & 8 != 0) layout.pushes[0].size = 64;
        layout.set_count = ordinal % 3 + 1;
        for (layout.sets[layout.set_count..]) |*unused| unused.* = .{};
    }
    const operation_t = struct { layout: usize, first: usize, count: usize };
    var operations: [160]operation_t = undefined;
    var count: usize = 0;
    for (0..layouts.len) |layout| for (0..layouts[layout].set_count) |first| {
        for (1..layouts[layout].set_count + 1 - first) |extent| {
            operations[count] = .{ .layout = layout, .first = first, .count = extent };
            count += 1;
        }
    };
    const tokens = [_]u64{ 42, 43, 44 };
    for (operations, 0..) |a, first_operation| for (operations, 0..) |b, second_operation| {
        var state = command_profile_t{};
        var oracle = origin_oracle_t{};
        try bind_sets(&state, &layouts[a.layout], a.first, tokens[0..a.count]);
        oracle.bind(&layouts[a.layout], a.first, tokens[0..a.count]);
        try compare_origin_oracle(&state, &oracle, &layouts);
        try bind_sets(&state, &layouts[b.layout], b.first, tokens[0..b.count]);
        oracle.bind(&layouts[b.layout], b.first, tokens[0..b.count]);
        try compare_origin_oracle(&state, &oracle, &layouts);
        // Include a distinct third rebind to exercise disturbed/unbound origins.
        var third_operation = (a.layout * 17 + b.layout * 7 + a.first * 3 + b.count) % operations.len;
        while (third_operation == first_operation or third_operation == second_operation) third_operation = (third_operation + 1) % operations.len;
        const third = operations[third_operation];
        try bind_sets(&state, &layouts[third.layout], third.first, tokens[0..third.count]);
        oracle.bind(&layouts[third.layout], third.first, tokens[0..third.count]);
        try compare_origin_oracle(&state, &oracle, &layouts);
    };
}

test "independent initial upper gap disturbance and compatible suffix origins" {
    const a = try fixture_layout(0, 32);
    const b = try fixture_layout(1, 32);
    var state = command_profile_t{};
    try bind_sets(&state, &a, 2, &.{42});
    try bind_sets(&state, &b, 1, &.{43});
    try std.testing.expectEqualSlices(u64, &.{ 0, 43, 0 }, state.sets[0..3]);
    state = .{};
    var compatible_lower = a;
    compatible_lower.sets[2] = b.sets[2];
    try bind_sets(&state, &a, 2, &.{42});
    try bind_sets(&state, &compatible_lower, 1, &.{43});
    try std.testing.expectEqualSlices(u64, &.{ 0, 43, 42 }, state.sets[0..3]);
    try std.testing.expect(layouts_compatible(&state.descriptor_layout, &a, 2));
    try std.testing.expect(!layouts_compatible(&state.descriptor_layout, &compatible_lower, 2));
}

test "256 push initialization spans every bitmap word and clears definition changes" {
    var state = command_profile_t{};
    var definition = try profiles.normalize_pipeline(&.{}, &.{.{ .stage_flags = 32, .size = 256 }});
    for ([_]u32{ 60, 124, 188, 252 }) |offset|
        try fixture_t.test_push_bytes(&state, &definition, 32, offset, if (offset == 252) 4 else 8);
    try std.testing.expectEqual([_]u64{ 0xf000000000000000, 0xf00000000000000f, 0xf00000000000000f, 0xf00000000000000f }, state.pushes[5].initialized);
    try fixture_t.test_push_bytes(&state, &definition, 32, 0, 256);
    try std.testing.expectEqual([_]u64{0xffffffffffffffff} ** 4, state.pushes[5].initialized);
    const before = state.pushes[5];
    for ([_][2]u32{ .{ 256, 4 }, .{ 252, 8 }, .{ 260, 4 }, .{ 0, 260 }, .{ 0xfffffffc, 4 } }) |range| {
        try std.testing.expectError(error.Invalid, fixture_t.test_push_bytes(&state, &definition, 32, range[0], range[1]));
        try std.testing.expectEqualDeep(before, state.pushes[5]);
    }
    definition.pushes[0].size = 4;
    try fixture_t.test_push_bytes(&state, &definition, 32, 0, 4);
    try std.testing.expectEqual([_]u64{ 15, 0, 0, 0 }, state.pushes[5].initialized);
    try std.testing.expectEqual(@as(usize, 424), @sizeOf(push_profile_t));
    try std.testing.expectEqual(@as(usize, 19600), @sizeOf(command_profile_t));
    try std.testing.expectEqual(@as(usize, 1254912), @sizeOf(registry_t));
}
