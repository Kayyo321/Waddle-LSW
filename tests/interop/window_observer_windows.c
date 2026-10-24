/**
 * @file window_observer_windows.c
 * @brief Observer tests driven through the scriptable API double and the REAL window graph (TODO #10).
 *
 * Every test builds a fixture: an api_double_t (window_observer_api_double.h), a real
 * interop_window_graph_t (linked from the Zig C ABI object) and an observer wired to both. The
 * observer's effects are checked through the graph's own event queue (next_event), live lookup,
 * retiring list and counters, and through the observer's counters. Nothing in the graph is stubbed.
 *
 * Contract. Each test is a function that returns 0 on success and nonzero on the first failed
 * check; main runs them in order and stops at the first failure. A failed check prints file, line
 * and expression to stderr. Every test releases everything it allocated on both the success and the
 * failure path (fixture_close), so the file is clean under AddressSanitizer and LeakSanitizer.
 * Process-wide observer slot: only one observer may be Started at a time; fixture_close destroys the
 * observer of the fixture, so a failed test cannot poison the next one.
 *
 * Threading: all observer and graph calls run on the main thread (the graph is bound to it). The
 * only second thread is the helper of api_double_run_on_other_thread, which reports a different fake
 * thread id and is joined before the test continues.
 *
 * Platform: portable C11. On Linux the native Win32 table does not exist, so the "NULL api selects
 * the native table" and Unsupported cases are skipped (the latter is untestable here by design).
 */
#include "window_observer_api_double.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Check that returns 1 from a helper function that itself returns int. */
#define Check(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "window_observer_windows.c:%d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

/** Check inside a test function: records failure in `result` and jumps to the `done` label. */
#define Require(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "window_observer_windows.c:%d: %s\n", __LINE__, #condition); \
            result = 1; \
            goto done; \
        } \
    } while (0)

/** Session epoch of every fixture graph. */
#define TestEpoch UINT64_C(5)
/** Highest representable stamp; UINT64_MAX itself is the observer's "exhausted" sentinel. */
#define StampSentinel UINT64_MAX
/** WinEvent id EVENT_OBJECT_LOCATIONCHANGE (inside the object range). */
#define EventObjectLocationChange 0x800Bu
/** WinEvent id EVENT_OBJECT_REORDER (inside the object range). */
#define EventObjectReorder 0x8004u
/** Capacity of the graph event ring, re-exported under a short test name. */
#define RingCapacity InteropWindowGraphEventCapacity

/* ------------------------------------------------------------------------------------------ */
/* Fixture                                                                                    */
/* ------------------------------------------------------------------------------------------ */

/** @brief Test fixture: heap object owning the double, the graph and (until destroyed) the observer. */
typedef struct fixture_t {
    api_double_t *dbl; /**< Scriptable api double; owned. */
    window_observer_t *observer; /**< Observer under test; owned; NULL once a test destroyed it. */
    interop_window_graph_t *graph; /**< Real graph; owned. */
    uint32_t lookup_calls; /**< Number of app_key_lookup calls. */
} fixture_t;

/** Graph app-key callback: every window gets the key "a-test". */
static int fixture_lookup(void *context, const interop_window_facts_t *facts, uint8_t *key, uint32_t capacity,
                          uint32_t *length) {
    fixture_t *f = (fixture_t *)context;
    (void)facts;
    if (capacity < 6u) {
        return 1;
    }
    f->lookup_calls++;
    memcpy(key, "a-test", 6u);
    *length = 6u;
    return 0;
}

/** Release a fixture completely; safe on NULL and on a partially built fixture. */
static void fixture_close(fixture_t *f) {
    if (f == NULL) {
        return;
    }
    if (f->observer != NULL) {
        (void)window_observer_destroy(&f->observer);
    }
    if (f->graph != NULL) {
        (void)interop_window_graph_destroy(&f->graph);
    }
    api_double_free(f->dbl);
    free(f);
}

/** Build a fixture; optionally start the observer. Returns NULL on any failure (nothing leaks). */
static fixture_t *fixture_open(uint32_t uac_mode, uint64_t first_stamp, int start) {
    fixture_t *f = (fixture_t *)calloc(1u, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    f->dbl = api_double_new();
    if (f->dbl == NULL) {
        fixture_close(f);
        return NULL;
    }
    interop_window_graph_config_t graph_config;
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = TestEpoch;
    graph_config.uac_mode = uac_mode;
    graph_config.app_key_lookup = fixture_lookup;
    graph_config.app_key_context = f;
    if (interop_window_graph_create(&graph_config, &f->graph) != InteropWindowGraphStatusOk) {
        fixture_close(f);
        return NULL;
    }
    window_observer_config_t config;
    api_double_config(&config, f->dbl, uac_mode, first_stamp);
    if (window_observer_create(&config, &f->observer) != WindowObserverStatusOk) {
        fixture_close(f);
        return NULL;
    }
    if (start && window_observer_start(f->observer) != WindowObserverStatusOk) {
        fixture_close(f);
        return NULL;
    }
    return f;
}

/** Create a second, independent observer on the fixture's double (not started). NULL on failure. */
static window_observer_t *make_observer(fixture_t *f, uint64_t first_stamp) {
    window_observer_config_t config;
    window_observer_t *observer = NULL;
    api_double_config(&config, f->dbl, 0u, first_stamp);
    if (window_observer_create(&config, &observer) != WindowObserverStatusOk) {
        return NULL;
    }
    return observer;
}

/** pump with the fixture's graph and epoch; no stats snapshot. */
static window_observer_status_t do_pump(fixture_t *f, uint32_t budget) {
    return window_observer_pump(f->observer, f->graph, TestEpoch, budget, NULL);
}

/** refresh with the fixture's graph and epoch; no stats snapshot. */
static window_observer_status_t do_refresh(fixture_t *f) {
    return window_observer_refresh(f->observer, f->graph, TestEpoch, NULL);
}

/** Read observer stats; returns 0 on success. */
static int obs_stats(fixture_t *f, window_observer_stats_t *stats) {
    Check(window_observer_status(f->observer, stats) == WindowObserverStatusOk);
    return 0;
}

/** Read graph stats; returns 0 on success. */
static int graph_stats(fixture_t *f, interop_window_graph_stats_t *stats) {
    Check(interop_window_graph_get_stats(f->graph, stats) == InteropWindowGraphStatusOk);
    return 0;
}

/** Emulate a well-formed WinEvent for a window. */
static void hint(fixture_t *f, uint32_t event, uint64_t window) {
    api_double_hint(f->dbl, event, window);
}

/** Add a default (visible, foreign, top-level) fake window. */
static api_double_window_t *add_window(fixture_t *f, uint64_t hwnd) {
    return api_double_add(f->dbl, hwnd);
}

/** Add a fake window owned by `owner` (owner and owner_root both name it). */
static api_double_window_t *add_owned(fixture_t *f, uint64_t hwnd, uint64_t owner) {
    api_double_window_t *w = api_double_add(f->dbl, hwnd);
    if (w != NULL) {
        w->facts.owner = owner;
        w->facts.owner_root = owner;
    }
    return w;
}

/** Add a consent.exe "Credential Dialog Xaml Host" window. */
static api_double_window_t *add_consent(fixture_t *f, uint64_t hwnd) {
    api_double_window_t *w = api_double_add(f->dbl, hwnd);
    if (w != NULL) {
        api_double_set_text(w->facts.class_name, InteropWindowGraphMaxClassBytes, &w->facts.class_length,
                            "Credential Dialog Xaml Host");
        api_double_set_text(w->facts.image_name, InteropWindowGraphMaxImageBytes, &w->facts.image_length,
                            "consent.exe");
    }
    return w;
}

/** Mark a fake window hidden or visible in its facts template. */
static void set_visible(api_double_window_t *w, int visible) {
    if (visible) {
        w->facts.flags |= (uint32_t)InteropWindowFactVisible;
    } else {
        w->facts.flags &= ~(uint32_t)InteropWindowFactVisible;
    }
}

/** hint Create + pump(8); both must succeed. Returns 0 on success. */
static int admit(fixture_t *f, uint64_t hwnd) {
    hint(f, WindowObserverEventObjectCreate, hwnd);
    Check(do_pump(f, 8u) == WindowObserverStatusOk);
    return 0;
}

/* ---- graph event log ---- */

/** Events popped by drain_events, in queue order. */
static interop_window_event_t event_log[InteropWindowGraphEventCapacity + 1u];
/** Number of valid entries in event_log. */
static uint32_t event_count = 0u;

/** Pop every queued graph event into event_log; returns the count, or UINT32_MAX on a graph error. */
static uint32_t drain_events(fixture_t *f) {
    event_count = 0u;
    while (event_count < InteropWindowGraphEventCapacity + 1u) {
        const interop_window_graph_status_t status =
            interop_window_graph_next_event(f->graph, TestEpoch, &event_log[event_count]);
        if (status == InteropWindowGraphStatusNone) {
            return event_count;
        }
        if (status != InteropWindowGraphStatusOk) {
            return UINT32_MAX;
        }
        event_count++;
    }
    return UINT32_MAX;
}

/** Index of the first event with kind and window at or after `from`, or -1. */
static int find_event(uint32_t kind, uint64_t window, uint32_t from) {
    for (uint32_t i = from; i < event_count; i++) {
        if (event_log[i].kind == kind && event_log[i].window == window) {
            return (int)i;
        }
    }
    return -1;
}

/** Number of events of one kind in event_log. */
static uint32_t count_kind(uint32_t kind) {
    uint32_t count = 0u;
    for (uint32_t i = 0; i < event_count; i++) {
        if (event_log[i].kind == kind) {
            count++;
        }
    }
    return count;
}

/** 1 when the graph has an Active entry for the window, 0 when it has none, -1 on a graph error. */
static int is_live(fixture_t *f, uint64_t hwnd) {
    interop_window_live_t live;
    const interop_window_graph_status_t status = interop_window_graph_find_live(f->graph, hwnd, &live);
    if (status == InteropWindowGraphStatusOk) {
        return 1;
    }
    return status == InteropWindowGraphStatusNone ? 0 : -1;
}

/** Complete every quarantined entry with a full proof; returns 0 on success. */
static int complete_all_retiring(fixture_t *f) {
    interop_window_retiring_entry_t entries[InteropMaxWindows];
    uint32_t count = 0;
    Check(interop_window_graph_list_retiring(f->graph, entries, InteropMaxWindows, &count) ==
          InteropWindowGraphStatusOk);
    Check(count <= InteropMaxWindows);
    const interop_window_retire_proof_t proof = {1u, 1u};
    for (uint32_t i = 0; i < count; i++) {
        Check(interop_window_graph_complete_retire(f->graph, TestEpoch, entries[i].window, entries[i].generation,
                                                   &proof) == InteropWindowGraphStatusOk);
    }
    return 0;
}

/**
 * Fill the graph's event ring to exactly RingCapacity events with a live window.
 *
 * Precondition: `hwnd` is a visible fake window that the graph holds Active. Drains the queue, then
 * runs RingCapacity / 2 hide/complete_retire/show cycles (one RetireIntent and one Announce each).
 * Postcondition: events_pending == RingCapacity and the window is Active again. Returns 0 on success.
 */
static int fill_event_ring(fixture_t *f, uint64_t hwnd) {
    api_double_window_t *w = api_double_find(f->dbl, hwnd);
    Check(w != NULL);
    Check(drain_events(f) != UINT32_MAX);
    for (uint32_t i = 0; i < RingCapacity / 2u; i++) {
        set_visible(w, 0);
        hint(f, WindowObserverEventObjectHide, hwnd);
        Check(do_pump(f, 4u) == WindowObserverStatusOk);
        Check(complete_all_retiring(f) == 0);
        set_visible(w, 1);
        hint(f, WindowObserverEventObjectShow, hwnd);
        Check(do_pump(f, 4u) == WindowObserverStatusOk);
    }
    interop_window_graph_stats_t gs;
    Check(graph_stats(f, &gs) == 0);
    Check(gs.events_pending == RingCapacity);
    Check(is_live(f, hwnd) == 1);
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Create and lifecycle                                                                       */
/* ------------------------------------------------------------------------------------------ */

/** create returns InvalidArgument and a NULL handle for this config. */
static int expect_create_invalid(const window_observer_config_t *config) {
    window_observer_t *observer = (window_observer_t *)config; /* non-NULL sentinel, never dereferenced */
    Check(window_observer_create(config, &observer) == WindowObserverStatusInvalidArgument);
    Check(observer == NULL);
    return 0;
}

/** create: every bad config field, a NULL slot, a malformed api table, and the success state. */
static int test_create_validation(void) {
    int result = 0;
    api_double_t *dbl = api_double_new();
    window_observer_t *observer = NULL;
    window_observer_config_t config;
    window_observer_stats_t stats;
    Require(dbl != NULL);

    api_double_config(&config, dbl, 0u, 1u);
    Require(window_observer_create(&config, NULL) == WindowObserverStatusInvalidArgument);
    observer = (window_observer_t *)dbl; /* sentinel */
    Require(window_observer_create(NULL, &observer) == WindowObserverStatusInvalidArgument);
    Require(observer == NULL);

    api_double_config(&config, dbl, 0u, 0u); /* first_stamp 0 */
    Require(expect_create_invalid(&config) == 0);
    api_double_config(&config, dbl, 0u, StampSentinel); /* the exhausted sentinel is not a legal first stamp */
    Require(expect_create_invalid(&config) == 0);
    api_double_config(&config, dbl, 0u, 1u);
    config.own_process_id = 0u;
    Require(expect_create_invalid(&config) == 0);
    api_double_config(&config, dbl, 3u, 1u); /* uac_mode above 2 */
    Require(expect_create_invalid(&config) == 0);
    api_double_config(&config, dbl, 0xFFFFFFFFu, 1u);
    Require(expect_create_invalid(&config) == 0);
#ifndef _WIN32
    api_double_config(&config, dbl, 0u, 1u); /* NULL api selects the native table only under _WIN32 */
    config.api = NULL;
    Require(expect_create_invalid(&config) == 0);
#endif
    /* Each of the eight api entries is required. */
    for (uint32_t slot = 0; slot < 8u; slot++) {
        window_observer_api_t table = ApiDoubleTable;
        memset((unsigned char *)&table + (size_t)slot * sizeof(void *), 0, sizeof(void *));
        api_double_config(&config, dbl, 0u, 1u);
        config.api = &table;
        Require(expect_create_invalid(&config) == 0);
    }

    /* Success: Created state, zeroed counters, first stamp kept. */
    api_double_config(&config, dbl, 2u, 77u);
    config.preedit_window = 0x55u;
    Require(window_observer_create(&config, &observer) == WindowObserverStatusOk);
    Require(observer != NULL);
    Require(window_observer_status(observer, &stats) == WindowObserverStatusOk);
    Require(stats.state == WindowObserverStateCreated);
    Require(stats.next_stamp == 77u);
    Require(stats.dirty_pending == 0u && stats.refresh_requested == 0u && stats.overflow_latched == 0u);
    Require(stats.hints_received == 0u && stats.hints_filtered == 0u && stats.keys_processed == 0u);
    Require(stats.last_graph_status == (uint32_t)InteropWindowGraphStatusOk);
    Require(dbl->install_calls == 0u && dbl->query_total == 0u); /* create touches no hook and queries nothing */
    Require(window_observer_status(NULL, &stats) == WindowObserverStatusInvalidArgument);
    Require(window_observer_status(observer, NULL) == WindowObserverStatusInvalidArgument);

    Require(window_observer_destroy(NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_destroy(&observer) == WindowObserverStatusOk);
    Require(observer == NULL);
    Require(window_observer_destroy(&observer) == WindowObserverStatusOk); /* NULL slot content: no-op */
done:
    if (observer != NULL && observer != (window_observer_t *)dbl) {
        (void)window_observer_destroy(&observer);
    }
    api_double_free(dbl);
    return result;
}

/** start, stop, restart: hook ranges and order, removal order, state, stamps and latch. */
static int test_lifecycle_start_stop_restart(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 0);
    window_observer_stats_t stats;
    Require(f != NULL);
    api_double_t *d = f->dbl;

    /* Created: stop is a no-op success, pump/refresh are BadState, queries are inert. */
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(d->remove_calls == 0u);
    Require(do_pump(f, 1u) == WindowObserverStatusBadState);
    Require(do_refresh(f) == WindowObserverStatusBadState);
    Require(window_observer_refresh_due(f->observer, d->monotonic) == 0);
    Require(window_observer_next_timeout_ms(f->observer, d->monotonic) == WindowObserverWaitForever);
    Require(window_observer_set_preedit_window(f->observer, 0x55u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refresh_requested == 0u); /* no latch while not Started */
    hint(f, WindowObserverEventObjectShow, 0x10u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hints_received == 0u && stats.dirty_pending == 0u);

    /* Start: three hooks in order, state, latch, cadence. */
    d->monotonic = 5000u;
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    Require(d->install_calls == 3u);
    Require(d->install_min[0] == WindowObserverEventObjectCreate && d->install_max[0] == WindowObserverEventObjectParentChange);
    Require(d->install_min[1] == WindowObserverEventSystemMinimizeStart && d->install_max[1] == WindowObserverEventSystemMinimizeEnd);
    Require(d->install_min[2] == WindowObserverEventObjectCloaked && d->install_max[2] == WindowObserverEventObjectUncloaked);
    Require(d->install_tokens[0] != 0u && d->install_tokens[1] != 0u && d->install_tokens[2] != 0u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateStarted);
    Require(stats.refresh_requested == 1u);
    Require(window_observer_refresh_due(f->observer, 5000u) == 1);
    Require(window_observer_next_timeout_ms(f->observer, 5000u) == 0u);
    Require(window_observer_start(f->observer) == WindowObserverStatusBadState);
    Require(d->install_calls == 3u);

    hint(f, WindowObserverEventObjectShow, 0x10u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hints_received == 1u && stats.dirty_pending == 1u);
    const uint64_t stamp_after_hint = stats.next_stamp;
    Require(stamp_after_hint == 2u);

    /* Stop: reverse removal order, state, tables cleared, hints ignored afterwards. */
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(d->remove_calls == 3u);
    Require(d->removed_tokens[0] == d->install_tokens[2]);
    Require(d->removed_tokens[1] == d->install_tokens[1]);
    Require(d->removed_tokens[2] == d->install_tokens[0]);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateStopped);
    Require(stats.dirty_pending == 0u && stats.refresh_requested == 0u && stats.overflow_latched == 0u);
    Require(window_observer_refresh_due(f->observer, 9000u) == 0);
    Require(window_observer_next_timeout_ms(f->observer, 9000u) == WindowObserverWaitForever);
    Require(do_pump(f, 1u) == WindowObserverStatusBadState);
    hint(f, WindowObserverEventObjectShow, 0x11u); /* no active observer: ignored */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hints_received == 1u && stats.dirty_pending == 0u);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk); /* second stop: no-op */
    Require(d->remove_calls == 3u);

    /* Restart: hooks again, logical clock keeps counting, table empty, latch set again. */
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    Require(d->install_calls == 6u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateStarted);
    Require(stats.next_stamp == stamp_after_hint);
    Require(stats.dirty_pending == 0u && stats.refresh_requested == 1u);
    Require(window_observer_refresh_due(f->observer, 5000u) == 1);
done:
    fixture_close(f);
    return result;
}

