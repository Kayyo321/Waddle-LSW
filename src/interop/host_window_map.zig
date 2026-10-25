//! Pure bounded mapping of guest `(window, generation)` identities to host surfaces (TODO #11a).
//!
//! The table holds at most 64 realized windows per session epoch. It decides whether one announced
//! identity may be realized and which identities a retire removes, and in which order; it performs
//! no native or Wayland call. Realized entries are kept in realization order, so every parent
//! precedes its children. The module owns only caller storage, allocates nothing, and never wraps a
//! counter.
//!
//! A child whose exact parent pair is not realized is refused as a stale parent, never deferred. The
//! guest defers such an announcement until the parent is exported (IMPL_DESC section 3.2.8), so the
//! host keeps no deferred state. The contract is
//! `impl/seamless-interop/claude_45_percent/host_window_map_contract.md`.
const std = @import("std");

/// Maximum realized windows (exported surfaces) per session epoch.
pub const MaxRealized: usize = 64;
/// Minimum caller output capacity accepted by retire.
pub const MaxOutput: usize = 64;

/// Frozen `interop_window_role_t` value: Toplevel, xdg_toplevel without a parent.
pub const RoleToplevel: u8 = 1;
/// Frozen `interop_window_role_t` value: Dialog, xdg_toplevel with an optional parent.
pub const RoleDialog: u8 = 2;
/// Frozen `interop_window_role_t` value: Menu, grabbing xdg_popup that requires a parent.
pub const RoleMenu: u8 = 3;
/// Frozen `interop_window_role_t` value: Tooltip, non-grabbing xdg_popup that requires a parent.
pub const RoleTooltip: u8 = 4;
/// Frozen `interop_window_role_t` value: UacConsent, xdg_toplevel without a parent.
pub const RoleUacConsent: u8 = 5;
/// Frozen `interop_window_role_t` value: ImePreedit, non-grabbing xdg_popup that requires a parent.
pub const RoleImePreedit: u8 = 6;

/// Exact identity of one window incarnation. Both fields zero is "none" when used as a parent.
pub const window_ref_t = extern struct {
    /// Protocol window handle (HWND value). Zero means no window.
    window: u64,
    /// Protocol incarnation generation, never reused within an epoch. Zero means no generation.
    generation: u64,
};

/// One realized window, stored in realization order.
pub const window_entry_t = extern struct {
    /// Realized identity.
    window: window_ref_t,
    /// Requested parent identity, or none. Retire cascades follow this field.
    parent: window_ref_t,
    /// Popup only: positioning root, a non-popup window. None for every other role.
    root: window_ref_t,
    /// Raw `interop_window_role_t` value, 1..6.
    role: u8,
};

/// Complete table state. The caller owns the storage; the table holds no pointers.
pub const host_window_map_t = extern struct {
    /// Number of initialized entries at the front of `realized`, at most 64.
    realized_count: u32,
    /// Realized windows in realization order.
    realized: [MaxRealized]window_entry_t,
};

/// Result code of `announce`.
pub const announce_status_t = enum(c_int) {
    /// The window is realized and appended to the table.
    Realize = 1,
    /// The exact pair is already realized. No change.
    RefuseDuplicate = 2,
    /// The parent pair is not realized: it was never announced, was retired, or its window is
    /// realized under another generation. No change.
    RefuseStaleParent = 3,
    /// The window is realized under another generation; that generation must be retired first.
    RefuseWindowConflict = 4,
    /// The table already holds 64 realized windows. No change.
    RefuseExhausted = 5,
    /// Zero identity, invalid role, invalid parent shape. No change.
    RefuseInvalid = 6,
};

/// Result code of `retire`.
pub const retire_status_t = enum(c_int) {
    /// The realized subtree was removed; the output list and count describe it.
    Retired = 1,
    /// No realized entry with that exact pair exists (never realized, already retired, or another
    /// generation). No change.
    Stale = 2,
    /// Zero identity or output capacity below MaxOutput. No change.
    RefuseInvalid = 3,
};

/// Result of `retire`. `count` is the number of entries written to the output list.
pub const retire_result_t = extern struct {
    /// Outcome of the retire.
    status: retire_status_t,
    /// Removed windows written to the output list, target included; zero unless Retired.
    count: u32,
};

/// Whether a window reference names a real identity (both fields nonzero).
fn valid_ref(id: window_ref_t) bool {
    return id.window != 0 and id.generation != 0;
}

