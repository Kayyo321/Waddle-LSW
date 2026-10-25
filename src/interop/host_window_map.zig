//! Pure bounded mapping of guest `(window, generation)` identities to host surfaces (TODO #11a).
//!
//! The table holds at most 64 realized windows and at most 64 deferred children per session
//! epoch. It decides which identities the caller must realize or retire and in which order; it
//! performs no native or Wayland call. Realized entries are kept in realization order, so every
//! parent precedes its children. Deferred entries are kept in deferral order. The module owns only
//! caller storage, allocates nothing, and never wraps a counter.
//!
//! The contract is `impl/seamless-interop/claude_45_percent/host_window_map_contract.md`.
const std = @import("std");

/// Maximum realized windows (exported surfaces) per session epoch.
pub const MaxRealized: usize = 64;
/// Maximum deferred children per session epoch.
pub const MaxDeferred: usize = 64;
/// Minimum caller output capacity accepted by announce and retire.
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

/// One deferred child, stored in deferral order.
pub const deferred_entry_t = extern struct {
    /// Announced identity that waits for its parent.
    window: window_ref_t,
    /// Exact parent identity that must become realized before this child can realize.
    parent: window_ref_t,
    /// Raw `interop_window_role_t` value, 1..6.
    role: u8,
};

/// Complete table state. The caller owns the storage; the table holds no pointers.
pub const host_window_map_t = extern struct {
    /// Number of initialized entries at the front of `realized`.
    realized_count: u32,
    /// Number of initialized entries at the front of `deferred`.
    deferred_count: u32,
    /// Realized windows in realization order.
    realized: [MaxRealized]window_entry_t,
    /// Deferred children in deferral order.
    deferred: [MaxDeferred]deferred_entry_t,
};

/// Result code of `announce`.
pub const announce_status_t = enum(c_int) {
    /// The window and the parent-closed deferred children were realized; see the output list.
    Realize = 1,
    /// The window waits for a parent that is not realized; it was appended to the deferred table.
    Defer = 2,
    /// The exact pair is already realized or already deferred.
    RefuseDuplicate = 3,
    /// The parent window is realized under a different generation; the reference is stale.
    RefuseStaleParent = 4,
    /// The window is known under a different generation; that generation must be retired first.
    RefuseWindowConflict = 5,
    /// A table limit or the realize closure would exceed 64 realized or 64 deferred entries.
    RefuseExhausted = 6,
    /// Zero identity, invalid role, invalid parent shape, or output capacity below MaxOutput.
    RefuseInvalid = 7,
};

/// Result of `announce`. `count` is the number of entries written to the output list.
pub const announce_result_t = extern struct {
    /// Outcome of the announce.
    status: announce_status_t,
    /// Output entries written; nonzero only for Realize.
    count: u32,
};

/// Result code of `retire`.
pub const retire_status_t = enum(c_int) {
    /// The realized subtree was removed; output lists and counts describe it.
    Retired = 1,
    /// No realized entry with that window and generation exists; nothing changed.
    Stale = 2,
    /// Zero identity or output capacity below MaxOutput; nothing changed.
    RefuseInvalid = 3,
};