/** A second observer cannot start while another is Started; it can after the first stops. */
static int test_lifecycle_second_observer(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_t *second = NULL;
    window_observer_stats_t stats;
    Require(f != NULL);
    second = make_observer(f, 1000u);
    Require(second != NULL);
    const uint32_t installs = f->dbl->install_calls;

    Require(window_observer_start(second) == WindowObserverStatusAlreadyActive);
    Require(f->dbl->install_calls == installs); /* refused before any hook */
    Require(window_observer_status(second, &stats) == WindowObserverStatusOk);
    Require(stats.state == WindowObserverStateCreated);
    Require(window_observer_stop(second) == WindowObserverStatusOk); /* Created: no-op */

    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(window_observer_start(second) == WindowObserverStatusOk);
    Require(window_observer_start(f->observer) == WindowObserverStatusAlreadyActive);
    /* Hints now reach the second observer only. */
    hint(f, WindowObserverEventObjectShow, 0x20u);
    Require(window_observer_status(second, &stats) == WindowObserverStatusOk);
    Require(stats.hints_received == 1u && stats.dirty_pending == 1u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hints_received == 0u);
    Require(window_observer_destroy(&second) == WindowObserverStatusOk); /* destroy of a Started observer */
    Require(second == NULL);
    Require(window_observer_start(f->observer) == WindowObserverStatusOk); /* slot released by destroy */
done:
    if (second != NULL) {
        (void)window_observer_destroy(&second);
    }
    fixture_close(f);
    return result;
}

/** A failing install at the 1st, 2nd or 3rd hook removes the installed ones in reverse and fails the state. */
static int test_lifecycle_hook_failure_rollback(void) {
    int result = 0;
    for (uint32_t failing = 1u; failing <= 3u; failing++) {
        fixture_t *f = fixture_open(0u, 1u, 0);
        window_observer_t *other = NULL;
        window_observer_stats_t stats;
        Require(f != NULL);
        api_double_t *d = f->dbl;
        d->install_fail_at = failing;
        Require(window_observer_start(f->observer) == WindowObserverStatusHookFailed);
        Require(d->install_calls == failing);
        Require(d->remove_calls == failing - 1u);
        for (uint32_t i = 0; i + 1u < failing; i++) { /* reverse order of installation */
            Require(d->removed_tokens[i] == d->install_tokens[failing - 2u - i]);
        }
        Require(obs_stats(f, &stats) == 0);
        Require(stats.state == WindowObserverStateFailed);
        hint(f, WindowObserverEventObjectShow, 0x30u); /* slot was released: ignored */
        Require(obs_stats(f, &stats) == 0);
        Require(stats.hints_received == 0u);

        d->install_fail_at = 0u;
        Require(window_observer_start(f->observer) == WindowObserverStatusBadState);
        Require(window_observer_stop(f->observer) == WindowObserverStatusBadState);
        Require(do_pump(f, 1u) == WindowObserverStatusBadState);
        Require(window_observer_refresh_due(f->observer, d->monotonic) == 0);
        Require(window_observer_next_timeout_ms(f->observer, d->monotonic) == WindowObserverWaitForever);

        other = make_observer(f, 1u); /* the rollback released the process-wide slot */
        Require(other != NULL);
        Require(window_observer_start(other) == WindowObserverStatusOk);
        Require(window_observer_stop(other) == WindowObserverStatusOk);
        Require(window_observer_destroy(&other) == WindowObserverStatusOk);
        Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk); /* Failed observers can be destroyed */
        Require(f->observer == NULL);
    done:
        fixture_close(f);
        if (result != 0) {
            return result;
        }
    }
    return result;
}

/** A failing remove_hook at any position still attempts all three, fails the state and refuses restart. */
static int test_lifecycle_unhook_failure(void) {
    int result = 0;
    for (uint32_t failing = 1u; failing <= 3u; failing++) {
        fixture_t *f = fixture_open(0u, 1u, 1);
        window_observer_stats_t stats;
        Require(f != NULL);
        f->dbl->remove_fail_at = failing;
        Require(window_observer_stop(f->observer) == WindowObserverStatusUnhookFailed);
        Require(f->dbl->remove_calls == 3u); /* every hook was still attempted */
        Require(obs_stats(f, &stats) == 0);
        Require(stats.state == WindowObserverStateFailed);
        f->dbl->remove_fail_at = 0u;
        Require(window_observer_start(f->observer) == WindowObserverStatusBadState);
        Require(window_observer_stop(f->observer) == WindowObserverStatusBadState);
        Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk);
        Require(f->observer == NULL);
        fixture_close(f);
        f = NULL;

        /* Destroy of a Started observer whose unhook fails: freed, UnhookFailed, slot released. */
        f = fixture_open(0u, 1u, 1);
        Require(f != NULL);
        f->dbl->remove_fail_at = failing;
        Require(window_observer_destroy(&f->observer) == WindowObserverStatusUnhookFailed);
        Require(f->observer == NULL);
        Require(f->dbl->remove_calls == 3u);
        f->observer = make_observer(f, 1u);
        Require(f->observer != NULL);
        Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    done:
        fixture_close(f);
        if (result != 0) {
            return result;
        }
    }
    return result;
}