/// Whether a parent reference is valid or none (window and generation are zero together).
fn valid_optional_ref(id: window_ref_t) bool {
    return if (id.window == 0) id.generation == 0 else id.generation != 0;
}

fn valid_role(role: u8) bool {
    return role >= RoleToplevel and role <= RoleImePreedit;
}

fn is_popup_role(role: u8) bool {
    return role == RoleMenu or role == RoleTooltip or role == RoleImePreedit;
}

fn forbids_parent(role: u8) bool {
    return role == RoleToplevel or role == RoleUacConsent;
}

fn same_ref(left: window_ref_t, right: window_ref_t) bool {
    return left.window == right.window and left.generation == right.generation;
}

fn realized_len(map: *const host_window_map_t) usize {
    return map.realized_count;
}

fn find_realized(map: *const host_window_map_t, window: u64) ?usize {
    for (map.realized[0..realized_len(map)], 0..) |entry, index| {
        if (entry.window.window == window) return index;
    }
    return null;
}

/// Clear the table. Zero-filled storage is the same state. Callers must reset before first use
/// of storage they did not zero themselves. Owner-thread only; emits no output.
///
/// @param map Table to clear; must not be null.
pub fn reset(map: *host_window_map_t) void {
    map.* = std.mem.zeroes(host_window_map_t);
}

/// Look up a realized entry by exact identity.
///
/// @param map Table; read only.
/// @param window Identity to find; both fields must match.
/// @return A copy of the realized entry, or null when `window.window` is not realized under
///         `window.generation`.
pub fn lookup(map: *const host_window_map_t, window: window_ref_t) ?window_entry_t {
    const index = find_realized(map, window.window) orelse return null;
    const entry = map.realized[index];
    if (entry.window.generation != window.generation) return null;
    return entry;
}

/// Whether the exact identity is currently realized.
///
/// @param map Table; read only.
/// @param window Identity to test.
/// @return True exactly when `lookup` would return an entry.
pub fn is_current(map: *const host_window_map_t, window: window_ref_t) bool {
    return lookup(map, window) != null;
}

/// Number of realized windows in the table.
///
/// @param map Table; read only.
/// @return Current realized count, never above MaxRealized.
pub fn realized_count(map: *const host_window_map_t) u32 {
    return map.realized_count;
}

/// Realize one window if its parent, when named, is exactly realized.
///
/// A parent pair that is not realized is refused as a stale parent. The table never defers.
/// The guest announces a child only after its parent is exported (IMPL_DESC section 3.2.8).
///
/// @param map Table; must not be null.
/// @param window Identity to realize; both fields nonzero.
/// @param parent Exact parent identity, or none (zero in both fields). Menu, Tooltip and ImePreedit
///               require a realized parent. Toplevel and UacConsent require none.
/// @param role Raw role value 1..6.
/// @return Status. Changes state only for Realize.
/// @thread_safety Owner thread only; no internal synchronization.
pub fn announce(map: *host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8) announce_status_t {
    if (!valid_ref(window) or !valid_optional_ref(parent) or !valid_role(role)) return .RefuseInvalid;
    if (parent.window == window.window) return .RefuseInvalid;
    if (forbids_parent(role) and parent.window != 0) return .RefuseInvalid;
    if (is_popup_role(role) and parent.window == 0) return .RefuseInvalid;

    if (find_realized(map, window.window)) |index| {
        return if (map.realized[index].window.generation == window.generation) .RefuseDuplicate else .RefuseWindowConflict;
    }
    if (parent.window != 0) {
        const index = find_realized(map, parent.window) orelse return .RefuseStaleParent;
        if (map.realized[index].window.generation != parent.generation) return .RefuseStaleParent;
    }
    if (realized_len(map) == MaxRealized) return .RefuseExhausted;

    append_realized(map, window, parent, role);
    return .Realize;
}

/// Append a realized entry. A popup's parent is realized by the caller's check.
fn append_realized(map: *host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8) void {
    const root = if (is_popup_role(role)) popup_root(map, parent) else window_ref_t{ .window = 0, .generation = 0 };
    map.realized[realized_len(map)] = .{ .window = window, .parent = parent, .root = root, .role = role };
    map.realized_count += 1;
}

/// Positioning root of a popup whose parent is realized: a popup parent contributes its own root,
/// any other parent is its own root.
fn popup_root(map: *const host_window_map_t, parent: window_ref_t) window_ref_t {
    const index = find_realized(map, parent.window) orelse unreachable;
    const entry = map.realized[index];
    return if (is_popup_role(entry.role)) entry.root else entry.window;
}

