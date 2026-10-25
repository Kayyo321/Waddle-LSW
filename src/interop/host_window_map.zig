//! Pure bounded mapping of guest `(window, generation)` identities to host surfaces (TODO #11a).
//!
//! The table holds at most 64 realized windows per session epoch in fixed slots. A slot keeps its
//! index for as long as its window stays realized: retiring another window never moves it, and a freed
//! slot is reused by the next realize, lowest index first. Realization order is kept in a per-slot
//! sequence number. The module performs no native or Wayland call, allocates nothing, and never wraps
//! a counter.
//!
//! A child whose exact parent pair is not realized is refused as a stale parent, never deferred. The
//! guest defers such an announcement until the parent is exported (IMPL_DESC section 3.2.8).
//! The contract is `impl/seamless-interop/claude_45_percent/host_window_map_contract.md`.
const std = @import("std");

/// Maximum realized windows (exported surfaces) per session epoch, and the number of slots.
pub const MaxRealized: usize = 64;
/// Minimum caller output capacity accepted by retire.
pub const MaxOutput: usize = 64;
/// Slot index reported by a refused announce.
pub const NoSlot: u32 = MaxRealized;

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

/// Protocol `interop_error_t` values used as refusal codes. They equal `include/waddle/interop_protocol.h`,
/// and the Zig tests and the C header check that equality.
pub const ErrorNone: u32 = 0;
/// `InteropErrorProtocolViolation`: a duplicate announce.
pub const ErrorProtocolViolation: u32 = 1;
/// `InteropErrorMalformedFrame`: a zero identity, invalid role, invalid parent shape or capacity.
pub const ErrorMalformedFrame: u32 = 2;
/// `InteropErrorStaleWindow`: a parent or window generation that is not realized.
pub const ErrorStaleWindow: u32 = 5;
/// `InteropErrorQuotaExceeded`: the 64-entry table is full.
pub const ErrorQuotaExceeded: u32 = 6;

/// Exact identity of one window incarnation. Both fields zero is "none" when used as a parent.
pub const window_ref_t = extern struct {
    /// Protocol window handle (HWND value). Zero means no window.
    window: u64,
    /// Protocol incarnation generation, never reused within an epoch. Zero means no generation.
    generation: u64,
};

/// One fixed slot. `occupied` is zero for a free slot, and then the other fields are meaningless.
pub const slot_t = extern struct {
    /// Realized identity.
    window: window_ref_t,
    /// Requested direct parent, or none. Retire cascades follow this field.
    parent: window_ref_t,
    /// Realization sequence number, unique per epoch and increasing with each realize.
    realize_seq: u64,
    /// Opaque value owned by the adapter for this window. Zero when realized; set with `set_user_data`.
    user_data: u64,
    /// Raw `interop_window_role_t` value, 1..6.
    role: u8,
    /// Nonzero while the slot holds a realized window.
    occupied: u8,
    /// Nonzero once the compositor reported the popup dismissed (`popup_done`). A dismissed popup stays
    /// realized until the guest retires it.
    dismissed: u8,
};

/// Copy of an occupied slot returned by `lookup`.
pub const window_entry_t = extern struct {
    /// Realized identity.
    window: window_ref_t,
    /// Requested direct parent, or none.
    parent: window_ref_t,
    /// Realization sequence number.
    realize_seq: u64,
    /// Adapter value stored for this window.
    user_data: u64,
    /// Stable slot index of this window.
    slot: u32,
    /// Raw `interop_window_role_t` value, 1..6.
    role: u8,
    /// Nonzero once the popup was marked dismissed.
    dismissed: u8,
};

/// Complete table state. The caller owns the storage; the table holds no pointers.
pub const host_window_map_t = extern struct {
    /// Number of occupied slots. Must equal the occupied count and be at most 64; otherwise the table is
    /// corrupt and every call refuses with RefuseInvalid.
    realized_count: u32,
    /// Sequence number the next realize will take. Stops at the maximum value without wrapping.
    next_seq: u64,
    /// Fixed slots. A slot keeps its index while its window stays realized.
    slots: [MaxRealized]slot_t,
};

/// Result code of `announce`.
pub const announce_status_t = enum(c_int) {
    /// The window is realized in `slot`.
    Realize = 1,
    /// The exact pair is already realized. No change.
    RefuseDuplicate = 2,
    /// The parent pair is not realized: it was never announced, was retired, or its window is realized
    /// under another generation. No change.
    RefuseStaleParent = 3,
    /// The window is realized under another generation; that generation must be retired first.
    RefuseWindowConflict = 4,
    /// The table holds 64 realized windows or the sequence counter is exhausted. No change.
    RefuseExhausted = 5,
    /// Zero identity, invalid role, invalid parent shape, or a corrupt table. No change.
    RefuseInvalid = 6,
};

/// Result of `announce`. `slot` is the stable slot index on Realize and NoSlot otherwise.
pub const announce_result_t = extern struct {
    /// Outcome of the announce.
    status: announce_status_t,
    /// Slot of the realized window, or NoSlot.
    slot: u32,
};

/// Result code of `retire`.
pub const retire_status_t = enum(c_int) {
    /// The realized subtree was removed; the output list and count describe it.
    Retired = 1,
    /// No realized entry with that exact pair exists (never realized, already retired, or another
    /// generation). No change.
    Stale = 2,
    /// Zero identity, output capacity below MaxOutput, or a corrupt table. No change.
    RefuseInvalid = 3,
};

/// Result code of `reset`.
pub const reset_status_t = enum(c_int) {
    /// The table was torn down: the teardown list was written and the table cleared.
    Reset = 1,
    /// Output capacity below MaxOutput, or a corrupt table. Nothing was written and the table is unchanged.
    RefuseInvalid = 2,
};

/// Result of `reset`. `count` is the number of entries written to the teardown list.
pub const reset_result_t = extern struct {
    /// Outcome of the reset.
    status: reset_status_t,
    /// Realized windows written to the teardown list; zero unless Reset.
    count: u32,
};

/// Result code of `mark_dismissed`.
pub const dismiss_status_t = enum(c_int) {
    /// The popup is marked dismissed. Marking it again is also Marked and changes nothing further.
    Marked = 1,
    /// No realized entry with that exact pair. Nothing changed.
    Stale = 2,
    /// Zero identity, a corrupt table, or a window that is not a popup. Nothing changed.
    RefuseInvalid = 3,
};

/// Result code of `set_user_data` and `get_user_data`.
pub const user_data_status_t = enum(c_int) {
    /// The value was set or read.
    Ok = 1,
    /// The slot does not hold exactly this window. Nothing changed.
    Stale = 2,
    /// Zero identity, slot index outside the table, NULL output, overlap, or a corrupt table. Nothing changed.
    RefuseInvalid = 3,
};

/// Result of `retire`. `count` is the number of entries written to the output list.
pub const retire_result_t = extern struct {
    /// Outcome of the retire.
    status: retire_status_t,
    /// Removed windows written to the output list, target included; zero unless Retired.
    count: u32,
};

/// Protocol error code for an announce status, as sent in `Error{refused_type = WindowAnnounce}`.
///
/// Realize is `ErrorNone`. A full table is `ErrorQuotaExceeded`. A parent that is not realized, or a
/// window known under another generation, is `ErrorStaleWindow`. A duplicate is `ErrorProtocolViolation`.
/// An invalid identity, role or parent shape, or a corrupt table, is `ErrorMalformedFrame`.
///
/// @param status Status returned by `announce`.
/// @return The `interop_error_t` value.
pub fn announce_error_code(status: announce_status_t) u32 {
    return switch (status) {
        .Realize => ErrorNone,
        .RefuseDuplicate => ErrorProtocolViolation,
        .RefuseStaleParent, .RefuseWindowConflict => ErrorStaleWindow,
        .RefuseExhausted => ErrorQuotaExceeded,
        .RefuseInvalid => ErrorMalformedFrame,
    };
}

