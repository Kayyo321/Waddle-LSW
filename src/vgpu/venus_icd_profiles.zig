//! Fixed compatibility metadata; callers exclusively serialize mutation and retire GPU uses first.
const std = @import("std");
/// Private push byte ceiling256; caller also checks actual host maxPushConstantsSize.
/// Immutable scalar budget, no allocation or ownership; shared concurrent reads are safe.
pub const MaxPushBytes: u32 = 256;
/// Maximum normalized binding records in a descriptor layout; no storage ownership.
pub const MaxBindings: usize = 64;
/// Maximum descriptor layouts copied into one pipeline layout; no storage ownership.
pub const MaxSets: usize = 16;
/// Maximum normalized push ranges in a pipeline layout; no storage ownership.
pub const MaxPushRanges: usize = 32;
/// Normalized immutable descriptor binding definition, owned by its containing profile.
pub const binding_t = struct {
    /// Native binding number, without pointers or ownership.
    binding: u32 = 0,
    /// Core descriptor type0..10.
    descriptor_type: u32 = 0,
    /// Declared scalar array capacity; modern sparse layouts admit0..65536.
    descriptor_count: u32 = 0,
    /// Core shader stage mask, no extension bits.
    stage_flags: u32 = 0,
    /// Owned descriptor-indexing binding flags, validated by the caller feature policy.
    binding_flags: u32 = 0,
    /// Immutable sampler identities owned by the layout snapshot, no native pointers.
    immutable_offset: u16 = 0,
    immutable_count: u16 = 0,
};
/// Normalized immutable layout snapshot; original native tokens may retire independently.
pub const descriptor_layout_t = struct {
    /// Initialized binding prefix length0..64.
    binding_count: usize = 0,
    /// Owned sorted binding definitions; no retained native input or handles.
    bindings: [MaxBindings]binding_t = [_]binding_t{.{}} ** MaxBindings,
    /// Bounded immutable sampler token prefix; the caller retains referenced samplers.
    immutable_count: usize = 0,
    /// Owned copied private sampler tokens, zero outside the initialized prefix.
    immutable_samplers: [128]u64 = [_]u64{0} ** 128,
};
/// Owned push range definition, with no pointer or resource ownership.
pub const push_range_t = struct {
    /// Nonzero core stage mask.
    stage_flags: u32 = 0,
    /// Four-byte-aligned offset in the bounded256-byte push-constant budget.
    offset: u32 = 0,
    /// Positive aligned byte count; offset+size<=256.
    size: u32 = 0,
};
/// Owned pipeline compatibility snapshot; input layout identities are never retained.
pub const pipeline_layout_t = struct {
    /// Initialized copied set-layout prefix0..16.
    set_count: usize = 0,
    /// Owned structural layout snapshots in descriptor set index order.
    sets: [MaxSets]descriptor_layout_t = [_]descriptor_layout_t{.{}} ** MaxSets,
    /// Initialized normalized push-range prefix0..32.
    push_count: usize = 0,
    /// Owned ranges sorted by offset then stage flags.
    pushes: [MaxPushRanges]push_range_t = [_]push_range_t{.{}} ** MaxPushRanges,
};
/// A tracked descriptor element; copied handles are validated again before recording/submission.
pub const descriptor_t = struct {
    /// Binding number from copied layout definition.
    binding: u32 = 0,
    /// Array element in the binding's declared extent.
    array_element: u32 = 0,
    /// Core descriptor type0..10; payload fields depend on its family.
    descriptor_type: u32 = 0,
    /// Private buffer token, zero while unwritten; no pointer dereference.
    buffer: u64 = 0,
    /// Byte offset within the private buffer.
    offset: u64 = 0,
    /// Positive byte extent or WHOLE_SIZE; checked against live buffer before use.
    range: u64 = 0,
    /// Owned private image view token, revalidated before GPU use.
    image_view: u64 = 0,
    /// Owned private sampler token, zero for immutable or enabled null descriptors.
    sampler: u64 = 0,
    /// Named Vulkan descriptor image layout.
    image_layout: u32 = 0,
    /// Owned private texel buffer-view token, revalidated before GPU use.
    texel_view: u64 = 0,
};
/// Owned allocated-set layout and mutable mixed snapshot; at most128 written elements.
pub const descriptor_set_t = struct {
    /// Copied normalized layout definition; original layout token may retire.
    layout: descriptor_layout_t = .{},
    /// Initialized written element prefix0..128.
    descriptor_count: usize = 0,
    /// Owned copied elements; tokens are revalidated before command execution.
    descriptors: [128]descriptor_t = [_]descriptor_t{.{}} ** 128,
    /// Sparse profiles own only written elements of potentially large binding arrays.
    sparse: bool = false,
    /// Effective allocation extent of the variable-count binding, no array allocation.
    variable_count: u32 = 0,
    /// Declared binding number to which variable_count applies.
    variable_binding: u32 = 0,
    /// True only when allocation supplied an effective variable descriptor count.
    has_variable_count: bool = false,
};
fn slot_type(comptime profile_t: type) type {
    return struct {
        /// Live owner flag; cleared after native retirement, never concurrently mutated.
        occupied: bool = false,
        /// Owned copied definition, scrubbed deterministically at release.
        profile: profile_t = .{},
    };
}
/// Allocation-free fixed registry; caller owns it until teardown and exclusively serializes mutation.
/// Clear only after receiver retirement; normal releases follow validated native destruction.
pub const registry_t = struct {
    ///32 descriptor layout profile owners; zeroed unused entries retain no identity.
    descriptor_layouts: [32]slot_type(descriptor_layout_t) = [_]slot_type(descriptor_layout_t){.{}} ** 32,
    ///32 pipeline layout profile owners with copied definitions.
    pipeline_layouts: [32]slot_type(pipeline_layout_t) = [_]slot_type(pipeline_layout_t){.{}} ** 32,
    ///64 pipeline compatibility owners; independent of shader/layout token lifetime.
    pipelines: [64]slot_type(pipeline_layout_t) = [_]slot_type(pipeline_layout_t){.{}} ** 64,
    ///128 allocated set metadata owners; each bounded by128 written descriptor elements.
    sets: [128]slot_type(descriptor_set_t) = [_]slot_type(descriptor_set_t){.{}} ** 128,
};
/// [in,out] table nonnull exclusive fixed profile table; [in] profile copied by value, no retention.
/// Returns one-based owned index or Exhausted without mutation. No allocation/locks; caller serializes.
pub fn reserve_slot(table: anytype, profile: anytype) !u8 {
    if (table.len > 255) @compileError("profile table exceeds one-byte private slot index");
    for (table, 0..) |*entry, index| if (!entry.occupied) {
        entry.* = .{ .occupied = true, .profile = profile };
        return @intCast(index + 1);
    };
    return error.Exhausted;
}
/// [in,out] table exclusive borrowed owner; [in] index one-based live profile slot.
/// Returns borrowed profile pointer or null for zero/out-of-range/free indices. No mutation or retention.
/// Thread-safe only while caller holds exclusive registry synchronization for pointer lifetime.
pub fn get_profile(table: anytype, index: u8) ?*@TypeOf(table.*[0].profile) {
    if (index == 0 or index > table.len or !table[index - 1].occupied) return null;
    return &table[index - 1].profile;
}
/// [in,out] table exclusive owner; [in] index live one-based slot consumed on success.
/// Returns true after deterministic scrubbing, false for invalid/double release. No allocation/locking.
pub fn release_slot(table: anytype, index: u8) bool {
    const profile = get_profile(table, index) orelse return false;
    profile.* = .{};
    table[index - 1].occupied = false;
    return true;
}
/// [in] bindings borrowed definitions0..64, copied and sorted without retained pointers.
/// Returns owned normalized profile or Invalid; no allocation/shared state and no input mutation.
pub fn normalize_bindings(bindings: []const binding_t) !descriptor_layout_t {
    if (bindings.len > MaxBindings) return error.Invalid;
    var profile = descriptor_layout_t{ .binding_count = bindings.len };
    for (bindings, 0..) |binding, index| {
        if (binding.descriptor_type > 10 or binding.descriptor_count == 0 or binding.descriptor_count > 1024 or
            binding.stage_flags == 0 or binding.stage_flags & ~@as(u32, 0x3f) != 0) return error.Invalid;
        var destination = index;
        while (destination > 0 and profile.bindings[destination - 1].binding > binding.binding) : (destination -= 1)
            profile.bindings[destination] = profile.bindings[destination - 1];
        if (destination > 0 and profile.bindings[destination - 1].binding == binding.binding) return error.Invalid;
        profile.bindings[destination] = binding;
    }
    return profile;
}
/// [in] layouts/ranges borrowed bounded snapshots, copied by value with no original token retention.
/// Returns owned normalized pipeline profile or Invalid. Distinct profiles thread-safe; no allocation.
pub fn normalize_pipeline(layouts: []const descriptor_layout_t, ranges: []const push_range_t) !pipeline_layout_t {
    if (layouts.len > MaxSets or ranges.len > MaxPushRanges) return error.Invalid;
    var profile = pipeline_layout_t{ .set_count = layouts.len, .push_count = ranges.len };
    for (layouts, 0..) |layout, index| {
        if (layout.binding_count > MaxBindings) return error.Invalid;
        _ = try create_sparse_set_profile(&layout);
        profile.sets[index] = layout;
    }
    for (ranges, 0..) |range, index| {
        if (range.stage_flags == 0 or range.stage_flags & ~@as(u32, 0x3f) != 0 or range.size == 0 or
            range.offset % 4 != 0 or range.size % 4 != 0 or range.offset >= MaxPushBytes or range.size > MaxPushBytes - range.offset) return error.Invalid;
        for (ranges[0..index]) |previous| if (range.stage_flags & previous.stage_flags != 0) return error.Invalid;
        var destination = index;
        while (destination > 0 and (profile.pushes[destination - 1].offset > range.offset or
            (profile.pushes[destination - 1].offset == range.offset and profile.pushes[destination - 1].stage_flags > range.stage_flags))) : (destination -= 1)
            profile.pushes[destination] = profile.pushes[destination - 1];
        profile.pushes[destination] = range;
    }
    return profile;
}
/// [in] layout borrowed normalized definition, copied into an owned set profile.
/// Returns Invalid if malformed or more than64 elements; no allocation or native handle retention.
pub fn create_set_profile(layout: *const descriptor_layout_t) !descriptor_set_t {
    if (layout.binding_count > MaxBindings) return error.Invalid;
    var profile = descriptor_set_t{ .layout = try normalize_bindings(layout.bindings[0..layout.binding_count]) };
    for (profile.layout.bindings[0..profile.layout.binding_count]) |binding| {
        if (binding.descriptor_count > 64 - profile.descriptor_count) return error.Invalid;
        for (0..binding.descriptor_count) |element| {
            profile.descriptors[profile.descriptor_count] = .{ .binding = binding.binding, .array_element = @intCast(element), .descriptor_type = binding.descriptor_type };
            profile.descriptor_count += 1;
        }
    }
    return profile;
}
/// [in] bounded normalized layout snapshot; [out] owned sparse metadata with no elements.
/// Large descriptor capacities do not allocate proportional storage. Caller serializes;
/// sampler tokens are copied and their lifetime remains the caller's responsibility.
pub fn create_sparse_set_profile(layout: *const descriptor_layout_t) !descriptor_set_t {
    if (layout.binding_count > MaxBindings or layout.immutable_count > 128) return error.Invalid;
    for (layout.bindings[0..layout.binding_count], 0..) |binding, index| {
        if (binding.descriptor_type > 10 or binding.descriptor_count > 65536 or
            (binding.descriptor_count != 0 and binding.stage_flags == 0) or binding.stage_flags & ~@as(u32, 0x3f) != 0 or
            (index != 0 and layout.bindings[index-1].binding >= binding.binding) or
            @as(usize,binding.immutable_offset) + binding.immutable_count > layout.immutable_count) return error.Invalid;
    }
    return .{ .layout = layout.*, .sparse = true };
}
/// [in,out] exclusively owned sparse set; [in] binding/element target.
/// Returns an existing or newly owned element, Invalid for undeclared extent,
/// Exhausted without mutation when the bounded written-element ledger is full.
/// No heap or native pointers; caller validates resources before publication.
pub fn sparse_element(profile: *descriptor_set_t, binding_number: u32, element: u32) !*descriptor_t {
    if (!profile.sparse) return error.Invalid;
    for (profile.descriptors[0..profile.descriptor_count]) |*descriptor| {
        if (descriptor.binding == binding_number and descriptor.array_element == element) return descriptor;
    }
    for (profile.layout.bindings[0..profile.layout.binding_count]) |binding| {
        if (binding.binding != binding_number) continue;
        const extent = if (profile.has_variable_count and binding.binding == profile.variable_binding) profile.variable_count else binding.descriptor_count;
        if (element >= extent) return error.Invalid;
        if (profile.descriptor_count == profile.descriptors.len) return error.Exhausted;
        const destination = &profile.descriptors[profile.descriptor_count];
        destination.* = .{ .binding = binding_number, .array_element = element, .descriptor_type = binding.descriptor_type };
        profile.descriptor_count += 1;
        return destination;
    }
    return error.Invalid;
}