/// Remove a realized window and its realized subtree.
///
/// The output lists the removed pairs in reverse realization order: every descendant precedes its
/// ancestor and the target is last. The subtree follows requested parents to any depth.
///
/// @param map Table; must not be null and must not alias `out`.
/// @param window Identity to retire; both fields nonzero and matching the realized generation.
/// @param out Output list, length at least MaxOutput. Written only when the status is Retired.
/// @return Status and the number of removed windows. Stale and refusal results change nothing.
/// @thread_safety Owner thread only; no internal synchronization.
pub fn retire(map: *host_window_map_t, window: window_ref_t, out: []window_ref_t) retire_result_t {
    if (out.len < MaxOutput or !valid_ref(window)) return .{ .status = .RefuseInvalid, .count = 0 };
    const target = find_realized(map, window.window) orelse return .{ .status = .Stale, .count = 0 };
    if (map.realized[target].window.generation != window.generation) return .{ .status = .Stale, .count = 0 };

    const len = realized_len(map);
    var remove = [_]bool{false} ** MaxRealized;
    remove[target] = true;
    // Parents precede children in realization order, so one forward pass closes the subtree.
    for (target + 1..len) |index| {
        const parent = map.realized[index].parent;
        if (find_realized(map, parent.window)) |parent_index| {
            if (remove[parent_index] and same_ref(map.realized[parent_index].window, parent)) remove[index] = true;
        }
    }

    var count: usize = 0;
    var position = len;
    while (position > 0) {
        position -= 1;
        if (!remove[position]) continue;
        out[count] = map.realized[position].window;
        count += 1;
    }
    compact_realized(map, &remove);
    return .{ .status = .Retired, .count = @intCast(count) };
}

/// Remove realized entries flagged in `remove`, keeping realization order.
fn compact_realized(map: *host_window_map_t, remove: []const bool) void {
    var write: usize = 0;
    for (0..realized_len(map)) |read| {
        if (remove[read]) continue;
        if (write != read) map.realized[write] = map.realized[read];
        write += 1;
    }
    map.realized_count = @intCast(write);
}

// ---- C ABI ------------------------------------------------------------------------------------

/// Half-open address range used to reject aliased C arguments before any write.
const region_t = struct {
    start: usize,
    len: usize,
};

fn region_of_value(value: anytype) region_t {
    return .{ .start = @intFromPtr(value), .len = @sizeOf(std.meta.Child(@TypeOf(value))) };
}

fn region_of_slice(slice: anytype) region_t {
    return .{ .start = @intFromPtr(slice.ptr), .len = slice.len * @sizeOf(std.meta.Child(@TypeOf(slice))) };
}

/// Whether no two regions overlap. Saturating ends never wrap, so a region near the top of the
/// address space is still compared correctly.
fn regions_disjoint(regions: []const region_t) bool {
    for (regions, 0..) |left, index| {
        for (regions[index + 1 ..]) |right| {
            if (left.start < right.start +| right.len and right.start < left.start +| left.len) return false;
        }
    }
    return true;
}

/// C ABI: clear the table. A NULL `map` is ignored.
///
/// @param map Table to clear; NULL is a no-op. In, out.
/// @thread_safety Owner thread only.
export fn host_window_map_reset(map: ?*host_window_map_t) callconv(.C) void {
    reset(map orelse return);
}

/// C ABI: realize one window. See `announce`.
///
/// @param map Table; NULL gives RefuseInvalid. In, out.
/// @param window Identity by value; both fields nonzero. In.
/// @param parent Parent identity by value; both fields zero for none. In.
/// @param role Raw `interop_window_role_t` value 1..6. In.
/// @return Announce status. Thread safety: owner thread only.
export fn host_window_map_announce(map: ?*host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8) callconv(.C) announce_status_t {
    const target = map orelse return .RefuseInvalid;
    return announce(target, window, parent, role);
}

