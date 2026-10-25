/**
 * @file host_wayland_core_test.c
 * @brief Tests of host_wayland_core.c: connect, globals, the xdg_wm_base ping, seats, the poll loop, failure modes and
 * teardown order. The module source is included directly so its static helpers are reachable, and the recording
 * double (host_wayland_core_fake.h) replaces libwayland through `-Wl,--wrap`.
 */
#include "../../src/interop/host_wayland_core.c"

#include "host_wayland_core_fake.h"

#include <stdio.h>

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "host_wayland_core_test.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/** @brief Compare the recorded log with the expected sequence; on mismatch the test returns 1 after printing both. */
#define CheckLog(...) do { \
    static const char *const expected_log[] = { __VA_ARGS__ }; \
    if (!log_matches(expected_log, sizeof expected_log / sizeof expected_log[0])) { \
        dump_log(); \
        fprintf(stderr, "host_wayland_core_test.c:%d: log does not match\n", __LINE__); \
        return 1; \
    } \
} while (0)

/** @brief The core storage every test uses. The listener data pointer is this address, so it is never moved. */
static host_wayland_core_t core_storage;

/** @brief Token passed as the seat listener user pointer; the listener checks it on every call. */
static int seat_user_token;
static unsigned seat_user_mismatches;
static struct wl_seat *seen_seat[16];
static bool seen_added[16];
static unsigned seen_count;

/** @brief Zero the core storage and the seat-listener recording for a fresh test. */
static void reset_core(void) {
    memset(&core_storage, 0, sizeof core_storage);
    seat_user_mismatches = 0;
    seen_count = 0;
}

static void dump_log(void) {
    for (unsigned index = 0; index < fake_log_count; ++index) fprintf(stderr, "  log[%u] = %s\n", index, fake_log[index]);
}

static int log_matches(const char *const *expected, unsigned count) {
    if (fake_log_count != count) return 0;
    for (unsigned index = 0; index < count; ++index) {
        if (strcmp(fake_log[index], expected[index]) != 0) return 0;
    }
    return 1;
}

/** @brief Seat listener: records the call and appends "seat+" (added) or "seat-" (removed) to the log. */
static void seat_listener_fn(void *user, struct wl_seat *seat, bool added) {
    if (user != (void *)&seat_user_token) ++seat_user_mismatches;
    if (seen_count < 16) {
        seen_seat[seen_count] = seat;
        seen_added[seen_count] = added;
        ++seen_count;
    }
    fake_log_push(added ? "seat+" : "seat-");
}

/** @brief Queue the four globals a session needs: compositor v6, xdg_wm_base v5, shm v2, decoration v1. */
static void queue_standard_globals(void) {
    queue_announce(1, "wl_compositor", 6);
    queue_announce(2, "xdg_wm_base", 5);
    queue_announce(3, "wl_shm", 2);
    queue_announce(4, "zxdg_decoration_manager_v1", 1);
}

/** @brief Initialize the core on an injected display. Returns the init status. */
static host_wayland_core_status_t start_injected(void) {
    host_wayland_core_config_t config;
    memset(&config, 0, sizeof config);
    config.display = fake_make_display();
    return host_wayland_core_init(&core_storage, &config);
}

/** @brief Fresh double, reset core, standard globals queued, core started. Returns 0 when started. */
static int start_standard(void) {
    fake_begin();
    reset_core();
    queue_standard_globals();
    return start_injected() == HostWaylandCoreOk ? 0 : 1;
}

/** @brief Invoke the xdg_wm_base listener the core registered, as the compositor would deliver a ping. */
static void invoke_ping(uint32_t serial) {
    unsigned index = fake_live_nth("xdg_wm_base");
    const struct xdg_wm_base_listener *listener = fake_proxies[index].listener;
    listener->ping(fake_proxies[index].data, (struct xdg_wm_base *)fake_object(index), serial);
}

/** @brief Serial of the nth recorded xdg_wm_base.pong request, or -1 when there are fewer than nth + 1. */
static long long pong_serial(unsigned nth) {
    unsigned seen = 0;
    for (unsigned index = 0; index < fake_request_count; ++index) {
        const fake_request_t *request = &fake_requests[index];
        if (!strcmp(request->iface, "xdg_wm_base") && request->opcode == XDG_WM_BASE_PONG) {
            if (seen == nth) return request->a0;
            ++seen;
        }
    }
    return -1;
}

/* ---- init and globals -------------------------------------------------------------------------- */