/// Result of `retire`. Counts are the number of entries written to each output list.
pub const retire_result_t = extern struct {
    /// Outcome of the retire.
    status: retire_status_t,
    /// Realized windows removed, target included.
    retired_count: u32,
    /// Deferred children discarded because their parent pair was removed.
    discarded_count: u32,
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

fn deferred_len(map: *const host_window_map_t) usize {
    return map.deferred_count;
}

fn find_realized(map: *const host_window_map_t, window: u64) ?usize {
    for (map.realized[0..realized_len(map)], 0..) |entry, index| {
        if (entry.window.window == window) return index;
    }
    return null;
}

fn find_deferred(map: *const host_window_map_t, window: u64) ?usize {
    for (map.deferred[0..deferred_len(map)], 0..) |entry, index| {
        if (entry.window.window == window) return index;
    }
    return null;
}

fn refuse(status: announce_status_t) announce_result_t {
    return .{ .status = status, .count = 0 };
}

fn refuse_retire(status: retire_status_t) retire_result_t {
    return .{ .status = status, .retired_count = 0, .discarded_count = 0 };
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
/// @param window Identity to find; both fields must match. Returns null for any other pair.
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

/// Number of deferred children in the table.
///
/// @param map Table; read only.
/// @return Current deferred count, never above MaxDeferred.
pub fn deferred_count(map: *const host_window_map_t) u32 {
    return map.deferred_count;
}

/// Announce one window. Writes the realize list in output order when the status is Realize.
///
/// The announced pair is `out[0]`. Deferred children whose parent is realized by this call follow
/// in breadth-first order: children of `out[0]` in deferral order, then children of `out[1]`, and
/// so on. Refusals and Defer change no state and write no output.
///
/// @param map Table; must not be null and must not alias `out`.
/// @param window Identity to realize; both fields nonzero.
/// @param parent Exact parent identity, or none (zero in both fields).
/// @param role Raw role value 1..6. Menu, Tooltip and ImePreedit require a valid parent.
///             Toplevel and UacConsent require none.
/// @param out Caller output list, length at least MaxOutput. Written only on Realize.
/// @return Status and the number of output entries. Never fails with an error union.
/// @thread_safety Owner thread only; no internal synchronization.
pub fn announce(map: *host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8, out: []window_ref_t) announce_result_t {
    if (out.len < MaxOutput or !valid_ref(window) or !valid_optional_ref(parent) or !valid_role(role)) return refuse(.RefuseInvalid);
    if (parent.window == window.window) return refuse(.RefuseInvalid);
    if (forbids_parent(role) and parent.window != 0) return refuse(.RefuseInvalid);
    if (is_popup_role(role) and parent.window == 0) return refuse(.RefuseInvalid);

    if (find_realized(map, window.window)) |index| {
        return refuse(if (map.realized[index].window.generation == window.generation) .RefuseDuplicate else .RefuseWindowConflict);
    }
    if (find_deferred(map, window.window)) |index| {
        return refuse(if (map.deferred[index].window.generation == window.generation) .RefuseDuplicate else .RefuseWindowConflict);
    }
    if (parent.window != 0) {
        if (find_realized(map, parent.window)) |index| {
            if (map.realized[index].window.generation != parent.generation) return refuse(.RefuseStaleParent);
        } else {
            return defer_child(map, window, parent, role);
        }
    }
    return realize_closure(map, window, parent, role, out);
}

fn defer_child(map: *host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8) announce_result_t {
    if (deferred_len(map) == MaxDeferred) return refuse(.RefuseExhausted);
    map.deferred[deferred_len(map)] = .{ .window = window, .parent = parent, .role = role };
    map.deferred_count += 1;
    return .{ .status = .Defer, .count = 0 };
}

/// Plan the breadth-first realize closure, then commit it. Nothing is mutated until the whole
/// closure is known to fit, so an exhausted refusal leaves the table unchanged.
fn realize_closure(map: *host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8, out: []window_ref_t) announce_result_t {
    if (realized_len(map) >= MaxRealized) return refuse(.RefuseExhausted);
    var marked = [_]bool{false} ** MaxDeferred;
    var take: [MaxDeferred]usize = undefined;
    out[0] = window;
    var planned: usize = 1;
    var head: usize = 0;
    while (head < planned) : (head += 1) {
        const pair = out[head];
        for (map.deferred[0..deferred_len(map)], 0..) |entry, index| {
            if (marked[index] or !same_ref(entry.parent, pair)) continue;
            if (realized_len(map) + planned >= MaxRealized) return refuse(.RefuseExhausted);
            marked[index] = true;
            take[planned - 1] = index;
            out[planned] = entry.window;
            planned += 1;
        }
    }

    append_realized(map, window, parent, role);
    for (take[0 .. planned - 1]) |index| {
        const entry = map.deferred[index];
        append_realized(map, entry.window, entry.parent, entry.role);
    }
    compact_deferred(map, &marked);
    return .{ .status = .Realize, .count = @intCast(planned) };
}

/// Append a realized entry. Its parent, when any, is already realized by construction.
fn append_realized(map: *host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8) void {
    const root = if (is_popup_role(role)) popup_root(map, parent) else window_ref_t{ .window = 0, .generation = 0 };
    map.realized[realized_len(map)] = .{ .window = window, .parent = parent, .root = root, .role = role };
    map.realized_count += 1;
}

/// Positioning root of a popup whose requested parent is already realized: a popup parent
/// contributes its own root, any other parent is its own root.
fn popup_root(map: *const host_window_map_t, parent: window_ref_t) window_ref_t {
    const index = find_realized(map, parent.window) orelse unreachable;
    const entry = map.realized[index];
    return if (is_popup_role(entry.role)) entry.root else entry.window;
}

/// Remove every deferred entry whose `remove` flag is set, keeping the relative order.
fn compact_deferred(map: *host_window_map_t, remove: []const bool) void {
    var write: usize = 0;
    for (0..deferred_len(map)) |read| {
        if (remove[read]) continue;
        if (write != read) map.deferred[write] = map.deferred[read];
        write += 1;
    }
    map.deferred_count = @intCast(write);
}

/// Remove a realized window and its realized subtree, and discard deferred children that can no
/// longer realize.
///
/// Retired output is the removed set in reverse realization order: every descendant precedes its
/// ancestor and the target is last. Discarded output lists discarded deferred children in deferral
/// order. A discard cascades through deferred children of a discarded deferred pair.
///
/// Discard candidates cannot exist in a table built only by `announce` and `retire`: a deferred
/// entry is created only when its parent is not realized, and a parent pair is unblocked the moment
/// it realizes. The discard pass therefore runs only against states that the public operations
/// cannot reach, and it keeps the reporting contract exact if such a state is ever constructed.
///
/// @param map Table; must not be null and must not alias either output list.
/// @param window Identity to retire; both fields nonzero and matching the realized generation.
/// @param retired_out Output list, length at least MaxOutput.
/// @param discarded_out Output list, length at least MaxOutput.
/// @return Status and both output counts. Stale and refusal results change nothing.
/// @thread_safety Owner thread only; no internal synchronization.
pub fn retire(map: *host_window_map_t, window: window_ref_t, retired_out: []window_ref_t, discarded_out: []window_ref_t) retire_result_t {
    if (retired_out.len < MaxOutput or discarded_out.len < MaxOutput or !valid_ref(window)) return refuse_retire(.RefuseInvalid);
    const target = find_realized(map, window.window) orelse return refuse_retire(.Stale);
    if (map.realized[target].window.generation != window.generation) return refuse_retire(.Stale);

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

    var retired_count: usize = 0;
    var position = len;
    while (position > 0) {
        position -= 1;
        if (!remove[position]) continue;
        retired_out[retired_count] = map.realized[position].window;
        retired_count += 1;
    }

    const deferred_total = deferred_len(map);
    var discard = [_]bool{false} ** MaxDeferred;
    var changed = true;
    while (changed) {
        changed = false;
        for (map.deferred[0..deferred_total], 0..) |entry, index| {
            if (discard[index]) continue;
            if (!(parent_removed(map, &remove, entry.parent) or parent_discarded(map, &discard, entry.parent))) continue;
            discard[index] = true;
            changed = true;
        }
    }
    var discarded_count: usize = 0;
    for (map.deferred[0..deferred_total], 0..) |entry, index| {
        if (!discard[index]) continue;
        discarded_out[discarded_count] = entry.window;
        discarded_count += 1;
    }

    compact_realized(map, &remove);
    compact_deferred(map, &discard);
    return .{ .status = .Retired, .retired_count = @intCast(retired_count), .discarded_count = @intCast(discarded_count) };
}

/// Whether `parent` names a realized entry scheduled for removal.
fn parent_removed(map: *const host_window_map_t, remove: []const bool, parent: window_ref_t) bool {
    for (map.realized[0..realized_len(map)], 0..) |entry, index| {
        if (remove[index] and same_ref(entry.window, parent)) return true;
    }
    return false;
}

/// Whether `parent` names a deferred entry already scheduled for discard.
fn parent_discarded(map: *const host_window_map_t, discard: []const bool, parent: window_ref_t) bool {
    for (map.deferred[0..deferred_len(map)], 0..) |entry, index| {
        if (discard[index] and same_ref(entry.window, parent)) return true;
    }
    return false;
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
const Region = struct {
    start: usize,
    len: usize,
};

fn region_of_value(value: anytype) Region {
    return .{ .start = @intFromPtr(value), .len = @sizeOf(std.meta.Child(@TypeOf(value))) };
}

fn region_of_slice(slice: anytype) Region {
    return .{ .start = @intFromPtr(slice.ptr), .len = slice.len * @sizeOf(std.meta.Child(@TypeOf(slice))) };
}

/// Whether no two regions overlap. Saturating ends never wrap, so a region near the top of the
/// address space is still compared correctly.
fn regions_disjoint(regions: []const Region) bool {
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

/// C ABI: announce one window. See `announce` for the status rules and output order.
///
/// @param map Table; NULL gives RefuseInvalid. In, out.
/// @param window Identity by value; both fields nonzero.
/// @param parent Parent identity by value; both fields zero for none.
/// @param role Raw `interop_window_role_t` value 1..6.
/// @param out Output array of at least `out_capacity` entries; NULL gives RefuseInvalid. Written
///            only on Realize. Must not overlap `map` or `out_count`. Out.
/// @param out_capacity Entries available in `out`; below 64 gives RefuseInvalid. In.
/// @param out_count Receives the output count. NULL gives RefuseInvalid. Not written for an
///                  argument refusal (NULL or overlap). Must not overlap `map` or `out`. Out.
/// @return Announce status. Never writes memory on RefuseInvalid from argument validation.
/// @thread_safety Owner thread only; no internal synchronization.
export fn host_window_map_announce(map: ?*host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8, out: ?[*]window_ref_t, out_capacity: u32, out_count: ?*u32) callconv(.C) announce_status_t {
    const count_ptr = out_count orelse return .RefuseInvalid;
    const target = map orelse return .RefuseInvalid;
    const out_base = out orelse return .RefuseInvalid;
    if (out_capacity < MaxOutput) return .RefuseInvalid;
    const out_slice = out_base[0..out_capacity];
    if (!regions_disjoint(&.{ region_of_value(target), region_of_slice(out_slice), region_of_value(count_ptr) })) return .RefuseInvalid;
    const result = announce(target, window, parent, role, out_slice);
    count_ptr.* = result.count;
    return result.status;
}

/// C ABI: remove a realized subtree. See `retire` for the output order and discard rules.
///
/// @param map Table; NULL gives RefuseInvalid. In, out.
/// @param window Identity by value; both fields nonzero.
/// @param retired_out Output array of at least `retired_capacity` entries; NULL gives RefuseInvalid.
///                    Must not overlap any other argument region. Out.
/// @param retired_capacity Entries available in `retired_out`; below 64 gives RefuseInvalid. In.
/// @param retired_count Receives the retired count; NULL gives RefuseInvalid. Out.
/// @param discarded_out Output array of at least `discarded_capacity` entries; NULL gives RefuseInvalid.
///                      Must not overlap any other argument region. Out.
/// @param discarded_capacity Entries available in `discarded_out`; below 64 gives RefuseInvalid. In.
/// @param discarded_count Receives the discarded count; NULL gives RefuseInvalid. Out.
/// @return Retire status. Argument refusals write no memory.
/// @thread_safety Owner thread only; no internal synchronization.
export fn host_window_map_retire(map: ?*host_window_map_t, window: window_ref_t, retired_out: ?[*]window_ref_t, retired_capacity: u32, retired_count: ?*u32, discarded_out: ?[*]window_ref_t, discarded_capacity: u32, discarded_count: ?*u32) callconv(.C) retire_status_t {
    const retired_count_ptr = retired_count orelse return .RefuseInvalid;
    const discarded_count_ptr = discarded_count orelse return .RefuseInvalid;
    const target = map orelse return .RefuseInvalid;
    const retired_base = retired_out orelse return .RefuseInvalid;
    const discarded_base = discarded_out orelse return .RefuseInvalid;
    if (retired_capacity < MaxOutput or discarded_capacity < MaxOutput) return .RefuseInvalid;
    const retired_slice = retired_base[0..retired_capacity];
    const discarded_slice = discarded_base[0..discarded_capacity];
    if (!regions_disjoint(&.{
        region_of_value(target),
        region_of_slice(retired_slice),
        region_of_slice(discarded_slice),
        region_of_value(retired_count_ptr),
        region_of_value(discarded_count_ptr),
    })) return .RefuseInvalid;
    const result = retire(target, window, retired_slice, discarded_slice);
    retired_count_ptr.* = result.retired_count;
    discarded_count_ptr.* = result.discarded_count;
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

/// C ABI: number of deferred children; NULL gives 0.
///
/// @param map Table, read only. In.
/// @return Deferred count, at most 64.
/// @thread_safety Owner thread only.
export fn host_window_map_deferred_count(map: ?*const host_window_map_t) callconv(.C) u32 {
    return if (map) |target| deferred_count(target) else 0;
}

comptime {
    // Layout frozen by src/interop/host_window_map.h (and checked there by _Static_assert).
    if (@sizeOf(window_ref_t) != 16 or @sizeOf(window_entry_t) != 56 or @sizeOf(deferred_entry_t) != 40) @compileError("host window map entry layout changed");
    if (@sizeOf(host_window_map_t) != 6152) @compileError("host window map table layout changed");
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

test "reset produces an empty table with zero counts and zeroed entries" {
    var map = new_map();
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
    try testing.expectEqual(@as(u32, 0), deferred_count(&map));
    try expect_absent(&map, ref(1, 1));
}

test "announce toplevel realizes and records a single output entry" {
    var map = new_map();
    var out = new_out();
    const result = announce(&map, ref(10, 1), none(), RoleToplevel, &out);
    try testing.expectEqual(announce_status_t.Realize, result.status);
    try testing.expectEqual(@as(u32, 1), result.count);
    try expect_ref(ref(10, 1), out[0]);
    try testing.expect(is_current(&map, ref(10, 1)));
    const entry = lookup(&map, ref(10, 1)).?;
    try testing.expectEqual(RoleToplevel, entry.role);
    try expect_ref(none(), entry.parent);
    try expect_ref(none(), entry.root);
}

test "every announce refusal reports its status and changes nothing" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel, &out);
    const before = map;

    // Invalid identity and parent shapes.
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(0, 1), none(), RoleToplevel, &out).status);
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 0), none(), RoleToplevel, &out).status);
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), ref(0, 1), RoleDialog, &out).status);
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), ref(1, 0), RoleDialog, &out).status);
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), ref(2, 1), RoleDialog, &out).status);
    // Invalid roles.
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), none(), 0, &out).status);
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), none(), 7, &out).status);
    // Role parent rules.
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), ref(1, 1), RoleToplevel, &out).status);
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), ref(1, 1), RoleUacConsent, &out).status);
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), none(), RoleMenu, &out).status);
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), none(), RoleTooltip, &out).status);
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), none(), RoleImePreedit, &out).status);
    // Output capacity below MaxOutput.
    var short: [MaxOutput - 1]window_ref_t = undefined;
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce(&map, ref(2, 1), none(), RoleToplevel, &short).status);

    // Duplicate and conflict.
    try testing.expectEqual(announce_status_t.RefuseDuplicate, announce(&map, ref(1, 1), none(), RoleToplevel, &out).status);
    try testing.expectEqual(announce_status_t.RefuseWindowConflict, announce(&map, ref(1, 2), none(), RoleToplevel, &out).status);
    try testing.expectEqual(@as(u32, 0), announce(&map, ref(1, 1), none(), RoleToplevel, &out).count);

    try testing.expectEqual(before.realized_count, map.realized_count);
    try testing.expectEqual(before.deferred_count, map.deferred_count);
    try expect_ref(before.realized[0].window, map.realized[0].window);
    try testing.expectEqual(before.realized[0].role, map.realized[0].role);
}