/// C ABI: remove a realized subtree. See `retire` for the output order.
///
/// @param map Table; NULL gives RefuseInvalid. In, out.
/// @param window Identity by value; both fields nonzero. In.
/// @param out Output array of at least `out_capacity` entries; NULL gives RefuseInvalid. Written only
///            on Retired. Must not overlap `map` or `out_count`. Out.
/// @param out_capacity Entries available in `out`; below 64 gives RefuseInvalid. In.
/// @param out_count Receives the removed count; NULL gives RefuseInvalid. Must not overlap `map` or `out`. Out.
/// @return Retire status. Argument refusals write no memory.
/// @thread_safety Owner thread only; no internal synchronization.
export fn host_window_map_retire(map: ?*host_window_map_t, window: window_ref_t, out: ?[*]window_ref_t, out_capacity: u32, out_count: ?*u32) callconv(.C) retire_status_t {
    const count_ptr = out_count orelse return .RefuseInvalid;
    const target = map orelse return .RefuseInvalid;
    const out_base = out orelse return .RefuseInvalid;
    if (out_capacity < MaxOutput) return .RefuseInvalid;
    const out_slice = out_base[0..out_capacity];
    if (!regions_disjoint(&.{ region_of_value(target), region_of_slice(out_slice), region_of_value(count_ptr) })) return .RefuseInvalid;
    const result = retire(target, window, out_slice);
    count_ptr.* = result.count;
    return result.status;
}

/// C ABI: exact-identity lookup of a realized entry.
///
/// @param map Table, read only; NULL gives false. In.
/// @param window Identity by value. In.
/// @param out Receives a copy of the entry when found; NULL is a presence test. Out.
/// @return True when `window` is realized under its generation.
/// @thread_safety Owner thread only.
export fn host_window_map_lookup(map: ?*const host_window_map_t, window: window_ref_t, out: ?*window_entry_t) callconv(.C) bool {
    const target = map orelse return false;
    const entry = lookup(target, window) orelse return false;
    if (out) |destination| destination.* = entry;
    return true;
}

/// C ABI: whether the exact identity is realized.
///
/// @param map Table, read only; NULL gives false. In.
/// @param window Identity by value. In.
/// @return True exactly when `host_window_map_lookup` would succeed.
/// @thread_safety Owner thread only.
export fn host_window_map_is_current(map: ?*const host_window_map_t, window: window_ref_t) callconv(.C) bool {
    const target = map orelse return false;
    return is_current(target, window);
}

/// C ABI: number of realized windows; NULL gives 0.
///
/// @param map Table, read only. In.
/// @return Realized count, at most 64.
/// @thread_safety Owner thread only.
export fn host_window_map_realized_count(map: ?*const host_window_map_t) callconv(.C) u32 {
    return if (map) |target| realized_count(target) else 0;
}

comptime {
    // Layout frozen by src/interop/host_window_map.h (and checked there by _Static_assert).
    if (@sizeOf(window_ref_t) != 16 or @sizeOf(window_entry_t) != 56) @compileError("host window map entry layout changed");
    if (@sizeOf(host_window_map_t) != 3592) @compileError("host window map table layout changed");
}

// Test-only fixtures.
const testing = std.testing;

fn ref(window: u64, generation: u64) window_ref_t {
    return .{ .window = window, .generation = generation };
}

fn none() window_ref_t {
    return ref(0, 0);
}

fn expect_ref(expected: window_ref_t, actual: window_ref_t) !void {
    try testing.expectEqual(expected.window, actual.window);
    try testing.expectEqual(expected.generation, actual.generation);
}

fn expect_absent(map: *const host_window_map_t, window: window_ref_t) !void {
    try testing.expect(lookup(map, window) == null);
    try testing.expect(!is_current(map, window));
}

fn new_map() host_window_map_t {
    var map: host_window_map_t = undefined;
    reset(&map);
    return map;
}

fn new_out() [MaxOutput]window_ref_t {
    return std.mem.zeroes([MaxOutput]window_ref_t);
}

/// The public functions are called through `never_inline` shims so that the optimized bodies keep
/// their own branches. A call with constant arguments would otherwise fold refusal paths away.
const abi_shim_t = struct {
    fn reset(map: ?*host_window_map_t) void {
        @call(.never_inline, host_window_map_reset, .{map});
    }

    fn announce(map: ?*host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8) announce_status_t {
        return @call(.never_inline, host_window_map_announce, .{ map, window, parent, role });
    }

    fn retire(map: ?*host_window_map_t, window: window_ref_t, out: ?[*]window_ref_t, out_capacity: u32, out_count: ?*u32) retire_status_t {
        return @call(.never_inline, host_window_map_retire, .{ map, window, out, out_capacity, out_count });
    }

    fn lookup(map: ?*const host_window_map_t, window: window_ref_t, out: ?*window_entry_t) bool {
        return @call(.never_inline, host_window_map_lookup, .{ map, window, out });
    }

    fn is_current(map: ?*const host_window_map_t, window: window_ref_t) bool {
        return @call(.never_inline, host_window_map_is_current, .{ map, window });
    }

    fn realized(map: ?*const host_window_map_t) u32 {
        return @call(.never_inline, host_window_map_realized_count, .{map});
    }
};