static int test_init_binds_all_globals(void) {
    fake_begin();
    reset_core();
    queue_announce(1, "wl_compositor", 6);
    queue_announce(2, "xdg_wm_base", 3);
    queue_announce(3, "wl_shm", 2);
    queue_announce(4, "zxdg_decoration_manager_v1", 1);
    queue_announce(5, "wl_seat", 9);
    queue_announce(6, "wl_seat", 4);
    queue_announce(7, "wl_output", 3);
    queue_announce(8, "wl_compositor", 6);
    queue_announce(9, "xdg_wm_base", 5);
    queue_announce(10, "wl_shm", 2);
    queue_announce(11, "zxdg_decoration_manager_v1", 1);
    host_wayland_core_config_t config;
    memset(&config, 0, sizeof config);
    config.display = fake_make_display();
    Check(host_wayland_core_init(&core_storage, &config) == HostWaylandCoreOk);

    Check(core_storage.initialized == 1);
    Check(core_storage.failed == 0);
    Check(core_storage.owns_display == 0);
    Check(core_storage.output_pending == 0);
    Check(core_storage.display == config.display);
    const host_wayland_core_globals_t *globals = host_wayland_core_globals(&core_storage);
    Check(globals == &core_storage.globals);
    Check(globals->compositor != NULL);
    Check(fake_version_of(globals->compositor) == 4);
    Check(globals->wm_base != NULL);
    Check(globals->wm_base_version == 3);
    Check(fake_version_of(globals->wm_base) == 3);
    Check(globals->shm != NULL);
    Check(fake_version_of(globals->shm) == 1);
    Check(globals->decoration_manager != NULL);
    Check(fake_version_of(globals->decoration_manager) == 1);

    /* The second compositor, wm_base, shm and decoration are announced but not bound again. */
    Check(fake_count_binds("wl_compositor") == 1);
    Check(fake_count_binds("xdg_wm_base") == 1);
    Check(fake_count_binds("wl_shm") == 1);
    Check(fake_count_binds("zxdg_decoration_manager_v1") == 1);
    Check(fake_count_binds("wl_seat") == 2);
    Check(fake_count_binds("wl_output") == 0);
    Check(fake_count_requests("wl_registry", WL_REGISTRY_BIND) == 6);

    Check(host_wayland_core_seat_count(&core_storage) == 2);
    Check(host_wayland_core_seat_at(&core_storage, 0) != NULL);
    Check(host_wayland_core_seat_at(&core_storage, 1) != NULL);
    Check(host_wayland_core_seat_at(&core_storage, 0) != host_wayland_core_seat_at(&core_storage, 1));
    Check(host_wayland_core_seat_at(&core_storage, 2) == NULL);
    Check(host_wayland_core_seat_at(&core_storage, HostWaylandCoreMaxSeats) == NULL);
    Check(fake_version_of(host_wayland_core_seat_at(&core_storage, 0)) == 7);
    Check(fake_version_of(host_wayland_core_seat_at(&core_storage, 1)) == 4);

    Check(host_wayland_core_fd(&core_storage) == FakeFd);
    Check(host_wayland_core_poll_events(&core_storage) == POLLIN);

    host_wayland_core_close(&core_storage);
    CheckLog("wl_seat", "wl_seat", "zxdg_decoration_manager_v1", "wl_shm", "xdg_wm_base", "wl_compositor", "wl_registry");
    Check(fake_count_requests("wl_seat", WL_SEAT_RELEASE) == 1);
    Check(fake_disconnects == 0);
    Check(core_storage.initialized == 0);
    Check(host_wayland_core_globals(&core_storage) == NULL);
    return 0;
}

static int test_missing_wm_base(void) {
    fake_begin();
    reset_core();
    queue_announce(1, "wl_compositor", 6);
    queue_announce(3, "wl_shm", 2);
    Check(start_injected() == HostWaylandCoreMissingGlobal);
    Check(core_storage.initialized == 0);
    Check(host_wayland_core_globals(&core_storage) == NULL);
    CheckLog("wl_shm", "wl_compositor", "wl_registry");
    Check(fake_disconnects == 0);
    return 0;
}

static int test_missing_compositor(void) {
    fake_begin();
    reset_core();
    queue_announce(2, "xdg_wm_base", 5);
    queue_announce(3, "wl_shm", 2);
    Check(start_injected() == HostWaylandCoreMissingGlobal);
    Check(core_storage.initialized == 0);
    CheckLog("wl_shm", "xdg_wm_base", "wl_registry");
    return 0;
}