test "stale parent generation refuses and duplicate or conflicting deferral is refused" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel, &out);
    try testing.expectEqual(announce_status_t.RefuseStaleParent, announce(&map, ref(2, 1), ref(1, 2), RoleDialog, &out).status);
    try testing.expectEqual(@as(u32, 0), deferred_count(&map));

    // A child whose parent is not realized is deferred; a second copy of that pair is a duplicate.
    try testing.expectEqual(announce_status_t.Defer, announce(&map, ref(3, 1), ref(9, 1), RoleDialog, &out).status);
    try testing.expectEqual(announce_status_t.RefuseDuplicate, announce(&map, ref(3, 1), ref(9, 1), RoleDialog, &out).status);
    try testing.expectEqual(announce_status_t.RefuseWindowConflict, announce(&map, ref(3, 2), ref(9, 1), RoleDialog, &out).status);
    try testing.expectEqual(@as(u32, 1), deferred_count(&map));
}

test "deferred children realize breadth-first in deferral order when their parent arrives" {
    var map = new_map();
    var out = new_out();
    // Deferred in this order: A and B wait for P, C waits for A.
    try testing.expectEqual(announce_status_t.Defer, announce(&map, ref(11, 1), ref(20, 1), RoleDialog, &out).status);
    try testing.expectEqual(announce_status_t.Defer, announce(&map, ref(12, 1), ref(20, 1), RoleDialog, &out).status);
    try testing.expectEqual(announce_status_t.Defer, announce(&map, ref(13, 1), ref(11, 1), RoleDialog, &out).status);
    try testing.expectEqual(@as(u32, 3), deferred_count(&map));

    const result = announce(&map, ref(20, 1), none(), RoleToplevel, &out);
    try testing.expectEqual(announce_status_t.Realize, result.status);
    try testing.expectEqual(@as(u32, 4), result.count);
    try expect_ref(ref(20, 1), out[0]);
    try expect_ref(ref(11, 1), out[1]);
    try expect_ref(ref(12, 1), out[2]);
    try expect_ref(ref(13, 1), out[3]);
    try testing.expectEqual(@as(u32, 0), deferred_count(&map));
    try testing.expectEqual(@as(u32, 4), realized_count(&map));
    // Realization order keeps every parent before its children.
    try expect_ref(ref(20, 1), map.realized[0].window);
    try expect_ref(ref(13, 1), map.realized[3].window);
    try expect_ref(ref(11, 1), map.realized[3].parent);
}