/// Protocol error code for a retire status. Retired is `ErrorNone`, a stale pair is `ErrorStaleWindow`,
/// and an invalid identity, output capacity or corrupt table is `ErrorMalformedFrame`.
///
/// @param status Status returned by `retire`.
/// @return The `interop_error_t` value.
pub fn retire_error_code(status: retire_status_t) u32 {
    return switch (status) {
        .Retired => ErrorNone,
        .Stale => ErrorStaleWindow,
        .RefuseInvalid => ErrorMalformedFrame,
    };
}

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

/// Index of the occupied slot holding `window`'s identifier, whatever its generation.
fn find_window(map: *const host_window_map_t, window: u64) ?usize {
    for (map.slots, 0..) |slot, index| {
        if (slot.occupied != 0 and slot.window.window == window) return index;
    }
    return null;
}

/// Index of the occupied slot holding exactly `id`.
fn find_exact(map: *const host_window_map_t, id: window_ref_t) ?usize {
    for (map.slots, 0..) |slot, index| {
        if (slot.occupied != 0 and same_ref(slot.window, id)) return index;
    }
    return null;
}

/// Lowest free slot index, or null when every slot is occupied.
fn find_free(map: *const host_window_map_t) ?usize {
    for (map.slots, 0..) |slot, index| {
        if (slot.occupied == 0) return index;
    }
    return null;
}

/// Number of occupied slots, counted from the slots themselves.
fn occupied_count(map: *const host_window_map_t) usize {
    var count: usize = 0;
    for (map.slots) |slot| {
        if (slot.occupied != 0) count += 1;
    }
    return count;
}

/// Whether the stored count agrees with the slots and fits the table. Every operation checks this
/// first, so a corrupt count in caller storage is refused instead of used.
fn is_consistent(map: *const host_window_map_t) bool {
    return map.realized_count <= MaxRealized and occupied_count(map) == map.realized_count;
}

/// Tear down the whole table: write every realized window to `out` in teardown order, then clear it.
///
/// Teardown order: roots (windows with no parent) in descending realization order, and within each
/// root the subtree children first, exactly as `retire` orders a subtree. Every descendant therefore
/// precedes its ancestor. The table is cleared only when the whole teardown list was written. A corrupt
/// table is refused and left as it is, so the caller can tell that its storage is not a valid table; such
/// storage is cleared by zero-filling it.
///
/// @param map Table; must not be null.
/// @param out Teardown list, length at least MaxOutput. Written only on Reset.
/// @return Status and the teardown count.
/// @thread_safety Owner thread only; no internal synchronization.
pub fn reset(map: *host_window_map_t, out: []window_ref_t) reset_result_t {
    const refused = reset_result_t{ .status = .RefuseInvalid, .count = 0 };
    if (out.len < MaxOutput or !is_consistent(map)) return refused;

    var order: [MaxRealized]u32 = undefined;
    var count: usize = 0;
    var bound: u64 = std.math.maxInt(u64);
    while (true) {
        var best: ?usize = null;
        for (map.slots, 0..) |slot, index| {
            if (slot.occupied == 0 or slot.parent.window != 0) continue;
            if (slot.realize_seq >= bound) continue;
            if (best == null or slot.realize_seq > map.slots[best.?].realize_seq) best = index;
        }
        const root = best orelse break;
        if (!emit_subtree(map, root, 0, &order, &count)) return refused;
        bound = map.slots[root].realize_seq;
    }
    // A parent that names no realized window, or a cycle, leaves slots unreachable from a root.
    if (count != map.realized_count) return refused;

    for (order[0..count], 0..) |index, position| {
        out[position] = map.slots[index].window;
    }
    map.* = std.mem.zeroes(host_window_map_t);
    return .{ .status = .Reset, .count = @intCast(count) };
}