static int test_missing_shm(void) {
    fake_begin();
    reset_core();
    queue_announce(1, "wl_compositor", 6);
    queue_announce(2, "xdg_wm_base", 5);
    Check(start_injected() == HostWaylandCoreMissingGlobal);
    Check(core_storage.initialized == 0);
    CheckLog("xdg_wm_base", "wl_compositor", "wl_registry");
    return 0;
}

static int test_missing_decoration_manager_is_tolerated(void) {
    fake_begin();
    reset_core();
    queue_announce(1, "wl_compositor", 6);
    queue_announce(2, "xdg_wm_base", 5);
    queue_announce(3, "wl_shm", 2);
    Check(start_injected() == HostWaylandCoreOk);
    Check(core_storage.globals.decoration_manager == NULL);
    Check(host_wayland_core_globals(&core_storage)->decoration_manager == NULL);
    Check(host_wayland_core_globals(&core_storage)->wm_base != NULL);
    host_wayland_core_close(&core_storage);
    CheckLog("wl_shm", "xdg_wm_base", "wl_compositor", "wl_registry");
    return 0;
}

static int test_bind_failures(void) {
    static const char *const required[] = { "wl_compositor", "xdg_wm_base", "wl_shm" };
    for (unsigned index = 0; index < sizeof required / sizeof required[0]; ++index) {
        fake_begin();
        reset_core();
        queue_standard_globals();
        fake_fail_create = required[index];
        Check(start_injected() == HostWaylandCoreMissingGlobal);
        Check(core_storage.initialized == 0);
    }

    fake_begin();
    reset_core();
    queue_standard_globals();
    fake_fail_create = "zxdg_decoration_manager_v1";
    Check(start_injected() == HostWaylandCoreOk);
    Check(core_storage.globals.decoration_manager == NULL);
    host_wayland_core_close(&core_storage);

    fake_begin();
    reset_core();
    queue_standard_globals();
    queue_announce(5, "wl_seat", 9);
    fake_fail_create = "wl_seat";
    Check(start_injected() == HostWaylandCoreOk);
    Check(host_wayland_core_seat_count(&core_storage) == 0);
    Check(core_storage.seats_refused == 0);
    host_wayland_core_close(&core_storage);
    return 0;
}

static int test_wm_base_listener_refused(void) {
    fake_begin();
    reset_core();
    queue_standard_globals();
    fake_fail_listener = "xdg_wm_base";
    Check(start_injected() == HostWaylandCoreMissingGlobal);
    Check(core_storage.initialized == 0);
    CheckLog("xdg_wm_base", "zxdg_decoration_manager_v1", "wl_shm", "wl_compositor", "wl_registry");
    return 0;
}

static int test_registry_listener_refused(void) {
    fake_begin();
    reset_core();
    fake_fail_listener = "wl_registry";
    Check(start_injected() == HostWaylandCoreRoundtripFailed);
    Check(core_storage.initialized == 0);
    CheckLog("wl_registry");
    return 0;
}

static int test_registry_creation_failed(void) {
    fake_begin();
    reset_core();
    fake_fail_create = "wl_registry";
    Check(start_injected() == HostWaylandCoreRoundtripFailed);
    Check(core_storage.initialized == 0);
    Check(fake_log_count == 0);
    return 0;
}

static int test_startup_roundtrip_failure(void) {
    fake_begin();
    reset_core();
    queue_standard_globals();
    fake_roundtrip_mode = FakeEio;
    Check(start_injected() == HostWaylandCoreRoundtripFailed);
    Check(core_storage.initialized == 0);
    CheckLog("wl_registry");
    Check(fake_roundtrips == 1);

    fake_begin();
    reset_core();
    queue_standard_globals();
    fake_roundtrip_mode = FakeFault;
    Check(start_injected() == HostWaylandCoreRoundtripFailed);
    Check(core_storage.initialized == 0);
    CheckLog("wl_registry");
    return 0;
}

static int test_connect_owned_display(void) {
    fake_begin();
    reset_core();
    queue_standard_globals();
    host_wayland_core_config_t config;
    memset(&config, 0, sizeof config);
    config.display_name = "wayland-test";
    Check(host_wayland_core_init(&core_storage, &config) == HostWaylandCoreOk);
    Check(fake_connects == 1);
    Check(!strcmp(fake_connect_name, "wayland-test"));
    Check(core_storage.owns_display == 1);
    host_wayland_core_close(&core_storage);
    CheckLog("zxdg_decoration_manager_v1", "wl_shm", "xdg_wm_base", "wl_compositor", "wl_registry", "wl_display_disconnect");
    Check(fake_disconnects == 1);

    fake_begin();
    reset_core();
    queue_standard_globals();
    memset(&config, 0, sizeof config);
    Check(host_wayland_core_init(&core_storage, &config) == HostWaylandCoreOk);
    Check(fake_connects == 1);
    Check(fake_connect_name == NULL);
    host_wayland_core_close(&core_storage);
    Check(fake_disconnects == 1);
    return 0;
}