test "unblocking realizes only the children of the new pair and keeps the rest blocked in order" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(30, 1), ref(40, 1), RoleDialog, &out);
    _ = announce(&map, ref(31, 1), ref(41, 1), RoleDialog, &out);
    _ = announce(&map, ref(32, 1), ref(40, 1), RoleDialog, &out);
    try testing.expectEqual(@as(u32, 3), deferred_count(&map));
    const result = announce(&map, ref(40, 1), none(), RoleToplevel, &out);
    try testing.expectEqual(@as(u32, 3), result.count);
    try expect_ref(ref(30, 1), out[1]);
    try expect_ref(ref(32, 1), out[2]);
    try testing.expectEqual(@as(u32, 1), deferred_count(&map));
    try expect_ref(ref(31, 1), map.deferred[0].window);
}

test "retire cascades children first in reverse realization order then the target" {
    var map = new_map();
    var out = new_out();
    var retired = new_out();
    var discarded = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel, &out);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleDialog, &out);
    _ = announce(&map, ref(3, 1), ref(2, 1), RoleDialog, &out);
    _ = announce(&map, ref(4, 1), ref(1, 1), RoleDialog, &out);
    _ = announce(&map, ref(5, 1), none(), RoleToplevel, &out);
    // An unrelated child of a surviving window must not be swept up by the cascade.
    _ = announce(&map, ref(6, 1), ref(5, 1), RoleDialog, &out);

    const result = retire(&map, ref(1, 1), &retired, &discarded);
    try testing.expectEqual(retire_status_t.Retired, result.status);
    try testing.expectEqual(@as(u32, 4), result.retired_count);
    try testing.expectEqual(@as(u32, 0), result.discarded_count);
    try expect_ref(ref(4, 1), retired[0]);
    try expect_ref(ref(3, 1), retired[1]);
    try expect_ref(ref(2, 1), retired[2]);
    try expect_ref(ref(1, 1), retired[3]);
    try testing.expectEqual(@as(u32, 2), realized_count(&map));
    try testing.expect(is_current(&map, ref(5, 1)));
    try testing.expect(is_current(&map, ref(6, 1)));
}

test "retire discards a deferred child of the removed pair and cascades through its own children" {
    // Reachable only from a state that announce cannot build (see retire's documentation): a
    // deferred entry whose parent is realized. The table is built directly to exercise the rule.
    var map = new_map();
    var out = new_out();
    var retired = new_out();
    var discarded = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel, &out);
    _ = announce(&map, ref(9, 1), none(), RoleToplevel, &out);
    map.deferred[0] = .{ .window = ref(5, 1), .parent = ref(1, 1), .role = RoleDialog };
    map.deferred[1] = .{ .window = ref(6, 1), .parent = ref(5, 1), .role = RoleDialog };
    map.deferred[2] = .{ .window = ref(7, 1), .parent = ref(9, 1), .role = RoleDialog };
    map.deferred_count = 3;

    const result = retire(&map, ref(1, 1), &retired, &discarded);
    try testing.expectEqual(retire_status_t.Retired, result.status);
    try testing.expectEqual(@as(u32, 1), result.retired_count);
    try testing.expectEqual(@as(u32, 2), result.discarded_count);
    try expect_ref(ref(5, 1), discarded[0]);
    try expect_ref(ref(6, 1), discarded[1]);
    try testing.expectEqual(@as(u32, 1), deferred_count(&map));
    try expect_ref(ref(7, 1), map.deferred[0].window);
}