// Test-only fixtures.
const fixture_t = struct {
    fn bindings(input: []const binding_t) !descriptor_layout_t {
        return @call(.never_inline, normalize_bindings, .{input});
    }
    fn pipeline(layouts: []const descriptor_layout_t, ranges: []const push_range_t) !pipeline_layout_t {
        return @call(.never_inline, normalize_pipeline, .{ layouts, ranges });
    }
    fn create_set(layout: *const descriptor_layout_t) !descriptor_set_t {
        return @call(.never_inline, create_set_profile, .{layout});
    }
};
test "fixed slots exhaust scrub and reject invalid double release" {
    var registry = registry_t{};
    const tables = .{ &registry.descriptor_layouts, &registry.pipeline_layouts, &registry.pipelines, &registry.sets };
    inline for (tables) |table| {
        for (0..table.len) |index| try std.testing.expectEqual(@as(u8, @intCast(index + 1)), try reserve_slot(table, @as(@TypeOf(table.*[0].profile), .{})));
        try std.testing.expectError(error.Exhausted, reserve_slot(table, @as(@TypeOf(table.*[0].profile), .{})));
        try std.testing.expect(get_profile(table, 0) == null);
        try std.testing.expect(get_profile(table, 255) == null);
        for (1..table.len + 1) |index| {
            try std.testing.expect(get_profile(table, @intCast(index)) != null);
            try std.testing.expect(release_slot(table, @intCast(index)));
            try std.testing.expect(!release_slot(table, @intCast(index)));
        }
        try std.testing.expectEqual(@as(u8, 1), try reserve_slot(table, @as(@TypeOf(table.*[0].profile), .{})));
    }
}
test "normalization copies structural compatibility independent of input order and lifetime" {
    var bindings = [_]binding_t{ .{ .binding = 4, .descriptor_type = 7, .descriptor_count = 2, .stage_flags = 32 }, .{ .binding = 1, .descriptor_type = 6, .descriptor_count = 1, .stage_flags = 1 } };
    const layout = try fixture_t.bindings(&bindings);
    try std.testing.expectEqual(@as(u32, 1), layout.bindings[0].binding);
    try std.testing.expectEqual(@as(u32, 4), layout.bindings[1].binding);
    bindings[0].descriptor_count = 55;
    try std.testing.expectEqual(@as(u32, 2), layout.bindings[1].descriptor_count);
    const ranges = [_]push_range_t{ .{ .stage_flags = 32, .offset = 16, .size = 16 }, .{ .stage_flags = 1, .size = 16 } };
    const pipeline = try fixture_t.pipeline(&.{layout}, &ranges);
    try std.testing.expectEqual(@as(u32, 0), pipeline.pushes[0].offset);
    const set = try fixture_t.create_set(&layout);
    try std.testing.expectEqual(@as(usize, 3), set.descriptor_count);
    try std.testing.expectEqual(@as(u32, 1), set.descriptors[2].array_element);
}
test "normalization rejects duplicate keys stages malformed extents and total descriptor overflow" {
    const valid = binding_t{ .binding = 0, .descriptor_type = 7, .descriptor_count = 1, .stage_flags = 32 };
    try std.testing.expectError(error.Invalid, fixture_t.bindings(&.{ valid, valid }));
    inline for (.{ "descriptor_type", "descriptor_count", "stage_flags" }) |field| {
        var value = valid;
        @field(value, field) = 0xffffffff;
        try std.testing.expectError(error.Invalid, fixture_t.bindings(&.{value}));
    }
    var value = valid;
    value.descriptor_count = 0;
    try std.testing.expectError(error.Invalid, fixture_t.bindings(&.{value}));
    value = valid;
    value.stage_flags = 0;
    try std.testing.expectError(error.Invalid, fixture_t.bindings(&.{value}));
    var layout = try fixture_t.bindings(&.{valid});
    layout.binding_count = 65;
    try std.testing.expectError(error.Invalid, fixture_t.create_set(&layout));
    try std.testing.expectError(error.Invalid, fixture_t.pipeline(&.{layout}, &.{}));
    layout = try fixture_t.bindings(&.{.{ .descriptor_type = 7, .descriptor_count = 65, .stage_flags = 32 }});
    try std.testing.expectError(error.Invalid, fixture_t.create_set(&layout));
    const valid_range = push_range_t{ .stage_flags = 32, .size = 4 };
    try std.testing.expectError(error.Invalid, fixture_t.pipeline(&.{}, &.{ valid_range, valid_range }));
    inline for (.{ "stage_flags", "offset", "size" }) |field| {
        var range = valid_range;
        @field(range, field) = 0xffffffff;
        try std.testing.expectError(error.Invalid, fixture_t.pipeline(&.{}, &.{range}));
    }
    const too_many_bindings = [_]binding_t{valid} ** 65;
    try std.testing.expectError(error.Invalid, fixture_t.bindings(&too_many_bindings));
    const too_many_sets = [_]descriptor_layout_t{.{}} ** 17;
    try std.testing.expectError(error.Invalid, fixture_t.pipeline(&too_many_sets, &.{}));
    const too_many_ranges = [_]push_range_t{valid_range} ** 33;
    try std.testing.expectError(error.Invalid, fixture_t.pipeline(&.{}, &too_many_ranges));
}
test "sorted and equal-offset inputs preserve canonical order and bounded empty maximum profiles" {
    const first = binding_t{ .binding = 0, .descriptor_type = 7, .descriptor_count = 1, .stage_flags = 32 };
    var second = first;
    second.binding = 1;
    const ordered = try fixture_t.bindings(&.{ first, second });
    const reversed = try fixture_t.bindings(&.{ second, first });
    try std.testing.expectEqualDeep(ordered, reversed);
    try std.testing.expectError(error.Invalid, fixture_t.bindings(&.{ second, first, second }));
    try std.testing.expectEqual(@as(usize, 0), (try fixture_t.bindings(&.{})).binding_count);
    const ranges = [_]push_range_t{ .{ .stage_flags = 32, .size = 4 }, .{ .stage_flags = 1, .size = 4 } };
    const normalized = try fixture_t.pipeline(&.{ordered}, &ranges);
    try std.testing.expectEqual(@as(u32, 1), normalized.pushes[0].stage_flags);
    _ = try fixture_t.pipeline(&.{ordered}, &.{ ranges[1], ranges[0] });
    _ = try fixture_t.pipeline(&.{ordered}, &.{ .{ .stage_flags = 1, .size = 4 }, .{ .stage_flags = 32, .offset = 4, .size = 4 } });
    var invalid_range = push_range_t{ .stage_flags = 1, .size = 4 };
    invalid_range.stage_flags = 0;
    try std.testing.expectError(error.Invalid, fixture_t.pipeline(&.{}, &.{invalid_range}));
    invalid_range.stage_flags = 1;
    invalid_range.size = 0;
    try std.testing.expectError(error.Invalid, fixture_t.pipeline(&.{}, &.{invalid_range}));
    invalid_range.size = 3;
    try std.testing.expectError(error.Invalid, fixture_t.pipeline(&.{}, &.{invalid_range}));
    invalid_range.size = 4;
    invalid_range.offset = 2;
    try std.testing.expectError(error.Invalid, fixture_t.pipeline(&.{}, &.{invalid_range}));
    invalid_range.offset = 252;
    invalid_range.size = 8;
    try std.testing.expectError(error.Invalid, fixture_t.pipeline(&.{}, &.{invalid_range}));
    invalid_range.size = 4;
    _ = try fixture_t.pipeline(&.{}, &.{invalid_range});
    const full = try fixture_t.bindings(&.{.{ .descriptor_type = 7, .descriptor_count = 64, .stage_flags = 32 }});
    try std.testing.expectEqual(@as(usize, 64), (try fixture_t.create_set(&full)).descriptor_count);
    const empty = try fixture_t.bindings(&.{});
    try std.testing.expectEqual(@as(usize, 0), (try fixture_t.create_set(&empty)).descriptor_count);
}
test "malformed copied layout contents cannot enter pipeline or set ownership" {
    var corrupted = descriptor_layout_t{ .binding_count = 1 };
    corrupted.bindings[0].descriptor_type = 11;
    try std.testing.expectError(error.Invalid, fixture_t.pipeline(&.{corrupted}, &.{}));
    try std.testing.expectError(error.Invalid, fixture_t.create_set(&corrupted));
}