static int test_connect_failed_leaves_core_unchanged(void) {
    fake_begin();
    reset_core();
    fake_connect_fail = 1;
    host_wayland_core_t before;
    memcpy(&before, &core_storage, sizeof before);
    host_wayland_core_config_t config;
    memset(&config, 0, sizeof config);
    config.display_name = "wayland-missing";
    Check(host_wayland_core_init(&core_storage, &config) == HostWaylandCoreConnectFailed);
    Check(memcmp(&before, &core_storage, sizeof before) == 0);
    Check(fake_connects == 1);
    Check(fake_disconnects == 0);
    return 0;
}

static int test_invalid_init_arguments(void) {
    fake_begin();
    reset_core();
    host_wayland_core_config_t config;
    memset(&config, 0, sizeof config);
    config.display = fake_make_display();
    Check(host_wayland_core_init(NULL, &config) == HostWaylandCoreInvalidArgument);
    Check(host_wayland_core_init(&core_storage, NULL) == HostWaylandCoreInvalidArgument);
    Check(core_storage.initialized == 0);
    Check(fake_connects == 0);

    fake_begin();
    reset_core();
    queue_standard_globals();
    Check(start_injected() == HostWaylandCoreOk);
    Check(start_injected() == HostWaylandCoreInvalidArgument);
    Check(core_storage.initialized == 1);
    Check(host_wayland_core_fd(&core_storage) == FakeFd);
    host_wayland_core_close(&core_storage);
    return 0;
}

/* ---- ping ------------------------------------------------------------------------------------- */

static int test_ping_answered_in_dispatch_and_by_listener(void) {
    Check(start_standard() == 0);
    unsigned index = fake_live_nth("xdg_wm_base");
    Check(fake_proxies[index].data == &core_storage);

    queue_ping(41);
    unsigned flushes_before = fake_flushes;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(fake_flushes == flushes_before + 1);
    Check(fake_count_requests("xdg_wm_base", XDG_WM_BASE_PONG) == 1);
    Check(pong_serial(0) == 41);

    invoke_ping(42);
    Check(fake_count_requests("xdg_wm_base", XDG_WM_BASE_PONG) == 2);
    Check(pong_serial(1) == 42);
    host_wayland_core_close(&core_storage);
    return 0;
}

/* ---- seats ------------------------------------------------------------------------------------ */

static int test_seats_recorded_and_reported_through_listener(void) {
    Check(start_standard() == 0);
    Check(host_wayland_core_set_seat_listener(&core_storage, seat_listener_fn, &seat_user_token) == HostWaylandCoreOk);
    queue_announce(5, "wl_seat", 9);
    queue_announce(6, "wl_seat", 9);
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(host_wayland_core_seat_count(&core_storage) == 2);
    Check(seen_count == 2);
    Check(seen_added[0] && seen_added[1]);
    Check(seen_seat[0] == host_wayland_core_seat_at(&core_storage, 0));
    Check(seen_seat[1] == host_wayland_core_seat_at(&core_storage, 1));

    struct wl_seat *removed = host_wayland_core_seat_at(&core_storage, 0);
    queue_remove(5);
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(host_wayland_core_seat_count(&core_storage) == 1);
    Check(host_wayland_core_seat_at(&core_storage, 0) == NULL);
    Check(host_wayland_core_seat_at(&core_storage, 1) != NULL);
    Check(seen_count == 3);
    Check(!seen_added[2] && seen_seat[2] == removed);

    queue_announce(7, "wl_seat", 9);
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(host_wayland_core_seat_at(&core_storage, 0) != NULL);
    Check(host_wayland_core_seat_count(&core_storage) == 2);

    queue_remove(99);
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(host_wayland_core_seat_count(&core_storage) == 2);

    Check(host_wayland_core_set_seat_listener(&core_storage, NULL, NULL) == HostWaylandCoreOk);
    queue_remove(6);
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(host_wayland_core_seat_count(&core_storage) == 1);

    host_wayland_core_close(&core_storage);
    Check(seat_user_mismatches == 0);
    CheckLog("seat+", "seat+", "seat-", "wl_seat", "seat+", "wl_seat", "wl_seat",
             "zxdg_decoration_manager_v1", "wl_shm", "xdg_wm_base", "wl_compositor", "wl_registry");
    return 0;
}