/** Destroying a Started observer with queued hints removes its hooks and frees everything. */
static int test_lifecycle_destroy_started(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    Require(f != NULL);
    for (uint64_t hwnd = 0x100u; hwnd < 0x110u; hwnd++) {
        hint(f, WindowObserverEventObjectCreate, hwnd);
    }
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 16u);
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk);
    Require(f->observer == NULL);
    Require(f->dbl->remove_calls == 3u);
    hint(f, WindowObserverEventObjectCreate, 0x200u); /* no observer: ignored, must not crash */
    f->observer = make_observer(f, 1u);
    Require(f->observer != NULL);
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 0u && stats.hints_received == 0u);
done:
    fixture_close(f);
    return result;
}

/** Results of every entry point called from a foreign thread. */
typedef struct thread_probe_t {
    fixture_t *f; /**< Fixture under test. */
    window_observer_status_t status; /**< window_observer_status result. */
    window_observer_status_t pump; /**< pump result. */
    window_observer_status_t refresh; /**< refresh result. */
    window_observer_status_t start; /**< start result. */
    window_observer_status_t stop; /**< stop result. */
    window_observer_status_t preedit; /**< set_preedit_window result. */
    window_observer_status_t destroy; /**< destroy result. */
    window_observer_t *slot; /**< Copy of the handle passed to destroy. */
    int due; /**< refresh_due result. */
    uint32_t timeout; /**< next_timeout_ms result. */
} thread_probe_t;

/** Helper-thread body: call every entry point. */
static void thread_probe_run(void *argument) {
    thread_probe_t *p = (thread_probe_t *)argument;
    window_observer_stats_t stats;
    memset(&stats, 0xAB, sizeof(stats));
    p->status = window_observer_status(p->f->observer, &stats);
    p->pump = window_observer_pump(p->f->observer, p->f->graph, TestEpoch, 1u, NULL);
    p->refresh = window_observer_refresh(p->f->observer, p->f->graph, TestEpoch, NULL);
    p->start = window_observer_start(p->f->observer);
    p->stop = window_observer_stop(p->f->observer);
    p->preedit = window_observer_set_preedit_window(p->f->observer, 0x66u);
    p->due = window_observer_refresh_due(p->f->observer, p->f->dbl->monotonic + 100000u);
    p->timeout = window_observer_next_timeout_ms(p->f->observer, p->f->dbl->monotonic + 100000u);
    p->slot = p->f->observer;
    p->destroy = window_observer_destroy(&p->slot);
}

/** Every entry point returns WrongThread (or the scalar default) off the owner thread and touches nothing. */
static int test_lifecycle_wrong_thread(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    thread_probe_t probe;
    window_observer_stats_t before;
    window_observer_stats_t after;
    Require(f != NULL);
    hint(f, WindowObserverEventObjectShow, 0x10u);
    Require(obs_stats(f, &before) == 0);
    memset(&probe, 0, sizeof(probe));
    probe.f = f;
    Require(api_double_run_on_other_thread(f->dbl, 2u, thread_probe_run, &probe) == 0);
    Require(probe.status == WindowObserverStatusWrongThread);
    Require(probe.pump == WindowObserverStatusWrongThread);
    Require(probe.refresh == WindowObserverStatusWrongThread);
    Require(probe.start == WindowObserverStatusWrongThread);
    Require(probe.stop == WindowObserverStatusWrongThread);
    Require(probe.preedit == WindowObserverStatusWrongThread);
    Require(probe.destroy == WindowObserverStatusWrongThread);
    Require(probe.slot == f->observer); /* a wrong-thread destroy frees nothing */
    Require(probe.due == 0);
    Require(probe.timeout == WindowObserverWaitForever);
    Require(obs_stats(f, &after) == 0);
    Require(memcmp(&before, &after, sizeof(before)) == 0); /* nothing changed */

    /* The same through the fake thread id of the double (no second real thread). */
    f->dbl->tid = 9u;
    Require(window_observer_stop(f->observer) == WindowObserverStatusWrongThread);
    Require(window_observer_set_preedit_window(f->observer, 1u) == WindowObserverStatusWrongThread);
    Require(do_pump(f, 1u) == WindowObserverStatusWrongThread);
    f->dbl->tid = ApiDoubleOwnerTid;
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
done:
    if (f != NULL) {
        f->dbl->tid = ApiDoubleOwnerTid;
    }
    fixture_close(f);
    return result;
}

/* ------------------------------------------------------------------------------------------ */
/* Hints and dirty table                                                                      */
/* ------------------------------------------------------------------------------------------ */

/** One row of the hint filter table. */
typedef struct hint_case_t {
    uint32_t event; /**< WinEvent id. */
    int32_t object_id; /**< Accessible object id. */
    int32_t child_id; /**< Accessible child id. */
    uint64_t window; /**< HWND (0 = rejected). */
    int accepted; /**< 1 when the hint must enter the dirty table. */
} hint_case_t;

/** Filter table: accepted ranges and edges first, then every rejection reason. */
static const hint_case_t HintCases[] = {
    {0x8000u, 0, 0, 0x2001u, 1}, /* object create (range low edge) */
    {0x8001u, 0, 0, 0x2002u, 1}, /* destroy */
    {0x8002u, 0, 0, 0x2003u, 1}, /* show */
    {0x8003u, 0, 0, 0x2004u, 1}, /* hide */
    {0x8004u, 0, 0, 0x2005u, 1}, /* reorder */
    {0x800Bu, 0, 0, 0x2006u, 1}, /* location change */
    {0x800Cu, 0, 0, 0x2007u, 1}, /* name change */
    {0x800Eu, 0, 0, 0x2008u, 1}, /* inside the range */
    {0x800Fu, 0, 0, 0x2009u, 1}, /* parent change (range high edge) */
    {0x0016u, 0, 0, 0x200Au, 1}, /* minimize start */
    {0x0017u, 0, 0, 0x200Bu, 1}, /* minimize end */
    {0x8017u, 0, 0, 0x200Cu, 1}, /* cloaked */
    {0x8018u, 0, 0, 0x200Du, 1}, /* uncloaked */
    {0x0000u, 0, 0, 0x2101u, 0}, /* zero */
    {0x0003u, 0, 0, 0x2102u, 0}, /* EVENT_SYSTEM_FOREGROUND */
    {0x0015u, 0, 0, 0x2103u, 0}, /* just below the system range */
    {0x0018u, 0, 0, 0x2104u, 0}, /* just above the system range */
    {0x7FFFu, 0, 0, 0x2105u, 0}, /* just below the object range */
    {0x8010u, 0, 0, 0x2106u, 0}, /* just above the object range */
    {0x8016u, 0, 0, 0x2107u, 0}, /* just below the cloak range */
    {0x8019u, 0, 0, 0x2108u, 0}, /* just above the cloak range */
    {0xFFFFFFFFu, 0, 0, 0x2109u, 0}, /* far outside */
    {0x8002u, -4, 0, 0x210Au, 0}, /* OBJID_CLIENT */
    {0x8002u, 1, 0, 0x210Bu, 0}, /* non-window object id */
    {0x8002u, 0, 1, 0x210Cu, 0}, /* child id */
    {0x8002u, 0, -1, 0x210Du, 0}, /* negative child id */
    {0x8002u, 0, 0, 0u, 0}, /* window handle 0 */
};

/** Hint filter: every accept/reject row changes exactly the counters it must. */
static int test_hint_filter_table(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t before;
    window_observer_stats_t after;
    uint32_t accepted = 0u;
    uint32_t rejected = 0u;
    Require(f != NULL);
    for (size_t i = 0; i < sizeof(HintCases) / sizeof(HintCases[0]); i++) {
        const hint_case_t *c = &HintCases[i];
        Require(obs_stats(f, &before) == 0);
        api_double_hint_full(f->dbl, c->event, c->window, c->object_id, c->child_id, 4242u);
        Require(obs_stats(f, &after) == 0);
        Require(after.hints_wrong_thread == before.hints_wrong_thread);
        Require(after.hints_coalesced == before.hints_coalesced);
        if (c->accepted) {
            accepted++;
            Require(after.hints_received == before.hints_received + 1u);
            Require(after.hints_filtered == before.hints_filtered);
            Require(after.dirty_pending == before.dirty_pending + 1u);
            Require(after.next_stamp == before.next_stamp + 1u); /* an accepted hint draws one stamp */
        } else {
            rejected++;
            Require(after.hints_received == before.hints_received);
            Require(after.hints_filtered == before.hints_filtered + 1u);
            Require(after.dirty_pending == before.dirty_pending);
            Require(after.next_stamp == before.next_stamp); /* a filtered hint draws none */
        }
    }
    Require(accepted == 13u && rejected == 14u);
    Require(obs_stats(f, &after) == 0);
    Require(after.dirty_pending == accepted);
    Require(after.hints_filtered == rejected);
done:
    fixture_close(f);
    return result;
}

/** Work of the hint helper thread. */
typedef struct hint_job_t {
    api_double_t *dbl; /**< Double whose emulator the helper drives. */
} hint_job_t;

/** Helper-thread body: five hints, well-formed and not. */
static void hint_job_run(void *argument) {
    hint_job_t *job = (hint_job_t *)argument;
    api_double_hint(job->dbl, WindowObserverEventObjectShow, 0x41u);
    api_double_hint(job->dbl, WindowObserverEventObjectCreate, 0x42u);
    api_double_hint_full(job->dbl, 0x1234u, 0x43u, 0, 0, 1u); /* outside every range */
    api_double_hint_full(job->dbl, WindowObserverEventObjectShow, 0u, 0, 0, 1u); /* handle 0 */
    api_double_hint_full(job->dbl, WindowObserverEventObjectShow, 0x44u, -4, 0, 1u); /* wrong object id */
}

/** Hints from another thread only bump the atomic wrong-thread counter, whatever they carry. */
static int test_hint_wrong_thread(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    hint_job_t job;
    window_observer_stats_t stats;
    Require(f != NULL);
    hint(f, WindowObserverEventObjectShow, 0x40u);
    job.dbl = f->dbl;
    Require(api_double_run_on_other_thread(f->dbl, 2u, hint_job_run, &job) == 0);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hints_wrong_thread == 5u);
    Require(stats.hints_received == 1u);
    Require(stats.hints_filtered == 0u);
    Require(stats.hints_coalesced == 0u);
    Require(stats.dirty_pending == 1u);
    Require(stats.next_stamp == 2u); /* only the owner-thread hint drew a stamp */
    Require(stats.overflow_latched == 0u);
done:
    fixture_close(f);
    return result;
}