test "reset produces an empty table and zero-filled storage is the same state" {
    var map = new_map();
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
    try expect_absent(&map, ref(1, 1));
    var zeroed: host_window_map_t = std.mem.zeroes(host_window_map_t);
    try testing.expectEqual(@as(u32, 0), realized_count(&zeroed));
}

test "announce toplevel realizes and records a rootless entry" {
    var map = new_map();
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(10, 1), none(), RoleToplevel));
    try testing.expect(is_current(&map, ref(10, 1)));
    const entry = lookup(&map, ref(10, 1)).?;
    try testing.expectEqual(RoleToplevel, entry.role);
    try expect_ref(none(), entry.parent);
    try expect_ref(none(), entry.root);
}

test "every announce refusal reports its status and changes nothing" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    const before = map;

    // Invalid identity and parent shapes.
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(0, 1), none(), RoleToplevel));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 0), none(), RoleToplevel));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), ref(0, 1), RoleDialog));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), ref(1, 0), RoleDialog));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), ref(2, 1), RoleDialog));
    // Invalid roles.
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), none(), 0));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), none(), 7));
    // Role parent rules.
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), ref(1, 1), RoleToplevel));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), ref(1, 1), RoleUacConsent));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), none(), RoleMenu));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), none(), RoleTooltip));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), none(), RoleImePreedit));

    // Duplicate and window conflict.
    try testing.expectEqual(announce_status_t.RefuseDuplicate, announce(&map, ref(1, 1), none(), RoleToplevel));
    try testing.expectEqual(announce_status_t.RefuseWindowConflict, announce(&map, ref(1, 2), none(), RoleToplevel));

    try testing.expectEqual(before.realized_count, map.realized_count);
    try expect_ref(before.realized[0].window, map.realized[0].window);
    try testing.expectEqual(before.realized[0].role, map.realized[0].role);
}

test "a child whose parent was never announced is refused as a stale parent and not stored" {
    var map = new_map();
    try testing.expectEqual(announce_status_t.RefuseStaleParent, announce(&map, ref(2, 1), ref(9, 1), RoleDialog));
    try testing.expectEqual(announce_status_t.RefuseStaleParent, announce(&map, ref(2, 1), ref(9, 1), RoleMenu));
    try expect_absent(&map, ref(2, 1));
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
    // The guest resends once the parent is exported; then the same child realizes.
    _ = announce(&map, ref(9, 1), none(), RoleToplevel);
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(2, 1), ref(9, 1), RoleDialog));
}

test "a parent realized under another generation is a stale parent" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    try testing.expectEqual(announce_status_t.RefuseStaleParent, announce(&map, ref(2, 1), ref(1, 2), RoleDialog));
    try testing.expectEqual(@as(u32, 1), realized_count(&map));
}

test "a child announced after its parent was retired is a stale parent" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    try testing.expectEqual(retire_status_t.Retired, retire(&map, ref(1, 1), &out).status);
    try testing.expectEqual(announce_status_t.RefuseStaleParent, announce(&map, ref(2, 1), ref(1, 1), RoleDialog));
    try testing.expectEqual(announce_status_t.RefuseStaleParent, announce(&map, ref(3, 1), ref(1, 1), RoleMenu));
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
}

test "dialog without a parent and uac consent are realized without a parent" {
    var map = new_map();
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(1, 1), none(), RoleDialog));
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(2, 1), none(), RoleUacConsent));
    try testing.expectEqual(RoleUacConsent, lookup(&map, ref(2, 1)).?.role);
}

test "a dialog keeps its parent and its own root stays none" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(2, 1), ref(1, 1), RoleDialog));
    const dialog = lookup(&map, ref(2, 1)).?;
    try expect_ref(ref(1, 1), dialog.parent);
    try expect_ref(none(), dialog.root);
    try testing.expectEqual(RoleDialog, dialog.role);
}

test "popup of a toplevel and of a dialog records that window as its root" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleDialog);
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(3, 1), ref(1, 1), RoleMenu));
    try expect_ref(ref(1, 1), lookup(&map, ref(3, 1)).?.root);
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(4, 1), ref(2, 1), RoleTooltip));
    try expect_ref(ref(2, 1), lookup(&map, ref(4, 1)).?.root);
}

