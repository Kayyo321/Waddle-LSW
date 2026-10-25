/**
 * @file host_window_map_abi.c
 * @brief C consumer checks for the host window mapping C ABI (`src/interop/host_window_map.h`).
 *
 * Built with -fsanitize=address,leak,undefined. Every table is caller-owned static or stack storage;
 * the consumer never allocates, so any leak report would come from the library and fails the run.
 */
#include "host_window_map.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "host_window_map_abi.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static host_window_ref_t ref(uint64_t window, uint64_t generation) {
    host_window_ref_t value;
    value.window = window;
    value.generation = generation;
    return value;
}

static host_window_ref_t none(void) {
    return ref(0, 0);
}

static int same(host_window_ref_t left, host_window_ref_t right) {
    return left.window == right.window && left.generation == right.generation;
}

static void clear_table(host_window_map_t *map) {
    host_window_ref_t list[64];
    uint32_t count = 0;
    (void)host_window_map_reset(map, list, 64, &count);
}

static host_window_map_announce_status_t announce(host_window_map_t *map, host_window_ref_t window, host_window_ref_t parent, uint8_t role) {
    return host_window_map_announce(map, window, parent, role).status;
}

static int check_layout(void) {
    Check(sizeof(host_window_ref_t) == 16);
    Check(sizeof(host_window_slot_t) == 56);
    Check(sizeof(host_window_entry_t) == 56);
    Check(sizeof(host_window_map_t) == 3600);
    Check(HostWindowMapMaxRealized == 64);
    Check(HostWindowMapMaxOutput == 64);
    Check(HostWindowMapNoSlot == 64);
    Check(HostWindowMapRoleImePreedit == 6);
    return 0;
}

static int check_error_codes(void) {
    Check(host_window_map_announce_error_code(HostWindowMapRealize) == InteropErrorNone);
    Check(host_window_map_announce_error_code(HostWindowMapRefuseDuplicate) == InteropErrorProtocolViolation);
    Check(host_window_map_announce_error_code(HostWindowMapRefuseStaleParent) == InteropErrorStaleWindow);
    Check(host_window_map_announce_error_code(HostWindowMapRefuseWindowConflict) == InteropErrorStaleWindow);
    Check(host_window_map_announce_error_code(HostWindowMapRefuseExhausted) == InteropErrorQuotaExceeded);
    Check(host_window_map_announce_error_code(HostWindowMapRefuseInvalid) == InteropErrorMalformedFrame);
    Check(host_window_map_retire_error_code(HostWindowMapRetired) == InteropErrorNone);
    Check(host_window_map_retire_error_code(HostWindowMapStale) == InteropErrorStaleWindow);
    Check(host_window_map_retire_error_code(HostWindowMapRetireRefuseInvalid) == InteropErrorMalformedFrame);
    return 0;
}

static int check_null_and_refusals(host_window_map_t *map) {
    host_window_ref_t out[64];
    uint32_t count = 77;
    host_window_map_announce_result_t result;

    Check(host_window_map_reset(NULL, out, 64, &count) == HostWindowMapResetRefuseInvalid);
    Check(host_window_map_reset(map, NULL, 64, &count) == HostWindowMapResetRefuseInvalid);
    Check(host_window_map_reset(map, out, 64, NULL) == HostWindowMapResetRefuseInvalid);
    Check(host_window_map_reset(map, out, 63, &count) == HostWindowMapResetRefuseInvalid);
    Check(count == 77);

    result = host_window_map_announce(NULL, ref(1, 1), none(), HostWindowMapRoleToplevel);
    Check(result.status == HostWindowMapRefuseInvalid && result.slot == HostWindowMapNoSlot);
    Check(announce(map, ref(0, 1), none(), HostWindowMapRoleToplevel) == HostWindowMapRefuseInvalid);
    Check(announce(map, ref(1, 1), none(), 0) == HostWindowMapRefuseInvalid);
    Check(announce(map, ref(1, 1), none(), 7) == HostWindowMapRefuseInvalid);
    Check(announce(map, ref(1, 1), none(), HostWindowMapRoleMenu) == HostWindowMapRefuseInvalid);
    Check(host_window_map_realized_count(map) == 0);

    Check(host_window_map_retire(NULL, ref(1, 1), out, 64, &count) == HostWindowMapRetireRefuseInvalid);
    Check(host_window_map_retire(map, ref(1, 1), NULL, 64, &count) == HostWindowMapRetireRefuseInvalid);
    Check(host_window_map_retire(map, ref(1, 1), out, 64, NULL) == HostWindowMapRetireRefuseInvalid);
    Check(host_window_map_retire(map, ref(1, 1), out, 10, &count) == HostWindowMapRetireRefuseInvalid);
    Check(count == 77);

    Check(host_window_map_realized_count(NULL) == 0);
    Check(!host_window_map_is_current(NULL, ref(1, 1)));
    Check(!host_window_map_lookup(NULL, ref(1, 1), NULL));
    return 0;
}