/** Coalescing, first-seen order, budget, and the re-add of a key hinted while it is being queried. */
static int test_dirty_coalesce_order_budget(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    Require(f != NULL);
    for (uint64_t hwnd = 0x101u; hwnd <= 0x103u; hwnd++) {
        Require(add_window(f, hwnd) != NULL);
    }
    hint(f, WindowObserverEventObjectCreate, 0x101u);
    hint(f, WindowObserverEventObjectCreate, 0x102u);
    hint(f, WindowObserverEventObjectCreate, 0x103u);
    hint(f, WindowObserverEventObjectShow, 0x102u);
    hint(f, WindowObserverEventObjectHide, 0x101u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hints_received == 5u && stats.hints_coalesced == 2u && stats.dirty_pending == 3u);
    Require(do_pump(f, 3u) == WindowObserverStatusOk);
    Require(f->dbl->query_total == 3u);
    Require(f->dbl->query_log[0] == 0x101u && f->dbl->query_log[1] == 0x102u && f->dbl->query_log[2] == 0x103u);
    Require(drain_events(f) == 3u);
    Require(event_log[0].window == 0x101u && event_log[1].window == 0x102u && event_log[2].window == 0x103u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.keys_processed == 3u && stats.dirty_pending == 0u);

    /* Budget: five keys, budget 2 twice -> More, More, then the last one -> Ok. */
    for (uint64_t hwnd = 0x201u; hwnd <= 0x205u; hwnd++) {
        Require(add_window(f, hwnd) != NULL);
        hint(f, WindowObserverEventObjectCreate, hwnd);
    }
    Require(do_pump(f, 2u) == WindowObserverStatusMore);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 3u);
    Require(do_pump(f, 2u) == WindowObserverStatusMore);
    Require(do_pump(f, 2u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 0u && stats.keys_processed == 8u);

    /* A hint for the key being queried re-adds it; pump pops only what was present at its start. */
    for (uint64_t hwnd = 0x301u; hwnd <= 0x303u; hwnd++) {
        Require(add_window(f, hwnd) != NULL);
        hint(f, WindowObserverEventObjectCreate, hwnd);
    }
    f->dbl->hint_pending = 1u;
    f->dbl->hint_event = WindowObserverEventObjectShow;
    f->dbl->hint_window = 0x301u;
    Require(do_pump(f, 100u) == WindowObserverStatusMore);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 1u && stats.keys_processed == 11u);
    Require(do_pump(f, 100u) == WindowObserverStatusOk);

    /* Invalid arguments. */
    Require(window_observer_pump(f->observer, f->graph, TestEpoch, 0u, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_pump(f->observer, NULL, TestEpoch, 1u, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_pump(NULL, f->graph, TestEpoch, 1u, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_refresh(f->observer, NULL, TestEpoch, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_refresh(NULL, f->graph, TestEpoch, NULL) == WindowObserverStatusInvalidArgument);
done:
    fixture_close(f);
    return result;
}

/** Table overflow latches at the 513th distinct window and recovers through invalidate_all + refresh. */
static int test_dirty_overflow_recovery(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    uint64_t list[1] = {0x101u};
    Require(f != NULL);
    Require(add_window(f, 0x101u) != NULL);
    api_double_enum_set(f->dbl, list, 1u);
    Require(admit(f, 0x101u) == 0);
    Require(drain_events(f) == 1u);
    Require(event_log[0].generation == 1u);

    for (uint64_t i = 0; i < WindowObserverDirtyCapacity; i++) {
        hint(f, WindowObserverEventObjectShow, 0x5000u + i);
    }
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == WindowObserverDirtyCapacity && stats.overflow_latched == 0u);
    Require(stats.hint_overflows == 0u);
    hint(f, WindowObserverEventObjectShow, 0x6000u); /* the 513th distinct window */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.overflow_latched == 1u && stats.hint_overflows == 1u);
    Require(stats.dirty_pending == WindowObserverDirtyCapacity && stats.hints_received == 513u);
    hint(f, WindowObserverEventObjectHide, 0x5000u); /* an existing window still coalesces */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hints_coalesced == 1u && stats.hint_overflows == 1u && stats.hints_received == 514u);
    Require(window_observer_next_timeout_ms(f->observer, f->dbl->monotonic) == 0u);

    /* Recovery: one pump invalidates the graph, clears the table and runs a full refresh. */
    f->dbl->monotonic += 10u;
    Require(do_pump(f, 1u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.invalidations == 1u && stats.overflow_latched == 0u && stats.dirty_pending == 0u);
    Require(stats.refreshes_started == 1u && stats.refreshes_committed == 1u && stats.refresh_requested == 0u);
    Require(stats.keys_processed == 1u); /* only the admission pump counted keys; recovery popped none */
    Require(drain_events(f) == 2u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].window == 0x101u);
    Require(event_log[0].reason == InteropWindowReasonInvalidated);
    Require(event_log[1].kind == InteropWindowEventAnnounce && event_log[1].window == 0x101u);
    Require(event_log[1].generation == 2u);
    Require(window_observer_refresh_due(f->observer, f->dbl->monotonic) == 0);
    Require(window_observer_next_timeout_ms(f->observer, f->dbl->monotonic) == InteropWindowGraphRefreshIntervalMs);

    /* Recovery blocked by a full graph ring keeps the latch and the table, then succeeds after a drain. */
    Require(complete_all_retiring(f) == 0);
    Require(fill_event_ring(f, 0x101u) == 0);
    for (uint64_t i = 0; i <= WindowObserverDirtyCapacity; i++) {
        hint(f, WindowObserverEventObjectShow, 0x7000u + i);
    }
    Require(obs_stats(f, &stats) == 0);
    Require(stats.overflow_latched == 1u);
    Require(do_pump(f, 1u) == WindowObserverStatusGraphQueueFull);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.overflow_latched == 1u && stats.invalidations == 1u); /* unchanged: nothing was invalidated */
    Require(stats.dirty_pending == WindowObserverDirtyCapacity);
    Require(stats.last_graph_status == (uint32_t)InteropWindowGraphStatusQueueFull);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.active_count == 1u); /* the window is still Active */
    Require(drain_events(f) == RingCapacity);
    Require(do_pump(f, 1u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.invalidations == 2u && stats.overflow_latched == 0u && stats.dirty_pending == 0u);
done:
    fixture_close(f);
    return result;
}

/** The slot reserved for a popped key: a put-back at a full table never overflows; a new window does. */
static int test_dirty_reserved_slot(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    api_double_window_t *first = NULL;
    Require(f != NULL);
    first = add_window(f, 0x5000u);
    Require(first != NULL);
    first->mode = ApiDoubleModeFailed; /* the first key fails and is put back at the tail */
    for (uint64_t i = 0; i < WindowObserverDirtyCapacity; i++) {
        hint(f, WindowObserverEventObjectShow, 0x5000u + i);
    }
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == WindowObserverDirtyCapacity);

    Require(do_pump(f, 1u) == WindowObserverStatusMore);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == WindowObserverDirtyCapacity); /* put back, table full again */
    Require(stats.overflow_latched == 0u && stats.hint_overflows == 0u);
    Require(stats.queries_failed == 1u && stats.keys_processed == 1u);

    /* With 511 entries queued and one key popped, a NEW window finds the table full (511 + 1 reserved). */
    f->dbl->hint_pending = 1u;
    f->dbl->hint_event = WindowObserverEventObjectShow;
    f->dbl->hint_window = 0x9999u;
    Require(do_pump(f, 1u) == WindowObserverStatusMore); /* pops 0x5001 (Gone), hint arrives during its query */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hint_overflows == 1u && stats.overflow_latched == 1u);
    Require(stats.dirty_pending == WindowObserverDirtyCapacity - 1u);

    Require(window_observer_stop(f->observer) == WindowObserverStatusOk); /* teardown with a full table */
done:
    fixture_close(f);
    return result;
}

/* ------------------------------------------------------------------------------------------ */
/* Pump through the real graph                                                                */
/* ------------------------------------------------------------------------------------------ */

/** Admit, update, hide and re-show a window; check the graph events field by field. */
static int test_pump_admit_update_hide(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_live_t live;
    api_double_window_t *w = NULL;
    Require(f != NULL);
    w = add_window(f, 0x100u);
    Require(w != NULL);

    Require(admit(f, 0x100u) == 0);
    Require(drain_events(f) == 1u);
    const interop_window_event_t *e = &event_log[0];
    Require(e->kind == InteropWindowEventAnnounce && e->session_epoch == TestEpoch);
    Require(e->window == 0x100u && e->generation == 1u);
    Require(e->role == InteropRoleToplevel && e->parent_window == 0u && e->parent_generation == 0u);
    Require(e->x == 10 && e->y == 20 && e->width == 800u && e->height == 600u && e->dpi == 96u);
    Require(e->process_id == ApiDoubleForeignPid);
    Require(e->min_width == 0u && e->min_height == 0u);
    Require(e->title_length == 5u && memcmp(e->title, "title", 5u) == 0);
    Require(e->app_key_length == 6u && memcmp(e->app_key, "a-test", 6u) == 0);
    Require(e->changed_mask == InteropWindowChangeMask);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_exists == 1u && stats.keys_processed == 1u && stats.hints_received == 1u);
    Require(stats.last_graph_status == (uint32_t)InteropWindowGraphStatusOk && stats.graph_errors == 0u);
    Require(is_live(f, 0x100u) == 1);

    /* Move: Update with the Rect bit. */
    w->facts.x = 50;
    hint(f, EventObjectLocationChange, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventUpdate && event_log[0].generation == 1u);
    Require((event_log[0].changed_mask & InteropWindowChangeRect) != 0u && event_log[0].x == 50);

    /* Title. */
    api_double_set_text(w->facts.title, InteropWindowGraphMaxTitleBytes, &w->facts.title_length, "new title");
    hint(f, WindowObserverEventObjectNameChange, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventUpdate);
    Require((event_log[0].changed_mask & InteropWindowChangeTitle) != 0u);
    Require(event_log[0].title_length == 9u && memcmp(event_log[0].title, "new title", 9u) == 0);

    /* Minimize start: State bit. */
    w->facts.flags |= (uint32_t)InteropWindowFactIconic;
    hint(f, WindowObserverEventSystemMinimizeStart, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventUpdate);
    Require((event_log[0].changed_mask & InteropWindowChangeState) != 0u);
    Require((event_log[0].state & InteropStateMinimized) != 0u);

    /* An identical re-query changes nothing. */
    hint(f, EventObjectReorder, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 0u);

    /* Hide: RetireIntent with the NotVisible reason; the window is no longer live. */
    set_visible(w, 0);
    hint(f, WindowObserverEventObjectHide, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].generation == 1u);
    Require(event_log[0].reason == InteropWindowReasonNotVisible);
    Require(is_live(f, 0x100u) == 0);
    Require(interop_window_graph_find_live(f->graph, 0x100u, &live) == InteropWindowGraphStatusNone);

    /* Show again: a new generation. */
    set_visible(w, 1);
    w->facts.flags &= ~(uint32_t)InteropWindowFactIconic;
    hint(f, WindowObserverEventObjectShow, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventAnnounce && event_log[0].generation == 2u);
    Require(f->lookup_calls == 2u);
done:
    fixture_close(f);
    return result;
}

/** A cloak clears Visible in the double; the window is retired and re-announced on uncloak. */
static int test_pump_cloak(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    api_double_window_t *w = NULL;
    Require(f != NULL);
    w = add_window(f, 0x100u);
    Require(w != NULL);
    Require(admit(f, 0x100u) == 0);
    Require(drain_events(f) == 1u);

    set_visible(w, 0); /* DWM cloak: the adapter clears Visible */
    hint(f, WindowObserverEventObjectCloaked, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].reason == InteropWindowReasonNotVisible);
    Require(is_live(f, 0x100u) == 0);

    set_visible(w, 1);
    hint(f, WindowObserverEventObjectUncloaked, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventAnnounce && event_log[0].generation == 2u);
    Require(is_live(f, 0x100u) == 1);
done:
    fixture_close(f);
    return result;
}

/** Destroy hints: a stale one (older incarnation) is ignored, a real one applies, HWND reuse re-admits. */
static int test_pump_destroy_and_reuse(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    api_double_window_t *w = NULL;
    Require(f != NULL);
    w = add_window(f, 0x200u);
    Require(w != NULL);
    f->dbl->tick = 5000u;
    Require(admit(f, 0x200u) == 0); /* admitted with captured_ms 5000 */
    Require(drain_events(f) == 1u);

    /* (a) A destroy hint older than the admitting facts describes an earlier incarnation: ignored. */
    api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, 0x200u, 0, 0, 4000u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 0u);
    Require(is_live(f, 0x200u) == 1);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.destroys_noted == 1u && stats.queries_exists == 2u);

    /* (b) A destroy hint after the admitting facts applies; the HWND still exists (reuse): re-admitted. */
    f->dbl->tick = 6000u;
    api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, 0x200u, 0, 0, 6000u);
    hint(f, WindowObserverEventObjectCreate, 0x200u); /* coalesces with the destroy hint */
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 2u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].generation == 1u);
    Require(event_log[0].reason == InteropWindowReasonDestroyed);
    Require(event_log[1].kind == InteropWindowEventAnnounce && event_log[1].generation == 2u);
    Require(is_live(f, 0x200u) == 1);

    /* (c) A real destroy of a window that is gone: retire once; the Gone query adds no second event. */
    f->dbl->tick = 7000u;
    api_double_remove(f->dbl, 0x200u);
    api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, 0x200u, 0, 0, 7000u);
    Require(obs_stats(f, &stats) == 0);
    const uint64_t noted = stats.destroys_noted;
    const uint64_t gone = stats.queries_gone;
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].generation == 2u);
    Require(event_log[0].reason == InteropWindowReasonDestroyed);
    Require(is_live(f, 0x200u) == 0);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.destroys_noted == noted + 2u && stats.queries_gone == gone + 1u);

    /* (d) Equal ticks are ambiguous and count as destroyed (documented churn). */
    w = add_window(f, 0x200u);
    Require(w != NULL);
    f->dbl->tick = 8000u;
    Require(admit(f, 0x200u) == 0);
    Require(drain_events(f) == 1u);
    api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, 0x200u, 0, 0, 8000u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 2u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[1].kind == InteropWindowEventAnnounce);