test "push range profiles accept256 ceiling and reject all overflow directions" {
    const whole = try fixture_t.pipeline(&.{}, &.{.{ .stage_flags = 32, .size = 256 }});
    try std.testing.expectEqual(@as(u32, 256), whole.pushes[0].size);
    _ = try fixture_t.pipeline(&.{}, &.{.{ .stage_flags = 32, .offset = 252, .size = 4 }});
    for ([_]push_range_t{ .{ .stage_flags = 32, .size = 260 }, .{ .stage_flags = 32, .offset = 256, .size = 4 }, .{ .stage_flags = 32, .offset = 252, .size = 8 }, .{ .stage_flags = 32, .offset = 0xfffffffc, .size = 4 } }) |range|
        try std.testing.expectError(error.Invalid, fixture_t.pipeline(&.{}, &.{range}));
}

test "sparse descriptor capacities retain only written identities and fail atomically" {
    var layout = descriptor_layout_t{ .binding_count = 1 };
    layout.bindings[0] = .{ .binding = 4, .descriptor_type = 2, .descriptor_count = 65536, .stage_flags = 17 };
    var profile = try create_sparse_set_profile(&layout);
    const last = try sparse_element(&profile, 4, 65535);
    last.image_view = 99;
    try std.testing.expectEqual(@as(u64,99), (try sparse_element(&profile,4,65535)).image_view);
    try std.testing.expectError(error.Invalid, sparse_element(&profile,4,65536));
    for (0..127) |index| _ = try sparse_element(&profile,4,@intCast(index));
    try std.testing.expectError(error.Exhausted,sparse_element(&profile,4,999));
    try std.testing.expectEqual(@as(usize,128), profile.descriptor_count);
}