static int check_realize_and_stale_parents(host_window_map_t *map) {
    host_window_map_announce_result_t result;

    Check(announce(map, ref(11, 1), ref(20, 1), HostWindowMapRoleDialog) == HostWindowMapRefuseStaleParent);
    Check(host_window_map_realized_count(map) == 0);
    result = host_window_map_announce(map, ref(20, 1), none(), HostWindowMapRoleToplevel);
    Check(result.status == HostWindowMapRealize && result.slot == 0);
    result = host_window_map_announce(map, ref(11, 1), ref(20, 1), HostWindowMapRoleDialog);
    Check(result.status == HostWindowMapRealize && result.slot == 1);
    Check(announce(map, ref(11, 1), ref(20, 1), HostWindowMapRoleDialog) == HostWindowMapRefuseDuplicate);
    Check(announce(map, ref(11, 2), ref(20, 1), HostWindowMapRoleDialog) == HostWindowMapRefuseWindowConflict);
    Check(announce(map, ref(12, 1), ref(20, 2), HostWindowMapRoleDialog) == HostWindowMapRefuseStaleParent);
    Check(host_window_map_realized_count(map) == 2);
    return 0;
}

static int check_popups_and_lookup(host_window_map_t *map) {
    host_window_entry_t entry;

    Check(announce(map, ref(30, 1), ref(20, 1), HostWindowMapRoleMenu) == HostWindowMapRealize);
    Check(announce(map, ref(31, 1), ref(30, 1), HostWindowMapRoleTooltip) == HostWindowMapRealize);
    Check(announce(map, ref(32, 1), ref(31, 1), HostWindowMapRoleImePreedit) == HostWindowMapRealize);

    Check(host_window_map_lookup(map, ref(32, 1), &entry));
    Check(same(entry.parent, ref(31, 1)));
    Check(entry.slot == 4);
    Check(entry.role == HostWindowMapRoleImePreedit);
    Check(host_window_map_lookup(map, ref(20, 1), NULL));
    Check(!host_window_map_lookup(map, ref(20, 2), &entry));
    Check(host_window_map_is_current(map, ref(31, 1)));
    Check(!host_window_map_is_current(map, ref(31, 2)));
    return 0;
}

static int check_retire_cascade(host_window_map_t *map) {
    host_window_ref_t out[64];
    uint32_t count = 0;

    Check(host_window_map_retire(map, ref(20, 9), out, 64, &count) == HostWindowMapStale);
    Check(count == 0);
    Check(host_window_map_retire(map, ref(20, 1), out, 64, &count) == HostWindowMapRetired);
    /* Children first: the popup chain, then the dialog, then the root. */
    Check(count == 5);
    Check(same(out[0], ref(32, 1)));
    Check(same(out[1], ref(31, 1)));
    Check(same(out[2], ref(30, 1)));
    Check(same(out[3], ref(11, 1)));
    Check(same(out[4], ref(20, 1)));
    Check(host_window_map_realized_count(map) == 0);
    Check(host_window_map_retire(map, ref(20, 1), out, 64, &count) == HostWindowMapStale);
    Check(count == 0);
    return 0;
}