done:
    fixture_close(f);
    return result;
}

/** Gone retires through note_destroyed; Failed never retires, retries once, then drops the key. */
static int test_pump_gone_vs_failed(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    api_double_window_t *w = NULL;
    Require(f != NULL);
    w = add_window(f, 0x100u);
    Require(w != NULL);
    Require(add_window(f, 0x101u) != NULL);
    Require(admit(f, 0x100u) == 0);
    Require(admit(f, 0x101u) == 0);
    Require(drain_events(f) == 2u);

    /* Failed: first pump retries (More), second drops the key (Ok); nothing retires. */
    w->mode = ApiDoubleModeFailed;
    hint(f, WindowObserverEventObjectShow, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusMore);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_failed == 1u && stats.dirty_pending == 1u && stats.query_retries_exhausted == 0u);
    Require(drain_events(f) == 0u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_failed == 2u && stats.dirty_pending == 0u && stats.query_retries_exhausted == 1u);
    Require(drain_events(f) == 0u);
    Require(is_live(f, 0x100u) == 1);
    Require(stats.destroys_noted == 0u);

    /* A transient failure that clears: retry succeeds with the new facts. */
    w->mode = ApiDoubleModeExists;
    w->fail_count = 1u;
    w->facts.y = 99;
    hint(f, WindowObserverEventObjectShow, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusMore);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventUpdate && event_log[0].y == 99);

    /* A mismatching facts->window counts as Failed. */
    w->report_window = 0x777u;
    hint(f, WindowObserverEventObjectShow, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusMore);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(is_live(f, 0x100u) == 1 && drain_events(f) == 0u);
    w->report_window = 0u;

    /* Gone: note_destroyed with a fresh stamp and tick retires with reason Destroyed. */
    api_double_remove(f->dbl, 0x101u);
    hint(f, WindowObserverEventObjectHide, 0x101u);
    Require(obs_stats(f, &stats) == 0);
    const uint64_t gone = stats.queries_gone;
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].window == 0x101u);
    Require(event_log[0].reason == InteropWindowReasonDestroyed);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_gone == gone + 1u && stats.destroys_noted == 1u);
    Require(is_live(f, 0x101u) == 0 && is_live(f, 0x100u) == 1);
done:
    fixture_close(f);
    return result;
}

/** A full graph ring puts the key back at the tail; a drain lets the next pump finish. */
static int test_pump_queue_full(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    Require(f != NULL);
    Require(add_window(f, 0x300u) != NULL);
    Require(admit(f, 0x300u) == 0);
    Require(fill_event_ring(f, 0x300u) == 0);

    Require(add_window(f, 0x301u) != NULL);
    Require(add_window(f, 0x302u) != NULL);
    hint(f, WindowObserverEventObjectCreate, 0x301u);
    hint(f, WindowObserverEventObjectCreate, 0x302u);
    Require(obs_stats(f, &stats) == 0);
    const uint64_t processed_before = stats.keys_processed;
    Require(do_pump(f, 8u) == WindowObserverStatusGraphQueueFull);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 2u && stats.keys_processed == processed_before + 1u); /* stopped at the first key */
    Require(stats.last_graph_status == (uint32_t)InteropWindowGraphStatusQueueFull && stats.graph_errors == 1u);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.rejected_queue_full >= 1u && gs.events_pending == RingCapacity);
    Require(is_live(f, 0x301u) == 0);

    Require(drain_events(f) == RingCapacity);
    Require(do_pump(f, 8u) == WindowObserverStatusOk);
    Require(drain_events(f) == 2u);
    /* The failed key went to the tail, so the second window is announced first. */
    Require(event_log[0].window == 0x302u && event_log[1].window == 0x301u);
    Require(is_live(f, 0x301u) == 1 && is_live(f, 0x302u) == 1);
done:
    fixture_close(f);
    return result;
}

/** A QueueFull put-back keeps (or correctly clears) the destroy information of the key. */
static int test_pump_queue_full_destroy_information(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    Require(f != NULL);
    Require(add_window(f, 0x400u) != NULL);
    f->dbl->tick = 10000u;
    Require(admit(f, 0x400u) == 0);
    Require(drain_events(f) == 1u);

    /* Each cycle: destroy hint (window recycled, still exists) -> RetireIntent + Announce. */
    window_observer_status_t status = WindowObserverStatusOk;
    uint64_t retire_before = 0u;
    uint64_t noted_before = 0u;
    uint32_t cycles = 0u;
    while (cycles < 300u) {
        Require(graph_stats(f, &gs) == 0);
        retire_before = gs.retire_intents;
        Require(obs_stats(f, &stats) == 0);
        noted_before = stats.destroys_noted;
        api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, 0x400u, 0, 0, 10000u);
        status = do_pump(f, 4u);
        if (status != WindowObserverStatusOk) {
            break;
        }
        Require(complete_all_retiring(f) == 0);
        cycles++;
    }
    Require(status == WindowObserverStatusGraphQueueFull);
    Require(graph_stats(f, &gs) == 0);
    const int destroy_applied = gs.retire_intents > retire_before;
    Require(obs_stats(f, &stats) == 0);
    Require(stats.destroys_noted == noted_before + 1u);
    Require(stats.dirty_pending == 1u);

    Require(drain_events(f) == RingCapacity);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    /* Applied destroys are cleared before the put-back and never repeated; unapplied ones are retried. */
    Require(stats.destroys_noted == noted_before + (destroy_applied ? 1u : 2u));
    const uint32_t events = drain_events(f);
    Require(events == (destroy_applied ? 1u : 2u));
    Require(event_log[events - 1u].kind == InteropWindowEventAnnounce);
    if (!destroy_applied) {
        Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].reason == InteropWindowReasonDestroyed);
    }
    Require(is_live(f, 0x400u) == 1);
done:
    fixture_close(f);
    return result;
}

/** A desktop that is not Default processes nothing, keeps hints, latches a refresh and does not spin. */
static int test_pump_desktop_inactive(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 0);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    Require(f != NULL);
    api_double_t *d = f->dbl;
    d->monotonic = 2000u;
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    Require(add_window(f, 0x100u) != NULL);
    hint(f, WindowObserverEventObjectCreate, 0x100u);

    d->desktop = WindowObserverDesktopOther;
    Require(do_pump(f, 4u) == WindowObserverStatusDesktopInactive);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.pumps_desktop_inactive == 1u && stats.desktop_active == 0u);
    Require(stats.dirty_pending == 1u && stats.keys_processed == 0u && stats.refresh_requested == 1u);
    Require(d->query_total == 0u && d->enum_calls == 0u);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.admitted == 0u && gs.events_pending == 0u && gs.refresh_commits == 0u);

    /* The refusal counts as the refresh attempt: no spinning on the backlog with a frozen clock. */
    Require(window_observer_refresh_due(f->observer, 2000u) == 0);
    Require(window_observer_next_timeout_ms(f->observer, 2000u) == InteropWindowGraphRefreshIntervalMs);
    Require(window_observer_next_timeout_ms(f->observer, 2100u) == InteropWindowGraphRefreshIntervalMs - 100u);
    Require(window_observer_refresh_due(f->observer, 2249u) == 0);
    Require(window_observer_refresh_due(f->observer, 2250u) == 1);
    Require(window_observer_next_timeout_ms(f->observer, 2250u) == 0u);

    /* A refresh refuses the same way, before it enumerates. */
    d->monotonic = 2250u;
    Require(do_refresh(f) == WindowObserverStatusDesktopInactive);
    Require(d->enum_calls == 0u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.pumps_desktop_inactive == 2u && stats.refreshes_started == 0u);
    Require(window_observer_refresh_due(f->observer, 2250u) == 0);

    d->desktop = WindowObserverDesktopDenied; /* Denied is treated like Other */
    Require(do_pump(f, 4u) == WindowObserverStatusDesktopInactive);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.pumps_desktop_inactive == 3u && stats.dirty_pending == 1u);

    d->desktop = WindowObserverDesktopDefault;
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.desktop_active == 1u && stats.dirty_pending == 0u && stats.refresh_requested == 1u);
    Require(drain_events(f) == 1u && event_log[0].kind == InteropWindowEventAnnounce);
    Require(do_refresh(f) == WindowObserverStatusOk); /* the latch clears only on a committed refresh */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refresh_requested == 0u && stats.refreshes_committed == 1u);
done:
    fixture_close(f);
    return result;
}

/** Context of the busy-flag probe fired inside query_facts. */
typedef struct reenter_probe_t {
    fixture_t *f; /**< Fixture under test. */
    window_observer_status_t pump; /**< pump result. */
    window_observer_status_t refresh; /**< refresh result. */
    window_observer_status_t start; /**< start result. */
    window_observer_status_t stop; /**< stop result. */
    window_observer_status_t preedit; /**< set_preedit_window result. */
    window_observer_status_t status; /**< status result. */
    window_observer_status_t destroy; /**< destroy result. */
    window_observer_t *slot; /**< Handle copy passed to destroy. */
    int due; /**< refresh_due result. */
    uint32_t timeout; /**< next_timeout_ms result. */
    uint32_t fired; /**< Number of firings. */
} reenter_probe_t;

/** query_facts hook: call every observer entry point while the outer call is active. */
static void reenter_hook(api_double_t *dbl, uint64_t window, void *argument) {
    reenter_probe_t *p = (reenter_probe_t *)argument;
    window_observer_stats_t stats;
    (void)window;
    p->fired++;
    p->pump = window_observer_pump(p->f->observer, p->f->graph, TestEpoch, 1u, NULL);
    p->refresh = window_observer_refresh(p->f->observer, p->f->graph, TestEpoch, NULL);
    p->start = window_observer_start(p->f->observer);
    p->stop = window_observer_stop(p->f->observer);
    p->preedit = window_observer_set_preedit_window(p->f->observer, 0x77u);
    p->status = window_observer_status(p->f->observer, &stats);
    p->slot = p->f->observer;
    p->destroy = window_observer_destroy(&p->slot);
    p->due = window_observer_refresh_due(p->f->observer, dbl->monotonic + 100000u);
    p->timeout = window_observer_next_timeout_ms(p->f->observer, dbl->monotonic + 100000u);
    api_double_hint(dbl, WindowObserverEventObjectShow, 0x9999u); /* note_hint ignores the busy flag */
}

/** Re-entry from inside query_facts: Reentrant everywhere, nothing freed, note_hint still works. */
static int test_pump_busy_reentrant(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    reenter_probe_t probe;
    window_observer_stats_t stats;
    uint64_t list[1] = {0x100u};
    Require(f != NULL);
    Require(add_window(f, 0x100u) != NULL);
    api_double_enum_set(f->dbl, list, 1u);
    memset(&probe, 0, sizeof(probe));
    probe.f = f;
    f->dbl->query_hook = reenter_hook;
    f->dbl->query_hook_arg = &probe;
    f->dbl->query_hook_once = 1u;

    hint(f, WindowObserverEventObjectCreate, 0x100u);
    Require(do_pump(f, 4u) == WindowObserverStatusMore); /* the hint from inside the query stays queued */
    Require(probe.fired == 1u);
    Require(probe.pump == WindowObserverStatusReentrant && probe.refresh == WindowObserverStatusReentrant);
    Require(probe.start == WindowObserverStatusReentrant && probe.stop == WindowObserverStatusReentrant);
    Require(probe.preedit == WindowObserverStatusReentrant && probe.status == WindowObserverStatusReentrant);
    Require(probe.destroy == WindowObserverStatusReentrant && probe.slot == f->observer);
    Require(probe.due == 0 && probe.timeout == WindowObserverWaitForever);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateStarted);
    Require(stats.dirty_pending == 1u); /* the hint emitted from inside the query was recorded */
    Require(stats.hints_received == 2u);
    Require(drain_events(f) == 1u);

    /* The same during a refresh (the busy flag also covers enumeration and queries). */
    memset(&probe, 0, sizeof(probe));
    probe.f = f;
    f->dbl->query_hook = reenter_hook;
    f->dbl->query_hook_arg = &probe;
    f->dbl->query_hook_once = 1u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(probe.fired == 1u && probe.pump == WindowObserverStatusReentrant);
    Require(probe.refresh == WindowObserverStatusReentrant && probe.destroy == WindowObserverStatusReentrant);
    Require(window_observer_set_preedit_window(f->observer, 0x77u) == WindowObserverStatusOk); /* idle again */