/// Look up an occupied slot by exact identity.
///
/// @param map Table; read only.
/// @param window Identity to find; both fields must match.
/// @return A copy of the slot as a window entry, or null when the pair is not realized or the table is
///         corrupt.
pub fn lookup(map: *const host_window_map_t, window: window_ref_t) ?window_entry_t {
    if (!is_consistent(map)) return null;
    const index = find_exact(map, window) orelse return null;
    const slot = map.slots[index];
    return .{ .window = slot.window, .parent = slot.parent, .realize_seq = slot.realize_seq, .user_data = slot.user_data, .slot = @intCast(index), .role = slot.role, .dismissed = slot.dismissed };
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
/// @return The realized count when the table is consistent, otherwise 0.
pub fn realized_count(map: *const host_window_map_t) u32 {
    return if (is_consistent(map)) map.realized_count else 0;
}

/// Mark a realized popup as dismissed by the compositor (`popup_done`). The popup stays realized, with
/// its slot and user data, until the guest retires it.
///
/// @param map Table; must not be null.
/// @param window Exact identity of the popup; both fields nonzero.
/// @return `Marked`, `Stale` for an identity that is not realized, or `RefuseInvalid` for a zero identity, a
///         corrupt table, or a window that is not a popup.
pub fn mark_dismissed(map: *host_window_map_t, window: window_ref_t) dismiss_status_t {
    if (!valid_ref(window) or !is_consistent(map)) return .RefuseInvalid;
    const index = find_exact(map, window) orelse return .Stale;
    if (!is_popup_role(map.slots[index].role)) return .RefuseInvalid;
    map.slots[index].dismissed = 1;
    return .Marked;
}

/// Set the adapter value of the window in `slot`, if that slot holds exactly `window`.
///
/// @param map Table; must not be null.
/// @param slot Slot index from the announce or lookup that realized the window; must be below 64.
/// @param window Identity the caller believes the slot holds; both fields nonzero.
/// @param value Value to store.
/// @return `Ok` when stored, `Stale` when the slot is free or holds another pair, `RefuseInvalid` for bad
///         arguments or a corrupt table.
pub fn set_user_data(map: *host_window_map_t, slot: u32, window: window_ref_t, value: u64) user_data_status_t {
    const index = checked_slot(map, slot, window) orelse return .RefuseInvalid;
    if (!same_ref(map.slots[index].window, window) or map.slots[index].occupied == 0) return .Stale;
    map.slots[index].user_data = value;
    return .Ok;
}

/// Read the adapter value of the window in `slot`, if that slot holds exactly `window`.
///
/// @param map Table, read only.
/// @param slot Slot index; must be below 64.
/// @param window Identity the caller believes the slot holds; both fields nonzero.
/// @param value Out. Receives the stored value on Ok only.
/// @return `Ok`, `Stale` or `RefuseInvalid` as for `set_user_data`.
pub fn get_user_data(map: *const host_window_map_t, slot: u32, window: window_ref_t, value: *u64) user_data_status_t {
    const index = checked_slot(map, slot, window) orelse return .RefuseInvalid;
    if (!same_ref(map.slots[index].window, window) or map.slots[index].occupied == 0) return .Stale;
    value.* = map.slots[index].user_data;
    return .Ok;
}

/// Validate a slot index and identity for the user-data accessors. Returns the index, or null when the
/// arguments or the table are invalid.
fn checked_slot(map: *const host_window_map_t, slot: u32, window: window_ref_t) ?usize {
    if (!valid_ref(window) or slot >= MaxRealized or !is_consistent(map)) return null;
    return slot;
}

/// Realize one window if its parent, when named, is exactly realized.
///
/// The window takes the lowest free slot and the next sequence number. A parent pair that is not
/// realized is refused as a stale parent. The table never defers.
///
/// @param map Table; must not be null.
/// @param window Identity to realize; both fields nonzero.
/// @param parent Exact parent identity, or none (zero in both fields). Menu, Tooltip and ImePreedit
///               require a realized parent. Toplevel and UacConsent require none.
/// @param role Raw role value 1..6.
/// @return Status and the slot index on Realize (NoSlot otherwise). Changes state only for Realize.
/// @thread_safety Owner thread only; no internal synchronization.
pub fn announce(map: *host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8) announce_result_t {
    const refused = announce_result_t{ .status = .RefuseInvalid, .slot = NoSlot };
    if (!valid_ref(window) or !valid_optional_ref(parent) or !valid_role(role)) return refused;
    if (parent.window == window.window) return refused;
    if (forbids_parent(role) and parent.window != 0) return refused;
    if (is_popup_role(role) and parent.window == 0) return refused;
    if (!is_consistent(map)) return refused;

    if (find_window(map, window.window)) |index| {
        const status: announce_status_t = if (same_ref(map.slots[index].window, window)) .RefuseDuplicate else .RefuseWindowConflict;
        return .{ .status = status, .slot = NoSlot };
    }
    if (parent.window != 0) {
        const index = find_window(map, parent.window) orelse return .{ .status = .RefuseStaleParent, .slot = NoSlot };
        if (!same_ref(map.slots[index].window, parent)) return .{ .status = .RefuseStaleParent, .slot = NoSlot };
    }
    if (map.realized_count == MaxRealized or map.next_seq == std.math.maxInt(u64)) {
        return .{ .status = .RefuseExhausted, .slot = NoSlot };
    }

    // A consistent table with fewer than MaxRealized occupied slots always has a free one.
    const index = find_free(map) orelse unreachable;
    map.slots[index] = .{ .window = window, .parent = parent, .realize_seq = map.next_seq, .user_data = 0, .role = role, .occupied = 1, .dismissed = 0 };
    map.next_seq += 1;
    map.realized_count += 1;
    return .{ .status = .Realize, .slot = @intCast(index) };
}

/// Collect the realized subtree rooted at slot `index` into `order`, children first.
///
/// Children are visited in descending realization order, each child's own subtree before the child,
/// and the root last. The recursion depth is bounded by MaxRealized and every emitted slot is counted,
/// so a corrupt parent cycle returns false instead of looping.
///
/// @return False when the subtree is deeper than MaxRealized or would emit more than MaxRealized slots.
fn emit_subtree(map: *const host_window_map_t, index: usize, depth: usize, order: *[MaxRealized]u32, count: *usize) bool {
    if (depth > MaxRealized) return false;
    const self_window = map.slots[index].window;
    var bound: u64 = std.math.maxInt(u64);
    while (true) {
        var best: ?usize = null;
        for (map.slots, 0..) |slot, child| {
            if (slot.occupied == 0 or !same_ref(slot.parent, self_window)) continue;
            if (slot.realize_seq >= bound) continue;
            if (best == null or slot.realize_seq > map.slots[best.?].realize_seq) best = child;
        }
        const chosen = best orelse break;
        if (!emit_subtree(map, chosen, depth + 1, order, count)) return false;
        bound = map.slots[chosen].realize_seq;
    }
    if (count.* == MaxRealized) return false;
    order[count.*] = @intCast(index);
    count.* += 1;
    return true;
}

/// Remove a realized window and its realized subtree. Other slots keep their indices.
///
/// The output lists the removed pairs children first: every descendant precedes its ancestor, siblings
/// are in reverse realization order, and the target is last. The subtree follows requested parents to
/// any depth.
///
/// @param map Table; must not be null and must not alias `out`.
/// @param window Identity to retire; both fields nonzero and matching the realized generation.
/// @param out Output list, length at least MaxOutput. Written only when the status is Retired.
/// @return Status and the number of removed windows. Stale and refusal results change nothing.
/// @thread_safety Owner thread only; no internal synchronization.
pub fn retire(map: *host_window_map_t, window: window_ref_t, out: []window_ref_t) retire_result_t {
    if (out.len < MaxOutput or !valid_ref(window) or !is_consistent(map)) return .{ .status = .RefuseInvalid, .count = 0 };
    const target = find_exact(map, window) orelse return .{ .status = .Stale, .count = 0 };

    var order: [MaxRealized]u32 = undefined;
    var count: usize = 0;
    if (!emit_subtree(map, target, 0, &order, &count)) return .{ .status = .RefuseInvalid, .count = 0 };
    for (order[0..count], 0..) |index, position| {
        out[position] = map.slots[index].window;
    }
    for (order[0..count]) |index| {
        map.slots[index].occupied = 0;
    }
    map.realized_count -= @intCast(count);
    return .{ .status = .Retired, .count = @intCast(count) };
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

/// C ABI: tear down the table and write the teardown list. See `reset` for the order.
///
/// @param map In/out. Table; NULL gives RefuseInvalid. Cleared on Reset.
/// @param out Out. Teardown list of at least `out_capacity` entries; NULL gives RefuseInvalid. Written only on
///            Reset. Must not overlap `map` or `out_count`.
/// @param out_capacity In. Entries in `out`; below 64 gives RefuseInvalid.
/// @param out_count Out. Receives the teardown count; NULL gives RefuseInvalid. Not written when a NULL pointer,
///                  a short capacity, an overlap or a corrupt table refuses the call. Must not overlap `map` or `out`.
/// @return Reset status. Thread safety: owner thread only.
export fn host_window_map_reset(map: ?*host_window_map_t, out: ?[*]window_ref_t, out_capacity: u32, out_count: ?*u32) callconv(.C) reset_status_t {
    const count_ptr = out_count orelse return .RefuseInvalid;
    const target = map orelse return .RefuseInvalid;
    const out_base = out orelse return .RefuseInvalid;
    if (out_capacity < MaxOutput) return .RefuseInvalid;
    const out_slice = out_base[0..out_capacity];
    if (!regions_disjoint(&.{ region_of_value(target), region_of_slice(out_slice), region_of_value(count_ptr) })) return .RefuseInvalid;
    const result = reset(target, out_slice);
    count_ptr.* = result.count;
    return result.status;
}

/// C ABI: realize one window. See `announce`.
///
/// @param map Table; NULL gives RefuseInvalid. In, out.
/// @param window Identity by value; both fields nonzero. In.
/// @param parent Parent identity by value; both fields zero for none. In.
/// @param role Raw `interop_window_role_t` value 1..6. In.
/// @return Status and the slot index. Thread safety: owner thread only.
export fn host_window_map_announce(map: ?*host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8) callconv(.C) announce_result_t {
    const target = map orelse return .{ .status = .RefuseInvalid, .slot = NoSlot };
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

/// C ABI: exact-identity lookup of a realized window.
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
/// @return Realized count, at most 64; 0 for a corrupt table.
/// @thread_safety Owner thread only.
export fn host_window_map_realized_count(map: ?*const host_window_map_t) callconv(.C) u32 {
    return if (map) |target| realized_count(target) else 0;
}

/// C ABI: mark a realized popup dismissed. See `mark_dismissed`.
///
/// @param map In/out. Table; NULL gives RefuseInvalid.
/// @param window In. Identity by value; both fields nonzero.
/// @return Status. Thread safety: owner thread only.
export fn host_window_map_mark_dismissed(map: ?*host_window_map_t, window: window_ref_t) callconv(.C) dismiss_status_t {
    const target = map orelse return .RefuseInvalid;
    return mark_dismissed(target, window);
}

/// C ABI: set the adapter value of the window in `slot`. See `set_user_data`.
///
/// @param map In/out. Table; NULL gives RefuseInvalid.
/// @param slot In. Slot index from the announce that realized the window.
/// @param window In. Identity by value; both fields nonzero.
/// @param value In. Value to store.
/// @return Status. Thread safety: owner thread only.
export fn host_window_map_set_user_data(map: ?*host_window_map_t, slot: u32, window: window_ref_t, value: u64) callconv(.C) user_data_status_t {
    const target = map orelse return .RefuseInvalid;
    return set_user_data(target, slot, window, value);
}

/// C ABI: read the adapter value of the window in `slot`. See `get_user_data`.
///
/// @param map In. Table, read only; NULL gives RefuseInvalid.
/// @param slot In. Slot index from the announce that realized the window.
/// @param window In. Identity by value; both fields nonzero.
/// @param value Out. Receives the stored value on Ok only; NULL gives RefuseInvalid. Must not overlap `map`.
/// @return Status. Thread safety: owner thread only.
export fn host_window_map_get_user_data(map: ?*const host_window_map_t, slot: u32, window: window_ref_t, value: ?*u64) callconv(.C) user_data_status_t {
    const target = map orelse return .RefuseInvalid;
    const destination = value orelse return .RefuseInvalid;
    if (!regions_disjoint(&.{ region_of_value(target), region_of_value(destination) })) return .RefuseInvalid;
    return get_user_data(target, slot, window, destination);
}

/// C ABI: protocol error code for an announce status. See `announce_error_code`.
///
/// @param status In. Status returned by `host_window_map_announce`.
/// @return The `interop_error_t` value. Thread safety: pure; any thread.
export fn host_window_map_announce_error_code(status: announce_status_t) callconv(.C) u32 {
    return announce_error_code(status);
}

/// C ABI: protocol error code for a retire status. See `retire_error_code`.
///
/// @param status In. Status returned by `host_window_map_retire`.
/// @return The `interop_error_t` value. Thread safety: pure; any thread.
export fn host_window_map_retire_error_code(status: retire_status_t) callconv(.C) u32 {
    return retire_error_code(status);
}

comptime {
    // Layout frozen by src/interop/host_window_map.h (and checked there by _Static_assert).
    if (@sizeOf(window_ref_t) != 16 or @sizeOf(slot_t) != 56 or @sizeOf(window_entry_t) != 56) @compileError("host window map slot layout changed");
    if (@sizeOf(host_window_map_t) != 3600) @compileError("host window map table layout changed");
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
    return std.mem.zeroes(host_window_map_t);
}

fn new_out() [MaxOutput]window_ref_t {
    return std.mem.zeroes([MaxOutput]window_ref_t);
}

/// The public functions are called through `never_inline` shims so that the optimized bodies keep
/// their own branches. A call with constant arguments would otherwise fold refusal paths away.
const abi_shim_t = struct {
    fn reset(map: ?*host_window_map_t, out: ?[*]window_ref_t, out_capacity: u32, out_count: ?*u32) reset_status_t {
        return @call(.never_inline, host_window_map_reset, .{ map, out, out_capacity, out_count });
    }

    fn announce(map: ?*host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8) announce_result_t {
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

    fn announce_error_code(status: announce_status_t) u32 {
        return @call(.never_inline, host_window_map_announce_error_code, .{status});
    }

    fn retire_error_code(status: retire_status_t) u32 {
        return @call(.never_inline, host_window_map_retire_error_code, .{status});
    }

    fn set_user_data(map: ?*host_window_map_t, slot: u32, window: window_ref_t, value: u64) user_data_status_t {
        return @call(.never_inline, host_window_map_set_user_data, .{ map, slot, window, value });
    }

    fn mark_dismissed(map: ?*host_window_map_t, window: window_ref_t) dismiss_status_t {
        return @call(.never_inline, host_window_map_mark_dismissed, .{ map, window });
    }

    fn get_user_data(map: ?*const host_window_map_t, slot: u32, window: window_ref_t, value: ?*u64) user_data_status_t {
        return @call(.never_inline, host_window_map_get_user_data, .{ map, slot, window, value });
    }
};

fn announce_slot(map: *host_window_map_t, window: window_ref_t, parent: window_ref_t, role: u8) announce_status_t {
    return announce(map, window, parent, role).status;
}

test "reset of an empty table writes nothing and leaves zero-filled storage" {
    var map = new_map();
    var out = new_out();
    const result = reset(&map, &out);
    try testing.expectEqual(reset_status_t.Reset, result.status);
    try testing.expectEqual(@as(u32, 0), result.count);
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
    try expect_absent(&map, ref(1, 1));
}

test "reset writes the teardown list children first, roots in reverse realization order" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel); // A, seq 0
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleDialog); // B, child of A, seq 1
    _ = announce(&map, ref(3, 1), none(), RoleToplevel); // C, seq 2
    _ = announce(&map, ref(4, 1), ref(3, 1), RoleMenu); // D, child of C, seq 3
    _ = announce(&map, ref(5, 1), ref(2, 1), RoleTooltip); // E, child of B, seq 4

    const result = reset(&map, &out);
    try testing.expectEqual(reset_status_t.Reset, result.status);
    try testing.expectEqual(@as(u32, 5), result.count);
    // Root C (newest) first: its child D, then C. Then root A: B's subtree (E, B), then A.
    try expect_ref(ref(4, 1), out[0]);
    try expect_ref(ref(3, 1), out[1]);
    try expect_ref(ref(5, 1), out[2]);
    try expect_ref(ref(2, 1), out[3]);
    try expect_ref(ref(1, 1), out[4]);
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
    try expect_absent(&map, ref(1, 1));
    try testing.expectEqual(@as(u64, 0), map.next_seq);
}

test "reset refuses a short list or a corrupt table and leaves it unchanged" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleDialog);
    var short: [MaxOutput - 1]window_ref_t = undefined;
    try testing.expectEqual(reset_status_t.RefuseInvalid, reset(&map, &short).status);
    try testing.expectEqual(@as(u32, 2), realized_count(&map));

    var out = new_out();
    map.realized_count = 9;
    try testing.expectEqual(reset_status_t.RefuseInvalid, reset(&map, &out).status);
    try testing.expectEqual(@as(u32, 9), map.realized_count);

    // A parent cycle leaves slots unreachable from every root, so the teardown is incomplete.
    map.realized_count = 2;
    map.slots[0].parent = ref(2, 1);
    try testing.expectEqual(reset_status_t.RefuseInvalid, reset(&map, &out).status);
    try testing.expectEqual(@as(u32, 2), realized_count(&map));
}