static int check_stable_slots(host_window_map_t *map) {
    host_window_ref_t out[64];
    uint32_t count = 0;
    host_window_map_announce_result_t a;
    host_window_map_announce_result_t b;
    host_window_map_announce_result_t c;
    host_window_entry_t entry;

    clear_table(map);
    a = host_window_map_announce(map, ref(1, 1), none(), HostWindowMapRoleToplevel);
    b = host_window_map_announce(map, ref(2, 1), none(), HostWindowMapRoleToplevel);
    c = host_window_map_announce(map, ref(3, 1), none(), HostWindowMapRoleToplevel);
    Check(a.slot == 0 && b.slot == 1 && c.slot == 2);
    Check(host_window_map_retire(map, ref(1, 1), out, 64, &count) == HostWindowMapRetired);
    /* Retiring window 1 does not move windows 2 and 3. */
    Check(host_window_map_lookup(map, ref(2, 1), &entry) && entry.slot == 1);
    Check(host_window_map_lookup(map, ref(3, 1), &entry) && entry.slot == 2);
    /* The freed lowest slot is reused, and the sequence keeps increasing. */
    a = host_window_map_announce(map, ref(4, 1), none(), HostWindowMapRoleToplevel);
    Check(a.status == HostWindowMapRealize && a.slot == 0);
    Check(host_window_map_lookup(map, ref(4, 1), &entry) && entry.realize_seq == 3);
    Check(host_window_map_lookup(map, ref(3, 1), &entry) && entry.realize_seq == 2);
    return 0;
}

static int check_teardown(host_window_map_t *map) {
    host_window_ref_t out[64];
    uint32_t count = 0;

    /* Teardown lists children first and roots newest first; then the table is empty. */
    Check(announce(map, ref(1, 1), none(), HostWindowMapRoleToplevel) == HostWindowMapRealize);
    Check(announce(map, ref(2, 1), ref(1, 1), HostWindowMapRoleDialog) == HostWindowMapRealize);
    Check(announce(map, ref(3, 1), none(), HostWindowMapRoleToplevel) == HostWindowMapRealize);
    Check(host_window_map_reset(map, out, 64, &count) == HostWindowMapReset);
    Check(count == 3);
    Check(same(out[0], ref(3, 1)));
    Check(same(out[1], ref(2, 1)));
    Check(same(out[2], ref(1, 1)));
    Check(host_window_map_realized_count(map) == 0);
    Check(!host_window_map_is_current(map, ref(1, 1)));
    return 0;
}

static int check_capacity_edges(host_window_map_t *map) {
    uint64_t index;
    host_window_map_announce_result_t result;

    clear_table(map);
    for (index = 1; index <= HostWindowMapMaxRealized; index++) {
        result = host_window_map_announce(map, ref(index, 1), none(), HostWindowMapRoleToplevel);
        Check(result.status == HostWindowMapRealize);
    }
    Check(host_window_map_realized_count(map) == 64);
    result = host_window_map_announce(map, ref(100, 1), none(), HostWindowMapRoleToplevel);
    Check(result.status == HostWindowMapRefuseExhausted && result.slot == HostWindowMapNoSlot);
    Check(host_window_map_realized_count(map) == 64);
    return 0;
}