done:
    fixture_close(f);
    return result;
}

/** Context of the drain sink that re-enters the observer while the graph is inside drain. */
typedef struct sink_probe_t {
    fixture_t *f; /**< Fixture under test. */
    window_observer_status_t pump; /**< pump result inside the sink. */
    window_observer_status_t refresh; /**< refresh result inside the sink. */
    uint32_t calls; /**< Sink invocations. */
} sink_probe_t;

/** Drain sink: on its first event call pump and refresh; the graph answers Reentrant to the observer. */
static int reentrant_sink(void *context, const interop_window_event_t *event) {
    sink_probe_t *p = (sink_probe_t *)context;
    (void)event;
    p->calls++;
    if (p->calls == 1u) {
        p->pump = window_observer_pump(p->f->observer, p->f->graph, TestEpoch, 4u, NULL);
        p->refresh = window_observer_refresh(p->f->observer, p->f->graph, TestEpoch, NULL);
    }
    return 0;
}

/** A graph Reentrant status maps to the observer's Reentrant status, with the key kept at the head. */
static int test_pump_graph_reentrant(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    sink_probe_t probe;
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    uint32_t delivered = 0u;
    uint64_t list[1] = {0x100u};
    Require(f != NULL);
    Require(add_window(f, 0x100u) != NULL);
    Require(add_window(f, 0x101u) != NULL);
    api_double_enum_set(f->dbl, list, 1u);
    Require(admit(f, 0x100u) == 0); /* one Announce stays queued for the drain below */
    hint(f, WindowObserverEventObjectCreate, 0x101u);

    memset(&probe, 0, sizeof(probe));
    probe.f = f;
    Require(interop_window_graph_drain(f->graph, TestEpoch, reentrant_sink, &probe, 1u, &delivered) ==
            InteropWindowGraphStatusOk);
    Require(delivered == 1u && probe.calls == 1u);
    Require(probe.pump == WindowObserverStatusReentrant);
    Require(probe.refresh == WindowObserverStatusReentrant);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.last_graph_status == (uint32_t)InteropWindowGraphStatusReentrant);
    Require(stats.graph_errors >= 2u);
    Require(stats.dirty_pending == 1u); /* the key was put back */
    Require(stats.refreshes_committed == 0u && stats.refresh_requested == 1u);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.refresh_active == 0u && gs.active_count == 1u);

    Require(do_pump(f, 4u) == WindowObserverStatusOk); /* normal call afterwards */
    Require(drain_events(f) == 1u && event_log[0].window == 0x101u);
done:
    fixture_close(f);
    return result;
}

/** Stamp exhaustion: a refused key stays queued; a refresh aborts and leaves no open graph refresh. */
static int test_pump_stamp_exhausted(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, StampSentinel - 1u, 1);
    fixture_t *g = NULL;
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    Require(f != NULL);
    Require(add_window(f, 0x100u) != NULL);
    hint(f, WindowObserverEventObjectCreate, 0x100u); /* takes the last stamp */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.next_stamp == StampSentinel);
    Require(do_pump(f, 4u) == WindowObserverStatusStampExhausted);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 1u && stats.keys_processed == 1u);
    fixture_close(f);
    f = NULL;

    /* Two stamps: admit the window, then a refresh cannot draw a stamp. */
    g = fixture_open(0u, StampSentinel - 2u, 1);
    Require(g != NULL);
    Require(add_window(g, 0x100u) != NULL);
    Require(add_window(g, 0x101u) != NULL);
    Require(admit(g, 0x100u) == 0);
    Require(obs_stats(g, &stats) == 0);
    Require(stats.next_stamp == StampSentinel);
    hint(g, WindowObserverEventObjectCreate, 0x101u); /* no stamp left: latches overflow instead */
    Require(obs_stats(g, &stats) == 0);
    Require(stats.overflow_latched == 1u && stats.hints_received == 1u && stats.hint_overflows == 0u);
    uint64_t list[1] = {0x101u};
    api_double_enum_set(g->dbl, list, 1u);
    Require(do_pump(g, 4u) == WindowObserverStatusStampExhausted);
    Require(obs_stats(g, &stats) == 0);
    Require(stats.invalidations == 1u && stats.overflow_latched == 0u && stats.refreshes_aborted == 1u);
    Require(graph_stats(g, &gs) == 0);
    Require(gs.refresh_active == 0u);
    Require(do_refresh(g) == WindowObserverStatusStampExhausted);
    Require(graph_stats(g, &gs) == 0);
    Require(gs.refresh_active == 0u);
done:
    fixture_close(f);
    fixture_close(g);
    return result;
}

/* ------------------------------------------------------------------------------------------ */
/* Refresh through the real graph                                                             */
/* ------------------------------------------------------------------------------------------ */

/** A full refresh admits everything in enumeration order and commits. */
static int test_refresh_admits_in_order(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    uint64_t list[3] = {0x30u, 0x10u, 0x20u};
    Require(f != NULL);
    for (uint32_t i = 0; i < 3u; i++) {
        Require(add_window(f, list[i]) != NULL);
    }
    api_double_enum_set(f->dbl, list, 3u);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 3u);
    for (uint32_t i = 0; i < 3u; i++) {
        Require(event_log[i].kind == InteropWindowEventAnnounce && event_log[i].window == list[i]);
    }
    Require(f->dbl->query_log[0] == 0x30u && f->dbl->query_log[1] == 0x10u && f->dbl->query_log[2] == 0x20u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_started == 1u && stats.refreshes_committed == 1u && stats.refreshes_aborted == 0u);
    Require(stats.refresh_requested == 0u && stats.queries_exists == 3u);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.refresh_commits == 1u && gs.refresh_active == 0u && gs.active_count == 3u);

    /* Again: nothing changed, nothing retired, a second commit. */
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 0u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_committed == 2u);
    /* A stats snapshot is optional and written on Ok. */
    Require(window_observer_refresh(f->observer, f->graph, TestEpoch, &stats) == WindowObserverStatusOk);
    Require(stats.refreshes_committed == 3u);
done:
    fixture_close(f);
    return result;
}

/** A child enumerated before its owner is deferred and flushed when the owner is admitted. */
static int test_refresh_defers_child_before_parent(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    interop_window_live_t live;
    uint64_t list[2] = {0x601u, 0x600u};
    Require(f != NULL);
    Require(add_window(f, 0x600u) != NULL); /* owner */
    Require(add_owned(f, 0x601u, 0x600u) != NULL); /* owned dialog, enumerated first */
    api_double_enum_set(f->dbl, list, 2u);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 2u);
    Require(event_log[0].kind == InteropWindowEventAnnounce && event_log[0].window == 0x600u);
    Require(event_log[1].kind == InteropWindowEventAnnounce && event_log[1].window == 0x601u);
    Require(event_log[1].role == InteropRoleDialog);
    Require(event_log[1].parent_window == 0x600u && event_log[1].parent_generation == event_log[0].generation);
    Require(interop_window_graph_find_live(f->graph, 0x601u, &live) == InteropWindowGraphStatusOk);
    Require(live.parent_window == 0x600u);
    /* The owner sub-query is folded into the window query: D, O (sub-query), O. */
    Require(f->dbl->query_total == 3u);
    Require(f->dbl->query_log[0] == 0x601u && f->dbl->query_log[1] == 0x600u && f->dbl->query_log[2] == 0x600u);
done:
    fixture_close(f);
    return result;
}

/** A Failed window is kept while the healthy window behind it is admitted, and the refresh commits. */
static int test_refresh_failed_window_kept(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    api_double_window_t *failing = NULL;
    uint64_t first[1] = {0x10u};
    uint64_t both[2] = {0x10u, 0x20u};
    Require(f != NULL);
    failing = add_window(f, 0x10u);
    Require(failing != NULL);
    api_double_enum_set(f->dbl, first, 1u);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);

    failing->mode = ApiDoubleModeFailed;
    Require(add_window(f, 0x20u) != NULL);
    api_double_enum_set(f->dbl, both, 2u);
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventAnnounce && event_log[0].window == 0x20u);
    Require(count_kind(InteropWindowEventRetireIntent) == 0u);
    Require(is_live(f, 0x10u) == 1 && is_live(f, 0x20u) == 1);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_failed == 1u && stats.refreshes_committed == 2u && stats.refresh_requested == 0u);

    /* Three more refreshes with the window still failing: still live. */
    for (uint32_t i = 0; i < 3u; i++) {
        f->dbl->monotonic += 300u;
        Require(do_refresh(f) == WindowObserverStatusOk);
    }
    Require(drain_events(f) == 0u && is_live(f, 0x10u) == 1);
done:
    fixture_close(f);
    return result;
}

/** A window known through hints but missing from the enumeration survives ten refreshes. */
static int test_refresh_unenumerated_survives(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    uint64_t list[1] = {0x10u};
    Require(f != NULL);
    Require(add_window(f, 0x10u) != NULL);
    Require(add_window(f, 0x50u) != NULL);
    api_double_enum_set(f->dbl, list, 1u); /* 0x50 is never enumerated */
    Require(admit(f, 0x50u) == 0);
    Require(drain_events(f) == 1u);
    const uint32_t before = api_double_queries_of(f->dbl, 0x50u);
    for (uint32_t i = 0; i < 10u; i++) {
        f->dbl->monotonic += 300u;
        Require(do_refresh(f) == WindowObserverStatusOk);
        Require(is_live(f, 0x50u) == 1);
    }
    Require(drain_events(f) == 1u); /* only the Announce of 0x10; no RetireIntent */
    Require(event_log[0].window == 0x10u && event_log[0].kind == InteropWindowEventAnnounce);
    Require(api_double_queries_of(f->dbl, 0x50u) == before + 10u); /* requeried once per refresh */
done:
    fixture_close(f);
    return result;
}

/** A vanished window is retired at the refresh: as Destroyed when the hint rule accepts, else RefreshUnseen. */
static int test_refresh_vanished_retired(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    uint64_t list[1] = {0x10u};
    Require(f != NULL);
    Require(add_window(f, 0x10u) != NULL);
    Require(add_window(f, 0x61u) != NULL);
    Require(add_window(f, 0x62u) != NULL);
    api_double_enum_set(f->dbl, list, 1u);
    f->dbl->tick = 5000u;
    Require(admit(f, 0x61u) == 0);
    Require(admit(f, 0x62u) == 0);
    Require(drain_events(f) == 2u);

    /* 0x61 vanishes with a later tick: the Gone requery applies note_destroyed. */
    api_double_remove(f->dbl, 0x61u);
    f->dbl->tick = 6000u;
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 2u); /* Announce 0x10, RetireIntent 0x61 */
    Require(event_log[0].kind == InteropWindowEventAnnounce && event_log[0].window == 0x10u);
    Require(event_log[1].kind == InteropWindowEventRetireIntent && event_log[1].window == 0x61u);
    Require(event_log[1].reason == InteropWindowReasonDestroyed);
    Require(find_event(InteropWindowEventRetireIntent, 0x61u, 0u) == 1); /* after the enumeration offers */
    Require(find_event(InteropWindowEventRetireIntent, 0x62u, 0u) == -1);
    Require(is_live(f, 0x61u) == 0 && is_live(f, 0x62u) == 1);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_gone == 1u && stats.destroys_noted == 1u);

    /* 0x62 vanishes but the tick went backwards: the destroy rule refuses, the commit retires it. */
    api_double_remove(f->dbl, 0x62u);
    f->dbl->tick = 4000u;
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].window == 0x62u);
    Require(event_log[0].reason == InteropWindowReasonRefreshUnseen);
    Require(is_live(f, 0x62u) == 0);