test "a full table can be torn down with a list of exactly 64 entries" {
    var map = new_map();
    for (1..MaxRealized + 1) |index| {
        _ = announce(&map, ref(index, 1), none(), RoleToplevel);
    }
    var out = new_out();
    const result = reset(&map, &out);
    try testing.expectEqual(@as(u32, 64), result.count);
    try expect_ref(ref(64, 1), out[0]);
    try expect_ref(ref(1, 1), out[63]);
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
}

test "announce toplevel realizes into the lowest slot and records the none parent" {
    var map = new_map();
    const result = announce(&map, ref(10, 1), none(), RoleToplevel);
    try testing.expectEqual(announce_status_t.Realize, result.status);
    try testing.expectEqual(@as(u32, 0), result.slot);
    try testing.expect(is_current(&map, ref(10, 1)));
    const entry = lookup(&map, ref(10, 1)).?;
    try testing.expectEqual(RoleToplevel, entry.role);
    try testing.expectEqual(@as(u32, 0), entry.slot);
    try testing.expectEqual(@as(u64, 0), entry.realize_seq);
    try expect_ref(none(), entry.parent);
}

test "slots are stable: retiring another window never moves a realized window" {
    var map = new_map();
    var out = new_out();
    const a = announce(&map, ref(1, 1), none(), RoleToplevel);
    const b = announce(&map, ref(2, 1), none(), RoleToplevel);
    const c = announce(&map, ref(3, 1), none(), RoleToplevel);
    try testing.expectEqual(@as(u32, 0), a.slot);
    try testing.expectEqual(@as(u32, 1), b.slot);
    try testing.expectEqual(@as(u32, 2), c.slot);
    _ = retire(&map, ref(1, 1), &out);
    try testing.expectEqual(@as(u32, 2), lookup(&map, ref(3, 1)).?.slot);
    try testing.expectEqual(@as(u32, 1), lookup(&map, ref(2, 1)).?.slot);
    // The freed slot 0 is the lowest free index, so the next realize reuses it.
    try testing.expectEqual(@as(u32, 0), announce(&map, ref(4, 1), none(), RoleToplevel).slot);
    try testing.expectEqual(@as(u32, 0), lookup(&map, ref(4, 1)).?.slot);
    // Sequence numbers keep increasing across reuse; the reused slot is the newest window.
    try testing.expectEqual(@as(u64, 3), lookup(&map, ref(4, 1)).?.realize_seq);
    try testing.expectEqual(@as(u64, 2), lookup(&map, ref(3, 1)).?.realize_seq);
}