test "popup of a popup is re-parented to the root while keeping the requested parent" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleMenu);
    _ = announce(&map, ref(3, 1), ref(2, 1), RoleTooltip);
    _ = announce(&map, ref(4, 1), ref(3, 1), RoleImePreedit);
    const grandchild = lookup(&map, ref(4, 1)).?;
    try expect_ref(ref(3, 1), grandchild.parent);
    try expect_ref(ref(1, 1), grandchild.root);
    try expect_ref(ref(1, 1), lookup(&map, ref(3, 1)).?.root);

    // The cascade follows the requested parent chain, so the whole popup chain goes with its root.
    const result = retire(&map, ref(1, 1), &out);
    try testing.expectEqual(@as(u32, 4), result.count);
    try expect_ref(ref(4, 1), out[0]);
    try expect_ref(ref(1, 1), out[3]);
}

test "retire cascades children first in reverse realization order and leaves unrelated windows" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleDialog);
    _ = announce(&map, ref(3, 1), ref(2, 1), RoleDialog);
    _ = announce(&map, ref(4, 1), ref(1, 1), RoleDialog);
    _ = announce(&map, ref(5, 1), none(), RoleToplevel);
    // An unrelated child of a surviving window must not be swept up by the cascade.
    _ = announce(&map, ref(6, 1), ref(5, 1), RoleDialog);

    const result = retire(&map, ref(1, 1), &out);
    try testing.expectEqual(retire_status_t.Retired, result.status);
    try testing.expectEqual(@as(u32, 4), result.count);
    try expect_ref(ref(4, 1), out[0]);
    try expect_ref(ref(3, 1), out[1]);
    try expect_ref(ref(2, 1), out[2]);
    try expect_ref(ref(1, 1), out[3]);
    try testing.expectEqual(@as(u32, 2), realized_count(&map));
    try testing.expect(is_current(&map, ref(5, 1)));
    try testing.expect(is_current(&map, ref(6, 1)));
}

test "retire of unknown, stale, retired and invalid pairs changes nothing" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    try testing.expectEqual(retire_status_t.Stale, retire(&map, ref(3, 1), &out).status);
    try testing.expectEqual(retire_status_t.Stale, retire(&map, ref(1, 2), &out).status);
    try testing.expectEqual(retire_status_t.RefuseInvalid, retire(&map, ref(0, 1), &out).status);
    try testing.expectEqual(retire_status_t.RefuseInvalid, retire(&map, ref(1, 0), &out).status);
    var short: [MaxOutput - 1]window_ref_t = undefined;
    try testing.expectEqual(retire_status_t.RefuseInvalid, retire(&map, ref(1, 1), &short).status);
    try testing.expectEqual(@as(u32, 1), realized_count(&map));

    try testing.expectEqual(retire_status_t.Retired, retire(&map, ref(1, 1), &out).status);
    try testing.expectEqual(retire_status_t.Stale, retire(&map, ref(1, 1), &out).status);
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
}

test "exactly 64 realized windows succeed and the 65th is refused" {
    var map = new_map();
    for (1..MaxRealized + 1) |index| {
        try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(index, 1), none(), RoleToplevel));
    }
    try testing.expectEqual(@as(u32, 64), realized_count(&map));
    try testing.expectEqual(announce_status_t.RefuseExhausted, announce(&map, ref(65, 1), none(), RoleToplevel));
    try testing.expectEqual(announce_status_t.RefuseExhausted, announce(&map, ref(65, 1), ref(1, 1), RoleDialog));
    try testing.expectEqual(@as(u32, 64), realized_count(&map));
    // Retiring one window frees a slot; the refused window then realizes.
    var out = new_out();
    _ = retire(&map, ref(64, 1), &out);
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(65, 1), none(), RoleToplevel));
    try testing.expectEqual(@as(u32, 64), realized_count(&map));
}

test "epoch reset clears every realized entry" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleDialog);
    reset(&map);
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
    try expect_absent(&map, ref(1, 1));
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(1, 1), none(), RoleToplevel));
}