test "variable descriptor allocations preserve layout compatibility and bound only written elements" {
    var layout = descriptor_layout_t{ .binding_count = 2 };
    layout.bindings[0] = .{ .binding = 1, .descriptor_type = 6, .descriptor_count = 1, .stage_flags = 32 };
    layout.bindings[1] = .{ .binding = 9, .descriptor_type = 2, .descriptor_count = 65536, .stage_flags = 16 };
    var profile = try create_sparse_set_profile(&layout);
    profile.has_variable_count = true;
    profile.variable_binding = 9;
    profile.variable_count = 2;
    _ = try sparse_element(&profile, 1, 0);
    _ = try sparse_element(&profile, 9, 1);
    try std.testing.expectEqual(@as(u32, 65536), profile.layout.bindings[1].descriptor_count);
    try std.testing.expectError(error.Invalid, sparse_element(&profile, 9, 2));
    try std.testing.expectError(error.Invalid, sparse_element(&profile, 8, 0));
    profile.variable_count = 0;
    try std.testing.expectError(error.Invalid, sparse_element(&profile, 9, 0));
    profile.sparse = false;
    try std.testing.expectError(error.Invalid, sparse_element(&profile, 1, 0));
    var empty = try create_sparse_set_profile(&.{});
    try std.testing.expectError(error.Invalid, sparse_element(&empty, 1, 0));
}