test "every announce refusal reports its status and changes nothing" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    const before = map;

    // Invalid identity and parent shapes.
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(0, 1), none(), RoleToplevel));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 0), none(), RoleToplevel));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), ref(0, 1), RoleDialog));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), ref(1, 0), RoleDialog));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), ref(2, 1), RoleDialog));
    // Invalid roles.
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), none(), 0));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), none(), 7));
    // Role parent rules.
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), ref(1, 1), RoleToplevel));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), ref(1, 1), RoleUacConsent));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), none(), RoleMenu));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), none(), RoleTooltip));
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), none(), RoleImePreedit));

    // Duplicate and window conflict.
    try testing.expectEqual(announce_status_t.RefuseDuplicate, announce_slot(&map, ref(1, 1), none(), RoleToplevel));
    try testing.expectEqual(announce_status_t.RefuseWindowConflict, announce_slot(&map, ref(1, 2), none(), RoleToplevel));
    const refused = announce(&map, ref(1, 1), none(), RoleToplevel);
    try testing.expectEqual(NoSlot, refused.slot);

    try testing.expectEqual(before.realized_count, map.realized_count);
    try testing.expectEqual(before.next_seq, map.next_seq);
    try expect_ref(before.slots[0].window, map.slots[0].window);
    try testing.expectEqual(before.slots[0].role, map.slots[0].role);
}

test "a conflict is reported before a stale parent" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    // The conflict test precedes the parent test: the stale parent never gets to decide.
    try testing.expectEqual(announce_status_t.RefuseWindowConflict, announce_slot(&map, ref(1, 2), ref(9, 1), RoleDialog));
    try testing.expectEqual(announce_status_t.RefuseDuplicate, announce_slot(&map, ref(1, 1), ref(9, 1), RoleDialog));
    try testing.expectEqual(@as(u32, 1), realized_count(&map));
}

test "a child whose parent was never announced is refused as a stale parent and not stored" {
    var map = new_map();
    try testing.expectEqual(announce_status_t.RefuseStaleParent, announce_slot(&map, ref(2, 1), ref(9, 1), RoleDialog));
    try testing.expectEqual(announce_status_t.RefuseStaleParent, announce_slot(&map, ref(2, 1), ref(9, 1), RoleMenu));
    try expect_absent(&map, ref(2, 1));
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
    // The guest resends once the parent is exported; then the same child realizes.
    _ = announce(&map, ref(9, 1), none(), RoleToplevel);
    try testing.expectEqual(announce_status_t.Realize, announce_slot(&map, ref(2, 1), ref(9, 1), RoleDialog));
}

test "a parent realized under another generation is a stale parent" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    try testing.expectEqual(announce_status_t.RefuseStaleParent, announce_slot(&map, ref(2, 1), ref(1, 2), RoleDialog));
    try testing.expectEqual(@as(u32, 1), realized_count(&map));
}

test "a child announced after its parent was retired is a stale parent" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    try testing.expectEqual(retire_status_t.Retired, retire(&map, ref(1, 1), &out).status);
    try testing.expectEqual(announce_status_t.RefuseStaleParent, announce_slot(&map, ref(2, 1), ref(1, 1), RoleDialog));
    try testing.expectEqual(announce_status_t.RefuseStaleParent, announce_slot(&map, ref(3, 1), ref(1, 1), RoleMenu));
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
}

test "dialog without a parent and uac consent are realized without a parent" {
    var map = new_map();
    try testing.expectEqual(announce_status_t.Realize, announce_slot(&map, ref(1, 1), none(), RoleDialog));
    try testing.expectEqual(announce_status_t.Realize, announce_slot(&map, ref(2, 1), none(), RoleUacConsent));
    try testing.expectEqual(RoleUacConsent, lookup(&map, ref(2, 1)).?.role);
}

test "a dialog keeps its parent" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    try testing.expectEqual(announce_status_t.Realize, announce_slot(&map, ref(2, 1), ref(1, 1), RoleDialog));
    const dialog = lookup(&map, ref(2, 1)).?;
    try expect_ref(ref(1, 1), dialog.parent);
    try testing.expectEqual(RoleDialog, dialog.role);
}

test "a popup keeps its direct parent whether that parent is a toplevel, a dialog or a popup" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleDialog);
    try testing.expectEqual(announce_status_t.Realize, announce_slot(&map, ref(3, 1), ref(1, 1), RoleMenu));
    try expect_ref(ref(1, 1), lookup(&map, ref(3, 1)).?.parent);
    try testing.expectEqual(announce_status_t.Realize, announce_slot(&map, ref(4, 1), ref(2, 1), RoleTooltip));
    try expect_ref(ref(2, 1), lookup(&map, ref(4, 1)).?.parent);
    try testing.expectEqual(announce_status_t.Realize, announce_slot(&map, ref(5, 1), ref(4, 1), RoleImePreedit));
    try expect_ref(ref(4, 1), lookup(&map, ref(5, 1)).?.parent);
}

test "a nested popup keeps its direct parent and retire cascades through the chain" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleMenu);
    _ = announce(&map, ref(3, 1), ref(2, 1), RoleTooltip);
    _ = announce(&map, ref(4, 1), ref(3, 1), RoleImePreedit);
    try expect_ref(ref(3, 1), lookup(&map, ref(4, 1)).?.parent);
    try expect_ref(ref(2, 1), lookup(&map, ref(3, 1)).?.parent);

    // Retire follows the direct parent chain, children first, so the whole chain goes with the toplevel.
    const result = retire(&map, ref(1, 1), &out);
    try testing.expectEqual(@as(u32, 4), result.count);
    try expect_ref(ref(4, 1), out[0]);
    try expect_ref(ref(3, 1), out[1]);
    try expect_ref(ref(2, 1), out[2]);
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
        try testing.expectEqual(announce_status_t.Realize, announce_slot(&map, ref(index, 1), none(), RoleToplevel));
    }
    try testing.expectEqual(@as(u32, 64), realized_count(&map));
    try testing.expectEqual(announce_status_t.RefuseExhausted, announce_slot(&map, ref(65, 1), none(), RoleToplevel));
    try testing.expectEqual(announce_status_t.RefuseExhausted, announce_slot(&map, ref(65, 1), ref(1, 1), RoleDialog));
    try testing.expectEqual(@as(u32, 64), realized_count(&map));
    // Retiring one window frees a slot; the refused window then realizes into it.
    var out = new_out();
    _ = retire(&map, ref(64, 1), &out);
    try testing.expectEqual(@as(u32, 62), lookup(&map, ref(63, 1)).?.slot);
    try testing.expectEqual(announce_status_t.Realize, announce_slot(&map, ref(65, 1), none(), RoleToplevel));
    try testing.expectEqual(@as(u32, 63), lookup(&map, ref(65, 1)).?.slot);
    try testing.expectEqual(@as(u32, 64), realized_count(&map));
}

test "the sequence counter refuses instead of wrapping" {
    var map = new_map();
    map.next_seq = std.math.maxInt(u64) - 1;
    const last = announce(&map, ref(1, 1), none(), RoleToplevel);
    try testing.expectEqual(announce_status_t.Realize, last.status);
    try testing.expectEqual(std.math.maxInt(u64) - 1, lookup(&map, ref(1, 1)).?.realize_seq);
    try testing.expectEqual(announce_status_t.RefuseExhausted, announce_slot(&map, ref(2, 1), none(), RoleToplevel));
    try testing.expectEqual(std.math.maxInt(u64), map.next_seq);
    try testing.expectEqual(@as(u32, 1), realized_count(&map));
}