static int test_seats_reported_when_listener_is_set(void) {
    Check(start_standard() == 0);
    queue_announce(5, "wl_seat", 9);
    queue_announce(6, "wl_seat", 9);
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(seen_count == 0);

    Check(host_wayland_core_set_seat_listener(&core_storage, seat_listener_fn, &seat_user_token) == HostWaylandCoreOk);
    Check(seen_count == 2);
    Check(seen_added[0] && seen_added[1]);
    Check(seen_seat[0] == host_wayland_core_seat_at(&core_storage, 0));
    Check(seen_seat[1] == host_wayland_core_seat_at(&core_storage, 1));

    Check(host_wayland_core_set_seat_listener(&core_storage, NULL, NULL) == HostWaylandCoreOk);
    Check(seen_count == 2);
    Check(host_wayland_core_set_seat_listener(&core_storage, seat_listener_fn, &seat_user_token) == HostWaylandCoreOk);
    Check(seen_count == 4);

    host_wayland_core_close(&core_storage);
    CheckLog("seat+", "seat+", "seat+", "seat+",
             "seat-", "wl_seat", "seat-", "wl_seat",
             "zxdg_decoration_manager_v1", "wl_shm", "xdg_wm_base", "wl_compositor", "wl_registry");
    return 0;
}

static int test_seats_announced_during_startup_are_recorded(void) {
    fake_begin();
    reset_core();
    queue_standard_globals();
    queue_announce(5, "wl_seat", 9);
    Check(start_injected() == HostWaylandCoreOk);
    Check(host_wayland_core_seat_count(&core_storage) == 1);
    Check(host_wayland_core_set_seat_listener(&core_storage, seat_listener_fn, &seat_user_token) == HostWaylandCoreOk);
    Check(seen_count == 1 && seen_added[0]);
    host_wayland_core_close(&core_storage);
    return 0;
}

static int test_seat_table_full_refuses_ninth(void) {
    fake_begin();
    reset_core();
    queue_standard_globals();
    for (uint32_t name = 10; name < 19; ++name) queue_announce(name, "wl_seat", 9);
    Check(start_injected() == HostWaylandCoreOk);
    Check(host_wayland_core_seat_count(&core_storage) == HostWaylandCoreMaxSeats);
    Check(core_storage.seats_refused == 1);
    Check(fake_count_binds("wl_seat") == HostWaylandCoreMaxSeats);

    queue_remove(10);
    queue_announce(19, "wl_seat", 9);
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(host_wayland_core_seat_count(&core_storage) == HostWaylandCoreMaxSeats);
    Check(core_storage.seats_refused == 1);

    queue_announce(20, "wl_seat", 9);
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(core_storage.seats_refused == 2);
    /* Eight at startup, plus seat 19 that took the freed slot; seat 20 was refused before any bind. */
    Check(fake_count_binds("wl_seat") == HostWaylandCoreMaxSeats + 1);
    host_wayland_core_close(&core_storage);
    return 0;
}

static int test_seat_release_version_threshold(void) {
    fake_begin();
    reset_core();
    queue_standard_globals();
    queue_announce(21, "wl_seat", 4);
    queue_announce(22, "wl_seat", 5);
    Check(start_injected() == HostWaylandCoreOk);
    Check(fake_version_of(host_wayland_core_seat_at(&core_storage, 0)) == 4);
    Check(fake_version_of(host_wayland_core_seat_at(&core_storage, 1)) == 5);
    host_wayland_core_close(&core_storage);
    Check(fake_count_requests("wl_seat", WL_SEAT_RELEASE) == 1);
    CheckLog("wl_seat", "wl_seat", "zxdg_decoration_manager_v1", "wl_shm", "xdg_wm_base", "wl_compositor", "wl_registry");
    return 0;
}

static int test_global_remove_of_non_seat_is_ignored(void) {
    Check(start_standard() == 0);
    queue_remove(1);
    queue_remove(2);
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(core_storage.globals.compositor != NULL);
    Check(core_storage.globals.wm_base != NULL);
    Check(fake_log_count == 0);
    host_wayland_core_close(&core_storage);
    return 0;
}

static int test_close_order_with_seats_and_listener(void) {
    fake_begin();
    reset_core();
    queue_standard_globals();
    queue_announce(5, "wl_seat", 9);
    queue_announce(6, "wl_seat", 9);
    Check(start_injected() == HostWaylandCoreOk);
    Check(host_wayland_core_set_seat_listener(&core_storage, seat_listener_fn, &seat_user_token) == HostWaylandCoreOk);
    host_wayland_core_close(&core_storage);
    CheckLog("seat+", "seat+",
             "seat-", "wl_seat", "seat-", "wl_seat",
             "zxdg_decoration_manager_v1", "wl_shm", "xdg_wm_base", "wl_compositor", "wl_registry");
    Check(fake_disconnects == 0);
    return 0;
}

