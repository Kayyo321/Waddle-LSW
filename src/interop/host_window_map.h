#ifndef WaddleHostWindowMapH
#define WaddleHostWindowMapH
/**
 * @file host_window_map.h
 * @brief C ABI of the bounded host window mapping table (`host_window_map.zig`, TODO #11a).
 *
 * The table holds the guest `(window, generation)` identities that are realized on the host. It
 * decides whether an announced identity may be realized and which identities a retire removes, and in
 * which order. It makes no native or Wayland call, allocates nothing, and never wraps a counter.
 *
 * Caller rules common to every function:
 *  - The caller owns every object, including the storage of `host_window_map_t`. Call
 *    `host_window_map_reset` before first use of storage that was not zero-filled.
 *  - Every call on a table is owner-thread only. There is no internal locking.
 *  - The retire output array must hold at least `HostWindowMapMaxOutput` entries and must not overlap the
 *    table or the count pointer. Overlapping arguments are refused with no memory written.
 *  - No function retains a pointer after it returns.
 *
 * The full rules are in `impl/seamless-interop/claude_45_percent/host_window_map_contract.md`.
 */
#include "waddle/interop_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Table limit and output capacity. */
enum host_window_map_limits_t {
    HostWindowMapMaxRealized = 64, /**< Realized windows per session epoch. */
    HostWindowMapMaxOutput = 64 /**< Minimum output capacity for retire. */
};

/** @brief Frozen `interop_window_role_t` values accepted by announce. */
enum host_window_map_role_t {
    HostWindowMapRoleToplevel = 1, /**< xdg_toplevel without a parent; parent must be none. */
    HostWindowMapRoleDialog = 2, /**< xdg_toplevel; parent may be none or valid. */
    HostWindowMapRoleMenu = 3, /**< Grabbing xdg_popup; parent must be valid. */
    HostWindowMapRoleTooltip = 4, /**< Non-grabbing xdg_popup; parent must be valid. */
    HostWindowMapRoleUacConsent = 5, /**< xdg_toplevel for consent.exe; parent must be none. */
    HostWindowMapRoleImePreedit = 6 /**< Non-grabbing xdg_popup; parent must be valid. */
};

/* Role values are the frozen interop_window_role_t values from include/waddle/interop_protocol.h. */
_Static_assert((int)HostWindowMapRoleToplevel == (int)InteropRoleToplevel, "role Toplevel");
_Static_assert((int)HostWindowMapRoleDialog == (int)InteropRoleDialog, "role Dialog");
_Static_assert((int)HostWindowMapRoleMenu == (int)InteropRoleMenu, "role Menu");
_Static_assert((int)HostWindowMapRoleTooltip == (int)InteropRoleTooltip, "role Tooltip");
_Static_assert((int)HostWindowMapRoleUacConsent == (int)InteropRoleUacConsent, "role UacConsent");
_Static_assert((int)HostWindowMapRoleImePreedit == (int)InteropRoleImePreedit, "role ImePreedit");

/** @brief Exact window incarnation. Both fields zero is "none" when used as a parent. */
typedef struct host_window_ref_t {
    uint64_t window; /**< Protocol window handle. Zero means no window. In and out. */
    uint64_t generation; /**< Protocol generation, never reused in an epoch. Zero means no generation. In and out. */
} host_window_ref_t;

/** @brief One realized window; copied out by `host_window_map_lookup`. */
typedef struct host_window_entry_t {
    host_window_ref_t window; /**< Realized identity. */
    host_window_ref_t parent; /**< Requested direct parent, or none. A nested popup names its popup parent. Retire and positioning follow this field. */
    uint8_t role; /**< Raw `interop_window_role_t` value, 1..6. */
} host_window_entry_t;

/**
 * @brief Complete table state, caller-owned. Zero-filled storage equals a reset table.
 *
 * `realized_count` and `realized` are owned by the table and must not be written by the caller.
 */
typedef struct host_window_map_t {
    uint32_t realized_count; /**< Initialized entries at the front of `realized`, at most 64. */
    host_window_entry_t realized[64]; /**< Realized windows in realization order. */
} host_window_map_t;

/** @brief Result of `host_window_map_announce`. Only `Realize` changes the table. */
typedef enum host_window_map_announce_status_t {
    HostWindowMapRealize = 1, /**< The window is realized and appended to the table. */
    HostWindowMapRefuseDuplicate = 2, /**< The exact pair is already realized. No change. */
    HostWindowMapRefuseStaleParent = 3, /**< The parent pair is not realized: never announced, retired, or another generation. No change. */
    HostWindowMapRefuseWindowConflict = 4, /**< The window is realized under another generation; retire it first. No change. */
    HostWindowMapRefuseExhausted = 5, /**< The table already holds 64 realized windows. No change. */
    HostWindowMapRefuseInvalid = 6 /**< Zero identity, bad role or parent shape, or NULL map. No change. */
} host_window_map_announce_status_t;

/** @brief Result of `host_window_map_retire`. */
typedef enum host_window_map_retire_status_t {
    HostWindowMapRetired = 1, /**< Subtree removed; the output list and count are valid. */
    HostWindowMapStale = 2, /**< No realized entry with that exact pair. No change. */
    HostWindowMapRetireRefuseInvalid = 3 /**< Zero identity, short output, overlapping or NULL argument. No change. */
} host_window_map_retire_status_t;

