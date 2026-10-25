/**
 * @file host_window_map_abi.c
 * @brief C consumer checks for the host window mapping C ABI (`src/interop/host_window_map.h`).
 *
 * Built with -fsanitize=address,leak,undefined. Every table is caller-owned stack or static storage;
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

static int check_layout(void) {
    Check(sizeof(host_window_ref_t) == 16);
    Check(sizeof(host_window_entry_t) == 56);
    Check(sizeof(host_window_deferred_t) == 40);
    Check(sizeof(host_window_map_t) == 6152);
    Check(HostWindowMapMaxRealized == 64);
    Check(HostWindowMapMaxDeferred == 64);
    Check(HostWindowMapMaxOutput == 64);
    Check(HostWindowMapRoleImePreedit == 6);
    return 0;
}

static int check_null_and_refusals(host_window_map_t *map) {
    host_window_ref_t out[64];
    host_window_ref_t retired[64];
    host_window_ref_t discarded[64];
    uint32_t count = 77;
    uint32_t retired_count = 77;
    uint32_t discarded_count = 77;

    Check(host_window_map_announce(NULL, ref(1, 1), none(), HostWindowMapRoleToplevel, out, 64, &count) == HostWindowMapRefuseInvalid);
    Check(host_window_map_announce(map, ref(1, 1), none(), HostWindowMapRoleToplevel, NULL, 64, &count) == HostWindowMapRefuseInvalid);
    Check(host_window_map_announce(map, ref(1, 1), none(), HostWindowMapRoleToplevel, out, 64, NULL) == HostWindowMapRefuseInvalid);
    Check(host_window_map_announce(map, ref(1, 1), none(), HostWindowMapRoleToplevel, out, 63, &count) == HostWindowMapRefuseInvalid);
    Check(count == 77);
    Check(host_window_map_announce(map, ref(0, 1), none(), HostWindowMapRoleToplevel, out, 64, &count) == HostWindowMapRefuseInvalid);
    Check(host_window_map_announce(map, ref(1, 1), none(), 0, out, 64, &count) == HostWindowMapRefuseInvalid);
    Check(host_window_map_announce(map, ref(1, 1), none(), 7, out, 64, &count) == HostWindowMapRefuseInvalid);
    Check(host_window_map_announce(map, ref(1, 1), none(), HostWindowMapRoleMenu, out, 64, &count) == HostWindowMapRefuseInvalid);

    Check(host_window_map_retire(NULL, ref(1, 1), retired, 64, &retired_count, discarded, 64, &discarded_count) == HostWindowMapRetireRefuseInvalid);
    Check(host_window_map_retire(map, ref(1, 1), NULL, 64, &retired_count, discarded, 64, &discarded_count) == HostWindowMapRetireRefuseInvalid);
    Check(host_window_map_retire(map, ref(1, 1), retired, 64, NULL, discarded, 64, &discarded_count) == HostWindowMapRetireRefuseInvalid);
    Check(host_window_map_retire(map, ref(1, 1), retired, 64, &retired_count, NULL, 64, &discarded_count) == HostWindowMapRetireRefuseInvalid);
    Check(host_window_map_retire(map, ref(1, 1), retired, 64, &retired_count, discarded, 64, NULL) == HostWindowMapRetireRefuseInvalid);
    Check(host_window_map_retire(map, ref(1, 1), retired, 10, &retired_count, discarded, 64, &discarded_count) == HostWindowMapRetireRefuseInvalid);
    Check(host_window_map_retire(map, ref(1, 1), retired, 64, &retired_count, discarded, 10, &discarded_count) == HostWindowMapRetireRefuseInvalid);
    Check(retired_count == 77 && discarded_count == 77);

    Check(host_window_map_realized_count(NULL) == 0);
    Check(host_window_map_deferred_count(NULL) == 0);
    Check(!host_window_map_is_current(NULL, ref(1, 1)));
    Check(!host_window_map_lookup(NULL, ref(1, 1), NULL));
    return 0;
}

static int check_realize_and_unblock(host_window_map_t *map) {
    host_window_ref_t out[64];
    uint32_t count = 0;

    Check(host_window_map_announce(map, ref(11, 1), ref(20, 1), HostWindowMapRoleDialog, out, 64, &count) == HostWindowMapDefer);
    Check(count == 0);
    Check(host_window_map_announce(map, ref(12, 1), ref(20, 1), HostWindowMapRoleDialog, out, 64, &count) == HostWindowMapDefer);
    Check(host_window_map_announce(map, ref(13, 1), ref(11, 1), HostWindowMapRoleDialog, out, 64, &count) == HostWindowMapDefer);
    Check(host_window_map_deferred_count(map) == 3);
    Check(host_window_map_announce(map, ref(11, 1), ref(20, 1), HostWindowMapRoleDialog, out, 64, &count) == HostWindowMapRefuseDuplicate);
    Check(host_window_map_announce(map, ref(11, 2), ref(20, 1), HostWindowMapRoleDialog, out, 64, &count) == HostWindowMapRefuseWindowConflict);
    Check(count == 0);

    Check(host_window_map_announce(map, ref(20, 1), none(), HostWindowMapRoleToplevel, out, 64, &count) == HostWindowMapRealize);
    Check(count == 4);
    Check(same(out[0], ref(20, 1)));
    Check(same(out[1], ref(11, 1)));
    Check(same(out[2], ref(12, 1)));
    Check(same(out[3], ref(13, 1)));
    Check(host_window_map_deferred_count(map) == 0);
    Check(host_window_map_realized_count(map) == 4);
    return 0;
}

static int check_popups_and_lookup(host_window_map_t *map) {
    host_window_ref_t out[64];
    uint32_t count = 0;
    host_window_entry_t entry;

    Check(host_window_map_announce(map, ref(30, 1), ref(20, 1), HostWindowMapRoleMenu, out, 64, &count) == HostWindowMapRealize);
    Check(count == 1);
    Check(host_window_map_announce(map, ref(31, 1), ref(30, 1), HostWindowMapRoleTooltip, out, 64, &count) == HostWindowMapRealize);
    Check(host_window_map_announce(map, ref(32, 1), ref(31, 1), HostWindowMapRoleImePreedit, out, 64, &count) == HostWindowMapRealize);

    Check(host_window_map_lookup(map, ref(32, 1), &entry));
    Check(same(entry.parent, ref(31, 1)));
    Check(same(entry.root, ref(20, 1)));
    Check(entry.role == HostWindowMapRoleImePreedit);
    Check(host_window_map_lookup(map, ref(20, 1), NULL));
    Check(!host_window_map_lookup(map, ref(20, 2), &entry));
    Check(host_window_map_is_current(map, ref(31, 1)));
    Check(!host_window_map_is_current(map, ref(31, 2)));
    return 0;
}

static int check_retire_cascade(host_window_map_t *map) {
    host_window_ref_t out[64];
    host_window_ref_t retired[64];
    host_window_ref_t discarded[64];
    uint32_t count = 0;
    uint32_t retired_count = 0;
    uint32_t discarded_count = 0;

    Check(host_window_map_retire(map, ref(20, 9), retired, 64, &retired_count, discarded, 64, &discarded_count) == HostWindowMapStale);
    Check(retired_count == 0 && discarded_count == 0);
    Check(host_window_map_retire(map, ref(20, 1), retired, 64, &retired_count, discarded, 64, &discarded_count) == HostWindowMapRetired);
    /* Realized order 20, 11, 12, 13, 30, 31, 32: reverse realization order, target last. */
    Check(retired_count == 7);
    Check(discarded_count == 0);
    Check(same(retired[0], ref(32, 1)));
    Check(same(retired[1], ref(31, 1)));
    Check(same(retired[2], ref(30, 1)));
    Check(same(retired[3], ref(13, 1)));
    Check(same(retired[6], ref(20, 1)));
    Check(host_window_map_realized_count(map) == 0);

    Check(host_window_map_announce(map, ref(40, 1), none(), HostWindowMapRoleToplevel, out, 64, &count) == HostWindowMapRealize);
    Check(count == 1);
    return 0;
}