test "retire of unknown, stale and deferred pairs changes nothing" {
    var map = new_map();
    var out = new_out();
    var retired = new_out();
    var discarded = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel, &out);
    _ = announce(&map, ref(2, 1), ref(9, 1), RoleDialog, &out);
    try testing.expectEqual(retire_status_t.Stale, retire(&map, ref(3, 1), &retired, &discarded).status);
    try testing.expectEqual(retire_status_t.Stale, retire(&map, ref(1, 2), &retired, &discarded).status);
    try testing.expectEqual(retire_status_t.Stale, retire(&map, ref(2, 1), &retired, &discarded).status);
    try testing.expectEqual(retire_status_t.RefuseInvalid, retire(&map, ref(0, 1), &retired, &discarded).status);
    var short: [MaxOutput - 1]window_ref_t = undefined;
    try testing.expectEqual(retire_status_t.RefuseInvalid, retire(&map, ref(1, 1), &short, &discarded).status);
    try testing.expectEqual(retire_status_t.RefuseInvalid, retire(&map, ref(1, 1), &retired, &short).status);
    try expect_absent(&map, ref(1, 2));
    try testing.expectEqual(@as(u32, 1), realized_count(&map));
    try testing.expectEqual(@as(u32, 1), deferred_count(&map));
}

test "popup of a toplevel records the toplevel as its root" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel, &out);
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(2, 1), ref(1, 1), RoleMenu, &out).status);
    const entry = lookup(&map, ref(2, 1)).?;
    try expect_ref(ref(1, 1), entry.parent);
    try expect_ref(ref(1, 1), entry.root);
}

test "popup of a popup is re-parented to the root toplevel while keeping the requested parent" {
    var map = new_map();
    var out = new_out();
    var retired = new_out();
    var discarded = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel, &out);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleMenu, &out);
    _ = announce(&map, ref(3, 1), ref(2, 1), RoleTooltip, &out);
    _ = announce(&map, ref(4, 1), ref(3, 1), RoleImePreedit, &out);
    const grandchild = lookup(&map, ref(4, 1)).?;
    try expect_ref(ref(3, 1), grandchild.parent);
    try expect_ref(ref(1, 1), grandchild.root);
    try expect_ref(ref(1, 1), lookup(&map, ref(3, 1)).?.root);

    // The cascade follows the requested parent chain, so the whole popup chain goes with its root.
    const result = retire(&map, ref(1, 1), &retired, &discarded);
    try testing.expectEqual(@as(u32, 4), result.retired_count);
    try expect_ref(ref(4, 1), retired[0]);
}

test "a popup whose parent is a deferred popup gets the root when the chain unblocks" {
    var map = new_map();
    var out = new_out();
    try testing.expectEqual(announce_status_t.Defer, announce(&map, ref(3, 1), ref(2, 1), RoleTooltip, &out).status);
    try testing.expectEqual(announce_status_t.Defer, announce(&map, ref(2, 1), ref(1, 1), RoleMenu, &out).status);
    const result = announce(&map, ref(1, 1), none(), RoleDialog, &out);
    try testing.expectEqual(@as(u32, 3), result.count);
    try testing.expectEqual(@as(u32, 0), deferred_count(&map));
    try expect_ref(ref(1, 1), lookup(&map, ref(3, 1)).?.root);
    try expect_ref(ref(1, 1), lookup(&map, ref(2, 1)).?.root);
}

test "a dialog keeps its parent and its own root stays none" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel, &out);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleDialog, &out);
    const dialog = lookup(&map, ref(2, 1)).?;
    try expect_ref(ref(1, 1), dialog.parent);
    try expect_ref(none(), dialog.root);
    try testing.expectEqual(RoleDialog, dialog.role);
}

test "dialog without a parent and uac consent are realized without a parent" {
    var map = new_map();
    var out = new_out();
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(1, 1), none(), RoleDialog, &out).status);
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(2, 1), none(), RoleUacConsent, &out).status);
    try testing.expectEqual(RoleUacConsent, lookup(&map, ref(2, 1)).?.role);
}

test "a popup whose parent is not realized defers instead of refusing" {
    var map = new_map();
    var out = new_out();
    try testing.expectEqual(announce_status_t.Defer, announce(&map, ref(2, 1), ref(1, 1), RoleMenu, &out).status);
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(1, 1), none(), RoleToplevel, &out).status);
    try testing.expect(is_current(&map, ref(2, 1)));
}

test "realize refuses when the closure would exceed 64 realized windows and changes nothing" {
    var map = new_map();
    var out = new_out();
    for (1..MaxRealized) |index| {
        _ = announce(&map, ref(index, 1), none(), RoleToplevel, &out);
    }
    try testing.expectEqual(@as(u32, 63), realized_count(&map));
    try testing.expectEqual(announce_status_t.Defer, announce(&map, ref(900, 1), ref(901, 1), RoleDialog, &out).status);
    try testing.expectEqual(announce_status_t.Defer, announce(&map, ref(902, 1), ref(901, 1), RoleDialog, &out).status);
    // Realizing 901 unblocks 900 and 902: three slots are needed and only one is free.
    try testing.expectEqual(announce_status_t.RefuseExhausted, announce(&map, ref(901, 1), none(), RoleToplevel, &out).status);
    try testing.expectEqual(@as(u32, 63), realized_count(&map));
    try testing.expectEqual(@as(u32, 2), deferred_count(&map));
}

test "exactly 64 realized windows succeed and the 65th is refused" {
    var map = new_map();
    var out = new_out();
    for (1..MaxRealized + 1) |index| {
        try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(index, 1), none(), RoleToplevel, &out).status);
    }
    try testing.expectEqual(@as(u32, 64), realized_count(&map));
    try testing.expectEqual(announce_status_t.RefuseExhausted, announce(&map, ref(65, 1), none(), RoleToplevel, &out).status);
    try testing.expectEqual(announce_status_t.RefuseExhausted, announce(&map, ref(65, 1), ref(1, 1), RoleDialog, &out).status);
    try testing.expectEqual(@as(u32, 64), realized_count(&map));
    try testing.expectEqual(@as(u32, 0), deferred_count(&map));
}