/* ---- dispatch, flush and roundtrip ------------------------------------------------------------- */

static int test_fatal_revents_are_sticky(void) {
    static const short fatal[] = { POLLERR, POLLHUP, POLLNVAL };
    for (unsigned index = 0; index < sizeof fatal / sizeof fatal[0]; ++index) {
        Check(start_standard() == 0);
        Check(host_wayland_core_dispatch(&core_storage, fatal[index]) == HostWaylandCoreDisplayError);
        Check(core_storage.failed == 1);
        Check(host_wayland_core_poll_events(&core_storage) == 0);
        Check(host_wayland_core_fd(&core_storage) == -1);
        Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreDisplayError);
        Check(host_wayland_core_flush(&core_storage) == HostWaylandCoreDisplayError);
        Check(host_wayland_core_roundtrip(&core_storage) == HostWaylandCoreDisplayError);
        Check(host_wayland_core_dispatch(&core_storage, POLLIN | POLLERR) == HostWaylandCoreDisplayError);
        host_wayland_core_close(&core_storage);
        Check(core_storage.initialized == 0);
    }
    return 0;
}

static int test_read_results(void) {
    Check(start_standard() == 0);
    fake_read_mode = FakeEagain;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(fake_read_calls == 1);
    Check(core_storage.failed == 0);
    Check(host_wayland_core_poll_events(&core_storage) == POLLIN);
    host_wayland_core_close(&core_storage);

    Check(start_standard() == 0);
    fake_read_mode = FakeEintr;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    host_wayland_core_close(&core_storage);

    Check(start_standard() == 0);
    fake_read_mode = FakeEio;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreDisplayError);
    Check(host_wayland_core_dispatch(&core_storage, POLLOUT) == HostWaylandCoreDisplayError);
    host_wayland_core_close(&core_storage);

    Check(start_standard() == 0);
    fake_read_mode = FakeFault;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreDisplayError);
    Check(host_wayland_core_roundtrip(&core_storage) == HostWaylandCoreDisplayError);
    host_wayland_core_close(&core_storage);

    /* A read with EAGAIN but a display error set is fatal: the error wins over the errno. */
    Check(start_standard() == 0);
    fake_read_mode = FakeFaultEagain;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreDisplayError);
    host_wayland_core_close(&core_storage);
    return 0;
}

static int test_dispatch_pending_failures(void) {
    Check(start_standard() == 0);
    fake_dispatch_fail_at = fake_dispatch_calls + 1;
    Check(host_wayland_core_dispatch(&core_storage, 0) == HostWaylandCoreDisplayError);
    host_wayland_core_close(&core_storage);

    Check(start_standard() == 0);
    fake_dispatch_fail_at = fake_dispatch_calls + 2;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreDisplayError);
    Check(core_storage.failed == 1);
    host_wayland_core_close(&core_storage);
    return 0;
}

static int test_prepare_read_retry_and_stall(void) {
    /* Events queued between the first dispatch and the prepare: the prepare fails with EAGAIN, the queue is
     * dispatched, and the retry succeeds. The call is not an error. */
    Check(start_standard() == 0);
    fake_refill = 1;
    unsigned dispatches = fake_dispatch_calls;
    unsigned flushes = fake_flushes;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreOk);
    Check(fake_prepare_failures == 1);
    Check(fake_dispatch_calls == dispatches + 3);
    Check(fake_flushes == flushes + 1);
    host_wayland_core_close(&core_storage);

    /* A prepare that fails with a nonzero errno other than EAGAIN is fatal. */
    Check(start_standard() == 0);
    fake_prepare_errno = EIO;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreDisplayError);
    host_wayland_core_close(&core_storage);

    /* A prepare that keeps failing while nothing is queued makes no progress: fatal. */
    Check(start_standard() == 0);
    fake_prepare_force = 1;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreDisplayError);
    host_wayland_core_close(&core_storage);

    /* POLLOUT alone dispatches and flushes without reading. */
    Check(start_standard() == 0);
    unsigned reads = fake_prepare_reads;
    Check(host_wayland_core_dispatch(&core_storage, POLLOUT) == HostWaylandCoreOk);
    Check(fake_prepare_reads == reads);
    host_wayland_core_close(&core_storage);
    return 0;
}