// Reference model: a deliberately simple, separately written implementation of the same contract.
// It uses growing lists and recursion where the module uses fixed arrays and one forward pass, so
// agreement is evidence rather than a copy of the same code.
const reference_model_t = struct {
    realized: std.ArrayList(window_entry_t),

    fn model_init() reference_model_t {
        return .{ .realized = std.ArrayList(window_entry_t).init(testing.allocator) };
    }

    fn model_deinit(self: *reference_model_t) void {
        self.realized.deinit();
    }

    fn model_clear(self: *reference_model_t) void {
        self.realized.clearRetainingCapacity();
    }

    fn model_find(self: *const reference_model_t, window: u64) ?usize {
        for (self.realized.items, 0..) |entry, index| {
            if (entry.window.window == window) return index;
        }
        return null;
    }

    fn model_root(self: *const reference_model_t, parent: window_ref_t, role: u8) window_ref_t {
        if (!is_popup_role(role)) return none();
        const index = self.model_find(parent.window).?;
        const entry = self.realized.items[index];
        return if (is_popup_role(entry.role)) entry.root else entry.window;
    }

    fn model_announce(self: *reference_model_t, window: window_ref_t, parent: window_ref_t, role: u8) !announce_status_t {
        if (window.window == 0 or window.generation == 0) return .RefuseInvalid;
        if ((parent.window == 0) != (parent.generation == 0)) return .RefuseInvalid;
        if (parent.window == window.window) return .RefuseInvalid;
        if (role < 1 or role > 6) return .RefuseInvalid;
        const popup = role == 3 or role == 4 or role == 6;
        if (popup and parent.window == 0) return .RefuseInvalid;
        if ((role == 1 or role == 5) and parent.window != 0) return .RefuseInvalid;
        if (self.model_find(window.window)) |index| {
            return if (self.realized.items[index].window.generation == window.generation) .RefuseDuplicate else .RefuseWindowConflict;
        }
        if (parent.window != 0) {
            const index = self.model_find(parent.window) orelse return .RefuseStaleParent;
            if (self.realized.items[index].window.generation != parent.generation) return .RefuseStaleParent;
        }
        if (self.realized.items.len >= MaxRealized) return .RefuseExhausted;
        try self.realized.append(.{ .window = window, .parent = parent, .root = self.model_root(parent, role), .role = role });
        return .Realize;
    }

    fn model_collect(self: *const reference_model_t, index: usize, into: *std.ArrayList(usize)) !void {
        try into.append(index);
        const pair = self.realized.items[index].window;
        for (self.realized.items, 0..) |entry, child| {
            if (!same_ref(entry.parent, pair)) continue;
            try self.model_collect(child, into);
        }
    }

    fn model_retire(self: *reference_model_t, window: window_ref_t, out: *std.ArrayList(window_ref_t)) !retire_status_t {
        out.clearRetainingCapacity();
        if (window.window == 0 or window.generation == 0) return .RefuseInvalid;
        const target = self.model_find(window.window) orelse return .Stale;
        if (self.realized.items[target].window.generation != window.generation) return .Stale;

        var removed = std.ArrayList(usize).init(testing.allocator);
        defer removed.deinit();
        try self.model_collect(target, &removed);
        std.mem.sort(usize, removed.items, {}, std.sort.desc(usize));
        for (removed.items) |index| try out.append(self.realized.items[index].window);
        for (removed.items) |index| _ = self.realized.orderedRemove(index);
        return .Retired;
    }
};

fn expect_same_state(map: *const host_window_map_t, model: *const reference_model_t) !void {
    try testing.expectEqual(model.realized.items.len, realized_count(map));
    for (model.realized.items, 0..) |entry, index| {
        try expect_ref(entry.window, map.realized[index].window);
        try expect_ref(entry.parent, map.realized[index].parent);
        try expect_ref(entry.root, map.realized[index].root);
        try testing.expectEqual(entry.role, map.realized[index].role);
    }
}

fn run_random_sequence(seed: u64, windows: u64, operations: usize) !void {
    var prng = std.rand.DefaultPrng.init(seed);
    const random = prng.random();
    var map = new_map();
    var model = reference_model_t.model_init();
    defer model.model_deinit();
    var out = new_out();
    var out_list = std.ArrayList(window_ref_t).init(testing.allocator);
    defer out_list.deinit();

    var step: usize = 0;
    while (step < operations) : (step += 1) {
        const choice = random.uintLessThan(u8, 100);
        if (choice < 2) {
            reset(&map);
            model.model_clear();
        } else if (choice < 70) {
            // Zero and mismatched identities appear on purpose.
            const window = ref(random.uintLessThan(u64, windows + 1), random.uintLessThan(u64, 4));
            const parent = if (random.uintLessThan(u8, 10) < 3) none() else ref(random.uintLessThan(u64, windows + 1), random.uintLessThan(u64, 4));
            const role = random.uintLessThan(u8, 9);
            const got = announce(&map, window, parent, role);
            const expected = try model.model_announce(window, parent, role);
            try testing.expectEqual(expected, got);
        } else {
            const window = ref(random.uintLessThan(u64, windows + 1), random.uintLessThan(u64, 4));
            const got = retire(&map, window, &out);
            const expected = try model.model_retire(window, &out_list);
            try testing.expectEqual(expected, got.status);
            if (got.status == .Retired) {
                try testing.expectEqual(@as(u32, @intCast(out_list.items.len)), got.count);
                for (out_list.items, 0..) |pair, index| try expect_ref(pair, out[index]);
            }
        }
        try expect_same_state(&map, &model);
    }
}