test "a realize closure that lands exactly on 64 succeeds and one more member is refused" {
    // 61 realized, two children of 200: realizing 200 fills the table exactly to 64.
    var map = new_map();
    var out = new_out();
    for (1..MaxRealized - 2) |index| {
        _ = announce(&map, ref(index, 1), none(), RoleToplevel, &out);
    }
    _ = announce(&map, ref(300, 1), ref(200, 1), RoleDialog, &out);
    _ = announce(&map, ref(301, 1), ref(200, 1), RoleDialog, &out);
    const fits = announce(&map, ref(200, 1), none(), RoleToplevel, &out);
    try testing.expectEqual(announce_status_t.Realize, fits.status);
    try testing.expectEqual(@as(u32, 3), fits.count);
    try testing.expectEqual(@as(u32, 64), realized_count(&map));

    // 62 realized, three children of 200: the closure needs 4 slots, only 2 are free.
    var second = new_map();
    for (1..MaxRealized - 1) |index| {
        _ = announce(&second, ref(index, 1), none(), RoleToplevel, &out);
    }
    _ = announce(&second, ref(300, 1), ref(200, 1), RoleDialog, &out);
    _ = announce(&second, ref(301, 1), ref(200, 1), RoleDialog, &out);
    _ = announce(&second, ref(302, 1), ref(200, 1), RoleDialog, &out);
    try testing.expectEqual(announce_status_t.RefuseExhausted, announce(&second, ref(200, 1), none(), RoleToplevel, &out).status);
    try testing.expectEqual(@as(u32, 62), realized_count(&second));
    try testing.expectEqual(@as(u32, 3), deferred_count(&second));
    try expect_absent(&second, ref(200, 1));
}

test "exactly 64 deferred children succeed and the 65th is refused" {
    var map = new_map();
    var out = new_out();
    for (1..MaxDeferred + 1) |index| {
        try testing.expectEqual(announce_status_t.Defer, announce(&map, ref(index, 1), ref(500, 1), RoleDialog, &out).status);
    }
    try testing.expectEqual(@as(u32, 64), deferred_count(&map));
    try testing.expectEqual(announce_status_t.RefuseExhausted, announce(&map, ref(65, 1), ref(500, 1), RoleDialog, &out).status);
    try testing.expectEqual(@as(u32, 64), deferred_count(&map));
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
}

test "epoch reset clears realized and deferred entries together" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel, &out);
    _ = announce(&map, ref(2, 1), ref(7, 7), RoleDialog, &out);
    reset(&map);
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
    try testing.expectEqual(@as(u32, 0), deferred_count(&map));
    try expect_absent(&map, ref(1, 1));
    try testing.expectEqual(announce_status_t.Realize, announce(&map, ref(1, 1), none(), RoleToplevel, &out).status);
}