static int test_flush_would_block_and_pollout(void) {
    Check(start_standard() == 0);
    fake_flush_mode = FakeEagain;
    Check(host_wayland_core_flush(&core_storage) == HostWaylandCoreWouldBlock);
    Check(core_storage.output_pending == 1);
    Check(host_wayland_core_poll_events(&core_storage) == (POLLIN | POLLOUT));
    Check(host_wayland_core_fd(&core_storage) == FakeFd);

    fake_flush_mode = FakeOk;
    Check(host_wayland_core_flush(&core_storage) == HostWaylandCoreOk);
    Check(core_storage.output_pending == 0);
    Check(host_wayland_core_poll_events(&core_storage) == POLLIN);
    host_wayland_core_close(&core_storage);

    /* A dispatch that cannot flush reports WouldBlock, and a POLLOUT dispatch that can flush clears it. */
    Check(start_standard() == 0);
    fake_flush_mode = FakeEagain;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreWouldBlock);
    Check(host_wayland_core_poll_events(&core_storage) == (POLLIN | POLLOUT));
    fake_flush_mode = FakeOk;
    Check(host_wayland_core_dispatch(&core_storage, POLLOUT) == HostWaylandCoreOk);
    Check(host_wayland_core_poll_events(&core_storage) == POLLIN);
    host_wayland_core_close(&core_storage);

    /* A successful roundtrip flushes everything, so it clears the POLLOUT state too. */
    Check(start_standard() == 0);
    fake_flush_mode = FakeEagain;
    Check(host_wayland_core_flush(&core_storage) == HostWaylandCoreWouldBlock);
    fake_flush_mode = FakeOk;
    Check(host_wayland_core_roundtrip(&core_storage) == HostWaylandCoreOk);
    Check(host_wayland_core_poll_events(&core_storage) == POLLIN);
    host_wayland_core_close(&core_storage);
    return 0;
}

static int test_flush_failures_are_fatal(void) {
    static const enum fake_mode_t fatal[] = { FakeEintr, FakeEio, FakeFault, FakeFaultEagain };
    for (unsigned index = 0; index < sizeof fatal / sizeof fatal[0]; ++index) {
        Check(start_standard() == 0);
        fake_flush_mode = fatal[index];
        Check(host_wayland_core_flush(&core_storage) == HostWaylandCoreDisplayError);
        Check(core_storage.failed == 1);
        Check(host_wayland_core_flush(&core_storage) == HostWaylandCoreDisplayError);
        host_wayland_core_close(&core_storage);
    }

    Check(start_standard() == 0);
    fake_flush_mode = FakeEio;
    Check(host_wayland_core_dispatch(&core_storage, POLLIN) == HostWaylandCoreDisplayError);
    host_wayland_core_close(&core_storage);
    return 0;
}

static int test_roundtrip_results(void) {
    Check(start_standard() == 0);
    /* The startup roundtrip is the first call; this explicit one is the second. */
    Check(host_wayland_core_roundtrip(&core_storage) == HostWaylandCoreOk);
    Check(fake_roundtrips == 2);

    fake_roundtrip_mode = FakeEio;
    Check(host_wayland_core_roundtrip(&core_storage) == HostWaylandCoreRoundtripFailed);
    Check(core_storage.failed == 0);
    fake_roundtrip_mode = FakeOk;
    Check(host_wayland_core_roundtrip(&core_storage) == HostWaylandCoreOk);
    host_wayland_core_close(&core_storage);

    Check(start_standard() == 0);
    fake_roundtrip_mode = FakeFault;
    Check(host_wayland_core_roundtrip(&core_storage) == HostWaylandCoreDisplayError);
    Check(core_storage.failed == 1);
    fake_roundtrip_mode = FakeOk;
    Check(host_wayland_core_roundtrip(&core_storage) == HostWaylandCoreDisplayError);
    host_wayland_core_close(&core_storage);
    return 0;
}

/* ---- NULL and uninitialized ------------------------------------------------------------------- */