done:
    fixture_close(f);
    return result;
}

/** Truncated and Failed enumerations return EnumFailed before any graph call; nothing is retired. */
static int test_refresh_enum_failure(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    uint64_t one[1] = {0x10u};
    uint64_t none[1] = {0u};
    Require(f != NULL);
    Require(add_window(f, 0x10u) != NULL);
    Require(add_window(f, 0x11u) != NULL);
    api_double_enum_set(f->dbl, one, 1u);
    Require(admit(f, 0x11u) == 0);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 2u);
    Require(graph_stats(f, &gs) == 0);
    const uint64_t commits = gs.refresh_commits;

    /* Truncated: the partial list lacks 0x11, which must stay live. */
    f->dbl->enum_result = WindowObserverEnumTruncated;
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusEnumFailed);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_started == 1u); /* only the first refresh began */
    Require(graph_stats(f, &gs) == 0);
    Require(gs.refresh_commits == commits && gs.refresh_active == 0u);
    Require(is_live(f, 0x11u) == 1 && drain_events(f) == 0u);

    /* Failed. */
    f->dbl->enum_result = WindowObserverEnumFailed;
    f->dbl->monotonic += 300u;
    api_double_enum_set(f->dbl, none, 0u);
    Require(do_refresh(f) == WindowObserverStatusEnumFailed);
    Require(is_live(f, 0x10u) == 1 && is_live(f, 0x11u) == 1 && drain_events(f) == 0u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_started == 1u && stats.refreshes_committed == 1u);
    Require(f->dbl->enum_calls == 3u);

    /* Recovery. */
    f->dbl->enum_result = WindowObserverEnumOk;
    api_double_enum_set(f->dbl, one, 1u);
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(is_live(f, 0x11u) == 1); /* hinted window kept through the unseen requery */
done:
    fixture_close(f);
    return result;
}

/** 250 ms cadence, backwards clock, and the latch-once rule with a frozen clock. */
static int test_refresh_cadence_latch(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 0);
    window_observer_stats_t stats;
    Require(f != NULL);
    api_double_t *d = f->dbl;
    d->monotonic = 1000u;
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    Require(window_observer_refresh_due(f->observer, 1000u) == 1); /* no attempt since start */
    Require(window_observer_next_timeout_ms(f->observer, 1000u) == 0u);

    /* Persistently failing refresh with a frozen clock runs once per latch event. */
    d->enum_result = WindowObserverEnumFailed;
    uint32_t runs = 0u;
    while (runs < 1000u && window_observer_refresh_due(f->observer, d->monotonic) != 0) {
        Require(do_refresh(f) == WindowObserverStatusEnumFailed);
        runs++;
    }
    Require(runs == 1u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refresh_requested == 1u); /* only a commit clears the latch */
    Require(window_observer_next_timeout_ms(f->observer, 1000u) == InteropWindowGraphRefreshIntervalMs);
    Require(window_observer_next_timeout_ms(f->observer, 1100u) == InteropWindowGraphRefreshIntervalMs - 100u);
    Require(window_observer_refresh_due(f->observer, 1249u) == 0);
    Require(window_observer_next_timeout_ms(f->observer, 1249u) == 1u);
    Require(window_observer_refresh_due(f->observer, 1250u) == 1);
    Require(window_observer_next_timeout_ms(f->observer, 1250u) == 0u);
    /* A clock that went backwards counts as zero elapsed. */
    Require(window_observer_refresh_due(f->observer, 500u) == 0);
    Require(window_observer_next_timeout_ms(f->observer, 500u) == InteropWindowGraphRefreshIntervalMs);

    /* A committed refresh clears the latch; the cadence alone governs afterwards. */
    d->enum_result = WindowObserverEnumOk;
    d->monotonic = 1250u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refresh_requested == 0u);
    Require(window_observer_refresh_due(f->observer, 1250u) == 0);
    Require(window_observer_refresh_due(f->observer, 1499u) == 0);
    Require(window_observer_refresh_due(f->observer, 1500u) == 1);

    /* A new latch event (preedit change) after the attempt makes one refresh due at once. */
    d->monotonic = 1300u;
    Require(window_observer_set_preedit_window(f->observer, 0x77u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refresh_requested == 1u);
    Require(window_observer_refresh_due(f->observer, 1300u) == 1);
    Require(window_observer_next_timeout_ms(f->observer, 1300u) == 0u);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(window_observer_refresh_due(f->observer, 1300u) == 0);
    Require(window_observer_set_preedit_window(f->observer, 0x77u) == WindowObserverStatusOk); /* unchanged value */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refresh_requested == 0u);

    /* Hints pending (desktop Default) make the loop run immediately. */
    hint(f, WindowObserverEventObjectShow, 0x10u);
    Require(window_observer_next_timeout_ms(f->observer, 1300u) == 0u);
done:
    fixture_close(f);
    return result;
}

/** QueueFull while offering aborts the refresh (nothing retired); a later refresh succeeds. */
static int test_refresh_queue_full_abort(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    uint64_t list[2] = {0x300u, 0x301u};
    Require(f != NULL);
    Require(add_window(f, 0x300u) != NULL);
    Require(admit(f, 0x300u) == 0);
    Require(fill_event_ring(f, 0x300u) == 0);

    Require(add_window(f, 0x301u) != NULL);
    api_double_enum_set(f->dbl, list, 2u);
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusGraphQueueFull);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_started == 1u && stats.refreshes_aborted == 1u && stats.refreshes_committed == 0u);
    Require(stats.refresh_requested == 1u);
    Require(stats.last_graph_status == (uint32_t)InteropWindowGraphStatusQueueFull);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.refresh_active == 0u && gs.active_count == 1u && gs.retiring_count == 0u);

    Require(drain_events(f) == RingCapacity);
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(is_live(f, 0x301u) == 1);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_committed == 1u && stats.refresh_requested == 0u);
done:
    fixture_close(f);
    return result;
}

/** QueueFull on the commit leaves the graph refresh open; the next refresh aborts it and starts over. */
static int test_refresh_commit_queue_full(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    uint64_t list[1] = {0x300u};
    Require(f != NULL);
    Require(add_window(f, 0x300u) != NULL);
    Require(add_window(f, 0x62u) != NULL);
    api_double_enum_set(f->dbl, list, 1u);
    f->dbl->tick = 5000u;
    Require(admit(f, 0x62u) == 0);
    Require(admit(f, 0x300u) == 0);
    Require(fill_event_ring(f, 0x300u) == 0); /* the fill keeps the tick at 5000 */

    /* 0x62 vanishes with a tick in the past: the destroy rule refuses, so only the commit can retire it. */
    api_double_remove(f->dbl, 0x62u);
    f->dbl->tick = 4000u;
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusGraphQueueFull);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.refresh_active == 1u); /* left open for the retry */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_aborted == 0u && stats.refreshes_started == 1u);
    Require(is_live(f, 0x62u) == 1);

    Require(drain_events(f) == RingCapacity);
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_aborted == 1u && stats.refreshes_started == 3u); /* RefreshActive: abort + begin again */
    Require(stats.refreshes_committed == 1u);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.refresh_active == 0u);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].window == 0x62u);
    Require(event_log[0].reason == InteropWindowReasonRefreshUnseen);
done:
    fixture_close(f);
    return result;
}

/** A replacement observer that starts above stamp_high_water causes no retirements; one below it is dropped. */
static int test_refresh_replacement_observer(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    uint64_t list[2] = {0x10u, 0x20u};
    uint64_t high_water = 0u;
    Require(f != NULL);
    Require(add_window(f, 0x10u) != NULL);
    Require(add_window(f, 0x20u) != NULL);
    api_double_enum_set(f->dbl, list, 2u);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 2u);
    Require(interop_window_graph_stamp_high_water(f->graph, &high_water) == InteropWindowGraphStatusOk);
    Require(high_water >= 2u);
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk);

    f->observer = make_observer(f, high_water + 1u);
    Require(f->observer != NULL);
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 0u); /* identical facts: Unchanged, nothing retired */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.graph_errors == 0u && stats.refreshes_committed == 1u);
    Require(is_live(f, 0x10u) == 1 && is_live(f, 0x20u) == 1);

    /* A replacement that starts at stamp 1 offers stale facts: dropped and counted, still nothing retired. */
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk);
    f->observer = make_observer(f, 1u);
    Require(f->observer != NULL);
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.graph_errors == 2u && stats.last_graph_status == (uint32_t)InteropWindowGraphStatusStaleFacts);
    Require(drain_events(f) == 0u && is_live(f, 0x10u) == 1 && is_live(f, 0x20u) == 1);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.rejected_stale_facts == 2u);
done:
    fixture_close(f);
    return result;
}

/* ------------------------------------------------------------------------------------------ */
/* Owner, preedit, own-process, UAC                                                           */
/* ------------------------------------------------------------------------------------------ */

/** A hidden owner promotes its dialog to Toplevel; when the owner becomes visible the dialog is Reclassified. */
static int test_owner_hidden_promotion(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    api_double_window_t *owner = NULL;
    Require(f != NULL);
    owner = add_window(f, 0x400u);
    Require(owner != NULL);
    set_visible(owner, 0);
    Require(add_owned(f, 0x401u, 0x400u) != NULL);

    Require(admit(f, 0x401u) == 0);
    Require(f->dbl->query_total == 2u && f->dbl->query_log[0] == 0x401u && f->dbl->query_log[1] == 0x400u);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventAnnounce && event_log[0].window == 0x401u);
    Require(event_log[0].role == InteropRoleToplevel && event_log[0].parent_window == 0u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_exists == 1u); /* the owner sub-query is folded into the window query */

    set_visible(owner, 1);
    Require(admit(f, 0x400u) == 0);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventAnnounce && event_log[0].window == 0x400u);
    const uint64_t owner_generation = event_log[0].generation;

    hint(f, WindowObserverEventObjectShow, 0x401u); /* requery the dialog: OwnerHidden is now clear */
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 2u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].window == 0x401u);
    Require(event_log[0].reason == InteropWindowReasonReclassified);
    Require(event_log[1].kind == InteropWindowEventAnnounce && event_log[1].window == 0x401u);
    Require(event_log[1].role == InteropRoleDialog);
    Require(event_log[1].parent_window == 0x400u && event_log[1].parent_generation == owner_generation);

    /* The owner hides again: the dialog is promoted back to Toplevel. */
    set_visible(owner, 0);
    hint(f, WindowObserverEventObjectShow, 0x401u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 2u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[1].kind == InteropWindowEventAnnounce);
    Require(event_log[1].role == InteropRoleToplevel && event_log[1].parent_window == 0u);
done:
    fixture_close(f);
    return result;
}

/** A Gone owner sets OwnerHidden (not counted as Gone); a Failed owner fails the dialog's query. */
static int test_owner_hidden_gone_and_failed(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    api_double_window_t *owner = NULL;
    Require(f != NULL);
    Require(add_owned(f, 0x401u, 0x400u) != NULL); /* the owner 0x400 is absent: Gone */
    Require(admit(f, 0x401u) == 0);
    Require(drain_events(f) == 1u);
    Require(event_log[0].role == InteropRoleToplevel && event_log[0].parent_window == 0u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_exists == 1u && stats.queries_gone == 0u && stats.queries_failed == 0u);

    owner = add_window(f, 0x400u);
    Require(owner != NULL);
    owner->mode = ApiDoubleModeFailed;
    hint(f, WindowObserverEventObjectShow, 0x401u);
    Require(do_pump(f, 4u) == WindowObserverStatusMore); /* failed query: retried */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_failed == 1u && stats.dirty_pending == 1u);
    Require(drain_events(f) == 0u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk); /* retry fails too: dropped */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_failed == 2u && stats.query_retries_exhausted == 1u);
    Require(is_live(f, 0x401u) == 1 && drain_events(f) == 0u);
done:
    fixture_close(f);
    return result;
}