test "a corrupt realized count is refused by every operation and not used" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);

    map.realized_count = 65;
    try testing.expectEqual(NoSlot, announce(&map, ref(2, 1), none(), RoleToplevel).slot);
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), none(), RoleToplevel));
    try testing.expectEqual(retire_status_t.RefuseInvalid, retire(&map, ref(1, 1), &out).status);
    try testing.expect(lookup(&map, ref(1, 1)) == null);
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
    try testing.expectEqual(@as(u32, 65), map.realized_count);

    // A count that disagrees with the occupied slots is corrupt too.
    map.realized_count = 3;
    try testing.expectEqual(announce_status_t.RefuseInvalid, announce_slot(&map, ref(2, 1), none(), RoleToplevel));
    try testing.expectEqual(retire_status_t.RefuseInvalid, retire(&map, ref(1, 1), &out).status);
    try testing.expect(!is_current(&map, ref(1, 1)));
}

test "epoch reset clears every realized entry" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleDialog);
    try testing.expectEqual(reset_status_t.Reset, reset(&map, &out).status);
    try testing.expectEqual(@as(u32, 0), realized_count(&map));
    try expect_absent(&map, ref(1, 1));
    try testing.expectEqual(announce_status_t.Realize, announce_slot(&map, ref(1, 1), none(), RoleToplevel));
}

test "a parent cycle planted in caller storage is refused by retire instead of looping" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleDialog);
    // Corrupt storage: make window 1 a child of window 2, closing a cycle.
    map.slots[0].parent = ref(2, 1);
    try testing.expectEqual(retire_status_t.RefuseInvalid, retire(&map, ref(1, 1), &out).status);
    try testing.expectEqual(@as(u32, 2), realized_count(&map));
}

// ---- reference model -------------------------------------------------------------------------

/// Reference model: a deliberately simple, separately written implementation of the same contract. It
/// uses a plain array of optional entries and recursion where the module uses slot scans and a bounded
/// emitter, so agreement is evidence rather than a copy of the same code.
const reference_model_t = struct {
    entries: [MaxRealized]?model_entry_t,
    next_seq: u64,

    const model_entry_t = struct {
        window: window_ref_t,
        parent: window_ref_t,
        role: u8,
        seq: u64,
        user_data: u64,
        dismissed: bool,
    };

    fn model_init() reference_model_t {
        return .{ .entries = [_]?model_entry_t{null} ** MaxRealized, .next_seq = 0 };
    }

    fn model_count(self: *const reference_model_t) usize {
        var count: usize = 0;
        for (self.entries) |entry| {
            if (entry != null) count += 1;
        }
        return count;
    }

    fn model_find_window(self: *const reference_model_t, window: u64) ?usize {
        for (self.entries, 0..) |entry, index| {
            if (entry) |value| {
                if (value.window.window == window) return index;
            }
        }
        return null;
    }

    fn model_announce(self: *reference_model_t, window: window_ref_t, parent: window_ref_t, role: u8) announce_result_t {
        const refused = announce_result_t{ .status = .RefuseInvalid, .slot = NoSlot };
        if (window.window == 0 or window.generation == 0) return refused;
        if ((parent.window == 0) != (parent.generation == 0)) return refused;
        if (parent.window == window.window) return refused;
        if (role < 1 or role > 6) return refused;
        const popup = role == 3 or role == 4 or role == 6;
        if (popup and parent.window == 0) return refused;
        if ((role == 1 or role == 5) and parent.window != 0) return refused;
        if (self.model_find_window(window.window)) |index| {
            const same = same_ref(self.entries[index].?.window, window);
            return .{ .status = if (same) .RefuseDuplicate else .RefuseWindowConflict, .slot = NoSlot };
        }
        if (parent.window != 0) {
            const index = self.model_find_window(parent.window) orelse return .{ .status = .RefuseStaleParent, .slot = NoSlot };
            if (!same_ref(self.entries[index].?.window, parent)) return .{ .status = .RefuseStaleParent, .slot = NoSlot };
        }
        if (self.model_count() == MaxRealized or self.next_seq == std.math.maxInt(u64)) return .{ .status = .RefuseExhausted, .slot = NoSlot };
        var free: usize = 0;
        while (self.entries[free] != null) free += 1;
        self.entries[free] = .{ .window = window, .parent = parent, .role = role, .seq = self.next_seq, .user_data = 0, .dismissed = false };
        self.next_seq += 1;
        return .{ .status = .Realize, .slot = @intCast(free) };
    }

    /// Children of `index` in descending sequence order, each followed by its own subtree, then `index`.
    fn model_emit(self: *const reference_model_t, index: usize, out: *std.ArrayList(usize)) error{OutOfMemory}!void {
        const pair = self.entries[index].?.window;
        var children = std.ArrayList(usize).init(testing.allocator);
        defer children.deinit();
        for (self.entries, 0..) |entry, child| {
            if (entry) |value| {
                if (same_ref(value.parent, pair)) try children.append(child);
            }
        }
        std.mem.sort(usize, children.items, self, by_descending_seq);
        for (children.items) |child| try self.model_emit(child, out);
        try out.append(index);
    }

    fn by_descending_seq(self: *const reference_model_t, left: usize, right: usize) bool {
        return self.entries[left].?.seq > self.entries[right].?.seq;
    }

    fn model_retire(self: *reference_model_t, window: window_ref_t, out: *std.ArrayList(window_ref_t)) !retire_status_t {
        out.clearRetainingCapacity();
        if (window.window == 0 or window.generation == 0) return .RefuseInvalid;
        var target: ?usize = null;
        for (self.entries, 0..) |entry, index| {
            if (entry) |value| {
                if (same_ref(value.window, window)) target = index;
            }
        }
        const index = target orelse return .Stale;
        var order = std.ArrayList(usize).init(testing.allocator);
        defer order.deinit();
        try self.model_emit(index, &order);
        for (order.items) |position| try out.append(self.entries[position].?.window);
        for (order.items) |position| self.entries[position] = null;
        return .Retired;
    }

    /// Teardown: roots by descending sequence number, each subtree children first.
    fn model_mark_dismissed(self: *reference_model_t, window: window_ref_t) dismiss_status_t {
        if (window.window == 0 or window.generation == 0) return .RefuseInvalid;
        const index = self.model_find_window(window.window) orelse return .Stale;
        if (!same_ref(self.entries[index].?.window, window)) return .Stale;
        const role = self.entries[index].?.role;
        if (role != 3 and role != 4 and role != 6) return .RefuseInvalid;
        self.entries[index].?.dismissed = true;
        return .Marked;
    }

    fn model_user_data(self: *reference_model_t, slot: u32, window: window_ref_t, set: ?u64) user_data_status_t {
        if (window.window == 0 or window.generation == 0 or slot >= MaxRealized) return .RefuseInvalid;
        const entry = &(self.entries[slot] orelse return .Stale);
        if (!same_ref(entry.window, window)) return .Stale;
        if (set) |value| entry.user_data = value;
        return .Ok;
    }

    fn model_reset(self: *reference_model_t, out: *std.ArrayList(window_ref_t)) !reset_status_t {
        out.clearRetainingCapacity();
        var roots = std.ArrayList(usize).init(testing.allocator);
        defer roots.deinit();
        for (self.entries, 0..) |entry, index| {
            if (entry) |value| {
                if (value.parent.window == 0) try roots.append(index);
            }
        }
        std.mem.sort(usize, roots.items, self, by_descending_seq);
        var order = std.ArrayList(usize).init(testing.allocator);
        defer order.deinit();
        for (roots.items) |root| try self.model_emit(root, &order);
        for (order.items) |position| try out.append(self.entries[position].?.window);
        self.* = model_init();
        return .Reset;
    }
};

