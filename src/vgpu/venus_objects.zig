//! Bounds-checked caller-owned Vulkan handles; exact C ABI contract in venus_objects.h.
const std = @import("std");
const c = @cImport({
    @cInclude("waddle/venus_objects.h");
});
fn live(objects: *const c.venus_objects_t) bool {
    return objects.slots != null and objects.context != null;
}
fn find_id(objects: *const c.venus_objects_t, id: u64) ?*c.venus_object_t {
    for (objects.slots[0..objects.capacity]) |*slot| {
        if (slot.id == id) return slot;
    }
    return null;
}

/// Borrow/clear private records; header specifies actual extents/ownership/error/thread contract.
export fn venus_objects_init(
    optional_objects: ?*c.venus_objects_t,
    slots: ?[*]c.venus_object_t,
    capacity: u32,
    namespace_id: u32,
    context: ?*anyopaque,
) c_int {
    const objects = optional_objects orelse return c.RingInvalid;
    if (!std.meta.eql(objects.*, std.mem.zeroes(c.venus_objects_t)) or slots == null or
        context == null or namespace_id == 0 or capacity == 0 or
        capacity > c.VenusObjectsMaxCapacity) return c.RingInvalid;
    const bytes = @as(usize, capacity) * @sizeOf(c.venus_object_t);
    const start = @intFromPtr(slots.?);
    const owner = @intFromPtr(objects);
    if (bytes > std.math.maxInt(usize) - start) return c.RingInvalid;
    if (start < owner + @sizeOf(c.venus_objects_t) and owner < start + bytes)
        return c.RingInvalid;
    @memset(slots.?[0..capacity], std.mem.zeroes(c.venus_object_t));
    objects.* = .{
        .slots = slots.?,
        .context = context,
        .capacity = capacity,
        .live_count = 0,
        .next_id = 1,
        .namespace_id = namespace_id,
    };
    return c.RingOk;
}

/// Reserve one immutable private slot; header specifies rollback/lifetime/nullability/errors.
export fn venus_objects_reserve(
    optional_objects: ?*c.venus_objects_t,
    kind: u32,
    parent_id: u64,
    dispatchable: u32,
    optional_output: ?*?*c.venus_object_t,
) c_int {
    const output = optional_output orelse return c.RingInvalid;
    output.* = null;
    const objects = optional_objects orelse return c.RingInvalid;
    if (!live(objects) or kind == 0 or dispatchable > 1) return c.RingInvalid;
    if (parent_id != 0 and find_id(objects, parent_id) == null) return c.RingInvalid;
    if (objects.next_id == std.math.maxInt(u32) or objects.live_count == objects.capacity)
        return c.RingLimit;
    for (objects.slots[0..objects.capacity]) |*slot| {
        if (slot.id != 0) continue;
        const id: u64 = objects.next_id;
        slot.* = .{
            .loader_data = if (dispatchable == 1) c.VenusObjectsLoaderMagic else 0,
            .id = id,
            .handle = if (dispatchable == 1)
                @intFromPtr(slot)
            else
                (@as(u64, objects.namespace_id) << 32) | id,
            .parent_id = parent_id,
            .kind = kind,
            .dispatchable = dispatchable,
        };
        objects.next_id += 1;
        objects.live_count += 1;
        output.* = slot;
        return c.RingOk;
    }
    // Contradictory private count/storage requires old-session abandonment.
    return c.RingCorrupt;
}

/// Validate a token/address without dereferencing it; header specifies output lifetime/errors.
export fn venus_objects_lookup(
    optional_objects: ?*const c.venus_objects_t,
    handle: u64,
    kind: u32,
    dispatchable: u32,
    optional_output: ?*?*c.venus_object_t,
) c_int {
    const output = optional_output orelse return c.RingInvalid;
    output.* = null;
    const objects = optional_objects orelse return c.RingInvalid;
    if (!live(objects) or handle == 0 or kind == 0 or dispatchable > 1) return c.RingInvalid;
    var slot: *c.venus_object_t = undefined;
    if (dispatchable == 1) {
        const start = @intFromPtr(objects.slots);
        const bytes = @as(usize, objects.capacity) * @sizeOf(c.venus_object_t);
        if (handle < start or handle - start >= bytes or
            (handle - start) % @sizeOf(c.venus_object_t) != 0) return c.RingInvalid;
        slot = @ptrCast(&objects.slots[@intCast((handle - start) / @sizeOf(c.venus_object_t))]);
    } else {
        if (handle >> 32 != objects.namespace_id) return c.RingInvalid;
        slot = find_id(objects, handle & std.math.maxInt(u32)) orelse return c.RingInvalid;
    }
    if (slot.id == 0 or slot.handle != handle or slot.kind != kind or
        slot.dispatchable != dispatchable) return c.RingInvalid;
    output.* = slot;
    return c.RingOk;
}