/**
 * @brief Protocol error code (`interop_error_t`) for an announce status.
 *
 * Realize gives `InteropErrorNone`; a duplicate gives `InteropErrorProtocolViolation`; a stale parent or a
 * conflicting generation gives `InteropErrorStaleWindow`; a full table gives `InteropErrorQuotaExceeded`;
 * an invalid argument gives `InteropErrorMalformedFrame`.
 *
 * @param status In. Status returned by `host_window_map_announce`.
 * @return The `interop_error_t` value. Thread safety: any thread.
 */
uint32_t host_window_map_announce_error_code(host_window_map_announce_status_t status);

/**
 * @brief Protocol error code (`interop_error_t`) for a retire status.
 *
 * Retired gives `InteropErrorNone`; a stale pair gives `InteropErrorStaleWindow`; an invalid argument gives
 * `InteropErrorMalformedFrame`.
 *
 * @param status In. Status returned by `host_window_map_retire`.
 * @return The `interop_error_t` value. Thread safety: any thread.
 */
uint32_t host_window_map_retire_error_code(host_window_map_retire_status_t status);

/**
 * @brief Clear the table to the epoch-start state.
 *
 * @param map In/out. Table to clear. NULL is a no-op.
 * @return Nothing. Emits no output; the caller tears down its own native objects first.
 */
void host_window_map_reset(host_window_map_t *map);

/**
 * @brief Realize one window if its parent, when named, is exactly realized.
 *
 * A child whose parent pair is not realized is refused as a stale parent. The table never defers the
 * announcement; the guest resends it after the parent is exported.
 *
 * @param map In/out. Table. NULL gives `HostWindowMapRefuseInvalid`.
 * @param window In. Identity to realize; both fields nonzero.
 * @param parent In. Exact parent, or none (both fields zero). Menu, Tooltip and ImePreedit need a realized
 *               parent; Toplevel and UacConsent need none.
 * @param role In. `host_window_map_role_t` value 1..6. Other values are refused.
 * @return Announce status. Thread safety: owner thread only.
 */
host_window_map_announce_status_t host_window_map_announce(host_window_map_t *map, host_window_ref_t window, host_window_ref_t parent, uint8_t role);

/**
 * @brief Retire a realized window and its realized subtree.
 *
 * The output lists the removed pairs in reverse realization order: every descendant precedes its ancestor
 * and the target is last. The caller tears each one down in list order.
 *
 * @param map In/out. Table. NULL gives `HostWindowMapRetireRefuseInvalid`.
 * @param window In. Identity to retire; both fields nonzero and matching the realized generation.
 * @param out Out. Array of at least `out_capacity` entries. NULL gives `HostWindowMapRetireRefuseInvalid`.
 *            Written only on `HostWindowMapRetired`.
 * @param out_capacity In. Entries in `out`; below `HostWindowMapMaxOutput` gives refusal.
 * @param out_count Out. Receives the removed count. NULL gives `HostWindowMapRetireRefuseInvalid`. Must not
 *                  overlap the table or `out`. Written with the removed count (0 for `HostWindowMapStale`)
 *                  whenever the call reaches the table. It is NOT written when a NULL pointer, a short
 *                  `out_capacity`, or an overlap refuses the call; the caller's value is then unchanged.
 * @return Retire status. Thread safety: owner thread only.
 */
host_window_map_retire_status_t host_window_map_retire(host_window_map_t *map, host_window_ref_t window, host_window_ref_t *out, uint32_t out_capacity, uint32_t *out_count);

/**
 * @brief Look up a realized window by exact identity.
 *
 * @param map In. Table, read only. NULL gives false.
 * @param window In. Identity to find; both fields must match.
 * @param out Out. Receives a copy of the realized entry when found. NULL makes this a presence test.
 * @return True when `window` is realized under its generation. Thread safety: owner thread only.
 */
bool host_window_map_lookup(const host_window_map_t *map, host_window_ref_t window, host_window_entry_t *out);

/**
 * @brief Whether the exact identity is currently realized.
 *
 * @param map In. Table, read only. NULL gives false.
 * @param window In. Identity to test.
 * @return True exactly when `host_window_map_lookup` would succeed. Thread safety: owner thread only.
 */
bool host_window_map_is_current(const host_window_map_t *map, host_window_ref_t window);

/**
 * @brief Number of realized windows.
 *
 * @param map In. Table, read only. NULL gives 0.
 * @return Realized count, at most `HostWindowMapMaxRealized`. Thread safety: owner thread only.
 */
uint32_t host_window_map_realized_count(const host_window_map_t *map);

/* Layout frozen with the Zig extern structs; a change in either breaks the build. */
_Static_assert(sizeof(host_window_ref_t) == 16, "host_window_ref_t layout");
_Static_assert(sizeof(host_window_entry_t) == 40, "host_window_entry_t layout");
_Static_assert(sizeof(host_window_map_t) == 2568, "host_window_map_t layout");

#endif