fn expect_same_state(map: *const host_window_map_t, model: *const reference_model_t) !void {
    try testing.expectEqual(model.model_count(), realized_count(map));
    try testing.expectEqual(model.next_seq, map.next_seq);
    for (model.entries, 0..) |entry, index| {
        if (entry) |value| {
            try testing.expect(map.slots[index].occupied != 0);
            try expect_ref(value.window, map.slots[index].window);
            try expect_ref(value.parent, map.slots[index].parent);
            try testing.expectEqual(value.role, map.slots[index].role);
            try testing.expectEqual(value.seq, map.slots[index].realize_seq);
            try testing.expectEqual(value.user_data, map.slots[index].user_data);
            try testing.expectEqual(@intFromBool(value.dismissed), map.slots[index].dismissed);
        } else {
            try testing.expectEqual(@as(u8, 0), map.slots[index].occupied);
        }
    }
}

fn run_random_sequence(seed: u64, windows: u64, operations: usize) !void {
    var prng = std.rand.DefaultPrng.init(seed);
    const random = prng.random();
    var map = new_map();
    var model = reference_model_t.model_init();
    var out = new_out();
    var out_list = std.ArrayList(window_ref_t).init(testing.allocator);
    defer out_list.deinit();

    var step: usize = 0;
    while (step < operations) : (step += 1) {
        const choice = random.uintLessThan(u8, 100);
        if (choice < 2) {
            const got_reset = reset(&map, &out);
            const expected_reset = try model.model_reset(&out_list);
            try testing.expectEqual(expected_reset, got_reset.status);
            if (got_reset.status == .Reset) {
                try testing.expectEqual(@as(u32, @intCast(out_list.items.len)), got_reset.count);
                for (out_list.items, 0..) |pair, index| try expect_ref(pair, out[index]);
            }
        } else if (choice < 70) {
            // Zero and mismatched identities appear on purpose.
            const window = ref(random.uintLessThan(u64, windows + 1), random.uintLessThan(u64, 4));
            const parent = if (random.uintLessThan(u8, 10) < 3) none() else ref(random.uintLessThan(u64, windows + 1), random.uintLessThan(u64, 4));
            const role = random.uintLessThan(u8, 9);
            const got = announce(&map, window, parent, role);
            const expected = model.model_announce(window, parent, role);
            try testing.expectEqual(expected.status, got.status);
            try testing.expectEqual(expected.slot, got.slot);
        } else if (choice < 85) {
            const window = ref(random.uintLessThan(u64, windows + 1), random.uintLessThan(u64, 4));
            const got = retire(&map, window, &out);
            const expected = try model.model_retire(window, &out_list);
            try testing.expectEqual(expected, got.status);
            if (got.status == .Retired) {
                try testing.expectEqual(@as(u32, @intCast(out_list.items.len)), got.count);
                for (out_list.items, 0..) |pair, index| try expect_ref(pair, out[index]);
            }
        } else {
            // Per-slot user data and dismissal: slots past the table and mismatched windows are refused or stale.
            const slot: u32 = random.uintLessThan(u32, MaxRealized + 2);
            const window = ref(random.uintLessThan(u64, windows + 1), random.uintLessThan(u64, 4));
            switch (random.uintLessThan(u8, 3)) {
                0 => {
                    const value = random.int(u64);
                    try testing.expectEqual(model.model_user_data(slot, window, value), set_user_data(&map, slot, window, value));
                },
                1 => {
                    var got: u64 = 0xDEAD;
                    const status = get_user_data(&map, slot, window, &got);
                    try testing.expectEqual(model.model_user_data(slot, window, null), status);
                    if (status == .Ok) try testing.expectEqual(model.entries[slot].?.user_data, got) else try testing.expectEqual(@as(u64, 0xDEAD), got);
                },
                else => try testing.expectEqual(model.model_mark_dismissed(window), mark_dismissed(&map, window)),
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

test "user data is stored per slot, validated by slot and window, and reset when the slot is reused" {
    var map = new_map();
    var out = new_out();
    const a = announce(&map, ref(1, 1), none(), RoleToplevel);
    const b = announce(&map, ref(2, 1), none(), RoleToplevel);
    try testing.expectEqual(user_data_status_t.Ok, set_user_data(&map, a.slot, ref(1, 1), 0xAAAA));
    try testing.expectEqual(user_data_status_t.Ok, set_user_data(&map, b.slot, ref(2, 1), 0xBBBB));
    var value: u64 = 0;
    try testing.expectEqual(user_data_status_t.Ok, get_user_data(&map, a.slot, ref(1, 1), &value));
    try testing.expectEqual(@as(u64, 0xAAAA), value);
    try testing.expectEqual(@as(u64, 0xAAAA), lookup(&map, ref(1, 1)).?.user_data);
    // Retiring window 1 leaves window 2's slot and value alone.
    _ = retire(&map, ref(1, 1), &out);
    try testing.expectEqual(user_data_status_t.Ok, get_user_data(&map, b.slot, ref(2, 1), &value));
    try testing.expectEqual(@as(u64, 0xBBBB), value);
    // The freed slot is reused with user data zero.
    const c = announce(&map, ref(3, 1), none(), RoleToplevel);
    try testing.expectEqual(a.slot, c.slot);
    try testing.expectEqual(user_data_status_t.Ok, get_user_data(&map, c.slot, ref(3, 1), &value));
    try testing.expectEqual(@as(u64, 0), value);
    // A stale generation or a free slot is Stale and leaves the value unchanged.
    try testing.expectEqual(user_data_status_t.Stale, set_user_data(&map, c.slot, ref(3, 2), 1));
    try testing.expectEqual(user_data_status_t.Stale, get_user_data(&map, a.slot + 60, ref(3, 1), &value));
    try testing.expectEqual(user_data_status_t.Stale, set_user_data(&map, a.slot + 60, ref(3, 1), 1));
    try testing.expectEqual(@as(u64, 0), lookup(&map, ref(3, 1)).?.user_data);
}

test "a dismissed popup stays realized with its slot until the guest retires it" {
    var map = new_map();
    var out = new_out();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    const menu = announce(&map, ref(2, 1), ref(1, 1), RoleMenu);
    try testing.expectEqual(dismiss_status_t.Marked, mark_dismissed(&map, ref(2, 1)));
    try testing.expectEqual(dismiss_status_t.Marked, mark_dismissed(&map, ref(2, 1)));
    const entry = lookup(&map, ref(2, 1)).?;
    try testing.expectEqual(@as(u8, 1), entry.dismissed);
    try testing.expectEqual(menu.slot, entry.slot);
    try testing.expect(is_current(&map, ref(2, 1)));
    // A dismissed toplevel is refused: only popups are dismissed.
    try testing.expectEqual(dismiss_status_t.RefuseInvalid, mark_dismissed(&map, ref(1, 1)));
    try testing.expectEqual(@as(u8, 0), lookup(&map, ref(1, 1)).?.dismissed);
    // Retire removes it as usual; the reused slot is not dismissed.
    try testing.expectEqual(retire_status_t.Retired, retire(&map, ref(2, 1), &out).status);
    const tooltip = announce(&map, ref(3, 1), ref(1, 1), RoleTooltip);
    try testing.expectEqual(menu.slot, tooltip.slot);
    try testing.expectEqual(@as(u8, 0), lookup(&map, ref(3, 1)).?.dismissed);
}

test "dismissing refuses zero identities, unknown or other-generation pairs and a corrupt table" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleMenu);
    try testing.expectEqual(dismiss_status_t.RefuseInvalid, mark_dismissed(&map, ref(0, 1)));
    try testing.expectEqual(dismiss_status_t.Stale, mark_dismissed(&map, ref(9, 1)));
    try testing.expectEqual(dismiss_status_t.Stale, mark_dismissed(&map, ref(2, 2)));
    map.realized_count = 5;
    try testing.expectEqual(dismiss_status_t.RefuseInvalid, mark_dismissed(&map, ref(2, 1)));
    try testing.expectEqual(@as(u8, 0), map.slots[1].dismissed);
}

test "user data accessors refuse bad arguments and a corrupt table" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    var value: u64 = 5;
    try testing.expectEqual(user_data_status_t.RefuseInvalid, set_user_data(&map, MaxRealized, ref(1, 1), 1));
    try testing.expectEqual(user_data_status_t.RefuseInvalid, set_user_data(&map, 0, ref(0, 1), 1));
    try testing.expectEqual(user_data_status_t.RefuseInvalid, get_user_data(&map, 0, ref(1, 0), &value));
    try testing.expectEqual(@as(u64, 5), value);
    map.realized_count = 2;
    try testing.expectEqual(user_data_status_t.RefuseInvalid, set_user_data(&map, 0, ref(1, 1), 1));
    try testing.expectEqual(user_data_status_t.RefuseInvalid, get_user_data(&map, 0, ref(1, 1), &value));
    try testing.expectEqual(@as(u64, 5), value);
    map.realized_count = 1;
    try testing.expectEqual(user_data_status_t.Ok, set_user_data(&map, 0, ref(1, 1), 9));
}

test "C ABI user data accessors refuse NULL, overlapping and bad arguments without writing" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    var value: u64 = 5;
    try testing.expectEqual(user_data_status_t.RefuseInvalid, abi_shim_t.set_user_data(null, 0, ref(1, 1), 1));
    try testing.expectEqual(user_data_status_t.RefuseInvalid, abi_shim_t.get_user_data(null, 0, ref(1, 1), &value));
    try testing.expectEqual(user_data_status_t.RefuseInvalid, abi_shim_t.get_user_data(&map, 0, ref(1, 1), null));
    // A value pointer inside the table is refused before any read or write.
    const inside: *u64 = @ptrCast(&map.slots[0].user_data);
    try testing.expectEqual(user_data_status_t.RefuseInvalid, abi_shim_t.get_user_data(&map, 0, ref(1, 1), inside));
    try testing.expectEqual(user_data_status_t.Ok, abi_shim_t.set_user_data(&map, 0, ref(1, 1), 42));
    try testing.expectEqual(user_data_status_t.Ok, abi_shim_t.get_user_data(&map, 0, ref(1, 1), &value));
    try testing.expectEqual(@as(u64, 42), value);
}