/// Translate only preexisting validated host IDs; header defines ownership/output/errors.
export fn venus_objects_lookup_id(
    optional_objects: ?*const c.venus_objects_t,
    id: u64,
    kind: u32,
    optional_output: ?*?*c.venus_object_t,
) c_int {
    const output = optional_output orelse return c.RingInvalid;
    output.* = null;
    const objects = optional_objects orelse return c.RingInvalid;
    if (!live(objects) or id == 0 or kind == 0) return c.RingInvalid;
    const slot = find_id(objects, id) orelse return c.RingInvalid;
    if (slot.kind != kind) return c.RingInvalid;
    output.* = slot;
    return c.RingOk;
}

/// Retire an object only after children; header defines host ordering/errors/thread safety.
export fn venus_objects_release(
    optional_objects: ?*c.venus_objects_t,
    handle: u64,
    kind: u32,
    dispatchable: u32,
) c_int {
    const objects = optional_objects orelse return c.RingInvalid;
    var slot: ?*c.venus_object_t = null;
    const status = venus_objects_lookup(objects, handle, kind, dispatchable, &slot);
    if (status != c.RingOk) return status;
    for (objects.slots[0..objects.capacity]) |record| {
        if (record.id != 0 and record.parent_id == slot.?.id) return c.RingAgain;
    }
    slot.?.* = std.mem.zeroes(c.venus_object_t);
    objects.live_count -= 1;
    return c.RingOk;
}

/// Clear after host/session retirement; header defines borrowed storage and no cancellation.
export fn venus_objects_free(optional_objects: ?*c.venus_objects_t) void {
    const objects = optional_objects orelse return;
    if (live(objects)) {
        @memset(objects.slots[0..objects.capacity], std.mem.zeroes(c.venus_object_t));
    }
    objects.* = std.mem.zeroes(c.venus_objects_t);
}

// Test-only fixtures.
const fixture_t = struct {
    objects: c.venus_objects_t = std.mem.zeroes(c.venus_objects_t),
    slots: [8]c.venus_object_t = undefined,
    fn init(self: *fixture_t, namespace_id: u32) !void {
        try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_objects_init, .{
            &self.objects, &self.slots, self.slots.len, namespace_id, self,
        }));
    }
    fn reserve(
        self: *fixture_t,
        kind: u32,
        parent: u64,
        dispatchable: u32,
        expected: c_int,
    ) !?*c.venus_object_t {
        var slot: ?*c.venus_object_t = null;
        try std.testing.expectEqual(expected, @call(.never_inline, venus_objects_reserve, .{
            &self.objects, kind, parent, dispatchable, &slot,
        }));
        if (expected != c.RingOk) try std.testing.expect(slot == null);
        return slot;
    }
    fn lookup(
        self: *fixture_t,
        handle: u64,
        kind: u32,
        dispatchable: u32,
        expected: c_int,
    ) !?*c.venus_object_t {
        var slot: ?*c.venus_object_t = null;
        try std.testing.expectEqual(expected, @call(.never_inline, venus_objects_lookup, .{
            &self.objects, handle, kind, dispatchable, &slot,
        }));
        if (expected != c.RingOk) try std.testing.expect(slot == null);
        return slot;
    }
    fn lookup_id(self: *fixture_t, id: u64, kind: u32, expected: c_int) !?*c.venus_object_t {
        var slot: ?*c.venus_object_t = null;
        try std.testing.expectEqual(expected, @call(.never_inline, venus_objects_lookup_id, .{
            &self.objects, id, kind, &slot,
        }));
        if (expected != c.RingOk) try std.testing.expect(slot == null);
        return slot;
    }
    fn release(self: *fixture_t, handle: u64, kind: u32, dispatchable: u32, expected: c_int) !void {
        try std.testing.expectEqual(expected, @call(.never_inline, venus_objects_release, .{
            &self.objects, handle, kind, dispatchable,
        }));
    }
    fn free(self: *fixture_t) void {
        @call(.never_inline, venus_objects_free, .{&self.objects});
    }
};