static int test_null_and_uninitialized_arguments(void) {
    fake_begin();
    reset_core();
    host_wayland_core_t uninitialized;
    memset(&uninitialized, 0, sizeof uninitialized);
    host_wayland_core_config_t config;
    memset(&config, 0, sizeof config);

    host_wayland_core_close(NULL);
    host_wayland_core_close(&uninitialized);
    Check(fake_log_count == 0);
    Check(fake_disconnects == 0);

    Check(host_wayland_core_poll_events(NULL) == 0);
    Check(host_wayland_core_poll_events(&uninitialized) == 0);
    Check(host_wayland_core_fd(NULL) == -1);
    Check(host_wayland_core_fd(&uninitialized) == -1);

    Check(host_wayland_core_dispatch(NULL, POLLIN) == HostWaylandCoreInvalidArgument);
    Check(host_wayland_core_dispatch(&uninitialized, POLLIN) == HostWaylandCoreNotInitialized);
    Check(host_wayland_core_flush(NULL) == HostWaylandCoreInvalidArgument);
    Check(host_wayland_core_flush(&uninitialized) == HostWaylandCoreNotInitialized);
    Check(host_wayland_core_roundtrip(NULL) == HostWaylandCoreInvalidArgument);
    Check(host_wayland_core_roundtrip(&uninitialized) == HostWaylandCoreNotInitialized);
    Check(host_wayland_core_set_seat_listener(NULL, seat_listener_fn, NULL) == HostWaylandCoreInvalidArgument);
    Check(host_wayland_core_set_seat_listener(&uninitialized, seat_listener_fn, NULL) == HostWaylandCoreNotInitialized);

    Check(host_wayland_core_globals(NULL) == NULL);
    Check(host_wayland_core_globals(&uninitialized) == NULL);
    Check(host_wayland_core_seat_count(NULL) == 0);
    Check(host_wayland_core_seat_count(&uninitialized) == 0);
    Check(host_wayland_core_seat_at(NULL, 0) == NULL);
    Check(host_wayland_core_seat_at(&uninitialized, 0) == NULL);

    Check(host_wayland_core_init(NULL, &config) == HostWaylandCoreInvalidArgument);
    Check(host_wayland_core_init(&uninitialized, NULL) == HostWaylandCoreInvalidArgument);
    Check(fake_connects == 0);

    queue_standard_globals();
    Check(start_injected() == HostWaylandCoreOk);
    Check(host_wayland_core_seat_at(&core_storage, HostWaylandCoreMaxSeats) == NULL);
    Check(host_wayland_core_seat_at(&core_storage, 1000u) == NULL);
    Check(host_wayland_core_seat_at(&core_storage, 0) == NULL);
    Check(host_wayland_core_set_seat_listener(&core_storage, NULL, NULL) == HostWaylandCoreOk);
    host_wayland_core_close(&core_storage);
    Check(host_wayland_core_globals(&core_storage) == NULL);
    return 0;
}

/* ---- entry point ------------------------------------------------------------------------------ */

#define RunTest(test) do { \
    if ((test)() != 0) { \
        fprintf(stderr, "host_wayland_core_test: %s failed\n", #test); \
        ++failures; \
    } \
} while (0)

int main(void) {
    unsigned failures = 0;
    RunTest(test_init_binds_all_globals);
    RunTest(test_missing_wm_base);
    RunTest(test_missing_compositor);
    RunTest(test_missing_shm);
    RunTest(test_missing_decoration_manager_is_tolerated);
    RunTest(test_bind_failures);
    RunTest(test_wm_base_listener_refused);
    RunTest(test_registry_listener_refused);
    RunTest(test_registry_creation_failed);
    RunTest(test_startup_roundtrip_failure);
    RunTest(test_connect_owned_display);
    RunTest(test_connect_failed_leaves_core_unchanged);
    RunTest(test_invalid_init_arguments);
    RunTest(test_ping_answered_in_dispatch_and_by_listener);
    RunTest(test_seats_recorded_and_reported_through_listener);
    RunTest(test_seats_reported_when_listener_is_set);
    RunTest(test_seats_announced_during_startup_are_recorded);
    RunTest(test_seat_table_full_refuses_ninth);
    RunTest(test_seat_release_version_threshold);
    RunTest(test_global_remove_of_non_seat_is_ignored);
    RunTest(test_close_order_with_seats_and_listener);
    RunTest(test_fatal_revents_are_sticky);
    RunTest(test_read_results);
    RunTest(test_dispatch_pending_failures);
    RunTest(test_prepare_read_retry_and_stall);
    RunTest(test_flush_would_block_and_pollout);
    RunTest(test_flush_failures_are_fatal);
    RunTest(test_roundtrip_results);
    RunTest(test_null_and_uninitialized_arguments);
    if (fake_protocol_errors != 0 || fake_double_destroys != 0) {
        fprintf(stderr, "host_wayland_core_test: protocol errors %u, double destroys %u\n", fake_protocol_errors, fake_double_destroys);
        ++failures;
    }
    if (failures != 0) return 1;
    puts("host_wayland_core_test: all checks passed");
    return 0;
}