static int check_user_data(host_window_map_t *map) {
    host_window_map_announce_result_t a;
    host_window_map_announce_result_t b;
    host_window_ref_t out[64];
    uint32_t count = 0;
    uint64_t value = 0;

    clear_table(map);
    a = host_window_map_announce(map, ref(1, 1), none(), HostWindowMapRoleToplevel);
    b = host_window_map_announce(map, ref(2, 1), none(), HostWindowMapRoleToplevel);
    Check(host_window_map_set_user_data(map, a.slot, ref(1, 1), 0xAAAA) == HostWindowMapUserDataOk);
    Check(host_window_map_set_user_data(map, b.slot, ref(2, 1), 0xBBBB) == HostWindowMapUserDataOk);
    Check(host_window_map_get_user_data(map, a.slot, ref(1, 1), &value) == HostWindowMapUserDataOk && value == 0xAAAA);
    /* Stale generation and out-of-range slot are refused without touching the value. */
    Check(host_window_map_set_user_data(map, a.slot, ref(1, 2), 1) == HostWindowMapUserDataStale);
    Check(host_window_map_get_user_data(map, 63, ref(1, 1), &value) == HostWindowMapUserDataStale);
    Check(host_window_map_get_user_data(map, 64, ref(1, 1), &value) == HostWindowMapUserDataRefuseInvalid);
    Check(host_window_map_get_user_data(map, a.slot, ref(0, 1), &value) == HostWindowMapUserDataRefuseInvalid);
    Check(host_window_map_get_user_data(map, a.slot, ref(1, 1), NULL) == HostWindowMapUserDataRefuseInvalid);
    Check(host_window_map_get_user_data(map, a.slot, ref(1, 1), &value) == HostWindowMapUserDataOk && value == 0xAAAA);
    /* Retiring window 1 leaves window 2 alone; the freed slot is reused with zero user data. */
    Check(host_window_map_retire(map, ref(1, 1), out, 64, &count) == HostWindowMapRetired);
    Check(host_window_map_get_user_data(map, b.slot, ref(2, 1), &value) == HostWindowMapUserDataOk && value == 0xBBBB);
    a = host_window_map_announce(map, ref(3, 1), none(), HostWindowMapRoleToplevel);
    Check(a.slot == 0);
    Check(host_window_map_get_user_data(map, a.slot, ref(3, 1), &value) == HostWindowMapUserDataOk && value == 0);
    Check(host_window_map_lookup(map, ref(3, 1), NULL));
    clear_table(map);
    return 0;
}

static int check_dismissal(host_window_map_t *map) {
    host_window_entry_t entry;

    clear_table(map);
    Check(announce(map, ref(1, 1), none(), HostWindowMapRoleToplevel) == HostWindowMapRealize);
    Check(announce(map, ref(2, 1), ref(1, 1), HostWindowMapRoleMenu) == HostWindowMapRealize);
    Check(host_window_map_mark_dismissed(NULL, ref(2, 1)) == HostWindowMapDismissRefuseInvalid);
    Check(host_window_map_mark_dismissed(map, ref(1, 1)) == HostWindowMapDismissRefuseInvalid);
    Check(host_window_map_mark_dismissed(map, ref(2, 2)) == HostWindowMapDismissStale);
    Check(host_window_map_mark_dismissed(map, ref(2, 1)) == HostWindowMapDismissed);
    Check(host_window_map_lookup(map, ref(2, 1), &entry) && entry.dismissed == 1);
    Check(host_window_map_realized_count(map) == 2);
    clear_table(map);
    return 0;
}

static int check_aliasing(host_window_map_t *map) {
    host_window_ref_t *alias = (host_window_ref_t *)(void *)map;
    uint32_t *inside = &map->realized_count;
    uint32_t count = 5;
    host_window_ref_t out[64];

    clear_table(map);
    Check(announce(map, ref(1, 1), none(), HostWindowMapRoleToplevel) == HostWindowMapRealize);
    Check(host_window_map_retire(map, ref(1, 1), alias, 64, &count) == HostWindowMapRetireRefuseInvalid);
    Check(host_window_map_retire(map, ref(1, 1), out, 64, inside) == HostWindowMapRetireRefuseInvalid);
    Check(host_window_map_realized_count(map) == 1);
    Check(count == 5);
    clear_table(map);
    return 0;
}

int main(void) {
    static host_window_map_t map;
    if (check_layout() != 0) return 1;
    if (check_error_codes() != 0) return 1;
    if (check_null_and_refusals(&map) != 0) return 1;
    if (check_realize_and_stale_parents(&map) != 0) return 1;
    if (check_popups_and_lookup(&map) != 0) return 1;
    if (check_retire_cascade(&map) != 0) return 1;
    if (check_teardown(&map) != 0) return 1;
    if (check_stable_slots(&map) != 0) return 1;
    if (check_user_data(&map) != 0) return 1;
    if (check_dismissal(&map) != 0) return 1;
    if (check_capacity_edges(&map) != 0) return 1;
    if (check_aliasing(&map) != 0) return 1;
    clear_table(&map);
    Check(host_window_map_realized_count(&map) == 0);
    puts("host_window_map_abi: all checks passed");
    return 0;
}