// Reference model: a deliberately simple, separately written implementation of the same contract.
// It uses growing lists and recursion where the module uses fixed arrays and one-pass closures, so
// agreement is evidence rather than a copy of the same code.
const reference_model_t = struct {
    realized: std.ArrayList(window_entry_t),
    deferred: std.ArrayList(deferred_entry_t),

    fn model_init() reference_model_t {
        return .{ .realized = std.ArrayList(window_entry_t).init(testing.allocator), .deferred = std.ArrayList(deferred_entry_t).init(testing.allocator) };
    }

    fn model_deinit(self: *reference_model_t) void {
        self.realized.deinit();
        self.deferred.deinit();
    }

    fn model_clear(self: *reference_model_t) void {
        self.realized.clearRetainingCapacity();
        self.deferred.clearRetainingCapacity();
    }

    fn model_find_realized(self: *const reference_model_t, window: u64) ?usize {
        for (self.realized.items, 0..) |entry, index| {
            if (entry.window.window == window) return index;
        }
        return null;
    }

    fn model_find_deferred(self: *const reference_model_t, window: u64) ?usize {
        for (self.deferred.items, 0..) |entry, index| {
            if (entry.window.window == window) return index;
        }
        return null;
    }

    fn model_root(self: *const reference_model_t, parent: window_ref_t, role: u8) window_ref_t {
        if (!is_popup_role(role)) return none();
        const index = self.model_find_realized(parent.window).?;
        const entry = self.realized.items[index];
        return if (is_popup_role(entry.role)) entry.root else entry.window;
    }

    fn model_announce(self: *reference_model_t, window: window_ref_t, parent: window_ref_t, role: u8, out: *std.ArrayList(window_ref_t)) !announce_status_t {
        out.clearRetainingCapacity();
        if (window.window == 0 or window.generation == 0) return .RefuseInvalid;
        if ((parent.window == 0) != (parent.generation == 0)) return .RefuseInvalid;
        if (parent.window == window.window) return .RefuseInvalid;
        if (role < 1 or role > 6) return .RefuseInvalid;
        const popup = role == 3 or role == 4 or role == 6;
        if (popup and parent.window == 0) return .RefuseInvalid;
        if ((role == 1 or role == 5) and parent.window != 0) return .RefuseInvalid;

        if (self.model_find_realized(window.window)) |index| {
            return if (self.realized.items[index].window.generation == window.generation) .RefuseDuplicate else .RefuseWindowConflict;
        }
        if (self.model_find_deferred(window.window)) |index| {
            return if (self.deferred.items[index].window.generation == window.generation) .RefuseDuplicate else .RefuseWindowConflict;
        }
        if (parent.window != 0) {
            if (self.model_find_realized(parent.window)) |index| {
                if (self.realized.items[index].window.generation != parent.generation) return .RefuseStaleParent;
            } else {
                if (self.deferred.items.len == MaxDeferred) return .RefuseExhausted;
                try self.deferred.append(.{ .window = window, .parent = parent, .role = role });
                return .Defer;
            }
        }
        if (self.realized.items.len >= MaxRealized) return .RefuseExhausted;

        // Breadth-first closure, planned on separate lists so a refused closure leaves the model as it was.
        var pending = std.ArrayList(window_ref_t).init(testing.allocator);
        defer pending.deinit();
        var taken = std.ArrayList(usize).init(testing.allocator);
        defer taken.deinit();
        try pending.append(window);
        var head: usize = 0;
        while (head < pending.items.len) : (head += 1) {
            for (self.deferred.items, 0..) |entry, index| {
                if (std.mem.indexOfScalar(usize, taken.items, index) != null) continue;
                if (!same_ref(entry.parent, pending.items[head])) continue;
                if (self.realized.items.len + pending.items.len >= MaxRealized) return .RefuseExhausted;
                try taken.append(index);
                try pending.append(entry.window);
            }
        }

        try self.realized.append(.{ .window = window, .parent = parent, .root = self.model_root(parent, role), .role = role });
        for (taken.items) |index| {
            const entry = self.deferred.items[index];
            try self.realized.append(.{ .window = entry.window, .parent = entry.parent, .root = self.model_root(entry.parent, entry.role), .role = entry.role });
        }
        std.mem.sort(usize, taken.items, {}, std.sort.desc(usize));
        for (taken.items) |index| _ = self.deferred.orderedRemove(index);
        try out.resize(pending.items.len);
        @memcpy(out.items, pending.items);
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

    fn model_retire(self: *reference_model_t, window: window_ref_t, retired: *std.ArrayList(window_ref_t), discarded: *std.ArrayList(window_ref_t)) !retire_status_t {
        retired.clearRetainingCapacity();
        discarded.clearRetainingCapacity();
        if (window.window == 0 or window.generation == 0) return .RefuseInvalid;
        const target = self.model_find_realized(window.window) orelse return .Stale;
        if (self.realized.items[target].window.generation != window.generation) return .Stale;

        var removed = std.ArrayList(usize).init(testing.allocator);
        defer removed.deinit();
        try self.model_collect(target, &removed);
        std.mem.sort(usize, removed.items, {}, std.sort.desc(usize));
        for (removed.items) |index| try retired.append(self.realized.items[index].window);

        var removed_pairs = std.ArrayList(window_ref_t).init(testing.allocator);
        defer removed_pairs.deinit();
        for (removed.items) |index| try removed_pairs.append(self.realized.items[index].window);

        // Fixed point over deferred entries: discard when the parent was removed or discarded.
        var dead = std.ArrayList(usize).init(testing.allocator);
        defer dead.deinit();
        var dead_pairs = std.ArrayList(window_ref_t).init(testing.allocator);
        defer dead_pairs.deinit();
        var changed = true;
        while (changed) {
            changed = false;
            for (self.deferred.items, 0..) |entry, index| {
                if (std.mem.indexOfScalar(usize, dead.items, index) != null) continue;
                var hit = false;
                for (removed_pairs.items) |pair| {
                    if (same_ref(pair, entry.parent)) hit = true;
                }
                for (dead_pairs.items) |pair| {
                    if (same_ref(pair, entry.parent)) hit = true;
                }
                if (!hit) continue;
                try dead.append(index);
                try dead_pairs.append(entry.window);
                changed = true;
            }
        }
        std.mem.sort(usize, dead.items, {}, std.sort.asc(usize));
        for (dead.items) |index| try discarded.append(self.deferred.items[index].window);

        std.mem.sort(usize, removed.items, {}, std.sort.desc(usize));
        for (removed.items) |index| _ = self.realized.orderedRemove(index);
        std.mem.sort(usize, dead.items, {}, std.sort.desc(usize));
        for (dead.items) |index| _ = self.deferred.orderedRemove(index);
        return .Retired;
    }
};

fn expect_same_state(map: *const host_window_map_t, model: *const reference_model_t) !void {
    try testing.expectEqual(model.realized.items.len, realized_count(map));
    try testing.expectEqual(model.deferred.items.len, deferred_count(map));
    for (model.realized.items, 0..) |entry, index| {
        try expect_ref(entry.window, map.realized[index].window);
        try expect_ref(entry.parent, map.realized[index].parent);
        try expect_ref(entry.root, map.realized[index].root);
        try testing.expectEqual(entry.role, map.realized[index].role);
    }
    for (model.deferred.items, 0..) |entry, index| {
        try expect_ref(entry.window, map.deferred[index].window);
        try expect_ref(entry.parent, map.deferred[index].parent);
        try testing.expectEqual(entry.role, map.deferred[index].role);
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
    var retired = new_out();
    var retired_list = std.ArrayList(window_ref_t).init(testing.allocator);
    defer retired_list.deinit();
    var discarded = new_out();
    var discarded_list = std.ArrayList(window_ref_t).init(testing.allocator);
    defer discarded_list.deinit();

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
            const got = announce(&map, window, parent, role, &out);
            const expected = try model.model_announce(window, parent, role, &out_list);
            try testing.expectEqual(expected, got.status);
            try testing.expectEqual(@as(u32, @intCast(out_list.items.len)), got.count);
            for (out_list.items, 0..) |pair, index| try expect_ref(pair, out[index]);
        } else {
            const window = ref(random.uintLessThan(u64, windows + 1), random.uintLessThan(u64, 4));
            const got = retire(&map, window, &retired, &discarded);
            const expected = try model.model_retire(window, &retired_list, &discarded_list);
            try testing.expectEqual(expected, got.status);
            if (got.status == .Retired) {
                try testing.expectEqual(@as(u32, @intCast(retired_list.items.len)), got.retired_count);
                try testing.expectEqual(@as(u32, @intCast(discarded_list.items.len)), got.discarded_count);
                for (retired_list.items, 0..) |pair, index| try expect_ref(pair, retired[index]);
                for (discarded_list.items, 0..) |pair, index| try expect_ref(pair, discarded[index]);
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

/// The C ABI exports are called through `never_inline` shims so that the optimized wrapper bodies
/// keep their own branches. A directly inlined call with constant NULL arguments would fold them away.
const abi = struct {
    fn reset(map: ?*host_window_map_t) void {
        @call(.never_inline, host_window_map_reset, .{map});
    }

    fn announce(map: ?*host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8, out: ?[*]window_ref_t, out_capacity: u32, out_count: ?*u32) announce_status_t {
        return @call(.never_inline, host_window_map_announce, .{ map, window, parent, role, out, out_capacity, out_count });
    }

    fn retire(map: ?*host_window_map_t, window: window_ref_t, retired_out: ?[*]window_ref_t, retired_capacity: u32, retired_count: ?*u32, discarded_out: ?[*]window_ref_t, discarded_capacity: u32, discarded_count: ?*u32) retire_status_t {
        return @call(.never_inline, host_window_map_retire, .{ map, window, retired_out, retired_capacity, retired_count, discarded_out, discarded_capacity, discarded_count });
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

    fn deferred(map: ?*const host_window_map_t) u32 {
        return @call(.never_inline, host_window_map_deferred_count, .{map});
    }
};

test "C ABI counts and lookup accept NULL and report the table" {
    try testing.expectEqual(@as(u32, 0), abi.realized(null));
    try testing.expectEqual(@as(u32, 0), abi.deferred(null));
    try testing.expect(!abi.is_current(null, ref(1, 1)));
    try testing.expect(!abi.lookup(null, ref(1, 1), null));

    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel, &out);
    abi.reset(null);
    try testing.expectEqual(@as(u32, 1), abi.realized(&map));
    abi.reset(&map);
    try testing.expectEqual(@as(u32, 0), abi.realized(&map));
    try testing.expect(!abi.is_current(&map, ref(1, 1)));
}

test "C ABI announce and retire write outputs and counts on success" {
    var map = new_map();
    var out = new_out();
    var retired = new_out();
    var discarded = new_out();
    var count: u32 = 99;
    try testing.expectEqual(announce_status_t.Realize, abi.announce(&map, ref(1, 1), none(), RoleToplevel, &out, MaxOutput, &count));
    try testing.expectEqual(@as(u32, 1), count);
    try expect_ref(ref(1, 1), out[0]);
    try testing.expectEqual(announce_status_t.Defer, abi.announce(&map, ref(2, 1), ref(9, 1), RoleDialog, &out, MaxOutput, &count));
    try testing.expectEqual(@as(u32, 0), count);
    try testing.expectEqual(announce_status_t.Realize, abi.announce(&map, ref(9, 1), none(), RoleToplevel, &out, MaxOutput, &count));
    try testing.expectEqual(@as(u32, 2), count);
    try expect_ref(ref(2, 1), out[1]);

    var retired_count: u32 = 99;
    var discarded_count: u32 = 99;
    try testing.expectEqual(retire_status_t.Retired, abi.retire(&map, ref(9, 1), &retired, MaxOutput, &retired_count, &discarded, MaxOutput, &discarded_count));
    try testing.expectEqual(@as(u32, 2), retired_count);
    try testing.expectEqual(@as(u32, 0), discarded_count);
    try expect_ref(ref(2, 1), retired[0]);
    try expect_ref(ref(9, 1), retired[1]);
    try testing.expectEqual(@as(u32, 1), abi.realized(&map));
}

test "C ABI rejects NULL, short and aliased arguments without writing" {
    var map = new_map();
    var out = new_out();
    var count: u32 = 77;
    try testing.expectEqual(announce_status_t.RefuseInvalid, abi.announce(null, ref(1, 1), none(), RoleToplevel, &out, MaxOutput, &count));
    try testing.expectEqual(announce_status_t.RefuseInvalid, abi.announce(&map, ref(1, 1), none(), RoleToplevel, null, MaxOutput, &count));
    try testing.expectEqual(announce_status_t.RefuseInvalid, abi.announce(&map, ref(1, 1), none(), RoleToplevel, &out, MaxOutput, null));
    try testing.expectEqual(announce_status_t.RefuseInvalid, abi.announce(&map, ref(1, 1), none(), RoleToplevel, &out, MaxOutput - 1, &count));
    try testing.expectEqual(@as(u32, 77), count);

    // An output array that aliases the table, or a count that points into it, is refused untouched.
    const alias: [*]window_ref_t = @ptrCast(&map);
    try testing.expectEqual(announce_status_t.RefuseInvalid, abi.announce(&map, ref(1, 1), none(), RoleToplevel, alias, MaxOutput, &count));
    const inside: *u32 = &map.realized_count;
    try testing.expectEqual(announce_status_t.RefuseInvalid, abi.announce(&map, ref(1, 1), none(), RoleToplevel, &out, MaxOutput, inside));
    try testing.expectEqual(@as(u32, 0), map.realized_count);
    try testing.expectEqual(@as(u32, 0), map.deferred_count);

    var retired = new_out();
    var discarded = new_out();
    var retired_count: u32 = 5;
    var discarded_count: u32 = 5;
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi.retire(null, ref(1, 1), &retired, MaxOutput, &retired_count, &discarded, MaxOutput, &discarded_count));
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi.retire(&map, ref(1, 1), null, MaxOutput, &retired_count, &discarded, MaxOutput, &discarded_count));
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi.retire(&map, ref(1, 1), &retired, MaxOutput, null, &discarded, MaxOutput, &discarded_count));
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi.retire(&map, ref(1, 1), &retired, MaxOutput, &retired_count, null, MaxOutput, &discarded_count));
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi.retire(&map, ref(1, 1), &retired, MaxOutput, &retired_count, &discarded, MaxOutput, null));
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi.retire(&map, ref(1, 1), &retired, MaxOutput - 1, &retired_count, &discarded, MaxOutput, &discarded_count));
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi.retire(&map, ref(1, 1), &retired, MaxOutput, &retired_count, &discarded, MaxOutput - 1, &discarded_count));
    try testing.expectEqual(@as(u32, 5), retired_count);
    try testing.expectEqual(@as(u32, 5), discarded_count);
    // Retired and discarded output lists may not share storage with each other either.
    const shared: [*]window_ref_t = &retired;
    try testing.expectEqual(retire_status_t.RefuseInvalid, abi.retire(&map, ref(1, 1), &retired, MaxOutput, &retired_count, shared, MaxOutput, &discarded_count));
    try testing.expectEqual(retire_status_t.Stale, abi.retire(&map, ref(1, 1), &retired, MaxOutput, &retired_count, &discarded, MaxOutput, &discarded_count));
    try testing.expectEqual(@as(u32, 0), retired_count);
}

test "C ABI lookup returns a copy and a presence test needs no output" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel, &out);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleMenu, &out);
    var entry: window_entry_t = undefined;
    try testing.expect(abi.lookup(&map, ref(2, 1), &entry));
    try expect_ref(ref(1, 1), entry.root);
    try testing.expect(abi.lookup(&map, ref(1, 1), null));
    try testing.expect(!abi.lookup(&map, ref(1, 2), &entry));
    try testing.expect(abi.is_current(&map, ref(2, 1)));
    try testing.expect(!abi.is_current(&map, ref(3, 1)));
    try testing.expectEqual(@as(u32, 2), abi.realized(&map));
    try testing.expectEqual(@as(u32, 0), abi.deferred(&map));
}