test "randomized sequences agree with the reference model over small and large universes" {
    try run_random_sequence(1, 6, 4000);
    try run_random_sequence(2, 12, 4000);
    try run_random_sequence(3, 40, 4000);
    try run_random_sequence(4, 120, 6000);
    try run_random_sequence(0x9E3779B97F4A7C15, 200, 6000);
}

test "C ABI counts and lookup accept NULL and report the table" {
    try testing.expectEqual(@as(u32, 0), abi_shim_t.realized(null));
    try testing.expect(!abi_shim_t.is_current(null, ref(1, 1)));
    try testing.expect(!abi_shim_t.lookup(null, ref(1, 1), null));

    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    abi_shim_t.reset(null);
    try testing.expectEqual(@as(u32, 1), abi_shim_t.realized(&map));
    abi_shim_t.reset(&map);
    try testing.expectEqual(@as(u32, 0), abi_shim_t.realized(&map));
    try testing.expect(!abi_shim_t.is_current(&map, ref(1, 1)));
}

test "C ABI announce and retire write outputs and counts on success" {
    var map = new_map();
    var out = new_out();
    var count: u32 = 99;
    try testing.expectEqual(announce_status_t.Realize, abi_shim_t.announce(&map, ref(1, 1), none(), RoleToplevel));
    try testing.expectEqual(announce_status_t.RefuseStaleParent, abi_shim_t.announce(&map, ref(2, 1), ref(9, 1), RoleDialog));
    try testing.expectEqual(announce_status_t.RefuseInvalid, abi_shim_t.announce(null, ref(2, 1), none(), RoleToplevel));
    try testing.expectEqual(announce_status_t.Realize, abi_shim_t.announce(&map, ref(9, 1), none(), RoleToplevel));
    try testing.expectEqual(announce_status_t.Realize, abi_shim_t.announce(&map, ref(2, 1), ref(9, 1), RoleDialog));

    try testing.expectEqual(retire_status_t.Retired, abi_shim_t.retire(&map, ref(9, 1), &out, MaxOutput, &count));
    try testing.expectEqual(@as(u32, 2), count);
    try expect_ref(ref(2, 1), out[0]);
    try expect_ref(ref(9, 1), out[1]);
    try testing.expectEqual(@as(u32, 1), abi_shim_t.realized(&map));
}

test "C ABI rejects NULL, short and aliased arguments without writing" {
    var map = new_map();
    var out = new_out();
    var count: u32 = 77;
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi_shim_t.retire(null, ref(1, 1), &out, MaxOutput, &count));
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi_shim_t.retire(&map, ref(1, 1), null, MaxOutput, &count));
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi_shim_t.retire(&map, ref(1, 1), &out, MaxOutput, null));
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi_shim_t.retire(&map, ref(1, 1), &out, MaxOutput - 1, &count));
    try testing.expectEqual(@as(u32, 77), count);

    // An output array aliasing the table, or a count pointing into it, is refused untouched.
    const alias: [*]window_ref_t = @ptrCast(&map);
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi_shim_t.retire(&map, ref(1, 1), alias, MaxOutput, &count));
    const inside: *u32 = &map.realized_count;
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi_shim_t.retire(&map, ref(1, 1), &out, MaxOutput, inside));
    try testing.expectEqual(@as(u32, 0), map.realized_count);
    try testing.expectEqual(@as(u32, 77), count);
}

test "C ABI lookup returns a copy and a presence test needs no output" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleMenu);
    var entry: window_entry_t = undefined;
    try testing.expect(abi_shim_t.lookup(&map, ref(2, 1), &entry));
    try expect_ref(ref(1, 1), entry.root);
    try testing.expect(abi_shim_t.lookup(&map, ref(1, 1), null));
    try testing.expect(!abi_shim_t.lookup(&map, ref(1, 2), &entry));
    try testing.expect(abi_shim_t.is_current(&map, ref(2, 1)));
    try testing.expect(!abi_shim_t.is_current(&map, ref(3, 1)));
    try testing.expectEqual(@as(u32, 2), abi_shim_t.realized(&map));
}
