#ifndef WaddleHostWindowMapH
#define WaddleHostWindowMapH
/**
 * @file host_window_map.h
 * @brief C ABI of the bounded host window mapping table (`host_window_map.zig`, TODO #11a).
 *
 * The table maps guest `(window, generation)` identities to host surfaces. It decides which
 * identities must be realized or retired and in which order. It makes no native or Wayland call,
 * allocates nothing, and never wraps a counter.
 *
 * Caller rules common to every function:
 *  - The caller owns every object, and the storage of `host_window_map_t` is caller-owned. Call
 *    `host_window_map_reset` before first use of storage that was not zero-filled.
 *  - Every mutation and every read of a table is owner-thread only. There is no internal locking.
 *  - Output arrays must hold at least `HostWindowMapMaxOutput` entries. Output storage must not
 *    overlap the table, another output array, or a count pointer. Overlapping arguments are refused
 *    with no memory written.
 *  - No function retains a pointer after it returns.
 *
 * The full rules are in `impl/seamless-interop/claude_45_percent/host_window_map_contract.md`.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Table limits and output capacity, frozen by IMPL_DESC section 8.44. */
enum host_window_map_limits_t {
    HostWindowMapMaxRealized = 64, /**< Realized windows per session epoch. */
    HostWindowMapMaxDeferred = 64, /**< Deferred children per session epoch. */
    HostWindowMapMaxOutput = 64 /**< Minimum output capacity for announce and retire. */
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

/** @brief Exact window incarnation. Both fields zero is "none" when used as a parent. */
typedef struct host_window_ref_t {
    uint64_t window; /**< Protocol window handle. Zero means no window. In and out. */
    uint64_t generation; /**< Protocol generation, never reused in an epoch. Zero means no generation. In and out. */
} host_window_ref_t;

/** @brief One realized window; copied out by `host_window_map_lookup`. */
typedef struct host_window_entry_t {
    host_window_ref_t window; /**< Realized identity. */
    host_window_ref_t parent; /**< Requested parent, or none. Cascades follow this field. */
    host_window_ref_t root; /**< Popup only: positioning root (non-popup window). None for other roles. */
    uint8_t role; /**< Raw `interop_window_role_t` value, 1..6. */
} host_window_entry_t;

/** @brief One deferred child waiting for its parent. Internal to the table layout. */
typedef struct host_window_deferred_t {
    host_window_ref_t window; /**< Announced identity. */
    host_window_ref_t parent; /**< Exact parent identity that must realize first. */
    uint8_t role; /**< Raw `interop_window_role_t` value, 1..6. */
} host_window_deferred_t;

/**
 * @brief Complete table state, caller-owned. Zero-filled storage equals a reset table.
 *
 * Members other than the counts are owned by the table and must not be written by the caller.
 */
typedef struct host_window_map_t {
    uint32_t realized_count; /**< Initialized entries at the front of `realized`, at most 64. */
    uint32_t deferred_count; /**< Initialized entries at the front of `deferred`, at most 64. */
    host_window_entry_t realized[64]; /**< Realized windows in realization order. */
    host_window_deferred_t deferred[64]; /**< Deferred children in deferral order. */
} host_window_map_t;

/** @brief Result of `host_window_map_announce`. */
typedef enum host_window_map_announce_status_t {
    HostWindowMapRealize = 1, /**< Window and its unblocked children realized; output lists them in order. */
    HostWindowMapDefer = 2, /**< Parent not realized; the window was deferred. No output. */
    HostWindowMapRefuseDuplicate = 3, /**< The exact pair is already realized or deferred. No change. */
    HostWindowMapRefuseStaleParent = 4, /**< Parent window is realized under another generation. No change. */
    HostWindowMapRefuseWindowConflict = 5, /**< Window is known under another generation. No change. */
    HostWindowMapRefuseExhausted = 6, /**< A 64-entry limit would be exceeded. No change. */
    HostWindowMapRefuseInvalid = 7 /**< Zero identity, bad role or parent shape, short output, or bad pointer. No change. */
} host_window_map_announce_status_t;

/** @brief Result of `host_window_map_retire`. */
typedef enum host_window_map_retire_status_t {
    HostWindowMapRetired = 1, /**< Subtree removed; both output lists and counts are valid. */
    HostWindowMapStale = 2, /**< No realized entry with that exact pair. No change. */
    HostWindowMapRetireRefuseInvalid = 3 /**< Zero identity, short output, or bad pointer. No change. */
} host_window_map_retire_status_t;

/**
 * @brief Clear the table to the epoch-start state.
 *
 * @param map In/out. Table to clear. NULL is a no-op.
 * @return Nothing. Emits no output; the caller tears down its own native objects first.
 */
void host_window_map_reset(host_window_map_t *map);

/**
 * @brief Announce one window and collect the realize list.
 *
 * On `HostWindowMapRealize`, `out[0]` is the announced pair and `out[1..*out_count-1]` are the deferred
 * children it unblocked, breadth-first in deferral order. Realize the list in this order. Every other
 * status writes no output and changes no state.
 *
 * @param map In/out. Table. NULL gives `HostWindowMapRefuseInvalid`.
 * @param window In. Identity to realize; both fields nonzero.
 * @param parent In. Exact parent, or none (both fields zero). Menu, Tooltip and ImePreedit need a valid parent;
 *               Toplevel and UacConsent need none.
 * @param role In. `host_window_map_role_t` value 1..6. Other values are refused.
 * @param out Out. Array of at least `out_capacity` entries. NULL gives `HostWindowMapRefuseInvalid`.
 * @param out_capacity In. Entries in `out`; below `HostWindowMapMaxOutput` gives `HostWindowMapRefuseInvalid`.
 * @param out_count Out. Receives the output count. NULL gives `HostWindowMapRefuseInvalid`. Not written when an
 *                  argument is refused as NULL or overlapping.
 * @return Announce status. Thread safety: owner thread only.
 */
host_window_map_announce_status_t host_window_map_announce(host_window_map_t *map, host_window_ref_t window, host_window_ref_t parent, uint8_t role, host_window_ref_t *out, uint32_t out_capacity, uint32_t *out_count);

/**
 * @brief Retire a realized window and its realized subtree.
 *
 * `retired_out` lists the removed pairs in reverse realization order: every descendant precedes its ancestor
 * and the target is last. `discarded_out` lists deferred children that could never realize, in deferral order.
 * Realize nothing from either list; send the caller's teardown for each retired pair in list order.
 *
 * @param map In/out. Table. NULL gives `HostWindowMapRetireRefuseInvalid`.
 * @param window In. Identity to retire; both fields nonzero and matching the realized generation.
 * @param retired_out Out. Array of at least `retired_capacity` entries. NULL gives `HostWindowMapRetireRefuseInvalid`.
 * @param retired_capacity In. Entries in `retired_out`; below `HostWindowMapMaxOutput` gives refusal.
 * @param retired_count Out. Receives the retired count. NULL gives `HostWindowMapRetireRefuseInvalid`.
 * @param discarded_out Out. Array of at least `discarded_capacity` entries. NULL gives `HostWindowMapRetireRefuseInvalid`.
 * @param discarded_capacity In. Entries in `discarded_out`; below `HostWindowMapMaxOutput` gives refusal.
 * @param discarded_count Out. Receives the discarded count. NULL gives `HostWindowMapRetireRefuseInvalid`.
 * @return Retire status. Thread safety: owner thread only.
 */
host_window_map_retire_status_t host_window_map_retire(host_window_map_t *map, host_window_ref_t window, host_window_ref_t *retired_out, uint32_t retired_capacity, uint32_t *retired_count, host_window_ref_t *discarded_out, uint32_t discarded_capacity, uint32_t *discarded_count);

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

/**
 * @brief Number of deferred children.
 *
 * @param map In. Table, read only. NULL gives 0.
 * @return Deferred count, at most `HostWindowMapMaxDeferred`. Thread safety: owner thread only.
 */
uint32_t host_window_map_deferred_count(const host_window_map_t *map);

/* Layout frozen with the Zig extern structs; a change in either breaks the build. */
_Static_assert(sizeof(host_window_ref_t) == 16, "host_window_ref_t layout");
_Static_assert(sizeof(host_window_entry_t) == 56, "host_window_entry_t layout");
_Static_assert(sizeof(host_window_deferred_t) == 40, "host_window_deferred_t layout");
_Static_assert(sizeof(host_window_map_t) == 6152, "host_window_map_t layout");

#endif