test "loader header, both handle forms, exact kind and parent retirement" {
    var fixture = fixture_t{};
    try fixture.init(1);
    const instance = (try fixture.reserve(1, 0, 1, c.RingOk)).?;
    const child = (try fixture.reserve(9, instance.id, 0, c.RingOk)).?;
    const child_handle = child.handle;
    try std.testing.expectEqual(@as(u64, 1), instance.id);
    try std.testing.expectEqual(@intFromPtr(instance), instance.handle);
    try std.testing.expectEqual(@as(usize, c.VenusObjectsLoaderMagic), instance.loader_data);
    instance.loader_data = 0x1000; // Loader replacement must not affect owner identity.
    try std.testing.expectEqual(instance, (try fixture.lookup(instance.handle, 1, 1, 0)).?);
    try std.testing.expectEqual(child, (try fixture.lookup(child.handle, 9, 0, 0)).?);
    try std.testing.expectEqual(@as(u64, 0x100000002), child.handle);
    try std.testing.expectEqual(child, (try fixture.lookup_id(child.id, 9, 0)).?);
    _ = try fixture.lookup(instance.handle, 2, 1, c.RingInvalid);
    _ = try fixture.lookup(instance.handle, 1, 0, c.RingInvalid);
    _ = try fixture.lookup(child.handle, 9, 1, c.RingInvalid);
    _ = try fixture.lookup_id(child.id, 8, c.RingInvalid);
    _ = try fixture.lookup_id(9999, 9, c.RingInvalid);
    _ = try fixture.reserve(9, 9999, 0, c.RingInvalid);
    _ = try fixture.reserve(0, 0, 0, c.RingInvalid);
    _ = try fixture.reserve(9, 0, 2, c.RingInvalid);
    try fixture.release(instance.handle, 1, 1, c.RingAgain);
    try fixture.release(child_handle, 9, 0, c.RingOk);
    _ = try fixture.lookup(child_handle, 9, 0, c.RingInvalid);
    try fixture.release(child_handle, 9, 0, c.RingInvalid);
    try fixture.release(instance.handle, 1, 1, c.RingOk);
    try std.testing.expectEqual(@as(u32, 0), fixture.objects.live_count);
    fixture.free();
    fixture.free();
    var free_callback: *const fn (?*c.venus_objects_t) callconv(.C) void = venus_objects_free;
    const opaque_free = @as(*volatile @TypeOf(free_callback), &free_callback).*;
    opaque_free(null);
}

test "address bounds, alignment, namespaces and no host identity reuse" {
    var a = fixture_t{};
    var b = fixture_t{};
    try a.init(1);
    try b.init(2);
    const a_object = (try a.reserve(9, 0, 0, 0)).?;
    const b_object = (try b.reserve(9, 0, 0, 0)).?;
    _ = try a.lookup(b_object.handle, 9, 0, c.RingInvalid);
    _ = try b.lookup(a_object.handle, 9, 0, c.RingInvalid);
    const dispatched = (try a.reserve(1, 0, 1, 0)).?;
    _ = try b.lookup(dispatched.handle, 1, 1, c.RingInvalid);
    const start = @intFromPtr(&a.slots);
    for ([_]u64{
        0,
        1,
        start - 1,
        start + 1,
        start + @sizeOf(@TypeOf(a.slots)),
        std.math.maxInt(u64),
        @intFromPtr(&a.slots[7]),
    }) |handle|
        _ = try a.lookup(handle, 1, 1, c.RingInvalid);
    for ([_]u64{ 0, 0x100000000, 0x100000099, 0x200000001 }) |handle|
        _ = try a.lookup(handle, 9, 0, c.RingInvalid);
    _ = try a.lookup(dispatched.handle, 0, 1, c.RingInvalid);
    _ = try a.lookup(dispatched.handle, 1, 2, c.RingInvalid);
    _ = try a.lookup_id(0, 1, c.RingInvalid);
    _ = try a.lookup_id(dispatched.id, 0, c.RingInvalid);
    _ = try a.lookup((@as(u64, 1) << 32) | dispatched.id, 1, 0, c.RingInvalid);
    dispatched.dispatchable = 0; // Fault injection into private representation metadata.
    _ = try a.lookup(dispatched.handle, 1, 1, c.RingInvalid);
    dispatched.dispatchable = 1;
    const old = a_object.handle;
    try a.release(old, 9, 0, 0);
    const replacement = (try a.reserve(9, 0, 0, 0)).?;
    try std.testing.expectEqual(@as(u64, 3), replacement.id);
    try std.testing.expect(replacement.handle != old);
    _ = try a.lookup(old, 9, 0, c.RingInvalid);
    _ = try a.lookup_id(1, 9, c.RingInvalid);
    a.free();
    b.free();
}