/** The observer classifies the owner with the configured uac_mode; Native hides the consent owner. */
static int test_owner_hidden_uac(void) {
    int result = 0;
    fixture_t *native = fixture_open(2u, 1u, 1);
    fixture_t *window_mode = NULL;
    window_observer_stats_t stats;
    Require(native != NULL);
    Require(add_consent(native, 0x700u) != NULL);
    Require(add_owned(native, 0x701u, 0x700u) != NULL);
    Require(admit(native, 0x701u) == 0); /* owner hidden from export in mode Native */
    Require(drain_events(native) == 1u);
    Require(event_log[0].window == 0x701u && event_log[0].role == InteropRoleToplevel);
    Require(event_log[0].parent_window == 0u);
    Require(admit(native, 0x700u) == 0); /* the consent window itself: Excluded, UacNativeHidden */
    Require(drain_events(native) == 0u);
    Require(obs_stats(native, &stats) == 0);
    Require(stats.uac_native_hidden == 1u);
    fixture_close(native);
    native = NULL;

    window_mode = fixture_open(0u, 1u, 1);
    Require(window_mode != NULL);
    Require(add_consent(window_mode, 0x700u) != NULL);
    Require(add_owned(window_mode, 0x701u, 0x700u) != NULL);
    Require(admit(window_mode, 0x701u) == 0); /* owner Eligible: the dialog waits for its parent */
    Require(drain_events(window_mode) == 0u);
    Require(is_live(window_mode, 0x701u) == 0);
    Require(admit(window_mode, 0x700u) == 0);
    Require(drain_events(window_mode) == 2u);
    Require(event_log[0].window == 0x700u && event_log[0].role == InteropRoleUacConsent);
    Require(event_log[1].window == 0x701u && event_log[1].role == InteropRoleDialog);
    Require(event_log[1].parent_window == 0x700u);
    Require(obs_stats(window_mode, &stats) == 0);
    Require(stats.uac_native_hidden == 0u);
done:
    fixture_close(native);
    fixture_close(window_mode);
    return result;
}

/** The agent's preedit window is exported as ImePreedit under its owner with an empty title. */
static int test_preedit_parenting(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    api_double_window_t *preedit = NULL;
    Require(f != NULL);
    Require(add_window(f, 0x500u) != NULL);
    Require(admit(f, 0x500u) == 0);
    Require(drain_events(f) == 1u);
    const uint64_t owner_generation = event_log[0].generation;

    preedit = add_owned(f, 0x501u, 0x500u);
    Require(preedit != NULL);
    preedit->facts.process_id = ApiDoubleOwnPid;
    api_double_set_text(preedit->facts.class_name, InteropWindowGraphMaxClassBytes, &preedit->facts.class_length, "WaddlePreedit");
    api_double_set_text(preedit->facts.image_name, InteropWindowGraphMaxImageBytes, &preedit->facts.image_length, "waddle.exe");

    /* Without a registered preedit window it is just an excluded own-process window. */
    Require(admit(f, 0x501u) == 0);
    Require(drain_events(f) == 0u && is_live(f, 0x501u) == 0);

    Require(window_observer_set_preedit_window(f->observer, 0x501u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refresh_requested == 1u);
    hint(f, WindowObserverEventObjectShow, 0x501u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventAnnounce && event_log[0].window == 0x501u);
    Require(event_log[0].role == InteropRoleImePreedit);
    Require(event_log[0].parent_window == 0x500u && event_log[0].parent_generation == owner_generation);
    Require(event_log[0].title_length == 0u); /* the template title is dropped for own-process windows */
    Require(event_log[0].process_id == ApiDoubleOwnPid);

    /* Unregistered again: excluded and retired with the ExcludedAgent reason. */
    Require(window_observer_set_preedit_window(f->observer, 0u) == WindowObserverStatusOk);
    hint(f, WindowObserverEventObjectShow, 0x501u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].reason == InteropWindowReasonExcludedAgent);
done:
    fixture_close(f);
    return result;
}

/** The preedit window can also be passed in the create config. */
static int test_preedit_from_config(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 0);
    window_observer_config_t config;
    api_double_window_t *preedit = NULL;
    Require(f != NULL);
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk);
    api_double_config(&config, f->dbl, 0u, 1u);
    config.preedit_window = 0x501u;
    Require(window_observer_create(&config, &f->observer) == WindowObserverStatusOk);
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    preedit = add_window(f, 0x501u);
    Require(preedit != NULL);
    preedit->facts.process_id = ApiDoubleOwnPid;
    Require(admit(f, 0x501u) == 0);
    Require(drain_events(f) == 1u);
    Require(event_log[0].role == InteropRoleImePreedit && event_log[0].parent_window == 0u);
    Require(event_log[0].title_length == 0u);
done:
    fixture_close(f);
    return result;
}

/** Own-process windows other than the preedit are excluded and a refresh never exports them. */
static int test_own_process_excluded(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    api_double_window_t *own = NULL;
    uint64_t list[2] = {0x10u, 0x11u};
    Require(f != NULL);
    own = add_window(f, 0x11u);
    Require(own != NULL);
    own->facts.process_id = ApiDoubleOwnPid;
    Require(add_window(f, 0x10u) != NULL);
    api_double_enum_set(f->dbl, list, 2u);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].window == 0x10u);
    Require(is_live(f, 0x11u) == 0);
done:
    fixture_close(f);
    return result;
}

/* ------------------------------------------------------------------------------------------ */
/* Teardown and statistics                                                                    */
/* ------------------------------------------------------------------------------------------ */

/** Destroying with queued hints and an open overflow latch frees everything. */
static int test_teardown_with_hints(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    Require(f != NULL);
    for (uint64_t i = 0; i < WindowObserverDirtyCapacity + 20u; i++) {
        hint(f, WindowObserverEventObjectCreate, 0x8000u + i);
    }
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk);
    Require(f->observer == NULL);
    hint(f, WindowObserverEventObjectCreate, 0x1u); /* late hint after destroy */
    f->observer = make_observer(f, 1u);
    Require(f->observer != NULL);
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
done:
    fixture_close(f);
    return result;
}

/** Exact counter accounting across hints, pumps, failures and graph errors. */
static int test_stats_accounting(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    Require(f != NULL);
    Require(add_window(f, 0xA0u) != NULL);
    Require(add_window(f, 0xB0u) != NULL);
    Require(add_window(f, 0xC0u) != NULL);
    f->dbl->windows[2].mode = ApiDoubleModeFailed; /* 0xC0 */

    hint(f, WindowObserverEventObjectCreate, 0xA0u);
    hint(f, WindowObserverEventObjectShow, 0xA0u);
    hint(f, WindowObserverEventObjectCreate, 0xB0u);
    api_double_hint_full(f->dbl, WindowObserverEventObjectShow, 0xB0u, 1, 0, 0u); /* filtered */
    api_double_hint_full(f->dbl, WindowObserverEventObjectShow, 0u, 0, 0, 0u); /* filtered */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hints_received == 3u && stats.hints_coalesced == 1u && stats.hints_filtered == 2u);
    Require(stats.dirty_pending == 2u && stats.next_stamp == 4u);
    Require(do_pump(f, 8u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.keys_processed == 2u && stats.queries_exists == 2u && stats.queries_gone == 0u);
    Require(stats.destroys_noted == 0u && stats.graph_errors == 0u && stats.desktop_active == 1u);

    hint(f, WindowObserverEventObjectCreate, 0xC0u);
    Require(do_pump(f, 8u) == WindowObserverStatusMore);
    Require(do_pump(f, 8u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.keys_processed == 4u && stats.queries_failed == 2u && stats.query_retries_exhausted == 1u);

    /* Gone: one query, one note. */
    api_double_remove(f->dbl, 0xA0u);
    hint(f, WindowObserverEventObjectHide, 0xA0u);
    Require(do_pump(f, 8u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_gone == 1u && stats.destroys_noted == 1u && stats.keys_processed == 5u);

    /* A wrong epoch is a call-level graph error: GraphError, exact status kept, key put back. */
    hint(f, WindowObserverEventObjectShow, 0xB0u);
    Require(window_observer_pump(f->observer, f->graph, TestEpoch + 1u, 8u, &stats) == WindowObserverStatusGraphError);
    Require(stats.last_graph_status == (uint32_t)InteropWindowGraphStatusStaleEpoch);
    Require(stats.graph_errors == 1u && stats.dirty_pending == 1u);
    Require(window_observer_refresh(f->observer, f->graph, TestEpoch + 1u, &stats) == WindowObserverStatusGraphError);
    Require(stats.graph_errors == 2u && stats.refreshes_started == 1u && stats.refreshes_committed == 0u);
    Require(do_pump(f, 8u) == WindowObserverStatusOk); /* the right epoch processes the kept key */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 0u && stats.last_graph_status == (uint32_t)InteropWindowGraphStatusStaleEpoch);

    /* Desktop refusal. */
    f->dbl->desktop = WindowObserverDesktopOther;
    Require(do_pump(f, 8u) == WindowObserverStatusDesktopInactive);
    Require(window_observer_pump(f->observer, f->graph, TestEpoch, 8u, &stats) == WindowObserverStatusDesktopInactive);
    Require(stats.pumps_desktop_inactive == 2u && stats.desktop_active == 0u);
done:
    fixture_close(f);
    return result;
}

/* ------------------------------------------------------------------------------------------ */
/* Runner                                                                                     */
/* ------------------------------------------------------------------------------------------ */

/** Signature of one test. */
typedef int (*test_function_t)(void);

/** One registered test. */
typedef struct test_case_t {
    const char *name; /**< Name printed on success and failure. */
    test_function_t function; /**< Test body; returns 0 on success. */
} test_case_t;

#define TestEntry(name_text) {#name_text, name_text}

/** Every test, in execution order. */
static const test_case_t TestCases[] = {
    TestEntry(test_create_validation),
    TestEntry(test_lifecycle_start_stop_restart),
    TestEntry(test_lifecycle_second_observer),
    TestEntry(test_lifecycle_hook_failure_rollback),
    TestEntry(test_lifecycle_unhook_failure),
    TestEntry(test_lifecycle_destroy_started),
    TestEntry(test_lifecycle_wrong_thread),
    TestEntry(test_hint_filter_table),
    TestEntry(test_hint_wrong_thread),
    TestEntry(test_dirty_coalesce_order_budget),
    TestEntry(test_dirty_overflow_recovery),
    TestEntry(test_dirty_reserved_slot),
    TestEntry(test_pump_admit_update_hide),
    TestEntry(test_pump_cloak),
    TestEntry(test_pump_destroy_and_reuse),
    TestEntry(test_pump_gone_vs_failed),
    TestEntry(test_pump_queue_full),
    TestEntry(test_pump_queue_full_destroy_information),
    TestEntry(test_pump_desktop_inactive),
    TestEntry(test_pump_busy_reentrant),
    TestEntry(test_pump_graph_reentrant),
    TestEntry(test_pump_stamp_exhausted),
    TestEntry(test_refresh_admits_in_order),
    TestEntry(test_refresh_defers_child_before_parent),
    TestEntry(test_refresh_failed_window_kept),
    TestEntry(test_refresh_unenumerated_survives),
    TestEntry(test_refresh_vanished_retired),
    TestEntry(test_refresh_enum_failure),
    TestEntry(test_refresh_cadence_latch),
    TestEntry(test_refresh_queue_full_abort),
    TestEntry(test_refresh_commit_queue_full),
    TestEntry(test_refresh_replacement_observer),
    TestEntry(test_owner_hidden_promotion),
    TestEntry(test_owner_hidden_gone_and_failed),
    TestEntry(test_owner_hidden_uac),
    TestEntry(test_preedit_parenting),
    TestEntry(test_preedit_from_config),
    TestEntry(test_own_process_excluded),
    TestEntry(test_teardown_with_hints),
    TestEntry(test_stats_accounting),
};

/** Run every test; stop at the first failure and return nonzero. */
int main(void) {
    const size_t total = sizeof(TestCases) / sizeof(TestCases[0]);
    for (size_t i = 0; i < total; i++) {
        const int rc = TestCases[i].function();
        if (rc != 0) {
            fprintf(stderr, "FAILED: %s\n", TestCases[i].name);
            return 1;
        }
        printf("ok %s\n", TestCases[i].name);
    }
    printf("window_observer_windows: %zu tests passed\n", total);
    return 0;
}
