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
    /// Initialized byte bitmap for core128 push bytes, no pointers or allocation.
    initialized: [2]u64 = .{ 0, 0 },
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
/// [in] stages positive core mask, offset/size aligned and inside128 bytes.
/// Returns Invalid before mutation if any requested stage lacks whole range coverage.
/// A changed full push definition clears that stage's old initialized-byte bitmap.
pub fn push_bytes(state: *command_profile_t, layout: *const profiles.pipeline_layout_t, stages: u32, offset: u32, size: u32) !void {
    if (stages == 0 or stages & ~@as(u32, 0x3f) != 0 or offset % 4 != 0 or size == 0 or size % 4 != 0 or offset > 128 or size > 128 - offset) return error.Invalid;
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
        if (!compatible) push.initialized = .{ 0, 0 };
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