test "full table, exhausted counter and allocator-owned storage churn" {
    const slots = try std.testing.allocator.alloc(c.venus_object_t, 32);
    defer std.testing.allocator.free(slots);
    var objects = std.mem.zeroes(c.venus_objects_t);
    var cookie: u32 = 0;
    try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_objects_init, .{
        &objects, slots.ptr, @as(u32, @intCast(slots.len)), 123, &cookie,
    }));
    var output: ?*c.venus_object_t = null;
    for (0..128) |cycle| {
        for (slots, 0..) |_, index| {
            try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_objects_reserve, .{
                &objects, 9, 0, 0, &output,
            }));
            try std.testing.expectEqual(@as(u64, cycle * 32 + index + 1), output.?.id);
        }
        try std.testing.expectEqual(c.RingLimit, @call(.never_inline, venus_objects_reserve, .{
            &objects, 9, 0, 0, &output,
        }));
        try std.testing.expect(output == null);
        objects.live_count -= 1; // Contradictory count must not cause a panic/allocation.
        try std.testing.expectEqual(c.RingCorrupt, @call(.never_inline, venus_objects_reserve, .{
            &objects, 9, 0, 0, &output,
        }));
        objects.live_count += 1;
        for (slots) |slot| try std.testing.expectEqual(c.RingOk, @call(
            .never_inline,
            venus_objects_release,
            .{ &objects, slot.handle, 9, 0 },
        ));
    }
    objects.next_id = std.math.maxInt(u32) - 1;
    try std.testing.expectEqual(c.RingOk, @call(.never_inline, venus_objects_reserve, .{
        &objects, 9, 0, 0, &output,
    }));
    try std.testing.expectEqual(@as(u64, std.math.maxInt(u32) - 1), output.?.id);
    try std.testing.expectEqual(c.RingLimit, @call(.never_inline, venus_objects_reserve, .{
        &objects, 9, 0, 0, &output,
    }));
    @call(.never_inline, venus_objects_free, .{&objects});
    for (slots) |slot| try std.testing.expect(std.meta.eql(slot, std.mem.zeroes(c.venus_object_t)));
    try std.testing.expect(std.meta.eql(objects, std.mem.zeroes(c.venus_objects_t)));
}

test "null/empty APIs and initialization overflow/overlap preserve storage" {
    var fixture = fixture_t{};
    @memset(&fixture.slots, std.mem.zeroes(c.venus_object_t));
    var output: ?*c.venus_object_t = null;
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_objects_init, .{
        null, &fixture.slots, 8, 1, &fixture,
    }));
    const cases = [_]struct {
        slots: ?[*]c.venus_object_t,
        capacity: u32,
        namespace_id: u32,
        context: ?*anyopaque,
    }{
        .{ .slots = null, .capacity = 8, .namespace_id = 1, .context = &fixture },
        .{ .slots = &fixture.slots, .capacity = 0, .namespace_id = 1, .context = &fixture },
        .{ .slots = &fixture.slots, .capacity = 4097, .namespace_id = 1, .context = &fixture },
        .{ .slots = &fixture.slots, .capacity = 8, .namespace_id = 0, .context = &fixture },
        .{ .slots = &fixture.slots, .capacity = 8, .namespace_id = 1, .context = null },
        .{
            .slots = @ptrCast(&fixture.objects),
            .capacity = 1,
            .namespace_id = 1,
            .context = &fixture,
        },
        .{
            .slots = @ptrFromInt(std.math.maxInt(usize) - 7),
            .capacity = 8,
            .namespace_id = 1,
            .context = &fixture,
        },
    };
    for (cases) |case| try std.testing.expectEqual(c.RingInvalid, @call(
        .never_inline,
        venus_objects_init,
        .{ &fixture.objects, case.slots, case.capacity, case.namespace_id, case.context },
    ));
    _ = try fixture.reserve(9, 0, 0, c.RingInvalid);
    _ = try fixture.lookup(1, 9, 0, c.RingInvalid);
    _ = try fixture.lookup_id(1, 9, c.RingInvalid);
    try fixture.release(1, 9, 0, c.RingInvalid);
    try fixture.init(1);
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_objects_init, .{
        &fixture.objects, &fixture.slots, 8, 2, &fixture,
    }));
    for ([_]?*c.venus_objects_t{ null, &fixture.objects }) |objects| {
        try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_objects_reserve, .{
            objects, 9, 0, 0, @as(?*?*c.venus_object_t, null),
        }));
        try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_objects_lookup, .{
            objects, 1, 9, 0, @as(?*?*c.venus_object_t, null),
        }));
        try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_objects_lookup_id, .{
            objects, 1, 9, @as(?*?*c.venus_object_t, null),
        }));
    }
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_objects_reserve, .{
        null, 9, 0, 0, &output,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_objects_lookup, .{
        null, 1, 9, 0, &output,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_objects_lookup_id, .{
        null, 1, 9, &output,
    }));
    try std.testing.expectEqual(c.RingInvalid, @call(.never_inline, venus_objects_release, .{
        null, 1, 9, 0,
    }));
    fixture.free();
}