test "C ABI dismissal accepts NULL and reports the status" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleMenu);
    try testing.expectEqual(dismiss_status_t.RefuseInvalid, abi_shim_t.mark_dismissed(null, ref(2, 1)));
    try testing.expectEqual(dismiss_status_t.Marked, abi_shim_t.mark_dismissed(&map, ref(2, 1)));
}

test "C ABI counts and lookup accept NULL and report the table" {
    try testing.expectEqual(@as(u32, 0), abi_shim_t.realized(null));
    try testing.expect(!abi_shim_t.is_current(null, ref(1, 1)));
    try testing.expect(!abi_shim_t.lookup(null, ref(1, 1), null));
    try testing.expectEqual(NoSlot, abi_shim_t.announce(null, ref(1, 1), none(), RoleToplevel).slot);

    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    var out = new_out();
    var count: u32 = 0;
    try testing.expectEqual(reset_status_t.RefuseInvalid, abi_shim_t.reset(null, &out, MaxOutput, &count));
    try testing.expectEqual(@as(u32, 1), abi_shim_t.realized(&map));
    try testing.expectEqual(reset_status_t.Reset, abi_shim_t.reset(&map, &out, MaxOutput, &count));
    try testing.expectEqual(@as(u32, 1), count);
    try testing.expectEqual(@as(u32, 0), abi_shim_t.realized(&map));
    try testing.expect(!abi_shim_t.is_current(&map, ref(1, 1)));
}

test "C ABI announce and retire write outputs and counts on success" {
    var map = new_map();
    var out = new_out();
    var count: u32 = 99;
    try testing.expectEqual(announce_status_t.Realize, abi_shim_t.announce(&map, ref(1, 1), none(), RoleToplevel).status);
    try testing.expectEqual(announce_status_t.RefuseStaleParent, abi_shim_t.announce(&map, ref(2, 1), ref(9, 1), RoleDialog).status);
    try testing.expectEqual(announce_status_t.RefuseInvalid, abi_shim_t.announce(null, ref(2, 1), none(), RoleToplevel).status);
    try testing.expectEqual(announce_status_t.Realize, abi_shim_t.announce(&map, ref(9, 1), none(), RoleToplevel).status);
    try testing.expectEqual(announce_status_t.Realize, abi_shim_t.announce(&map, ref(2, 1), ref(9, 1), RoleDialog).status);

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

test "C ABI lookup returns a copy with its slot and a presence test needs no output" {
    var map = new_map();
    _ = announce(&map, ref(1, 1), none(), RoleToplevel);
    _ = announce(&map, ref(2, 1), ref(1, 1), RoleMenu);
    var entry: window_entry_t = undefined;
    try testing.expect(abi_shim_t.lookup(&map, ref(2, 1), &entry));
    try expect_ref(ref(1, 1), entry.parent);
    try testing.expectEqual(@as(u32, 1), entry.slot);
    try testing.expect(abi_shim_t.lookup(&map, ref(1, 1), null));
    try testing.expect(!abi_shim_t.lookup(&map, ref(1, 2), &entry));
    try testing.expect(abi_shim_t.is_current(&map, ref(2, 1)));
    try testing.expect(!abi_shim_t.is_current(&map, ref(3, 1)));
    try testing.expectEqual(@as(u32, 2), abi_shim_t.realized(&map));
}

test "C ABI error code helpers return the protocol values for every status" {
    try testing.expectEqual(ErrorStaleWindow, abi_shim_t.announce_error_code(.RefuseStaleParent));
    try testing.expectEqual(ErrorQuotaExceeded, abi_shim_t.announce_error_code(.RefuseExhausted));
    try testing.expectEqual(ErrorStaleWindow, abi_shim_t.retire_error_code(.Stale));
}

test "every announce and retire status maps to its protocol error code" {
    try testing.expectEqual(ErrorNone, announce_error_code(.Realize));
    try testing.expectEqual(ErrorProtocolViolation, announce_error_code(.RefuseDuplicate));
    try testing.expectEqual(ErrorStaleWindow, announce_error_code(.RefuseStaleParent));
    try testing.expectEqual(ErrorStaleWindow, announce_error_code(.RefuseWindowConflict));
    try testing.expectEqual(ErrorQuotaExceeded, announce_error_code(.RefuseExhausted));
    try testing.expectEqual(ErrorMalformedFrame, announce_error_code(.RefuseInvalid));
    try testing.expectEqual(ErrorNone, retire_error_code(.Retired));
    try testing.expectEqual(ErrorStaleWindow, retire_error_code(.Stale));
    try testing.expectEqual(ErrorMalformedFrame, retire_error_code(.RefuseInvalid));
}

test "role and error code values match the frozen protocol header" {
    const protocol = @cImport({
        @cInclude("waddle/interop_protocol.h");
    });
    try testing.expectEqual(@as(u8, protocol.InteropRoleToplevel), RoleToplevel);
    try testing.expectEqual(@as(u8, protocol.InteropRoleDialog), RoleDialog);
    try testing.expectEqual(@as(u8, protocol.InteropRoleMenu), RoleMenu);
    try testing.expectEqual(@as(u8, protocol.InteropRoleTooltip), RoleTooltip);
    try testing.expectEqual(@as(u8, protocol.InteropRoleUacConsent), RoleUacConsent);
    try testing.expectEqual(@as(u8, protocol.InteropRoleImePreedit), RoleImePreedit);
    try testing.expectEqual(@as(u32, protocol.InteropErrorNone), ErrorNone);
    try testing.expectEqual(@as(u32, protocol.InteropErrorProtocolViolation), ErrorProtocolViolation);
    try testing.expectEqual(@as(u32, protocol.InteropErrorMalformedFrame), ErrorMalformedFrame);
    try testing.expectEqual(@as(u32, protocol.InteropErrorStaleWindow), ErrorStaleWindow);
    try testing.expectEqual(@as(u32, protocol.InteropErrorQuotaExceeded), ErrorQuotaExceeded);
}