test "sparse copied layout rejects corrupt quotas and immutable ownership extents" {
    var layout = descriptor_layout_t{ .binding_count = MaxBindings + 1 };
    try std.testing.expectError(error.Invalid, create_sparse_set_profile(&layout));
    layout = .{ .immutable_count = 129 };
    try std.testing.expectError(error.Invalid, create_sparse_set_profile(&layout));
    layout = .{ .binding_count = 1 };
    layout.bindings[0] = .{ .descriptor_type = 2, .descriptor_count = 0 };
    _ = try create_sparse_set_profile(&layout);
    layout.bindings[0].immutable_count = 1;
    try std.testing.expectError(error.Invalid, create_sparse_set_profile(&layout));
    layout.bindings[0].immutable_count = 0;
    layout.bindings[0].descriptor_count = 65537;
    try std.testing.expectError(error.Invalid, create_sparse_set_profile(&layout));
    layout.bindings[0].descriptor_count = 1;
    try std.testing.expectError(error.Invalid, create_sparse_set_profile(&layout));
    layout.bindings[0].stage_flags = 64;
    try std.testing.expectError(error.Invalid, create_sparse_set_profile(&layout));
    layout.bindings[0].stage_flags = 16;
    layout.binding_count = 2;
    layout.bindings[1] = layout.bindings[0];
    try std.testing.expectError(error.Invalid, create_sparse_set_profile(&layout));
}