static int check_capacity_edges(host_window_map_t *map) {
    host_window_ref_t out[64];
    uint32_t count = 0;
    uint64_t index;

    host_window_map_reset(map);
    for (index = 1; index <= HostWindowMapMaxRealized; index++) {
        Check(host_window_map_announce(map, ref(index, 1), none(), HostWindowMapRoleToplevel, out, 64, &count) == HostWindowMapRealize);
    }
    Check(host_window_map_realized_count(map) == 64);
    Check(host_window_map_announce(map, ref(100, 1), none(), HostWindowMapRoleToplevel, out, 64, &count) == HostWindowMapRefuseExhausted);
    Check(count == 0);
    Check(host_window_map_realized_count(map) == 64);
    return 0;
}

static int check_aliasing(host_window_map_t *map) {
    host_window_ref_t *alias = (host_window_ref_t *)(void *)map;
    uint32_t *inside = &map->realized_count;
    uint32_t count = 5;
    host_window_map_reset(map);
    Check(host_window_map_announce(map, ref(1, 1), none(), HostWindowMapRoleToplevel, alias, 64, &count) == HostWindowMapRefuseInvalid);
    Check(host_window_map_announce(map, ref(1, 1), none(), HostWindowMapRoleToplevel, alias, 64, inside) == HostWindowMapRefuseInvalid);
    Check(host_window_map_realized_count(map) == 0);
    Check(count == 5);
    return 0;
}

int main(void) {
    static host_window_map_t map;
    if (check_layout() != 0) return 1;
    host_window_map_reset(&map);
    if (check_null_and_refusals(&map) != 0) return 1;
    if (check_realize_and_unblock(&map) != 0) return 1;
    if (check_popups_and_lookup(&map) != 0) return 1;
    if (check_retire_cascade(&map) != 0) return 1;
    if (check_capacity_edges(&map) != 0) return 1;
    if (check_aliasing(&map) != 0) return 1;
    host_window_map_reset(&map);
    Check(host_window_map_realized_count(&map) == 0);
    Check(host_window_map_deferred_count(&map) == 0);
    puts("host_window_map_abi: all checks passed");
    return 0;
}
