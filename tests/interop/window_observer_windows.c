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

#ifdef WgObsWrapCalloc
/*
 * Allocation-failure injection (Linux, GNU ld/gold/lld): the makefile builds with -DWgObsWrapCalloc=1
 * and -Wl,--wrap=calloc, so every calloc call of the linked objects reaches __wrap_calloc. The wrapper
 * counts the calls and fails exactly the next one when armed; it forwards to the real calloc otherwise.
 * Only window_observer_create calls calloc on the path under test.
 */
extern void *__real_calloc(size_t count, size_t size);
/** Calls that reached the wrapper; proves the link-time wrap is in effect. */
static _Atomic uint64_t g_wrapped_calloc_calls = 0;
/** 1 = the next calloc call returns NULL (and clears this flag). */
static _Atomic uint32_t g_wrapped_calloc_fail_next = 0;

/** Linker wrapper for calloc (see above). */
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size) {
    atomic_fetch_add(&g_wrapped_calloc_calls, 1u);
    if (atomic_exchange(&g_wrapped_calloc_fail_next, 0u) != 0u) {
        return NULL;
    }
    return __real_calloc(count, size);
}
#endif

/* ------------------------------------------------------------------------------------------ */
/* Fixture                                                                                    */
/* ------------------------------------------------------------------------------------------ */

/** @brief Test fixture: heap object owning the double, the graph and (until destroyed) the observer. */
typedef struct fixture_t {
    api_double_t *dbl; /**< Scriptable api double; owned. */
    window_observer_t *observer; /**< Observer under test; owned; NULL once a test destroyed it. */
    interop_window_graph_t *graph; /**< Real graph; owned. */
    uint32_t lookup_calls; /**< Number of app_key_lookup calls. */
    uint64_t lookup_destroy_trigger_window; /**< Lookup whose graph admission fires the scripted Destroy. */
    uint64_t lookup_destroy_window; /**< One-shot HWND destroyed from the graph app-key callback. */
    uint32_t lookup_destroy_once; /**< 1 while lookup_destroy_window is armed. */
    void (*graph_clock_hook)(struct fixture_t *fixture); /**< Optional graph-clock reentry script. */
    uint32_t graph_clock_calls; /**< Calls while the saturation script is armed. */
    uint64_t clock_old_candidate; /**< Old reservation used by transition tests. */
    uint64_t clock_new_candidate; /**< Proposed reservation used by transition tests. */
} fixture_t;

/** Graph app-key callback: every window gets the key "a-test". */
static int fixture_lookup(void *context, const interop_window_facts_t *facts, uint8_t *key, uint32_t capacity,
                          uint32_t *length) {
    fixture_t *f = (fixture_t *)context;
    if (capacity < 6u) {
        return 1;
    }
    f->lookup_calls++;
    if (f->lookup_destroy_once != 0u &&
        (f->lookup_destroy_trigger_window == 0u || facts->window == f->lookup_destroy_trigger_window)) {
        f->lookup_destroy_once = 0u;
        api_double_hint(f->dbl, WindowObserverEventObjectDestroy, f->lookup_destroy_window);
    }
    memcpy(key, "a-test", 6u);
    *length = 6u;
    return 0;
}

/** Release only after genuine hook quiescence; failed cleanup never frees borrowed owners. */
static void fixture_close(fixture_t *f) {
    if (f == NULL) return;
    f->graph_clock_hook = NULL;
    if (f->dbl != NULL) {
        if (atomic_load(&f->dbl->helper_active) != 0) {
            fprintf(stderr, "observer fixture cleanup: helper thread remains active\n");
            abort();
        }
        /* Disarm fixture-only failure scripts; production ownership remains unchanged. */
        f->dbl->tid = ApiDoubleOwnerTid;
        f->dbl->remove_fail_at = 0u;
        f->dbl->remove_hook_fn = NULL;
        f->dbl->install_hook_fn = NULL;
        f->dbl->monotonic_hook = NULL;
        f->dbl->tick_hook = NULL;
        f->dbl->query_hook = NULL;
    }
    if (f->observer != NULL) {
        window_observer_stats_t stats;
        if (window_observer_status(f->observer, &stats) != WindowObserverStatusOk) abort();
        if (stats.state != WindowObserverStateCreated) {
            for (uint32_t attempt = 0; attempt < 3u; ++attempt) {
                const window_observer_status_t stopped = window_observer_stop(f->observer);
                if (stopped == WindowObserverStatusOk || stopped == WindowObserverStatusTrackingFault) break;
                if (stopped == WindowObserverStatusGraphQueueFull && f->graph != NULL) {
                    /* Pure fixture has no capture/transport/pins; discard queued values, not graph ownership. */
                    interop_window_event_t event;
                    while (interop_window_graph_next_event(f->graph, TestEpoch, &event) ==
                           InteropWindowGraphStatusOk) {}
                }
            }
        } else {
            /* Bound Created needs explicit stop; unbound Created is destroyed directly below. */
            (void)window_observer_stop(f->observer);
        }
        (void)window_observer_drain_protected_destroys(f->observer);
        window_observer_quarantine_record_t quarantine;
        memset(&quarantine, 0, sizeof(quarantine));
        if (window_observer_quarantine_handoff(&f->observer, &quarantine) != WindowObserverStatusOk)
            (void)window_observer_destroy(&f->observer);
        if (f->observer != NULL) {
            fprintf(stderr, "observer fixture cleanup: live owner retained; borrowed graph/API preserved\n");
            abort();
        }
    }
    /* This graph fixture never created a capture, mapping, transport or pin owner. */
    if (f->graph != NULL && interop_window_graph_destroy(&f->graph) != InteropWindowGraphStatusOk) abort();
    api_double_free(f->dbl);
    free(f);
}

/** Optional graph clock permits real callback reentry without a fabricated graph state. */
static uint64_t fixture_graph_clock(void *context) {
    fixture_t *f = context;
    if (f->graph_clock_hook != NULL) f->graph_clock_hook(f);
    return f->dbl->monotonic;
}

/** Build a fixture; optionally inject its real graph clock. */
static fixture_t *fixture_open_ex(uint32_t uac_mode, uint64_t first_stamp, int start, int scripted_clock) {
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
    if (scripted_clock) {
        graph_config.now_ms = fixture_graph_clock;
        graph_config.now_ms_context = f;
    }
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
    if (window_observer_bind_graph(f->observer, f->graph, TestEpoch) != WindowObserverStatusOk) {
        fixture_close(f);
        return NULL;
    }
    const window_observer_status_t start_status = start ? window_observer_start(f->observer) : WindowObserverStatusOk;
    if (start_status != WindowObserverStatusOk) {
        fixture_close(f);
        return NULL;
    }
    return f;
}

/** Build the ordinary fixture with the graph's default monotonic timer. */
static fixture_t *fixture_open(uint32_t uac_mode, uint64_t first_stamp, int start) {
    return fixture_open_ex(uac_mode, first_stamp, start, 0);
}

/** Create a second, independent observer on the fixture's double (not started). NULL on failure. */
static window_observer_t *make_observer(fixture_t *f, uint64_t first_stamp) {
    window_observer_config_t config;
    window_observer_t *observer = NULL;
    api_double_config(&config, f->dbl, 0u, first_stamp);
    if (window_observer_create(&config, &observer) != WindowObserverStatusOk) {
        return NULL;
    }
    if (window_observer_bind_graph(observer, f->graph, TestEpoch) != WindowObserverStatusOk) {
        (void)window_observer_destroy(&observer);
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

    /* Stop: reverse removal order, state, and retained evidence is kept for restart/handoff. */
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(d->remove_calls == 3u);
    Require(d->removed_tokens[0] == d->install_tokens[2]);
    Require(d->removed_tokens[1] == d->install_tokens[1]);
    Require(d->removed_tokens[2] == d->install_tokens[0]);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateStopped);
    Require(stats.dirty_pending == 1u && stats.refresh_requested == 1u && stats.overflow_latched == 0u);
    Require(window_observer_refresh_due(f->observer, 9000u) == 0);
    Require(window_observer_next_timeout_ms(f->observer, 9000u) == WindowObserverWaitForever);
    Require(do_pump(f, 1u) == WindowObserverStatusBadState);
    hint(f, WindowObserverEventObjectShow, 0x11u); /* no active observer: ignored */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hints_received == 1u && stats.dirty_pending == 1u);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk); /* second stop: no-op */
    Require(d->remove_calls == 3u);

    /* Restart: hooks again, logical clock and retained dirty evidence survive, latch is set again. */
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    Require(d->install_calls == 6u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateStarted);
    Require(stats.next_stamp == stamp_after_hint);
    Require(stats.dirty_pending == 1u && stats.refresh_requested == 1u);
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
    Require(window_observer_stop(second) == WindowObserverStatusOk);
    window_observer_quarantine_record_t second_quarantine;
    memset(&second_quarantine, 0, sizeof(second_quarantine));
    Require(window_observer_quarantine_handoff(&second, &second_quarantine) == WindowObserverStatusOk);
    Require(second == NULL);
    Require(window_observer_start(f->observer) == WindowObserverStatusOk); /* slot released by destroy */
done:
    if (second != NULL) {
        (void)window_observer_stop(second);
        window_observer_quarantine_record_t second_quarantine;
        memset(&second_quarantine, 0, sizeof(second_quarantine));
        (void)window_observer_quarantine_handoff(&second, &second_quarantine);
    }
    fixture_close(f);
    return result;
}

/** A second observer cannot claim the singleton during the first observer's remove_hook callback. */
static int test_lifecycle_reservation_covers_hook_removal(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_t *other = NULL;
    Require(f != NULL);
    other = make_observer(f, 1u);
    Require(other != NULL);
    f->dbl->start_during_remove = other;
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(f->dbl->start_during_remove == NULL);
    Require(f->dbl->start_during_remove_status == WindowObserverStatusAlreadyActive);
    Require(window_observer_start(other) == WindowObserverStatusOk);
    Require(window_observer_stop(other) == WindowObserverStatusOk);
    Require(window_observer_destroy(&other) == WindowObserverStatusOk);
done:
    if (other != NULL) {
        (void)window_observer_destroy(&other);
    }
    fixture_close(f);
    return result;
}

/** A failed install rolls back every acquired hook in reverse order and releases its reservation. */
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
        Require(stats.state == WindowObserverStateStopped);
        hint(f, WindowObserverEventObjectShow, 0x30u); /* slot was released: ignored */
        Require(obs_stats(f, &stats) == 0);
        Require(stats.hints_received == 0u);

        d->install_fail_at = 0u;
        Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
        Require(do_pump(f, 1u) == WindowObserverStatusBadState);
        Require(window_observer_refresh_due(f->observer, d->monotonic) == 0);
        Require(window_observer_next_timeout_ms(f->observer, d->monotonic) == WindowObserverWaitForever);

        other = make_observer(f, 1u); /* the rollback released the process-wide lifecycle */
        Require(other != NULL);
        Require(window_observer_start(other) == WindowObserverStatusOk);
        Require(window_observer_stop(other) == WindowObserverStatusOk);
        Require(window_observer_destroy(&other) == WindowObserverStatusOk);
        Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk);
        Require(f->observer == NULL);
    done:
        fixture_close(f);
        if (result != 0) {
            return result;
        }
    }
    return result;
}

/** Failed unhooks retain only failed tokens and remain retryable without releasing the lifecycle. */
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
        window_observer_t *other = make_observer(f, 1u);
        Require(other != NULL);
        Require(window_observer_start(other) == WindowObserverStatusAlreadyActive);
        Require(window_observer_start(f->observer) == WindowObserverStatusBadState);
        f->dbl->remove_fail_at = 0u;
        Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
        Require(f->dbl->remove_calls == 4u);
        Require(f->dbl->removed_tokens[3] == f->dbl->install_tokens[2u - (failing - 1u)]);
        Require(obs_stats(f, &stats) == 0);
        Require(stats.state == WindowObserverStateStopped);
        Require(window_observer_start(other) == WindowObserverStatusOk);
        Require(window_observer_stop(other) == WindowObserverStatusOk);
        Require(window_observer_destroy(&other) == WindowObserverStatusOk);
        Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk);
        Require(f->observer == NULL);
        fixture_close(f);
        f = NULL;

        /* Destroy never stops implicitly; explicit stop retries retained hooks. */
        f = fixture_open(0u, 1u, 1);
        Require(f != NULL);
        f->dbl->remove_fail_at = failing;
        Require(window_observer_destroy(&f->observer) == WindowObserverStatusProtectedStatePending);
        Require(window_observer_stop(f->observer) == WindowObserverStatusUnhookFailed);
        Require(f->observer != NULL);
        Require(f->dbl->remove_calls == 3u);
        other = make_observer(f, 1u);
        Require(other != NULL);
        Require(window_observer_start(other) == WindowObserverStatusAlreadyActive);
        f->dbl->remove_fail_at = 0u;
        Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
        window_observer_quarantine_record_t quarantine;
        memset(&quarantine, 0, sizeof(quarantine));
        Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
        Require(f->observer == NULL && f->dbl->remove_calls == 4u);
        Require(window_observer_start(other) == WindowObserverStatusOk);
        Require(window_observer_stop(other) == WindowObserverStatusOk);
        Require(window_observer_destroy(&other) == WindowObserverStatusOk);
    done:
        fixture_close(f);
        if (result != 0) {
            return result;
        }
    }
    return result;
}

/** Failed start rollback retains acquired hooks and the lifecycle reservation through destroy retries. */
static int test_lifecycle_start_rollback_unhook_retry(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 0);
    window_observer_t *other = NULL;
    window_observer_stats_t stats;
    Require(f != NULL);
    f->dbl->install_fail_at = 2u;
    f->dbl->remove_fail_at = 1u;
    Require(window_observer_start(f->observer) == WindowObserverStatusHookFailed);
    Require(f->dbl->install_calls == 2u && f->dbl->remove_calls == 1u);
    Require(f->dbl->removed_tokens[0] == f->dbl->install_tokens[0]);
    Require(obs_stats(f, &stats) == 0 && stats.state == WindowObserverStateFailed);

    other = make_observer(f, 1u);
    Require(other != NULL);
    Require(window_observer_start(other) == WindowObserverStatusAlreadyActive);
    f->dbl->remove_fail_at = 2u;
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusProtectedStatePending);
    Require(window_observer_stop(f->observer) == WindowObserverStatusUnhookFailed);
    Require(f->observer != NULL && f->dbl->remove_calls == 2u);
    Require(window_observer_start(other) == WindowObserverStatusAlreadyActive);
    f->dbl->remove_fail_at = 0u;
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    window_observer_quarantine_record_t quarantine;
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(f->observer == NULL && f->dbl->remove_calls == 3u);
    Require(f->dbl->removed_tokens[2] == f->dbl->install_tokens[0]);
    Require(window_observer_start(other) == WindowObserverStatusOk);
    Require(window_observer_stop(other) == WindowObserverStatusOk);
done:
    if (other != NULL) {
        (void)window_observer_destroy(&other);
    }
    fixture_close(f);
    return result;
}

/** Destroy requires explicit stop and evidence handoff retains queued hints. */
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
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusProtectedStatePending);
    Require(f->dbl->remove_calls == 0u);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    window_observer_quarantine_record_t quarantine;
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
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

/** A stopped bound observer with no graph-owned resources may discard ordinary hints on destroy. */
static int test_destroy_stopped_without_graph_resources(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    Require(f != NULL);
    hint(f, WindowObserverEventObjectCreate, 0xA101u);
    hint(f, WindowObserverEventObjectNameChange, 0xA102u);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateStopped);
    Require(stats.dirty_pending == 2u && stats.overflow_latched == 0u);
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk);
    Require(f->observer == NULL);
    Require(f->dbl->remove_calls == 3u);
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

/** Table overflow latches at the 513th distinct window and recovers through a full refresh (no invalidate_all). */
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

    /* Recovery: one pump clears the table and runs a full refresh; the live window is not touched. */
    f->dbl->monotonic += 10u;
    Require(do_pump(f, 1u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.invalidations == 1u && stats.overflow_latched == 0u && stats.dirty_pending == 0u);
    Require(stats.refreshes_started == 1u && stats.refreshes_committed == 1u && stats.refresh_requested == 0u);
    Require(stats.keys_processed == 1u); /* only the admission pump counted keys; recovery popped none */
    Require(drain_events(f) == 0u); /* no RetireIntent, no second Announce */
    Require(graph_stats(f, &gs) == 0);
    Require(gs.generations_issued == 1u && gs.active_count == 1u && gs.retiring_count == 0u);
    Require(is_live(f, 0x101u) == 1);
    Require(window_observer_refresh_due(f->observer, f->dbl->monotonic) == 0);
    Require(window_observer_next_timeout_ms(f->observer, f->dbl->monotonic) == InteropWindowGraphRefreshIntervalMs);

    /* Recovery blocked by a full graph ring (a new window needs room): GraphQueueFull, the hints are gone,
     * the refresh latch stays set and the refresh succeeds at the cadence after the ring is drained. */
    Require(fill_event_ring(f, 0x101u) == 0);
    Require(add_window(f, 0x102u) != NULL);
    list[0] = 0x101u;
    {
        uint64_t both[2] = {0x101u, 0x102u};
        api_double_enum_set(f->dbl, both, 2u);
    }
    for (uint64_t i = 0; i <= WindowObserverDirtyCapacity; i++) {
        hint(f, WindowObserverEventObjectShow, 0x7000u + i);
    }
    Require(obs_stats(f, &stats) == 0);
    Require(stats.overflow_latched == 1u);
    Require(do_pump(f, 1u) == WindowObserverStatusGraphQueueFull);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.overflow_latched == 0u && stats.dirty_pending == 0u && stats.invalidations == 2u);
    Require(stats.refresh_requested == 1u && stats.refreshes_aborted == 1u);
    Require(stats.last_graph_status == (uint32_t)InteropWindowGraphStatusQueueFull);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.active_count == 1u && gs.refresh_active == 0u); /* 0x101 still Active, refresh closed */
    Require(is_live(f, 0x102u) == 0);
    Require(drain_events(f) == RingCapacity);
    f->dbl->monotonic += InteropWindowGraphRefreshIntervalMs;
    Require(window_observer_refresh_due(f->observer, f->dbl->monotonic) == 1);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(is_live(f, 0x102u) == 1);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refresh_requested == 0u);
done:
    fixture_close(f);
    return result;
}

/**
 * R1: a flood of DESTROY hints for child windows (unknown to the graph and already gone) must not touch
 * the exported windows, below the table capacity (processed key by key) and above it (one refresh).
 */
static int test_flood_child_hints_no_churn(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    uint64_t list[10];
    Require(f != NULL);
    for (uint32_t i = 0; i < 10u; i++) {
        list[i] = 0x100u + i;
        Require(add_window(f, list[i]) != NULL);
    }
    api_double_enum_set(f->dbl, list, 10u);
    f->dbl->tick = 5000u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 10u);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.active_count == 10u && gs.generations_issued == 10u);

    /* 300 child destroys: below the capacity, drained by repeated pumps. */
    f->dbl->tick = 6000u;
    for (uint64_t k = 0; k < 300u; k++) {
        hint(f, WindowObserverEventObjectDestroy, 0x50000u + k);
    }
    for (uint32_t guard = 0; guard < 16u; guard++) {
        const window_observer_status_t status = do_pump(f, 64u);
        Require(status == WindowObserverStatusOk || status == WindowObserverStatusMore);
        if (status == WindowObserverStatusOk) {
            break;
        }
    }
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 0u && stats.invalidations == 0u && stats.keys_processed == 300u);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.active_count == 10u && gs.retiring_count == 0u && gs.generations_issued == 10u);
    Require(drain_events(f) == 0u);

    /* A dropped untracked Destroy cannot be healed by refresh; overflow permanently poisons tracking. */
    for (uint64_t k = 0; k < 600u; k++) {
        hint(f, WindowObserverEventObjectDestroy, 0x60000u + k);
    }
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateTrackingFault && stats.hint_overflows != 0u);
    Require(do_pump(f, 64u) == WindowObserverStatusTrackingFault);
done:
    fixture_close(f);
    return result;
}

/** Overflow recovery still retires a window that is really gone, and documents the one undetected case. */
static int test_overflow_recovery_gone_and_recycled(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    uint64_t list[2] = {0x101u, 0x102u};
    Require(f != NULL);
    Require(add_window(f, 0x101u) != NULL);
    Require(add_window(f, 0x102u) != NULL);
    api_double_enum_set(f->dbl, list, 2u);
    f->dbl->tick = 5000u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 2u);

    /* Protected destroys survive ordinary-hint overflow; an identical tuple cannot erase a retained destroy. */
    f->dbl->tick = 6000u;
    api_double_remove(f->dbl, 0x101u);
    hint(f, WindowObserverEventObjectDestroy, 0x101u);
    hint(f, WindowObserverEventObjectDestroy, 0x102u);
    Require(add_window(f, 0x102u) != NULL); /* recreated: identical tuple */
    {
        uint64_t only[1] = {0x102u};
        api_double_enum_set(f->dbl, only, 1u);
    }
    for (uint64_t k = 0; k < WindowObserverDirtyCapacity + 1u; k++) {
        hint(f, WindowObserverEventObjectShow, 0x9000u + k);
    }
    Require(obs_stats(f, &stats) == 0);
    Require(stats.overflow_latched == 1u);
    f->dbl->tick = 6100u;
    Require(do_pump(f, 8u) == WindowObserverStatusOk);
    Require(is_live(f, 0x101u) == 0); /* gone: retired through the unseen requery */
    const uint32_t recovered_events = drain_events(f);
    Require(recovered_events >= 1u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].window == 0x101u);
    Require(is_live(f, 0x102u) == 1);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.generations_issued >= 2u && gs.active_count >= 1u);
done:
    fixture_close(f);
    return result;
}

/** query_facts hook: deliver one hint while the recovery refresh runs. */
static void hint_during_refresh_hook(api_double_t *dbl, uint64_t window, void *argument) {
    (void)argument;
    (void)window;
    api_double_hint(dbl, WindowObserverEventObjectShow, 0x7777u);
}

/**
 * R5: the statuses of pump's overflow path. EnumFailed is returned (latch kept, nothing retired); Ok only
 * when the table is empty on return, More when a hint arrived during the recovery refresh.
 */
static int test_overflow_pump_statuses(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    uint64_t list[1] = {0x101u};
    Require(f != NULL);
    Require(add_window(f, 0x101u) != NULL);
    api_double_enum_set(f->dbl, list, 1u);
    f->dbl->tick = 5000u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);

    /* Overflow with a failing enumeration: pump reports EnumFailed; the refresh stays latched. */
    for (uint64_t k = 0; k < WindowObserverDirtyCapacity + 1u; k++) {
        hint(f, WindowObserverEventObjectShow, 0x9000u + k);
    }
    f->dbl->enum_result = WindowObserverEnumFailed;
    f->dbl->monotonic += 10u;
    Require(do_pump(f, 8u) == WindowObserverStatusEnumFailed);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.overflow_latched == 0u && stats.dirty_pending == 0u && stats.refresh_requested == 1u);
    Require(stats.invalidations == 1u && stats.refreshes_started == 1u && stats.refreshes_committed == 1u);
    Require(is_live(f, 0x101u) == 1 && drain_events(f) == 0u);
    Require(window_observer_refresh_due(f->observer, f->dbl->monotonic) == 0);
    Require(window_observer_refresh_due(f->observer, f->dbl->monotonic + InteropWindowGraphRefreshIntervalMs) == 1);

    /* Overflow again, enumeration back: a hint delivered during the recovery's queries makes pump return More. */
    f->dbl->enum_result = WindowObserverEnumOk;
    for (uint64_t k = 0; k < WindowObserverDirtyCapacity + 1u; k++) {
        hint(f, WindowObserverEventObjectShow, 0xA000u + k);
    }
    f->dbl->query_hook = hint_during_refresh_hook;
    f->dbl->query_hook_once = 1u;
    f->dbl->monotonic += 10u;
    Require(do_pump(f, 8u) == WindowObserverStatusMore);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 1u && stats.overflow_latched == 0u && stats.refresh_requested == 0u);
    Require(stats.refreshes_committed == 2u);
    Require(do_pump(f, 8u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 0u);
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
    window_observer_stats_t stats, before;
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
    Require(obs_stats(f, &before) == 0);
    Require(interop_window_graph_drain(f->graph, TestEpoch, reentrant_sink, &probe, 1u, &delivered) ==
            InteropWindowGraphStatusOk);
    Require(delivered == 1u && probe.calls == 1u);
    Require(probe.pump == WindowObserverStatusReentrant);
    Require(probe.refresh == WindowObserverStatusReentrant);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.last_graph_status == before.last_graph_status);
    Require(stats.graph_errors == before.graph_errors); /* Reentrant skips the optional stats snapshot. */
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
    Require(f != NULL);
    Require(add_window(f, 0x100u) != NULL);
    hint(f, WindowObserverEventObjectCreate, 0x100u); /* takes the last stamp */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.next_stamp == StampSentinel);
    Require(do_pump(f, 4u) == WindowObserverStatusTrackingFault);
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
    hint(g, WindowObserverEventObjectCreate, 0x101u); /* bound stamp exhaustion is a permanent tracking fault */
    Require(obs_stats(g, &stats) == 0);
    Require(stats.state == WindowObserverStateTrackingFault && stats.hints_received == 1u && stats.hint_overflows == 0u);
    uint64_t list[1] = {0x101u};
    api_double_enum_set(g->dbl, list, 1u);
    Require(do_pump(g, 4u) == WindowObserverStatusTrackingFault);
    Require(do_refresh(g) == WindowObserverStatusTrackingFault);
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

/** A refresh left open by the bound graph is aborted and reopened before the observer commits. */
static int test_refresh_reopens_existing_transaction(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t graph_snapshot;
    const uint64_t enumerated[] = {0x31u};
    Require(f != NULL);
    Require(add_window(f, 0x31u) != NULL);
    api_double_enum_set(f->dbl, enumerated, 1u);
    Require(interop_window_graph_refresh_begin(f->graph, TestEpoch) == InteropWindowGraphStatusOk);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventAnnounce && event_log[0].window == 0x31u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_started == 2u && stats.refreshes_aborted == 1u &&
            stats.refreshes_committed == 1u);
    Require(graph_stats(f, &graph_snapshot) == 0);
    Require(graph_snapshot.refresh_active == 0u && graph_snapshot.refresh_commits == 1u);
done:
    fixture_close(f);
    return result;
}

/** Repeated native Destroy delivery during query forces the bounded refresh retry guard to fail closed. */
static void repeat_destroy_during_refresh_query(api_double_t *dbl, uint64_t window, void *argument) {
    (void)argument;
    dbl->query_hook = repeat_destroy_during_refresh_query;
    dbl->query_hook_once = 1u;
    api_double_hint(dbl, WindowObserverEventObjectDestroy, window);
}

static int test_refresh_repeated_destroy_retry_bound(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t graph_snapshot;
    const uint64_t enumerated[] = {0x32u};
    Require(f != NULL);
    Require(add_window(f, 0x32u) != NULL);
    api_double_enum_set(f->dbl, enumerated, 1u);
    f->dbl->query_hook = repeat_destroy_during_refresh_query;
    f->dbl->query_hook_once = 1u;
    f->dbl->query_hook_window = 0x32u;
    Require(do_refresh(f) == WindowObserverStatusTrackingFault);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateTrackingFault && stats.queries_exists > 1u);
    Require(graph_stats(f, &graph_snapshot) == 0);
    Require(graph_snapshot.refresh_active == 0u && graph_snapshot.active_count == 0u &&
            graph_snapshot.retiring_count == 0u);
    Require(drain_events(f) == 0u);
done:
    fixture_close(f);
    return result;
}

/** A reentrant Destroy on every query poisons the bounded pump without exposing stale facts. */
static int test_pump_repeated_destroy_retry_bound(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t graph_snapshot;
    Require(f != NULL);
    Require(add_window(f, 0x33u) != NULL);
    f->dbl->query_hook = repeat_destroy_during_refresh_query;
    f->dbl->query_hook_once = 1u;
    f->dbl->query_hook_window = 0x33u;
    hint(f, WindowObserverEventObjectShow, 0x33u);
    Require(do_pump(f, 1u) == WindowObserverStatusTrackingFault);
    Require(f->dbl->query_total == WindowObserverDestroyCapacity + 1u);
    Require(obs_stats(f, &stats) == 0 && stats.state == WindowObserverStateTrackingFault);
    Require(graph_stats(f, &graph_snapshot) == 0 && graph_snapshot.active_count == 0u);
    Require(drain_events(f) == 0u);
    Require(window_observer_sync_tracked(f->observer) == WindowObserverStatusTrackingFault);
    Require(window_observer_set_preedit_window(f->observer, 0x33u) == WindowObserverStatusTrackingFault);
    Require(window_observer_prepare_callbacks(f->observer, 0u, 0u) == WindowObserverStatusTrackingFault);
    Require(window_observer_drain_protected_destroys(f->observer) == WindowObserverStatusTrackingFault);
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusTrackingFault);
    Require(f->observer != NULL);
done:
    fixture_close(f);
    return result;
}

/** Binding never invalidates a graph when the proposed stamp clock cannot exceed its high-water mark. */
static int test_bind_rejects_used_stamp_clock_before_mutation(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    interop_window_graph_stats_t before, after;
    window_observer_t *other = NULL;
    Require(f != NULL);
    Require(add_window(f, 0x34u) != NULL);
    Require(admit(f, 0x34u) == 0);
    Require(graph_stats(f, &before) == 0 && before.stamp_high_water != 0u);
    window_observer_config_t config;
    api_double_config(&config, f->dbl, 0u, 1u);
    Require(window_observer_create(&config, &other) == WindowObserverStatusOk);
    Require(window_observer_bind_graph(other, f->graph, TestEpoch) == WindowObserverStatusInvalidArgument);
    Require(graph_stats(f, &after) == 0 && memcmp(&before, &after, sizeof(before)) == 0);
    Require(is_live(f, 0x34u) == 1);
    Require(window_observer_destroy(&other) == WindowObserverStatusOk && other == NULL);
done:
    if (other != NULL) (void)window_observer_destroy(&other);
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

/** A Failed enumeration returns EnumFailed before any graph call; a Truncated one is applied but never committed. */
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

    /* Truncated: the partial list lacks 0x11, which must stay live (requeried individually). The refresh
     * is begun, runs its offers and unseen requery, and is ABORTED instead of committed. */
    f->dbl->enum_result = WindowObserverEnumTruncated;
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusEnumFailed);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_started == 2u && stats.refreshes_aborted == 1u && stats.refreshes_committed == 1u);
    Require(stats.refresh_requested == 0u); /* the earlier commit cleared it; a truncated refresh sets nothing */
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
    Require(stats.refreshes_started == 2u && stats.refreshes_committed == 1u); /* Failed begins nothing */
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

/** Observer replacement invalidates old Active generations and cannot claim continuity. */
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
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(drain_events(f) == 2u); /* stop retires but never releases old graph generations */
    window_observer_quarantine_record_t quarantine;
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);

    f->observer = make_observer(f, high_water + 1u);
    Require(f->observer != NULL);
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    Require(do_refresh(f) == WindowObserverStatusOk);
    const uint32_t replacement_events = drain_events(f);
    Require(replacement_events <= 2u); /* Retiring handles cannot be readmitted without exact proof */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.graph_errors == 0u && stats.refreshes_committed == 1u);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.retiring_count == 2u && gs.generations_issued >= 2u);
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

/** create returns NoMemory and a NULL handle when its single allocation fails; nothing leaks (LSan). */
static int test_create_no_memory(void) {
    int result = 0;
#ifdef WgObsWrapCalloc
    api_double_t *dbl = api_double_new();
    window_observer_config_t config;
    window_observer_t *observer = NULL;
    Require(dbl != NULL);
    api_double_config(&config, dbl, 0u, 1u);

    const uint64_t calls_before = atomic_load(&g_wrapped_calloc_calls);
    observer = (window_observer_t *)dbl; /* non-NULL sentinel, never dereferenced */
    atomic_store(&g_wrapped_calloc_fail_next, 1u);
    Require(window_observer_create(&config, &observer) == WindowObserverStatusNoMemory);
    Require(observer == NULL);
    Require(atomic_load(&g_wrapped_calloc_fail_next) == 0u); /* the armed failure was consumed by create */
    Require(atomic_load(&g_wrapped_calloc_calls) == calls_before + 1u);

    /* Disarmed, the same config succeeds and the observer works. */
    Require(window_observer_create(&config, &observer) == WindowObserverStatusOk);
    Require(observer != NULL);
    Require(window_observer_destroy(&observer) == WindowObserverStatusOk);
done:
    atomic_store(&g_wrapped_calloc_fail_next, 0u);
    api_double_free(dbl);
#else
    printf("skip test_create_no_memory: built without -DWgObsWrapCalloc=1 and --wrap=calloc\n");
#endif
    return result;
}

/** Helper-thread body: one well-formed hint (the helper reports a foreign thread id). */
static void inflight_hint_body(void *argument) {
    fixture_t *f = (fixture_t *)argument;
    api_double_hint(f->dbl, WindowObserverEventObjectCreate, 0x42u);
}

/**
 * R7: the code that clears the process-wide slot waits (yielding) for a hint that is inside note_hint.
 * The helper thread parks inside current_thread_id, i.e. after note_hint registered itself and loaded the
 * slot, for 200 ms. destroy runs meanwhile on the owner thread; it must not return before the park
 * ends, and the hint then touches the still-live observer (a use-after-free here would be an
 * AddressSanitizer failure). This is the only test that runs the contended branch of release_slot.
 */
static int test_release_slot_waits_for_inflight_hint(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    api_double_helper_t helper;
    int helper_started = 0;
    Require(f != NULL);
    f->dbl->park_ms = 200u;
    Require(api_double_helper_start(f->dbl, 77u, inflight_hint_body, f, &helper) == 0);
    helper_started = 1;
    while (atomic_load_explicit(&f->dbl->parked, memory_order_acquire) == 0) {
        api_double_yield();
    }
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    window_observer_quarantine_record_t quarantine;
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(f->observer == NULL);
    Require(api_double_wall_ms() >= f->dbl->park_deadline_ms); /* destroy waited for the in-flight hint */
done:
    if (helper_started) {
        api_double_helper_join(&helper);
    }
    fixture_close(f);
    return result;
}

/** R8: a window that is not visible is classified NotVisible first, so its owner is never queried. */
static int test_hidden_window_skips_owner_query(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    api_double_window_t *hidden = NULL;
    api_double_window_t *owner = NULL;
    Require(f != NULL);
    owner = add_window(f, 0x410u);
    Require(owner != NULL);
    owner->mode = ApiDoubleModeFailed; /* an owner query would make the window's query Failed */
    hidden = add_owned(f, 0x411u, 0x410u);
    Require(hidden != NULL);
    set_visible(hidden, 0);
    hint(f, WindowObserverEventObjectCreate, 0x411u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_exists == 1u && stats.queries_failed == 0u && stats.query_retries_exhausted == 0u);
    Require(api_double_queries_of(f->dbl, 0x410u) == 0u); /* no owner sub-query */
    Require(is_live(f, 0x411u) == 0 && drain_events(f) == 0u);

    /* The same window, visible again, does query (and here fails on) its owner. */
    set_visible(hidden, 1);
    hint(f, WindowObserverEventObjectShow, 0x411u);
    Require(do_pump(f, 4u) == WindowObserverStatusMore);
    Require(api_double_queries_of(f->dbl, 0x410u) == 1u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_failed == 1u);
done:
    fixture_close(f);
    return result;
}

/**
 * R6: a Truncated enumeration is not a dead end. The listed windows are offered (admissions and updates
 * flow), unlisted Active windows are requeried one by one (updates and Gone notes flow), and the refresh
 * is aborted instead of committed, so absence from the partial list retires nothing.
 */
static int test_refresh_truncated_degrades(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    api_double_window_t *listed = NULL;
    api_double_window_t *unlisted = NULL;
    uint64_t list[WindowObserverMaxEnumerated];
    Require(f != NULL);
    listed = add_window(f, 0x10u);
    unlisted = add_window(f, 0x11u);
    Require(listed != NULL && unlisted != NULL);
    Require(add_window(f, 0x12u) != NULL);
    f->dbl->tick = 5000u;
    Require(admit(f, 0x11u) == 0);
    Require(admit(f, 0x12u) == 0);
    Require(drain_events(f) == 2u);

    /* 0x10 is new and listed (first in the truncated list, followed by filler handles that are Gone);
     * 0x11 is Active, unlisted and moved; 0x12 is Active, unlisted, and vanished with a lost destroy hint. */
    list[0] = 0x10u;
    for (uint32_t i = 1; i < WindowObserverMaxEnumerated; i++) {
        list[i] = 0x100000u + i;
    }
    api_double_enum_set(f->dbl, list, WindowObserverMaxEnumerated);
    f->dbl->enum_result = WindowObserverEnumTruncated;
    unlisted->facts.x = 400;
    api_double_remove(f->dbl, 0x12u);
    f->dbl->tick = 6000u;
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusEnumFailed);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_started == 1u && stats.refreshes_aborted == 1u && stats.refreshes_committed == 0u);
    Require(stats.queries_gone >= WindowObserverMaxEnumerated); /* 2047 fillers plus 0x12 */
    Require(graph_stats(f, &gs) == 0);
    Require(gs.refresh_active == 0u && gs.refresh_commits == 0u);
    Require(drain_events(f) == 3u);
    Require(find_event(InteropWindowEventAnnounce, 0x10u, 0u) >= 0); /* admitted without any hint */
    Require(find_event(InteropWindowEventUpdate, 0x11u, 0u) >= 0); /* updated through the unseen requery */
    Require(find_event(InteropWindowEventRetireIntent, 0x12u, 0u) >= 0); /* Gone note with a fresh tick */
    Require(is_live(f, 0x10u) == 1 && is_live(f, 0x11u) == 1 && is_live(f, 0x12u) == 0);

    /* A vanished window whose Gone note the destroy rule refuses stays (nothing is retired by absence). */
    api_double_remove(f->dbl, 0x11u);
    f->dbl->tick = 3000u; /* tick went backwards: the rule refuses */
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusEnumFailed);
    Require(is_live(f, 0x11u) == 1 && drain_events(f) == 0u);
    f->dbl->enum_result = WindowObserverEnumOk; /* a complete enumeration commits and retires it */
    api_double_enum_set(f->dbl, list, 1u);
    f->dbl->monotonic += 300u;
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(is_live(f, 0x11u) == 0);
done:
    fixture_close(f);
    return result;
}

/** R11: a config whose storage overlaps the out slot is copied before the slot is written. */
static int test_create_config_overlaps_slot(void) {
    int result = 0;
    api_double_t *dbl = api_double_new();
    window_observer_t *observer = NULL;
    union {
        window_observer_config_t config;
        window_observer_t *slot_view[5]; /* slot_view[0] shares the bytes of config.api */
    } overlap;
    Require(dbl != NULL);
    api_double_config(&overlap.config, dbl, 0u, 1u);
    Require(window_observer_create(&overlap.config, &overlap.slot_view[0]) == WindowObserverStatusOk);
    observer = overlap.slot_view[0];
    Require(observer != NULL);
    Require(window_observer_destroy(&observer) == WindowObserverStatusOk);

    /* A failing create through an overlapping slot still reports NULL in the slot. */
    api_double_config(&overlap.config, dbl, 0u, 1u);
    overlap.config.own_process_id = 0u;
    Require(window_observer_create(&overlap.config, &overlap.slot_view[0]) == WindowObserverStatusInvalidArgument);
    Require(overlap.slot_view[0] == NULL);
done:
    api_double_free(dbl);
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
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk);
    api_double_config(&config, f->dbl, 0u, 1u);
    config.preedit_window = 0x501u;
    Require(window_observer_create(&config, &f->observer) == WindowObserverStatusOk);
    Require(window_observer_bind_graph(f->observer, f->graph, TestEpoch) == WindowObserverStatusOk);
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

/** Quiesced handoff preserves queued hints and an open overflow latch. */
static int test_teardown_with_hints(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    Require(f != NULL);
    for (uint64_t i = 0; i < WindowObserverDirtyCapacity + 20u; i++) {
        hint(f, WindowObserverEventObjectCreate, 0x8000u + i);
    }
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    window_observer_quarantine_record_t quarantine;
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(f->observer == NULL);
    hint(f, WindowObserverEventObjectCreate, 0x1u); /* late hint after destroy */
    f->observer = make_observer(f, 1u);
    Require(f->observer != NULL);
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
done:
    fixture_close(f);
    return result;
}

/** Protected mailbox drain preserves exact Destroy and requires explicit retirement completion. */
static int test_protected_destroy_handoff_contract(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    uint32_t pending = 0xA5A5A5A5u;
    window_observer_quarantine_record_t quarantine;
    Require(f != NULL);
    Require(add_window(f, 0x880u) != NULL);
    Require(admit(f, 0x880u) == 0);
    Require(drain_events(f) == 1u);
    Require(window_observer_destroy_pending(f->observer, 0x880u, &pending) == WindowObserverStatusOk);
    Require(pending == 0u);

    hint(f, WindowObserverEventObjectDestroy, 0x880u);
    pending = 0u;
    Require(window_observer_destroy_pending(f->observer, 0x880u, &pending) == WindowObserverStatusOk);
    Require(pending == 1u);
    pending = 0xA5A5A5A5u;
    Require(window_observer_destroy_pending(f->observer, 0x880u,
            (uint32_t *)((unsigned char *)&pending + 1u)) == WindowObserverStatusInvalidArgument);
    Require(pending == 0xA5A5A5A5u);
    Require(window_observer_prepare_callbacks(f->observer, 0x880u, 2u) == WindowObserverStatusInvalidArgument);
    memset(&quarantine, 0xA5, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusProtectedStatePending);
    Require(((unsigned char *)&quarantine)[0] == 0xA5u);
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusProtectedStatePending);
    Require(f->observer != NULL && f->dbl->remove_calls == 0u);
    const uint32_t query_count = f->dbl->query_total;
    Require(window_observer_drain_protected_destroys(f->observer) == WindowObserverStatusOk);
    Require(f->dbl->query_total == query_count); /* protected drain never performs native queries */
    Require(window_observer_destroy_pending(f->observer, 0x880u, &pending) == WindowObserverStatusOk);
    Require(pending == 0u);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent && event_log[0].reason == InteropWindowReasonDestroyed);
    Require(complete_all_retiring(f) == 0);
    Require(window_observer_sync_tracked(f->observer) == WindowObserverStatusOk);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);

    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(f->observer == NULL);
    Require(quarantine.version == WindowObserverQuarantineVersion && quarantine.size == sizeof(quarantine));
    Require(quarantine.borrowed_graph == f->graph && quarantine.session_epoch == TestEpoch);
    Require(quarantine.observer_state == WindowObserverStateStopped && quarantine.protected_count == 0u);
    Require(quarantine.tracked_count == 0u && quarantine.tracked_snapshot_valid == 1u);

    /* A null output and a misaligned output are rejected without touching supplied bytes. */
    Require(window_observer_destroy_pending(NULL, 0x880u, &pending) == WindowObserverStatusInvalidArgument);
done:
    fixture_close(f);
    return result;
}

/** An untracked Destroy that cannot enter the ordinary queue poisons continuity instead of being dropped. */
static int test_untracked_destroy_capacity_fault(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    window_observer_quarantine_record_t quarantine;
    Require(f != NULL);

    /* Untracked Destroy evidence stays in the ordinary queue until a graph snapshot promotes it. */
    for (uint32_t index = 0; index < WindowObserverDirtyCapacity; ++index)
        hint(f, WindowObserverEventObjectDestroy, UINT64_C(0x100000) + index);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateStarted && stats.dirty_pending == WindowObserverDirtyCapacity);

    /* The next distinct destroy cannot be represented, so the observer poisons itself. */
    hint(f, WindowObserverEventObjectDestroy,
         UINT64_C(0x100000) + WindowObserverDirtyCapacity);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateTrackingFault);
    Require(stats.dirty_pending == WindowObserverDirtyCapacity);
    Require(window_observer_stop(f->observer) == WindowObserverStatusTrackingFault);
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(quarantine.tracking_fault == 1u && quarantine.evidence_incomplete == 1u);
    Require(quarantine.dirty_count == WindowObserverDirtyCapacity);
    Require(quarantine.pending_dirty[WindowObserverDirtyCapacity - 1u].window ==
            UINT64_C(0x100000) + WindowObserverDirtyCapacity - 1u);
done:
    fixture_close(f);
    return result;
}

/** Overflow refresh discards only non-Destroy hints and retains untracked exact evidence. */
static int test_overflow_retains_untracked_destroy_payloads(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    window_observer_quarantine_record_t quarantine;
    const uint32_t DestroyCount = 3u;
    Require(f != NULL);
    for (uint32_t index = 0u; index < DestroyCount; ++index)
        api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, UINT64_C(0xE00000) + index,
                             WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 2700u + index);
    for (uint32_t index = DestroyCount; index < WindowObserverDirtyCapacity; ++index)
        hint(f, WindowObserverEventObjectShow, UINT64_C(0xF00000) + index);
    hint(f, WindowObserverEventObjectShow, UINT64_C(0xFFFFFF));
    Require(obs_stats(f, &stats) == 0 && stats.overflow_latched == 1u);
    Require(do_pump(f, 1u) == WindowObserverStatusMore);
    Require(obs_stats(f, &stats) == 0 && stats.state == WindowObserverStateStarted);
    Require(stats.overflow_latched == 0u && stats.dirty_pending == DestroyCount);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(quarantine.dirty_count == DestroyCount && quarantine.protected_count == 0u);
    Require(quarantine.tracking_fault == 0u && quarantine.evidence_incomplete == 0u);
    for (uint32_t index = 0u; index < DestroyCount; ++index) {
        Require(quarantine.pending_dirty[index].window == UINT64_C(0xE00000) + index);
        Require(quarantine.pending_dirty[index].destroy_stamp != 0u);
        Require(quarantine.pending_dirty[index].destroy_event_ms == 2700u + index);
    }
done:
    fixture_close(f);
    return result;
}

/** The no-candidate and candidate admission reservations fit the maximum 64-slot graph. */
static int test_protected_admission_reserve_boundary(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    uint64_t windows[InteropMaxWindows];
    Require(f != NULL);
    for (uint32_t index = 0; index < InteropMaxWindows; ++index) {
        windows[index] = UINT64_C(0x200000) + index;
        Require(add_window(f, windows[index]) != NULL);
    }
    api_double_enum_set(f->dbl, windows, InteropMaxWindows);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(drain_events(f) == InteropMaxWindows);
    /* The candidate is reserved alongside all 64 tracked HWNDs, before any callback or graph call. */
    Require(window_observer_prepare_callbacks(f->observer, UINT64_C(0x300000), 1u) ==
            WindowObserverStatusOk);
    Require(window_observer_prepare_callbacks(f->observer, 0u, 0u) == WindowObserverStatusOk);
done:
    fixture_close(f);
    return result;
}

/** A full graph event ring makes stop retryable without changing lifecycle or dropping graph ownership. */
static int test_protected_stop_queue_full_retry(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t before, after;
    Require(f != NULL);
    Require(add_window(f, 0x8F0u) != NULL);
    Require(admit(f, 0x8F0u) == 0);
    Require(drain_events(f) == 1u);
    Require(fill_event_ring(f, 0x8F0u) == 0);
    Require(obs_stats(f, &before) == 0 && before.state == WindowObserverStateStarted);

    Require(window_observer_stop(f->observer) == WindowObserverStatusGraphQueueFull);
    Require(obs_stats(f, &after) == 0);
    Require(after.state == before.state && after.dirty_pending == before.dirty_pending);
    Require(f->dbl->remove_calls == 0u); /* invalidation must succeed before hooks are removed */

    Require(drain_events(f) == RingCapacity);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(obs_stats(f, &after) == 0 && after.state == WindowObserverStateStopped);
done:
    fixture_close(f);
    return result;
}

/** Protected drain retains its exact mailbox entry when graph output is full, then retries cleanly. */
static int test_protected_drain_queue_full_retry(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    uint32_t pending = 0u;
    Require(f != NULL);
    Require(add_window(f, 0x8F6u) != NULL);
    Require(admit(f, 0x8F6u) == 0);
    Require(drain_events(f) == 1u);
    Require(fill_event_ring(f, 0x8F6u) == 0);
    hint(f, WindowObserverEventObjectDestroy, 0x8F6u);
    Require(window_observer_destroy_pending(f->observer, 0x8F6u, &pending) == WindowObserverStatusOk);
    Require(pending == 1u);

    Require(window_observer_drain_protected_destroys(f->observer) == WindowObserverStatusGraphQueueFull);
    Require(window_observer_destroy_pending(f->observer, 0x8F6u, &pending) == WindowObserverStatusOk);
    Require(pending == 1u);
    Require(drain_events(f) == RingCapacity);
    Require(window_observer_drain_protected_destroys(f->observer) == WindowObserverStatusOk);
    Require(window_observer_destroy_pending(f->observer, 0x8F6u, &pending) == WindowObserverStatusOk);
    Require(pending == 0u);
    Require(drain_events(f) == 1u);
    Require(event_log[0].kind == InteropWindowEventRetireIntent &&
            event_log[0].reason == InteropWindowReasonDestroyed);
    Require(complete_all_retiring(f) == 0);
    Require(window_observer_sync_tracked(f->observer) == WindowObserverStatusOk);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
done:
    fixture_close(f);
    return result;
}

/** A later Destroy callback replaces the exact mailbox stamp while a query's stale facts are discarded. */
static void replace_destroy_during_query(api_double_t *dbl, uint64_t window, void *argument) {
    (void)argument;
    api_double_hint_full(dbl, WindowObserverEventObjectDestroy, window,
                         WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 1700u);
    api_double_hint_full(dbl, WindowObserverEventObjectDestroy, window,
                         WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 1701u);
}

static int test_protected_destroy_exact_stamp_replacement(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_quarantine_record_t quarantine;
    uint32_t pending = 0u;
    Require(f != NULL);
    Require(add_window(f, 0x8F1u) != NULL);
    Require(admit(f, 0x8F1u) == 0);
    Require(drain_events(f) == 1u);
    Require(window_observer_prepare_callbacks(f->observer, 0x8F1u, 0u) == WindowObserverStatusOk);
    Require(fill_event_ring(f, 0x8F1u) == 0);

    f->dbl->query_hook = replace_destroy_during_query;
    f->dbl->query_hook_once = 1u;
    hint(f, WindowObserverEventObjectShow, 0x8F1u);
    Require(do_pump(f, 1u) == WindowObserverStatusGraphQueueFull);
    Require(window_observer_destroy_pending(f->observer, 0x8F1u, &pending) == WindowObserverStatusOk);
    Require(pending == 1u);
    Require(window_observer_prepare_callbacks(f->observer, 0x8F2u, 0u) == WindowObserverStatusGraphQueueFull);
    Require(drain_events(f) == RingCapacity);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);

    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(quarantine.protected_count == 1u);
    Require(quarantine.candidate_window == 0x8F1u); /* failed preflight did not replace the old reservation */
    Require(quarantine.protected_destroys[0].window == 0x8F1u);
    Require(quarantine.protected_destroys[0].event_time_ms == 1701u);
done:
    fixture_close(f);
    return result;
}

/** A mailbox member keeps its identity after external retirement and merges a later exact Destroy stamp. */
static int test_repeated_destroy_after_tracked_removal(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    window_observer_quarantine_record_t quarantine;
    interop_window_observation_t observation;
    interop_window_event_t event;
    uint32_t pending = 0u;
    Require(f != NULL);
    Require(add_window(f, 0x8F3u) != NULL);
    Require(admit(f, 0x8F3u) == 0);
    Require(drain_events(f) == 1u);

    api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, 0x8F3u,
                         WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 1700u);
    Require(window_observer_destroy_pending(f->observer, 0x8F3u, &pending) == WindowObserverStatusOk);
    Require(pending == 1u);

    /* Model an independent graph owner completing exact retirement; this proves graph bookkeeping only. */
    Require(obs_stats(f, &stats) == 0);
    memset(&observation, 0, sizeof(observation));
    Require(interop_window_graph_note_destroyed(f->graph, TestEpoch, 0x8F3u, stats.next_stamp,
                                                (uint64_t)f->dbl->tick + 1u, &observation) ==
            InteropWindowGraphStatusOk);
    Require(interop_window_graph_next_event(f->graph, TestEpoch, &event) == InteropWindowGraphStatusOk);
    Require(event.kind == InteropWindowEventRetireIntent && event.window == 0x8F3u);
    const interop_window_retire_proof_t proof = {1u, 1u};
    Require(interop_window_graph_complete_retire(f->graph, TestEpoch, event.window, event.generation,
                                                 &proof) == InteropWindowGraphStatusOk);
    Require(window_observer_sync_tracked(f->observer) == WindowObserverStatusOk);

    api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, 0x8F3u,
                         WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 1701u);
    Require(window_observer_destroy_pending(f->observer, 0x8F3u, &pending) == WindowObserverStatusOk);
    Require(pending == 1u);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(quarantine.protected_count == 1u);
    Require(quarantine.protected_destroys[0].window == 0x8F3u);
    Require(quarantine.protected_destroys[0].event_time_ms == 1701u);
done:
    fixture_close(f);
    return result;
}

/** Reenter the graph clock while old and proposed candidate reservations change. */
static void transition_clock_destroys(fixture_t *f) {
    f->graph_clock_calls += 1u;
    if (f->graph_clock_calls == 1u) {
        api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, f->clock_old_candidate,
                             WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 2301u);
    } else if (f->graph_clock_calls == 2u) {
        api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, f->clock_new_candidate,
                             WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 2302u);
    }
}

/** Full mailbox transitions retain the old key until the new key has admission headroom. */
static int test_full_candidate_transition_retains_both_keys(void) {
    int result = 0;
    fixture_t *f = fixture_open_ex(0u, 1u, 1, 1);
    window_observer_quarantine_record_t quarantine;
    window_observer_stats_t stats;
    const uint64_t base = UINT64_C(0xA00000);
    Require(f != NULL);
    /* Full non-Destroy hints prevent optional dirty mirrors; all Destroy evidence has its own mailbox. */
    for (uint32_t index = 0; index < WindowObserverDirtyCapacity; ++index)
        hint(f, WindowObserverEventObjectShow, UINT64_C(0xB00000) + index);
    for (uint32_t index = 0; index < WindowObserverDestroyCapacity; ++index) {
        Require(window_observer_prepare_callbacks(f->observer, base + index, 0u) == WindowObserverStatusOk);
        hint(f, WindowObserverEventObjectDestroy, base + index);
    }
    f->clock_old_candidate = base + WindowObserverDestroyCapacity - 1u;
    f->clock_new_candidate = base + WindowObserverDestroyCapacity;
    f->graph_clock_hook = transition_clock_destroys;
    Require(window_observer_prepare_callbacks(f->observer, f->clock_new_candidate, 1u) ==
            WindowObserverStatusOk);
    f->graph_clock_hook = NULL;
    Require(f->graph_clock_calls >= 2u);
    uint32_t pending = 0u;
    Require(window_observer_destroy_pending(f->observer, f->clock_old_candidate, &pending) ==
            WindowObserverStatusOk && pending == 1u);
    Require(window_observer_destroy_pending(f->observer, f->clock_new_candidate, &pending) ==
            WindowObserverStatusOk && pending == 1u);
    Require(obs_stats(f, &stats) == 0 && stats.state == WindowObserverStateStarted);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(quarantine.tracking_fault == 0u && quarantine.evidence_incomplete == 0u);
    Require(quarantine.candidate_window == f->clock_new_candidate);
    Require(quarantine.protected_count == 65u);
    Require(quarantine.dirty_count == WindowObserverDirtyCapacity);
    int old_found = 0, new_found = 0;
    for (uint32_t index = 0; index < quarantine.protected_count; ++index) {
        const window_observer_destroy_entry_t *entry = &quarantine.protected_destroys[index];
        if (entry->window == f->clock_old_candidate) old_found = entry->event_time_ms == 2301u;
        if (entry->window == f->clock_new_candidate) new_found = entry->event_time_ms == 2302u;
    }
    Require(old_found && new_found);
done:
    if (f != NULL) f->graph_clock_hook = NULL;
    fixture_close(f);
    return result;
}

/** Keep the exact first mailbox entry occupied while the graph clock reenters Destroy. */
static void repeat_front_destroy_during_clock(fixture_t *f) {
    f->graph_clock_calls += 1u;
    api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, f->clock_old_candidate,
                         WindowObserverObjectIdWindow, WindowObserverChildIdSelf,
                         2500u + f->graph_clock_calls);
}

/** Reentrant replacement exhausts only the work budget; neither evidence nor reservation is lost. */
static int test_full_candidate_transition_bounded_pending(void) {
    int result = 0;
    fixture_t *f = fixture_open_ex(0u, 1u, 1, 1);
    window_observer_quarantine_record_t quarantine;
    window_observer_stats_t stats;
    const uint64_t base = UINT64_C(0xC00000);
    Require(f != NULL);
    for (uint32_t index = 0; index < WindowObserverDirtyCapacity; ++index)
        hint(f, WindowObserverEventObjectShow, UINT64_C(0xD00000) + index);
    for (uint32_t index = 0; index < WindowObserverDestroyCapacity; ++index) {
        Require(window_observer_prepare_callbacks(f->observer, base + index, 0u) == WindowObserverStatusOk);
        hint(f, WindowObserverEventObjectDestroy, base + index);
    }
    f->clock_old_candidate = base;
    f->graph_clock_hook = repeat_front_destroy_during_clock;
    Require(window_observer_prepare_callbacks(f->observer, base + WindowObserverDestroyCapacity, 0u) ==
            WindowObserverStatusProtectedStatePending);
    Require(f->graph_clock_calls == WindowObserverDestroyCapacity);
    Require(window_observer_drain_protected_destroys(f->observer) == WindowObserverStatusProtectedStatePending);
    Require(f->graph_clock_calls == 2u * WindowObserverDestroyCapacity);
    f->graph_clock_hook = NULL;
    Require(obs_stats(f, &stats) == 0 && stats.state == WindowObserverStateStarted);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(quarantine.tracking_fault == 0u && quarantine.evidence_incomplete == 0u);
    Require(quarantine.candidate_window == base + WindowObserverDestroyCapacity - 1u);
    Require(quarantine.protected_count == WindowObserverDestroyCapacity);
    Require(quarantine.protected_destroys[0].window == base);
    Require(quarantine.protected_destroys[0].event_time_ms == 2500u + f->graph_clock_calls);
    Require(quarantine.dirty_count == WindowObserverDirtyCapacity);
done:
    if (f != NULL) f->graph_clock_hook = NULL;
    fixture_close(f);
    return result;
}

typedef struct candidate_transition_destroy_t {
    uint64_t old_candidate;
    uint64_t proposed_candidate;
} candidate_transition_destroy_t;

static void destroy_candidates_during_query(api_double_t *dbl, uint64_t window, void *argument) {
    (void)window;
    const candidate_transition_destroy_t *candidates = (const candidate_transition_destroy_t *)argument;
    api_double_hint_full(dbl, WindowObserverEventObjectDestroy, candidates->old_candidate,
                         WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 1800u);
    api_double_hint_full(dbl, WindowObserverEventObjectDestroy, candidates->old_candidate,
                         WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 1801u);
    api_double_hint_full(dbl, WindowObserverEventObjectDestroy, candidates->proposed_candidate,
                         WindowObserverObjectIdWindow, WindowObserverChildIdSelf, 1802u);
}

/** A query callback Destroy for the proposed candidate enters the mailbox before admission. */
static int test_candidate_transition_protects_proposed_key(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    window_observer_quarantine_record_t quarantine;
    interop_window_observation_t observation;
    interop_window_event_t event;
    candidate_transition_destroy_t candidates = {0x8F4u, 0x8F5u};
    uint32_t pending = 0u;
    Require(f != NULL);
    Require(add_window(f, candidates.old_candidate) != NULL);
    Require(admit(f, candidates.old_candidate) == 0);
    Require(drain_events(f) == 1u);
    Require(window_observer_prepare_callbacks(f->observer, candidates.old_candidate, 0u) ==
            WindowObserverStatusOk);

    /* An external graph owner retires the old key; the observer keeps its candidate reservation. */
    Require(obs_stats(f, &stats) == 0);
    memset(&observation, 0, sizeof(observation));
    Require(interop_window_graph_note_destroyed(f->graph, TestEpoch, candidates.old_candidate,
                                                stats.next_stamp, (uint64_t)f->dbl->tick + 1u,
                                                &observation) == InteropWindowGraphStatusOk);
    Require(interop_window_graph_next_event(f->graph, TestEpoch, &event) == InteropWindowGraphStatusOk);
    Require(event.kind == InteropWindowEventRetireIntent && event.window == candidates.old_candidate);
    const interop_window_retire_proof_t proof = {1u, 1u};
    Require(interop_window_graph_complete_retire(f->graph, TestEpoch, event.window, event.generation,
                                                 &proof) == InteropWindowGraphStatusOk);
    Require(window_observer_sync_tracked(f->observer) == WindowObserverStatusOk);

    Require(add_window(f, candidates.proposed_candidate) != NULL);
    api_double_find(f->dbl, candidates.proposed_candidate)->fail_count = 1u;
    f->dbl->query_hook = destroy_candidates_during_query;
    f->dbl->query_hook_arg = &candidates;
    f->dbl->query_hook_window = candidates.proposed_candidate;
    f->dbl->query_hook_once = 1u;
    hint(f, WindowObserverEventObjectCreate, candidates.proposed_candidate);
    Require(do_pump(f, 1u) == WindowObserverStatusMore);
    Require(window_observer_destroy_pending(f->observer, candidates.old_candidate, &pending) ==
            WindowObserverStatusOk);
    Require(pending == 0u);
    Require(window_observer_destroy_pending(f->observer, candidates.proposed_candidate, &pending) ==
            WindowObserverStatusOk);
    Require(pending == 0u); /* the query-raced Destroy was accepted before this HWND could be admitted. */

    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(quarantine.protected_count == 0u);
    int old_destroy_retained = 0;
    for (uint32_t index = 0u; index < quarantine.dirty_count; ++index) {
        if (quarantine.pending_dirty[index].window == candidates.old_candidate &&
            quarantine.pending_dirty[index].destroy_event_ms == 1801u) {
            old_destroy_retained = 1;
        }
    }
    Require(old_destroy_retained); /* stale untracked Destroy remains losslessly queued. */
done:
    fixture_close(f);
    return result;
}

/** One scripted hint callback that fires once from hook installation or removal. */
typedef struct observer_hint_script_t {
    uint64_t window;
    uint32_t event_time_ms;
    uint32_t fired;
} observer_hint_script_t;

static void observer_hint_during_hook(api_double_t *dbl, void *argument) {
    observer_hint_script_t *script = (observer_hint_script_t *)argument;
    if (script->fired == 0u) {
        script->fired = 1u;
        api_double_hint_full(dbl, WindowObserverEventObjectDestroy, script->window,
                             WindowObserverObjectIdWindow, WindowObserverChildIdSelf,
                             script->event_time_ms);
    }
}

/** Starting and failed rollback keep callbacks published and retain exact protected evidence. */
static int test_protected_destroy_during_start_rollback(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_quarantine_record_t quarantine;
    Require(f != NULL);
    Require(add_window(f, 0x8F2u) != NULL);
    Require(admit(f, 0x8F2u) == 0);
    Require(drain_events(f) == 1u);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(drain_events(f) == 1u); /* invalidation event; the tracked snapshot remains Retiring */

    observer_hint_script_t script = {0x8F2u, 1802u, 0u};
    f->dbl->install_hook_fn = observer_hint_during_hook;
    f->dbl->install_hook_arg = &script;
    f->dbl->install_fail_at = f->dbl->install_calls + 2u;
    Require(window_observer_start(f->observer) == WindowObserverStatusHookFailed);
    Require(script.fired == 1u && f->dbl->remove_calls == 4u);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(quarantine.observer_state == WindowObserverStateStopped && quarantine.protected_count == 1u);
    Require(quarantine.protected_destroys[0].window == 0x8F2u);
    Require(quarantine.protected_destroys[0].event_time_ms == 1802u);
done:
    fixture_close(f);
    return result;
}

/** A Destroy during failed unhook remains retained across the stop retry. */
static int test_protected_destroy_during_unhook_retry(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_quarantine_record_t quarantine;
    Require(f != NULL);
    Require(add_window(f, 0x8F3u) != NULL);
    Require(admit(f, 0x8F3u) == 0);
    Require(drain_events(f) == 1u);

    observer_hint_script_t script = {0x8F3u, 1803u, 0u};
    f->dbl->remove_hook_fn = observer_hint_during_hook;
    f->dbl->remove_hook_arg = &script;
    f->dbl->remove_fail_at = 1u;
    Require(window_observer_stop(f->observer) == WindowObserverStatusUnhookFailed);
    Require(script.fired == 1u);
    f->dbl->remove_fail_at = 0u;
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(quarantine.observer_state == WindowObserverStateStopped && quarantine.protected_count == 1u);
    Require(quarantine.protected_destroys[0].window == 0x8F3u);
    Require(quarantine.protected_destroys[0].event_time_ms == 1803u);
done:
    fixture_close(f);
    return result;
}

/** Stamp exhaustion from the final start clock callback cannot be overwritten by Started publication. */
static void exhaust_stamp_during_start_clock(api_double_t *dbl, void *argument) {
    uint32_t *fired = (uint32_t *)argument;
    if (*fired == 0u) {
        *fired = 1u;
        api_double_hint(dbl, WindowObserverEventObjectDestroy, 0x8F6u);
        api_double_hint(dbl, WindowObserverEventObjectDestroy, 0x8F7u);
    }
}

static int test_start_final_clock_fault_rolls_back(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, UINT64_MAX - 1u, 0);
    window_observer_stats_t stats;
    window_observer_quarantine_record_t quarantine;
    uint32_t fired = 0u;
    Require(f != NULL);
    f->dbl->monotonic_hook = exhaust_stamp_during_start_clock;
    f->dbl->monotonic_hook_arg = &fired;
    Require(window_observer_start(f->observer) == WindowObserverStatusTrackingFault);
    Require(fired == 1u && f->dbl->remove_calls == 3u);
    memset(&stats, 0, sizeof(stats));
    Require(window_observer_status(f->observer, &stats) == WindowObserverStatusOk);
    Require(stats.state == WindowObserverStateTrackingFault);
    memset(&quarantine, 0, sizeof(quarantine));
    Require(window_observer_quarantine_handoff(&f->observer, &quarantine) == WindowObserverStatusOk);
    Require(f->observer == NULL && quarantine.tracking_fault == 1u && quarantine.evidence_incomplete == 1u);
done:
    fixture_close(f);
    return result;
}

/** A fault from tick_ms is observed before query and cannot admit the already-popped key. */
static void exhaust_stamp_during_tick(api_double_t *dbl, void *argument) {
    uint32_t *fired = (uint32_t *)argument;
    if (*fired == 0u) {
        *fired = 1u;
        dbl->tick_hook = NULL;
        api_double_hint(dbl, WindowObserverEventObjectDestroy, 0x8F8u);
    }
}

static int test_tick_callback_fault_prevents_graph_mutation(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, UINT64_MAX - 2u, 1);
    window_observer_stats_t stats;
    uint32_t fired = 0u;
    Require(f != NULL);
    Require(add_window(f, 0x8F9u) != NULL);
    api_double_hint(f->dbl, WindowObserverEventObjectCreate, 0x8F9u); /* consumes MAX-2 */
    f->dbl->tick_hook = exhaust_stamp_during_tick;
    f->dbl->tick_hook_arg = &fired;
    Require(do_pump(f, 1u) == WindowObserverStatusTrackingFault);
    Require(fired == 1u && f->dbl->tick_calls == 1u && f->dbl->query_total == 0u);
    memset(&stats, 0, sizeof(stats));
    Require(window_observer_status(f->observer, &stats) == WindowObserverStatusOk);
    Require(stats.state == WindowObserverStateTrackingFault && stats.dirty_pending == 1u);
    Require(is_live(f, 0x8F9u) == 0);
done:
    fixture_close(f);
    return result;
}

/** A query callback that runs out of stamps after facts are captured. */
static void exhaust_stamp_during_query(api_double_t *dbl, uint64_t window, void *argument) {
    (void)window;
    uint32_t *fired = (uint32_t *)argument;
    if (*fired == 0u) {
        *fired = 1u;
        api_double_hint(dbl, WindowObserverEventObjectDestroy, 0x8FAu);
    }
}

static int test_query_callback_fault_prevents_graph_mutation(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, UINT64_MAX - 2u, 1);
    window_observer_stats_t stats;
    uint32_t fired = 0u;
    Require(f != NULL);
    Require(add_window(f, 0x8FBu) != NULL);
    api_double_hint(f->dbl, WindowObserverEventObjectCreate, 0x8FBu); /* consumes MAX-2 */
    f->dbl->query_hook = exhaust_stamp_during_query;
    f->dbl->query_hook_arg = &fired;
    f->dbl->query_hook_window = 0x8FBu;
    f->dbl->query_hook_once = 1u;
    Require(do_pump(f, 1u) == WindowObserverStatusTrackingFault);
    Require(fired == 1u && f->dbl->query_total == 1u);
    memset(&stats, 0, sizeof(stats));
    Require(window_observer_status(f->observer, &stats) == WindowObserverStatusOk);
    Require(stats.state == WindowObserverStateTrackingFault && stats.dirty_pending == 1u);
    Require(is_live(f, 0x8FBu) == 0);
done:
    fixture_close(f);
    return result;
}

/** A Destroy fired by the graph's app-key callback is promoted after a Deferred child becomes tracked. */
static int test_protected_destroy_during_deferred_promotion(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    uint64_t windows[2] = {0x8F5u, 0x8F4u};
    Require(f != NULL);
    Require(add_window(f, 0x8F4u) != NULL);
    Require(add_owned(f, 0x8F5u, 0x8F4u) != NULL);
    api_double_enum_set(f->dbl, windows, 2u); /* child is offered before its owner */
    f->lookup_destroy_trigger_window = 0x8F4u;
    f->lookup_destroy_window = 0x8F5u;
    f->lookup_destroy_once = 1u;

    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(f->lookup_destroy_once == 0u);
    Require(is_live(f, 0x8F4u) == 1);
    Require(is_live(f, 0x8F5u) == 0); /* promoted child was destroyed before refresh returned */
    Require(drain_events(f) == 3u);
    Require(event_log[0].kind == InteropWindowEventAnnounce && event_log[0].window == 0x8F4u);
    Require(event_log[0].kind == InteropWindowEventAnnounce && event_log[0].window == 0x8F4u);
    Require(event_log[1].kind == InteropWindowEventAnnounce && event_log[1].window == 0x8F5u);
    Require(event_log[2].kind == InteropWindowEventRetireIntent && event_log[2].window == 0x8F5u);
    Require(event_log[2].reason == InteropWindowReasonDestroyed);
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

    /* The immutable binding rejects a wrong epoch before stats, graph or queued evidence changes. */
    hint(f, WindowObserverEventObjectShow, 0xB0u);
    memset(&stats, 0xA5, sizeof(stats));
    Require(window_observer_pump(f->observer, f->graph, TestEpoch + 1u, 8u, &stats) == WindowObserverStatusGraphBindingMismatch);
    Require(((unsigned char *)&stats)[0] == 0xA5u);
    Require(window_observer_refresh(f->observer, f->graph, TestEpoch + 1u, &stats) == WindowObserverStatusGraphBindingMismatch);
    Require(do_pump(f, 8u) == WindowObserverStatusOk); /* the right epoch processes the kept key */
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 0u && stats.graph_errors == 0u);

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
/* Coverage closure: argument, state, thread, merge, refresh and cadence corner cases         */
/* ------------------------------------------------------------------------------------------ */

/** Results of every entry point called from a foreign thread. */
typedef struct foreign_probe_t {
    fixture_t *f; /**< Fixture under test. */
    window_observer_status_t pump; /**< pump result. */
    window_observer_status_t refresh; /**< refresh result. */
    window_observer_status_t start; /**< start result. */
    window_observer_status_t stop; /**< stop result. */
    window_observer_status_t preedit; /**< set_preedit_window result. */
    window_observer_status_t status; /**< status result. */
    window_observer_status_t sync_tracked; /**< sync_tracked result. */
    window_observer_status_t prepare_callbacks; /**< prepare_callbacks result. */
    window_observer_status_t destroy_pending; /**< destroy_pending result. */
    window_observer_status_t drain_protected; /**< drain_protected result. */
    window_observer_status_t quarantine_handoff; /**< quarantine_handoff result. */
    window_observer_status_t destroy; /**< destroy result. */
    window_observer_t *slot; /**< Handle copy passed to destroy. */
    uint32_t pending; /**< Sentinel output passed to destroy_pending. */
    window_observer_quarantine_record_t quarantine; /**< Sentinel output passed to handoff. */
    int due; /**< refresh_due result. */
    uint32_t timeout; /**< next_timeout_ms result. */
} foreign_probe_t;

/** Helper-thread body: call every owner-thread entry point once. */
static void foreign_job(void *argument) {
    foreign_probe_t *p = (foreign_probe_t *)argument;
    window_observer_stats_t stats;
    p->pump = window_observer_pump(p->f->observer, p->f->graph, TestEpoch, 1u, NULL);
    p->refresh = window_observer_refresh(p->f->observer, p->f->graph, TestEpoch, NULL);
    p->start = window_observer_start(p->f->observer);
    p->stop = window_observer_stop(p->f->observer);
    p->preedit = window_observer_set_preedit_window(p->f->observer, 0x55u);
    p->status = window_observer_status(p->f->observer, &stats);
    p->sync_tracked = window_observer_sync_tracked(p->f->observer);
    p->prepare_callbacks = window_observer_prepare_callbacks(p->f->observer, 0x55u, 1u);
    p->pending = 0xA5A5A5A5u;
    p->destroy_pending = window_observer_destroy_pending(p->f->observer, 0x55u, &p->pending);
    p->drain_protected = window_observer_drain_protected_destroys(p->f->observer);
    memset(&p->quarantine, 0xA5, sizeof(p->quarantine));
    p->slot = p->f->observer;
    p->quarantine_handoff = window_observer_quarantine_handoff(&p->slot, &p->quarantine);
    p->slot = p->f->observer;
    p->destroy = window_observer_destroy(&p->slot);
    p->due = window_observer_refresh_due(p->f->observer, 100000u);
    p->timeout = window_observer_next_timeout_ms(p->f->observer, 100000u);
}

/** Verify a foreign_probe_t where every call answered WrongThread (and nothing was freed). */
static int expect_all_wrong_thread(const foreign_probe_t *p, const fixture_t *f) {
    Check(p->pump == WindowObserverStatusWrongThread && p->refresh == WindowObserverStatusWrongThread);
    Check(p->start == WindowObserverStatusWrongThread && p->stop == WindowObserverStatusWrongThread);
    Check(p->preedit == WindowObserverStatusWrongThread && p->status == WindowObserverStatusWrongThread);
    Check(p->sync_tracked == WindowObserverStatusWrongThread);
    Check(p->prepare_callbacks == WindowObserverStatusWrongThread);
    Check(p->destroy_pending == WindowObserverStatusWrongThread && p->pending == 0xA5A5A5A5u);
    Check(p->drain_protected == WindowObserverStatusWrongThread);
    Check(p->quarantine_handoff == WindowObserverStatusWrongThread && p->slot == f->observer);
    Check(((const unsigned char *)&p->quarantine)[0] == 0xA5u);
    Check(p->destroy == WindowObserverStatusWrongThread && p->slot == f->observer);
    Check(p->due == 0 && p->timeout == WindowObserverWaitForever);
    return 0;
}

/** NULL / zero arguments on every entry point, and WrongThread on every entry point in two states. */
static int test_entry_arguments_and_threads(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 0);
    window_observer_stats_t stats;
    foreign_probe_t probe;
    Require(f != NULL);
    Require(window_observer_start(NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_stop(NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_set_preedit_window(NULL, 1u) == WindowObserverStatusInvalidArgument);
    Require(window_observer_status(NULL, &stats) == WindowObserverStatusInvalidArgument);
    Require(window_observer_status(f->observer, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_destroy(NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_pump(NULL, f->graph, TestEpoch, 1u, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_pump(f->observer, NULL, TestEpoch, 1u, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_pump(f->observer, f->graph, TestEpoch, 0u, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_refresh(NULL, f->graph, TestEpoch, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_refresh(f->observer, NULL, TestEpoch, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_refresh_due(NULL, 0u) == 0);
    Require(window_observer_next_timeout_ms(NULL, 0u) == WindowObserverWaitForever);

    /* Created: BadState is a local error; optional stats bytes are unchanged. */
    memset(&stats, 0xAB, sizeof(stats));
    Require(window_observer_pump(f->observer, f->graph, TestEpoch, 1u, &stats) == WindowObserverStatusBadState);
    { unsigned char expected[sizeof(stats)]; memset(expected, 0xAB, sizeof(expected));
      Require(memcmp(&stats, expected, sizeof(stats)) == 0); }
    memset(&stats, 0xAB, sizeof(stats));
    Require(window_observer_refresh(f->observer, f->graph, TestEpoch, &stats) == WindowObserverStatusBadState);
    { unsigned char expected[sizeof(stats)]; memset(expected, 0xAB, sizeof(expected));
      Require(memcmp(&stats, expected, sizeof(stats)) == 0); }
    Require(window_observer_refresh_due(f->observer, 100000u) == 0);
    Require(window_observer_next_timeout_ms(f->observer, 100000u) == WindowObserverWaitForever);
    memset(&probe, 0, sizeof(probe));
    probe.f = f;
    Require(api_double_run_on_other_thread(f->dbl, 9u, foreign_job, &probe) == 0);
    Require(expect_all_wrong_thread(&probe, f) == 0);

    /* Started. */
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    memset(&probe, 0, sizeof(probe));
    probe.f = f;
    Require(api_double_run_on_other_thread(f->dbl, 9u, foreign_job, &probe) == 0);
    Require(expect_all_wrong_thread(&probe, f) == 0);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateStarted && stats.pumps_desktop_inactive == 0u);

    /* Stopped: BadState for pump and refresh, a preedit change stays a plain store. */
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(do_pump(f, 1u) == WindowObserverStatusBadState);
    Require(do_refresh(f) == WindowObserverStatusBadState);
    Require(window_observer_set_preedit_window(f->observer, 0x66u) == WindowObserverStatusOk);
    Require(window_observer_refresh_due(f->observer, 100000u) == 0);
    Require(window_observer_next_timeout_ms(f->observer, 100000u) == WindowObserverWaitForever);

    /* Failed (unhook failure): no pumping/restart; stop retries the retained hook. */
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    f->dbl->remove_fail_at = f->dbl->remove_calls + 1u;
    Require(window_observer_stop(f->observer) == WindowObserverStatusUnhookFailed);
    Require(window_observer_start(f->observer) == WindowObserverStatusBadState);
    Require(do_pump(f, 1u) == WindowObserverStatusBadState);
    Require(do_refresh(f) == WindowObserverStatusBadState);
    Require(window_observer_refresh_due(f->observer, 100000u) == 0);
    Require(window_observer_next_timeout_ms(f->observer, 100000u) == WindowObserverWaitForever);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
done:
    fixture_close(f);
    return result;
}

/** O5 public APIs reject bad spans, phases, and aliases before touching outputs or ownership. */
static int test_o5_public_guard_matrix(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 0);
    window_observer_t *unbound = NULL;
    window_observer_t *alias_slot = NULL;
    window_observer_config_t config;
    uint32_t pending = 0xA5A5A5A5u;
    window_observer_quarantine_record_t record;
    unsigned char expected[sizeof(record)];
    Require(f != NULL);

    Require(window_observer_bind_graph(NULL, f->graph, TestEpoch) == WindowObserverStatusInvalidArgument);
    Require(window_observer_bind_graph(f->observer, NULL, TestEpoch) == WindowObserverStatusInvalidArgument);
    Require(window_observer_bind_graph(f->observer, f->graph, 0u) == WindowObserverStatusInvalidArgument);
    Require(window_observer_bind_graph(f->observer, f->graph, TestEpoch) == WindowObserverStatusBadState);
    Require(window_observer_sync_tracked(NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_sync_tracked(f->observer) == WindowObserverStatusOk);
    Require(window_observer_prepare_callbacks(NULL, 0u, 0u) == WindowObserverStatusInvalidArgument);
    Require(window_observer_prepare_callbacks(f->observer, 0u, 2u) == WindowObserverStatusInvalidArgument);
    Require(window_observer_prepare_callbacks(f->observer, 0u, 0u) == WindowObserverStatusBadState);
    Require(window_observer_destroy_pending(NULL, 1u, &pending) == WindowObserverStatusInvalidArgument);
    Require(window_observer_destroy_pending(f->observer, 0u, &pending) == WindowObserverStatusInvalidArgument);
    Require(window_observer_destroy_pending(f->observer, 1u, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_destroy_pending(f->observer, 1u,
            (uint32_t *)(uintptr_t)(UINTPTR_MAX & ~(uintptr_t)(sizeof(uint32_t) - 1u))) ==
            WindowObserverStatusInvalidArgument);
    Require(window_observer_destroy_pending(f->observer, 1u, (uint32_t *)f->observer) ==
            WindowObserverStatusInvalidArgument);
    Require(pending == 0xA5A5A5A5u);
    Require(window_observer_drain_protected_destroys(NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_drain_protected_destroys(f->observer) == WindowObserverStatusBadState);

    memset(&record, 0xA5, sizeof(record));
    memcpy(expected, &record, sizeof(record));
    Require(window_observer_quarantine_handoff(NULL, &record) == WindowObserverStatusInvalidArgument);
    Require(window_observer_quarantine_handoff(&f->observer, NULL) == WindowObserverStatusInvalidArgument);
    Require(window_observer_quarantine_handoff((window_observer_t **)((unsigned char *)&f->observer + 1u),
                                               &record) == WindowObserverStatusInvalidArgument);
    Require(window_observer_quarantine_handoff(&f->observer,
            (window_observer_quarantine_record_t *)(uintptr_t)(UINTPTR_MAX & ~(uintptr_t)(
                _Alignof(window_observer_quarantine_record_t) - 1u))) == WindowObserverStatusInvalidArgument);
    Require(window_observer_quarantine_handoff(&f->observer,
            (window_observer_quarantine_record_t *)f->observer) == WindowObserverStatusInvalidArgument);
    Require(window_observer_quarantine_handoff((window_observer_t **)&record,
                                               (window_observer_quarantine_record_t *)&record) ==
            WindowObserverStatusInvalidArgument);
    Require(memcmp(&record, expected, sizeof(record)) == 0);
    Require(window_observer_quarantine_handoff(&f->observer, &record) ==
            WindowObserverStatusProtectedStatePending); /* bound Created cannot be handed off */
    Require(memcmp(&record, expected, sizeof(record)) == 0 && f->observer != NULL);

    /* An unbound Created handle rejects graph-only operations, then destroys cleanly. */
    api_double_config(&config, f->dbl, 0u, 1u);
    Require(window_observer_create(&config, &unbound) == WindowObserverStatusOk);
    Require(window_observer_sync_tracked(unbound) == WindowObserverStatusBadState);
    Require(window_observer_prepare_callbacks(unbound, 0u, 0u) == WindowObserverStatusBadState);
    Require(window_observer_destroy_pending(unbound, 1u, &pending) == WindowObserverStatusBadState);
    Require(window_observer_drain_protected_destroys(unbound) == WindowObserverStatusBadState);
    Require(window_observer_bind_graph(unbound, f->graph, TestEpoch + 1u) == WindowObserverStatusInvalidArgument);
    memset(&record, 0xA5, sizeof(record));
    Require(window_observer_quarantine_handoff(&unbound, &record) == WindowObserverStatusProtectedStatePending);
    Require(window_observer_destroy(&unbound) == WindowObserverStatusOk && unbound == NULL);

    /* Started is an invalid handoff/drain-free phase for destroy, but APIs retain their outputs. */
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    alias_slot = f->observer;
    memset(&record, 0xA5, sizeof(record));
    memcpy(expected, &record, sizeof(record));
    Require(window_observer_quarantine_handoff(&alias_slot, &record) ==
            WindowObserverStatusProtectedStatePending);
    Require(alias_slot == f->observer && memcmp(&record, expected, sizeof(record)) == 0);
    Require(window_observer_destroy_pending(f->observer, 1u, &pending) == WindowObserverStatusOk);
    Require(pending == 0u);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
done:
    if (unbound != NULL) (void)window_observer_destroy(&unbound);
    fixture_close(f);
    return result;
}

/** Failed hooks permit cleanup and diagnostics only; rejected configuration remains unchanged. */
static int test_failed_lifecycle_rejects_sync_and_configuration(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t before, after;
    Require(f != NULL);
    f->dbl->remove_fail_at = 1u;
    Require(window_observer_stop(f->observer) == WindowObserverStatusUnhookFailed);
    Require(obs_stats(f, &before) == 0 && before.state == WindowObserverStateFailed);
    Require(window_observer_sync_tracked(f->observer) == WindowObserverStatusBadState);
    Require(window_observer_set_preedit_window(f->observer, UINT64_C(0xA55)) == WindowObserverStatusBadState);
    Require(obs_stats(f, &after) == 0 && memcmp(&before, &after, sizeof(before)) == 0);
    f->dbl->remove_fail_at = 0u;
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    const uint32_t clocks = f->dbl->monotonic_calls;
    Require(window_observer_set_preedit_window(f->observer, 0u) == WindowObserverStatusOk);
    Require(f->dbl->monotonic_calls == clocks); /* the rejected Failed-state value never replaced zero */
done:
    fixture_close(f);
    return result;
}

/** A hook that installs "successfully" with a zero token is a failed install. */
static int test_start_zero_token(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 0);
    window_observer_stats_t stats;
    Require(f != NULL);
    f->dbl->next_token = 0u;
    Require(window_observer_start(f->observer) == WindowObserverStatusHookFailed);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.state == WindowObserverStateStopped);
    Require(f->dbl->install_calls == 1u && f->dbl->remove_calls == 0u);
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    Require(window_observer_stop(f->observer) == WindowObserverStatusOk);
    Require(window_observer_destroy(&f->observer) == WindowObserverStatusOk);
done:
    fixture_close(f);
    return result;
}

/** install_hook callback: the hook procedure fires while the observer holds the slot but is not Started. */
static void install_time_hint(api_double_t *dbl, void *argument) {
    (void)argument;
    api_double_hint(dbl, WindowObserverEventObjectCreate, 0x10u);
}

/** A hint that races hook installation (state Created) is ignored without any accounting. */
static int test_hint_during_install(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 0);
    window_observer_stats_t stats;
    Require(f != NULL);
    f->dbl->install_hook_fn = install_time_hint;
    Require(window_observer_start(f->observer) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hints_received == 0u && stats.hints_filtered == 0u && stats.dirty_pending == 0u);
    Require(stats.next_stamp == 1u);
done:
    fixture_close(f);
    return result;
}

/** A Destroy hint coalescing into a queued entry records the destroy stamp and tick on that entry. */
static int test_hint_destroy_coalesces(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    Require(f != NULL);
    Require(add_window(f, 0x20u) != NULL);
    f->dbl->tick = 3000u;
    hint(f, WindowObserverEventObjectCreate, 0x20u);
    api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, 0x20u, 0, 0, 3000u);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.hints_received == 2u && stats.hints_coalesced == 1u && stats.dirty_pending == 1u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.destroys_noted == 1u && stats.dirty_pending == 0u);
    Require(is_live(f, 0x20u) == 1); /* the window still exists: it was admitted after the destroy note */
done:
    fixture_close(f);
    return result;
}

/** Overlay and owner-resolution corner cases of the facts assembly. */
static int test_facts_overlay_corners(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    api_double_window_t *w;
    Require(f != NULL);

    /* Own-process window while a different window is the preedit window: AgentOwned, not Preedit. */
    Require(window_observer_set_preedit_window(f->observer, 0x999u) == WindowObserverStatusOk);
    Require(window_observer_set_preedit_window(f->observer, 0x999u) == WindowObserverStatusOk); /* unchanged */
    w = add_window(f, 0x30u);
    Require(w != NULL);
    w->facts.process_id = ApiDoubleOwnPid;
    Require(admit(f, 0x30u) == 0);
    Require(is_live(f, 0x30u) == 0);

    /* owner_root equal to the window itself: no owner sub-query. */
    w = add_window(f, 0x31u);
    Require(w != NULL);
    w->facts.owner = 0x31u;
    w->facts.owner_root = 0x31u;
    Require(admit(f, 0x31u) == 0);
    Require(api_double_queries_of(f->dbl, 0x31u) == 1u);

    /* An owner whose facts fail validation cannot be judged: the owned window's query counts as Failed. */
    w = add_owned(f, 0x32u, 0x33u);
    Require(w != NULL);
    w = add_window(f, 0x33u);
    Require(w != NULL);
    w->facts.dpi = 0u;
    hint(f, WindowObserverEventObjectCreate, 0x32u);
    Require(obs_stats(f, &stats) == 0);
    const uint64_t failed_before = stats.queries_failed;
    Require(do_pump(f, 1u) == WindowObserverStatusMore);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_failed == failed_before + 1u);
    Require(is_live(f, 0x32u) == 0);
done:
    fixture_close(f);
    return result;
}

/** QueueFull put-back merges a hint that arrived during the query into the retried entry. */
static int test_putback_merge_queue_full(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    Require(f != NULL);
    Require(add_window(f, 0x300u) != NULL);
    Require(admit(f, 0x300u) == 0);
    Require(fill_event_ring(f, 0x300u) == 0);
    Require(add_window(f, 0x301u) != NULL);
    hint(f, WindowObserverEventObjectCreate, 0x301u);
    f->dbl->hint_event = WindowObserverEventObjectShow;
    f->dbl->hint_window = 0x301u;
    f->dbl->hint_pending = 1u;
    Require(do_pump(f, 8u) == WindowObserverStatusGraphQueueFull);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 1u);
    Require(drain_events(f) == RingCapacity);
    Require(do_pump(f, 8u) == WindowObserverStatusOk);
    Require(is_live(f, 0x301u) == 1);
done:
    fixture_close(f);
    return result;
}

/** Wrong-epoch public calls reject before callbacks, leaving the queued key for a valid call. */
static int test_putback_merge_fatal(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    Require(f != NULL);
    Require(add_window(f, 0x310u) != NULL);
    hint(f, WindowObserverEventObjectCreate, 0x310u);
    f->dbl->hint_event = WindowObserverEventObjectShow;
    f->dbl->hint_window = 0x310u;
    f->dbl->hint_pending = 1u;
    memset(&stats, 0xA6, sizeof(stats));
    Require(window_observer_pump(f->observer, f->graph, TestEpoch + 1u, 4u, &stats) == WindowObserverStatusGraphBindingMismatch);
    { unsigned char expected[sizeof(stats)]; memset(expected, 0xA6, sizeof(expected));
      Require(memcmp(&stats, expected, sizeof(stats)) == 0); }
    Require(do_pump(f, 4u) == WindowObserverStatusMore);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(is_live(f, 0x310u) == 1);
done:
    fixture_close(f);
    return result;
}

/** A failed query's retry entry merges a hint that arrived during that very query. */
static int test_putback_merge_retry(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    api_double_window_t *w;
    Require(f != NULL);
    w = add_window(f, 0x320u);
    Require(w != NULL);
    w->fail_count = 1u;
    hint(f, WindowObserverEventObjectCreate, 0x320u);
    f->dbl->hint_event = WindowObserverEventObjectShow;
    f->dbl->hint_window = 0x320u;
    f->dbl->hint_pending = 1u;
    Require(do_pump(f, 4u) == WindowObserverStatusMore);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.queries_failed == 1u && stats.dirty_pending == 1u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(is_live(f, 0x320u) == 1);
done:
    fixture_close(f);
    return result;
}

/** Sink that runs one pump while the graph is inside drain. */
static int pump_in_sink(void *context, const interop_window_event_t *event) {
    sink_probe_t *p = (sink_probe_t *)context;
    (void)event;
    p->calls++;
    if (p->calls == 1u) {
        p->pump = window_observer_pump(p->f->observer, p->f->graph, TestEpoch, 4u, NULL);
    }
    return 0;
}

/** Destroy-application statuses: fatal, reentrant, Gone plus fatal; plus a Drop status from observe. */
static int test_pump_destroy_status_paths(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats, before;
    sink_probe_t probe;
    uint32_t delivered = 0u;
    api_double_window_t *w;
    Require(f != NULL);
    Require(add_window(f, 0x400u) != NULL);
    Require(admit(f, 0x400u) == 0); /* leaves one Announce queued */

    /* Wrong epoch is rejected before touching protected Destroy evidence. */
    api_double_hint_full(f->dbl, WindowObserverEventObjectDestroy, 0x400u, 0, 0, f->dbl->tick);
    memset(&stats, 0xA5, sizeof(stats));
    Require(window_observer_pump(f->observer, f->graph, TestEpoch + 1u, 4u, &stats) == WindowObserverStatusGraphBindingMismatch);
    Require(((unsigned char *)&stats)[0] == 0xA5u);

    /* The same entry while the graph is inside drain: Reentrant, key kept. */
    memset(&probe, 0, sizeof(probe));
    probe.f = f;
    Require(obs_stats(f, &before) == 0);
    Require(interop_window_graph_drain(f->graph, TestEpoch, pump_in_sink, &probe, 1u, &delivered) ==
            InteropWindowGraphStatusOk);
    Require(delivered == 1u && probe.pump == WindowObserverStatusReentrant);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 1u && memcmp(&stats, &before, sizeof(stats)) == 0);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 0u);

    /* Gone window + wrong epoch leaves the queued key for the bound call. */
    Require(add_window(f, 0x410u) != NULL);
    Require(admit(f, 0x410u) == 0);
    api_double_remove(f->dbl, 0x410u);
    hint(f, WindowObserverEventObjectHide, 0x410u);
    memset(&stats, 0x5A, sizeof(stats));
    Require(window_observer_pump(f->observer, f->graph, TestEpoch + 1u, 4u, &stats) == WindowObserverStatusGraphBindingMismatch);
    Require(((unsigned char *)&stats)[0] == 0x5Au);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(is_live(f, 0x410u) == 0);

    /* Facts the graph rejects (InvalidFacts) drop the key. */
    w = add_window(f, 0x420u);
    Require(w != NULL);
    w->facts.dpi = 0u;
    hint(f, WindowObserverEventObjectCreate, 0x420u);
    Require(do_pump(f, 4u) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.dirty_pending == 0u && stats.last_graph_status == (uint32_t)InteropWindowGraphStatusInvalidFacts);
    Require(is_live(f, 0x420u) == 0);
done:
    fixture_close(f);
    return result;
}

/** Enumeration oddities: zero handles, vanished windows, failed unknown windows, an absurd count. */
static int test_refresh_enumeration_oddities(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    api_double_window_t *w;
    uint64_t list[5] = {0u, 0x600u, 0x601u, 0x601u, 0x602u};
    Require(f != NULL);
    Require(add_window(f, 0x601u) != NULL);
    w = add_window(f, 0x602u);
    Require(w != NULL);
    w->mode = ApiDoubleModeFailed;
    api_double_enum_set(f->dbl, list, 5u);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_committed == 1u && stats.queries_gone == 1u && stats.queries_failed == 1u);
    Require(is_live(f, 0x601u) == 1 && is_live(f, 0x602u) == 0 && is_live(f, 0x600u) == 0);

    f->dbl->enum_report_count = WindowObserverMaxEnumerated + 1u;
    Require(do_refresh(f) == WindowObserverStatusEnumFailed);
    f->dbl->enum_report_count = 0u;
    Require(obs_stats(f, &stats) == 0);
    Require(stats.refreshes_started == 1u && stats.refreshes_committed == 1u);
done:
    fixture_close(f);
    return result;
}

/** query_facts hook: abort the graph's open refresh behind the observer's back. */
static void abort_graph_refresh_hook(api_double_t *dbl, uint64_t window, void *argument) {
    fixture_t *f = (fixture_t *)argument;
    (void)dbl;
    (void)window;
    (void)interop_window_graph_refresh_abort(f->graph, TestEpoch);
}

/** A graph refresh that vanishes mid-call makes offer, unseen listing and the quiet abort all report errors. */
static int test_refresh_graph_refresh_lost(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    uint64_t list[1] = {0x700u};
    Require(f != NULL);
    Require(add_window(f, 0x700u) != NULL);
    api_double_enum_set(f->dbl, list, 1u);
    f->dbl->query_hook = abort_graph_refresh_hook;
    f->dbl->query_hook_arg = f;
    f->dbl->query_hook_once = 1u;
    Require(window_observer_refresh(f->observer, f->graph, TestEpoch, &stats) == WindowObserverStatusGraphError);
    Require(stats.last_graph_status == (uint32_t)InteropWindowGraphStatusNoRefresh);
    Require(stats.refreshes_aborted == 0u && stats.refreshes_committed == 0u);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.refresh_active == 0u);

    /* Only a vanished enumerated window is queried: the unseen listing is what fails. */
    list[0] = 0x701u;
    api_double_enum_set(f->dbl, list, 1u);
    f->dbl->query_hook = abort_graph_refresh_hook;
    f->dbl->query_hook_arg = f;
    f->dbl->query_hook_once = 1u;
    Require(window_observer_refresh(f->observer, f->graph, TestEpoch, &stats) == WindowObserverStatusGraphError);
    Require(stats.last_graph_status == (uint32_t)InteropWindowGraphStatusNoRefresh);
    Require(stats.graph_errors == 2u);
    Require(do_refresh(f) == WindowObserverStatusOk); /* a clean refresh afterwards */
done:
    fixture_close(f);
    return result;
}

/** Stamp exhaustion on the requery of an unseen window aborts the refresh. */
static int test_refresh_unseen_stamp_exhausted(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, StampSentinel - 3u, 1);
    window_observer_stats_t stats;
    interop_window_graph_stats_t gs;
    uint64_t list[1] = {0x801u};
    Require(f != NULL);
    Require(add_window(f, 0x800u) != NULL);
    Require(add_window(f, 0x801u) != NULL);
    Require(admit(f, 0x800u) == 0); /* hint stamp + query stamp */
    api_double_enum_set(f->dbl, list, 1u);
    Require(window_observer_refresh(f->observer, f->graph, TestEpoch, &stats) == WindowObserverStatusTrackingFault);
    Require(stats.refreshes_aborted == 1u && stats.next_stamp == StampSentinel);
    Require(graph_stats(f, &gs) == 0);
    Require(gs.refresh_active == 0u);
done:
    fixture_close(f);
    return result;
}

/** refresh_due / next_timeout_ms across no-attempt, backwards clock, backlog, blocked desktop and overflow. */
static int test_cadence_corners(void) {
    int result = 0;
    fixture_t *f = fixture_open(0u, 1u, 1);
    fixture_t *g = NULL;
    const uint64_t interval = InteropWindowGraphRefreshIntervalMs;
    Require(f != NULL);
    Require(add_window(f, 0x900u) != NULL);
    Require(window_observer_refresh_due(f->observer, 1000u) == 1); /* no attempt yet */
    Require(window_observer_next_timeout_ms(f->observer, 1000u) == 0u);
    Require(do_refresh(f) == WindowObserverStatusOk);
    Require(window_observer_refresh_due(f->observer, 1000u) == 0);
    Require(window_observer_next_timeout_ms(f->observer, 1000u) == (uint32_t)interval);
    Require(window_observer_next_timeout_ms(f->observer, 1100u) == (uint32_t)(interval - 100u));
    Require(window_observer_refresh_due(f->observer, 500u) == 0); /* clock went backwards */
    Require(window_observer_next_timeout_ms(f->observer, 500u) == (uint32_t)interval);
    Require(window_observer_refresh_due(f->observer, 1000u + interval) == 1);
    Require(window_observer_next_timeout_ms(f->observer, 1000u + interval) == 0u);

    hint(f, WindowObserverEventObjectCreate, 0x900u); /* backlog */
    Require(window_observer_refresh_due(f->observer, 1000u) == 0);
    Require(window_observer_next_timeout_ms(f->observer, 1000u) == 0u);
    f->dbl->desktop = WindowObserverDesktopOther; /* blocked desktop: the backlog cannot be worked */
    Require(do_pump(f, 4u) == WindowObserverStatusDesktopInactive);
    Require(window_observer_refresh_due(f->observer, 1000u) == 0);
    Require(window_observer_next_timeout_ms(f->observer, 1000u) == (uint32_t)interval);
    f->dbl->desktop = WindowObserverDesktopDefault;
    Require(do_pump(f, 4u) == WindowObserverStatusOk);

    fixture_close(f); /* only one observer may be Started per process */
    f = NULL;

    /* Bound stamp exhaustion faults tracking instead of pretending a refresh can recover it. */
    g = fixture_open(0u, StampSentinel - 1u, 1);
    Require(g != NULL);
    Require(do_refresh(g) == WindowObserverStatusOk);
    hint(g, WindowObserverEventObjectCreate, 0xA0u); /* takes the last stamp */
    hint(g, WindowObserverEventObjectCreate, 0xA1u);
    Require(window_observer_next_timeout_ms(g->observer, 1000u) == WindowObserverWaitForever);
    g->dbl->desktop = WindowObserverDesktopOther;
    Require(window_observer_pump(g->observer, g->graph, TestEpoch, 4u, NULL) == WindowObserverStatusTrackingFault);
done:
    fixture_close(f);
    fixture_close(g);
    return result;
}

/* ------------------------------------------------------------------------------------------ */
/* Convergence fuzzer: real observer + real graph through the api double                      */
/* ------------------------------------------------------------------------------------------ */

/** Fake top-level windows of the fuzz world (hwnd = FuzzBaseHwnd + slot). */
#define FuzzSlots 40
/** Session epoch of the fuzz graph. */
#define FuzzEpoch UINT64_C(7)
/** HWND of slot 0. */
#define FuzzBaseHwnd UINT64_C(0x1000)
/** Generation numbers the stream model can track per seed (checked; a seed issues a few thousand). */
#define FuzzMaxGenerations 65536u
/** Fuzz option: hint bursts of 600 new handles (hint-table overflow) at random steps. */
#define FuzzOptionBursts 1u
/** Fuzz option: the consumer drains the graph queue only rarely (backpressure, GraphQueueFull). */
#define FuzzOptionLazyDrain 2u
/** Fuzz option: enumeration results Truncated (partial list) are mixed in. */
#define FuzzOptionTruncated 4u

/** Independent model of the guest windows. */
typedef struct fuzz_world_t {
    int exists[FuzzSlots]; /**< 1 while the window exists. */
    int owner[FuzzSlots]; /**< Owner slot or -1. */
    int visible[FuzzSlots]; /**< Visible fact. */
    int excluded[FuzzSlots]; /**< Class "Progman" (excluded). */
    int zero[FuzzSlots]; /**< Zero width (ZeroArea). */
    uint32_t pid[FuzzSlots]; /**< Process id of the current incarnation (unique per creation). */
} fuzz_world_t;

/** Everything one fuzz seed owns; heap allocated (the generation tables are large). */
typedef struct fuzz_t {
    fuzz_world_t world; /**< Ground truth. */
    api_double_t *dbl; /**< Scriptable api. */
    interop_window_graph_t *graph; /**< Real graph. */
    window_observer_t *observer; /**< Real observer. */
    uint64_t rng; /**< xorshift64* state. */
    uint32_t next_pid; /**< Next fresh process id. */
    uint32_t lookup_fail_pct; /**< Percent of app_key_lookup calls that fail. */
    uint32_t sink_refuse_pct; /**< Percent of sink calls that refuse an event. */
    uint32_t options; /**< FuzzOption* bits. */
    uint32_t failures; /**< Oracle violations found (each prints one line, at most 20). */
    uint32_t tracking_fault; /**< 1 after the observer conservatively latches uncertain protected evidence. */
    uint64_t steps; /**< Random steps executed. */
    uint64_t generations; /**< Announce events seen. */
    uint64_t max_generation; /**< Highest Announce generation seen. */
    uint64_t overflows; /**< Observer overflow recoveries (stats.invalidations at the end). */
    uint8_t live_generation[FuzzMaxGenerations]; /**< 1 while an announced generation is not retired. */
    uint64_t generation_window[FuzzMaxGenerations]; /**< HWND of every announced generation. */
    uint64_t generation_parent[FuzzMaxGenerations]; /**< Parent generation of every announced generation. */
} fuzz_t;

/** Record one oracle violation (the first 20 print). */
#define FuzzFail(z, ...) \
    do { \
        if ((z)->failures < 20u) { \
            fprintf(stderr, "window_observer_windows.c:%d: fuzz: ", __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fprintf(stderr, "\n"); \
        } \
        (z)->failures++; \
    } while (0)

/** Next xorshift64* value. */
static uint64_t fuzz_random(fuzz_t *z) {
    z->rng ^= z->rng >> 12;
    z->rng ^= z->rng << 25;
    z->rng ^= z->rng >> 27;
    return z->rng * UINT64_C(0x2545F4914F6CDD1D);
}

/** Random percentage 0..99. */
static uint32_t fuzz_percent(fuzz_t *z) {
    return (uint32_t)(fuzz_random(z) % 100u);
}

/** HWND of a slot. */
static uint64_t fuzz_hwnd(int slot) {
    return FuzzBaseHwnd + (uint64_t)slot;
}

/** Write the identity-derived application key of a process id into buf; returns its length. */
static uint32_t fuzz_key_of(uint32_t process_id, char *buf, size_t capacity) {
    const int n = snprintf(buf, capacity, "a-p%u", (unsigned)process_id);
    return n > 0 ? (uint32_t)n : 0u;
}

/** Graph app-key callback: the key derives from the process id; fails at lookup_fail_pct. */
static int fuzz_lookup(void *context, const interop_window_facts_t *facts, uint8_t *key, uint32_t capacity,
                       uint32_t *length) {
    fuzz_t *z = (fuzz_t *)context;
    char buf[32];
    if (z->lookup_fail_pct != 0u && fuzz_percent(z) < z->lookup_fail_pct) {
        return 1;
    }
    const uint32_t n = fuzz_key_of(facts->process_id, buf, sizeof(buf));
    if (n == 0u || n > capacity) {
        return 1;
    }
    memcpy(key, buf, n);
    *length = n;
    return 0;
}

/** Graph event sink: refuses at sink_refuse_pct, otherwise checks the stream oracles. */
static int fuzz_sink(void *context, const interop_window_event_t *event) {
    fuzz_t *z = (fuzz_t *)context;
    char buf[32];
    if (z->sink_refuse_pct != 0u && fuzz_percent(z) < z->sink_refuse_pct) {
        return 1;
    }
    if (event->generation >= FuzzMaxGenerations) {
        FuzzFail(z, "generation %llu exceeds the model", (unsigned long long)event->generation);
        return 0;
    }
    const uint32_t n = fuzz_key_of(event->process_id, buf, sizeof(buf));
    if (event->app_key_length != n || memcmp(event->app_key, buf, n) != 0) {
        FuzzFail(z, "fabricated app key for window 0x%llx generation %llu", (unsigned long long)event->window,
                 (unsigned long long)event->generation);
    }
    if (event->kind == InteropWindowEventAnnounce) {
        if (z->live_generation[event->generation] != 0u) {
            FuzzFail(z, "generation %llu announced twice", (unsigned long long)event->generation);
        }
        if (event->generation <= z->max_generation) {
            FuzzFail(z, "generation %llu does not grow (max %llu)", (unsigned long long)event->generation,
                     (unsigned long long)z->max_generation);
        }
        z->max_generation = event->generation;
        z->generations++;
        if (event->parent_generation != 0u &&
            (z->live_generation[event->parent_generation] == 0u ||
             z->generation_window[event->parent_generation] != event->parent_window)) {
            FuzzFail(z, "child generation %llu announced before its parent %llu",
                     (unsigned long long)event->generation, (unsigned long long)event->parent_generation);
        }
        z->live_generation[event->generation] = 1u;
        z->generation_window[event->generation] = event->window;
        z->generation_parent[event->generation] = event->parent_generation;
    } else if (event->kind == InteropWindowEventUpdate) {
        if (z->live_generation[event->generation] == 0u) {
            FuzzFail(z, "update for non-live generation %llu", (unsigned long long)event->generation);
        }
    } else {
        if (z->live_generation[event->generation] == 0u) {
            FuzzFail(z, "retire for non-live generation %llu", (unsigned long long)event->generation);
        }
        for (uint64_t g = 1; g <= z->max_generation; g++) {
            if (z->live_generation[g] != 0u && z->generation_parent[g] == event->generation) {
                FuzzFail(z, "generation %llu retired before its child %llu", (unsigned long long)event->generation,
                         (unsigned long long)g);
            }
        }
        z->live_generation[event->generation] = 0u;
    }
    return 0;
}

/** Copy the model state of a slot into its double window (identity, owner, visibility, class, width). */
static void fuzz_sync_facts(fuzz_t *z, int slot) {
    api_double_window_t *w = api_double_find(z->dbl, fuzz_hwnd(slot));
    if (w == NULL) {
        return;
    }
    interop_window_facts_t *facts = &w->facts;
    facts->process_id = z->world.pid[slot];
    facts->thread_id = z->world.pid[slot] + 1u;
    facts->process_start_time = (uint64_t)z->world.pid[slot] * 10u;
    if (z->world.owner[slot] >= 0) {
        facts->owner = fuzz_hwnd(z->world.owner[slot]);
        facts->owner_root = fuzz_hwnd(z->world.owner[slot]);
    } else {
        facts->owner = 0u;
        facts->owner_root = 0u;
    }
    if (z->world.visible[slot]) {
        facts->flags |= (uint32_t)InteropWindowFactVisible;
    } else {
        facts->flags &= ~(uint32_t)InteropWindowFactVisible;
    }
    api_double_set_text(facts->class_name, InteropWindowGraphMaxClassBytes, &facts->class_length,
                        z->world.excluded[slot] ? "Progman" : "TestWindow");
    facts->width = z->world.zero[slot] ? 0u : 800u;
}

/** Emit a hint, losing 8% of them (the OS may drop events; the refresh must still converge). */
static void fuzz_hint(fuzz_t *z, uint32_t event, int slot) {
    if (fuzz_percent(z) < 92u) {
        api_double_hint(z->dbl, event, fuzz_hwnd(slot));
    }
}

/** Destroy a slot and, recursively, the windows it owns. */
static void fuzz_destroy_slot(fuzz_t *z, int slot) {
    if (!z->world.exists[slot]) {
        return;
    }
    z->world.exists[slot] = 0;
    api_double_remove(z->dbl, fuzz_hwnd(slot));
    fuzz_hint(z, WindowObserverEventObjectDestroy, slot);
    for (int c = 0; c < FuzzSlots; c++) {
        if (z->world.exists[c] && z->world.owner[c] == slot) {
            fuzz_destroy_slot(z, c);
        }
    }
}

/** Create a slot with a fresh process id (an HWND reuse is always a new incarnation). */
static void fuzz_create_slot(fuzz_t *z, int slot) {
    z->world.exists[slot] = 1;
    z->world.pid[slot] = z->next_pid++;
    z->world.owner[slot] = -1;
    if (slot > 0 && fuzz_percent(z) < 45u) {
        const int owner = (int)(fuzz_random(z) % (uint64_t)slot);
        if (z->world.exists[owner]) {
            z->world.owner[slot] = owner;
        }
    }
    z->world.visible[slot] = fuzz_percent(z) < 85u;
    z->world.excluded[slot] = fuzz_percent(z) < 5u;
    z->world.zero[slot] = fuzz_percent(z) < 5u;
    (void)api_double_add(z->dbl, fuzz_hwnd(slot));
    fuzz_sync_facts(z, slot);
    fuzz_hint(z, WindowObserverEventObjectCreate, slot);
    fuzz_hint(z, WindowObserverEventObjectShow, slot);
}

/** Publish the enumeration list (random order; with omit, 10% of the windows are left out). */
static void fuzz_set_enum(fuzz_t *z, int omit) {
    uint64_t list[FuzzSlots];
    uint32_t n = 0;
    for (int s = 0; s < FuzzSlots; s++) {
        if (z->world.exists[s] && !(omit && fuzz_percent(z) < 10u)) {
            list[n++] = fuzz_hwnd(s);
        }
    }
    for (uint32_t i = n; i > 1u; i--) {
        const uint32_t j = (uint32_t)(fuzz_random(z) % i);
        const uint64_t t = list[i - 1u];
        list[i - 1u] = list[j];
        list[j] = t;
    }
    api_double_enum_set(z->dbl, list, n);
}

/** Drain the graph queue into the sink in random batch sizes. */
static void fuzz_drain(fuzz_t *z) {
    uint32_t delivered = 0;
    for (int guard = 0; guard < 100; guard++) {
        const interop_window_graph_status_t status = interop_window_graph_drain(
            z->graph, FuzzEpoch, fuzz_sink, z, 1u + (uint32_t)(fuzz_random(z) % 256u), &delivered);
        if (status == InteropWindowGraphStatusNone) {
            return;
        }
        if (status != InteropWindowGraphStatusOk && status != InteropWindowGraphStatusSinkFailed) {
            FuzzFail(z, "drain status %d", (int)status);
            return;
        }
        if (status == InteropWindowGraphStatusSinkFailed && z->sink_refuse_pct != 0u) {
            return;
        }
    }
}

/** Complete quarantined entries with a full proof (a random half when `fraction` is set). */
static void fuzz_complete(fuzz_t *z, int fraction) {
    interop_window_retiring_entry_t entries[InteropMaxWindows];
    uint32_t count = 0;
    const interop_window_retire_proof_t proof = {1u, 1u};
    if (interop_window_graph_list_retiring(z->graph, entries, InteropMaxWindows, &count) !=
        InteropWindowGraphStatusOk) {
        FuzzFail(z, "list_retiring failed");
        return;
    }
    for (uint32_t i = 0; i < count && i < InteropMaxWindows; i++) {
        if (fraction && fuzz_percent(z) >= 50u) {
            continue;
        }
        if (interop_window_graph_complete_retire(z->graph, FuzzEpoch, entries[i].window, entries[i].generation,
                                                 &proof) != InteropWindowGraphStatusOk) {
            FuzzFail(z, "complete_retire failed");
        }
    }
}

/** Move the double's clocks forward. */
static void fuzz_advance(fuzz_t *z, uint32_t ms) {
    z->dbl->monotonic += ms;
    z->dbl->tick += ms;
}

/** One random pump; every status except the documented ones is a violation. */
static void fuzz_pump(fuzz_t *z) {
    window_observer_stats_t stats;
    const window_observer_status_t status = window_observer_pump(
        z->observer, z->graph, FuzzEpoch, 1u + (uint32_t)(fuzz_random(z) % 64u), &stats);
    switch (status) {
    case WindowObserverStatusOk:
    case WindowObserverStatusMore:
    case WindowObserverStatusDesktopInactive:
    case WindowObserverStatusEnumFailed:
        break;
    case WindowObserverStatusGraphQueueFull:
        fuzz_drain(z);
        break;
    case WindowObserverStatusTrackingFault:
        z->tracking_fault = 1u;
        break;
    default:
        FuzzFail(z, "pump status %d (last graph status %u)", (int)status, (unsigned)stats.last_graph_status);
        break;
    }
}

/** One refresh when the cadence says so; every status except the documented ones is a violation. */
static void fuzz_refresh(fuzz_t *z) {
    if (z->tracking_fault) return;
    window_observer_stats_t stats;
    if (!window_observer_refresh_due(z->observer, z->dbl->monotonic)) {
        return;
    }
    const window_observer_status_t status = window_observer_refresh(z->observer, z->graph, FuzzEpoch, &stats);
    switch (status) {
    case WindowObserverStatusOk:
    case WindowObserverStatusDesktopInactive:
    case WindowObserverStatusEnumFailed:
        break;
    case WindowObserverStatusGraphQueueFull:
        fuzz_drain(z);
        break;
    case WindowObserverStatusTrackingFault:
        z->tracking_fault = 1u;
        break;
    default:
        FuzzFail(z, "refresh status %d (last graph status %u)", (int)status, (unsigned)stats.last_graph_status);
        break;
    }
}

/** Own eligibility of a world window, ignoring its owner. */
static int fuzz_own_eligible(const fuzz_t *z, int slot) {
    return z->world.exists[slot] && z->world.visible[slot] && !z->world.excluded[slot] && !z->world.zero[slot];
}

/** Expected parent slot of an eligible window, -1 for none. */
static int fuzz_expected_parent(const fuzz_t *z, int slot) {
    const int owner = z->world.owner[slot];
    if (owner >= 0 && z->world.exists[owner] && fuzz_own_eligible(z, owner)) {
        return owner;
    }
    return -1;
}

/** Expected export state: eligible and every ancestor exported. */
static int fuzz_expected_exported(const fuzz_t *z, int slot) {
    if (!fuzz_own_eligible(z, slot)) {
        return 0;
    }
    const int parent = fuzz_expected_parent(z, slot);
    return parent < 0 ? 1 : fuzz_expected_exported(z, parent);
}

/** Stop all faults, run pumps and refreshes to quiescence and compare the graph with the model. */
static void fuzz_converge_and_check(fuzz_t *z) {
    z->lookup_fail_pct = 0u;
    z->sink_refuse_pct = 0u;
    z->dbl->desktop = WindowObserverDesktopDefault;
    z->dbl->enum_result = WindowObserverEnumOk;
    for (uint32_t i = 0; i < ApiDoubleMaxWindows; i++) {
        z->dbl->windows[i].fail_count = 0u;
    }
    fuzz_set_enum(z, 0);
    for (int round = 0; round < 8; round++) {
        fuzz_advance(z, 300u);
        for (int k = 0; k < 64; k++) {
            window_observer_stats_t stats;
            const window_observer_status_t status =
                window_observer_pump(z->observer, z->graph, FuzzEpoch, 64u, &stats);
            fuzz_drain(z);
            if (status == WindowObserverStatusOk) {
                break;
            }
        }
        fuzz_refresh(z);
        fuzz_drain(z);
        fuzz_complete(z, 0);
    }
    uint32_t expected_count = 0;
    for (int s = 0; s < FuzzSlots; s++) {
        interop_window_live_t live;
        const interop_window_graph_status_t status = interop_window_graph_find_live(z->graph, fuzz_hwnd(s), &live);
        const int want = z->world.exists[s] && fuzz_expected_exported(z, s);
        expected_count += want ? 1u : 0u;
        if (want && status != InteropWindowGraphStatusOk) {
            FuzzFail(z, "slot %d expected exported but not live", s);
            continue;
        }
        if (!want && status == InteropWindowGraphStatusOk) {
            FuzzFail(z, "slot %d live but not expected (exists %d)", s, z->world.exists[s]);
            continue;
        }
        if (!want) {
            continue;
        }
        const int parent = fuzz_expected_parent(z, s);
        const uint32_t role = parent < 0 ? (uint32_t)InteropRoleToplevel : (uint32_t)InteropRoleDialog;
        const uint64_t parent_window = parent < 0 ? 0u : fuzz_hwnd(parent);
        if (live.role != role || live.parent_window != parent_window || live.process_id != z->world.pid[s]) {
            FuzzFail(z, "slot %d role %u/%u parent 0x%llx/0x%llx pid %u/%u", s, (unsigned)live.role, (unsigned)role,
                     (unsigned long long)live.parent_window, (unsigned long long)parent_window,
                     (unsigned)live.process_id, (unsigned)z->world.pid[s]);
        }
    }
    interop_window_graph_stats_t graph_state;
    window_observer_stats_t observer_state;
    if (interop_window_graph_get_stats(z->graph, &graph_state) != InteropWindowGraphStatusOk ||
        window_observer_status(z->observer, &observer_state) != WindowObserverStatusOk) {
        FuzzFail(z, "stats unavailable");
        return;
    }
    z->overflows = observer_state.invalidations;
    if (graph_state.active_count != expected_count) {
        FuzzFail(z, "active %u, expected %u", (unsigned)graph_state.active_count, (unsigned)expected_count);
    }
}

/** One random step of the world and the observer. */
static void fuzz_step(fuzz_t *z, uint64_t seed) {
    if (z->tracking_fault) {
        z->steps += 1u; /* remaining ticks still run, but poisoned tracking admits no more work */
        return;
    }
    const uint32_t c = fuzz_percent(z);
    const int s = (int)(fuzz_random(z) % FuzzSlots);
    z->lookup_fail_pct = 2u;
    z->sink_refuse_pct = 5u;
    if (c < 14u) {
        if (!z->world.exists[s]) {
            fuzz_create_slot(z, s);
        }
    } else if (c < 22u) {
        fuzz_destroy_slot(z, s);
    } else if (c < 30u) {
        if (z->world.exists[s]) {
            z->world.visible[s] = !z->world.visible[s];
            fuzz_sync_facts(z, s);
            fuzz_hint(z, z->world.visible[s] ? WindowObserverEventObjectShow : WindowObserverEventObjectHide, s);
        }
    } else if (c < 34u) {
        if (z->world.exists[s]) {
            const int owner = s > 0 && fuzz_percent(z) < 60u ? (int)(fuzz_random(z) % (uint64_t)s) : -1;
            z->world.owner[s] = (owner >= 0 && z->world.exists[owner]) ? owner : -1;
            fuzz_sync_facts(z, s);
            fuzz_hint(z, WindowObserverEventObjectParentChange, s);
        }
    } else if (c < 37u) {
        if (z->world.exists[s]) {
            z->world.excluded[s] = !z->world.excluded[s];
            fuzz_sync_facts(z, s);
            fuzz_hint(z, WindowObserverEventObjectNameChange, s);
        }
    } else if (c < 40u) {
        if (z->world.exists[s]) {
            z->world.zero[s] = !z->world.zero[s];
            fuzz_sync_facts(z, s);
            fuzz_hint(z, EventObjectLocationChange, s);
        }
    } else if (c < 43u) {
        api_double_window_t *w = api_double_find(z->dbl, fuzz_hwnd(s));
        if (w != NULL) {
            w->fail_count = 1u + (uint32_t)(fuzz_random(z) % 3u);
        }
    } else if (c < 45u) {
        z->dbl->desktop = fuzz_percent(z) < 50u ? WindowObserverDesktopDefault : WindowObserverDesktopOther;
    } else if (c < 46u) {
        if ((z->options & FuzzOptionBursts) != 0u || (seed & 2u) != 0u) {
            for (uint32_t k = 0; k < 600u; k++) {
                api_double_hint(z->dbl, WindowObserverEventObjectCreate, UINT64_C(0x90000) + k);
            }
        }
    } else if (c < 52u) {
        fuzz_advance(z, (uint32_t)(fuzz_random(z) % 300u));
    } else if (c < 72u) {
        fuzz_pump(z);
    } else if (c < 82u) {
        fuzz_set_enum(z, 1);
        z->dbl->enum_result = WindowObserverEnumOk;
        if (fuzz_percent(z) < 4u) {
            z->dbl->enum_result = WindowObserverEnumFailed;
        } else if ((z->options & FuzzOptionTruncated) != 0u && fuzz_percent(z) < 10u) {
            z->dbl->enum_result = WindowObserverEnumTruncated;
        }
        fuzz_refresh(z);
    } else if (c < 92u) {
        if ((z->options & FuzzOptionLazyDrain) == 0u || fuzz_percent(z) < 15u) {
            fuzz_drain(z);
        }
    } else {
        fuzz_complete(z, 1);
    }
    z->steps++;
}

/** Run one seed: random steps, then convergence. Returns 0 when no oracle was violated. */
static int fuzz_run_seed(uint64_t seed, uint32_t steps, uint32_t options, uint64_t *step_total,
                         uint64_t *generation_total, uint64_t *overflow_total) {
    int result = 0;
    fuzz_t *z = (fuzz_t *)calloc(1u, sizeof(*z));
    interop_window_graph_config_t graph_config;
    window_observer_config_t config;
    Require(z != NULL);
    z->rng = seed | 1u;
    z->next_pid = 5000u;
    z->options = options;
    z->dbl = api_double_new();
    Require(z->dbl != NULL);
    memset(&graph_config, 0, sizeof(graph_config));
    graph_config.session_epoch = FuzzEpoch;
    graph_config.app_key_lookup = fuzz_lookup;
    graph_config.app_key_context = z;
    Require(interop_window_graph_create(&graph_config, &z->graph) == InteropWindowGraphStatusOk);
    api_double_config(&config, z->dbl, 0u, 1u);
    Require(window_observer_create(&config, &z->observer) == WindowObserverStatusOk);
    Require(window_observer_bind_graph(z->observer, z->graph, FuzzEpoch) == WindowObserverStatusOk);
    Require(window_observer_start(z->observer) == WindowObserverStatusOk);
    for (uint32_t i = 0; i < steps; i++) {
        fuzz_step(z, seed);
    }
    if (z->tracking_fault) {
        window_observer_stats_t fault_stats;
        if (window_observer_status(z->observer, &fault_stats) != WindowObserverStatusOk ||
            fault_stats.state != WindowObserverStateTrackingFault) FuzzFail(z, "tracking fault was not sticky");
    } else {
        fuzz_converge_and_check(z);
    }
    *step_total += z->steps;
    *generation_total += z->generations;
    *overflow_total += z->overflows + (z->tracking_fault != 0u ? 1u : 0u);
    if (z->failures != 0u) {
        fprintf(stderr, "window_observer_windows.c: fuzz seed 0x%llx: %u violations\n", (unsigned long long)seed,
                (unsigned)z->failures);
        result = 1;
    }
done:
    if (z != NULL) {
        if (z->observer != NULL) {
            (void)window_observer_stop(z->observer);
            window_observer_quarantine_record_t quarantine;
            memset(&quarantine, 0, sizeof(quarantine));
            (void)window_observer_quarantine_handoff(&z->observer, &quarantine);
        }
        if (z->graph != NULL) {
            (void)interop_window_graph_destroy(&z->graph);
        }
        api_double_free(z->dbl);
        free(z);
    }
    return result;
}

/** Run `seeds` seeds of `steps` steps with the given options; require non-vacuous coverage of the scenario. */
static int fuzz_run_many(uint32_t seeds, uint32_t steps, uint32_t options, uint64_t base) {
    int result = 0;
    uint64_t step_total = 0;
    uint64_t generation_total = 0;
    uint64_t overflow_total = 0;
    for (uint32_t k = 0; k < seeds; k++) {
        const int rc = fuzz_run_seed(base * (uint64_t)(k + 1u), steps, options, &step_total, &generation_total,
                                     &overflow_total);
        Require(rc == 0);
    }
    Require(step_total == (uint64_t)seeds * steps);
    Require(generation_total > 100u); /* non-vacuous work continues before any fail-closed seed */
    Require((options & FuzzOptionBursts) == 0u || overflow_total > 0u); /* ... and really overflows */
    printf("  fuzz: %u seeds x %u steps = %llu steps, %llu generations, %llu overflow recoveries\n", (unsigned)seeds,
           (unsigned)steps, (unsigned long long)step_total, (unsigned long long)generation_total,
           (unsigned long long)overflow_total);
done:
    return result;
}

/**
 * Convergence fuzz, general: create, destroy with owned-window cascade, HWND reuse with a new process
 * id, visibility, owner and class changes, zero area, transient Failed, desktop flips, overflow bursts on
 * every second seed, lost hints (8%), omitted enumeration entries, enumeration failure, lookup failure
 * (2%), sink refusal (5%). 40 seeds x 3000 steps. Oracles: parent Announce before child, child retire
 * before parent, generations only grow, app key equals the identity-derived key, and after a final
 * quiescence the Active set, roles, parents and process ids equal the independent model.
 */
static int test_fuzz_convergence(void) {
    return fuzz_run_many(40u, 3000u, 0u, UINT64_C(0x9E3779B97F4A7C15));
}

/** Convergence fuzz, stressed: every seed has overflow bursts, lazy draining (GraphQueueFull) and truncated enumerations. */
static int test_fuzz_convergence_stressed(void) {
    return fuzz_run_many(8u, 3000u, FuzzOptionBursts | FuzzOptionLazyDrain | FuzzOptionTruncated,
                         UINT64_C(0xD1B54A32D192ED03));
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
    TestEntry(test_create_config_overlaps_slot),
    TestEntry(test_create_no_memory),
    TestEntry(test_lifecycle_start_stop_restart),
    TestEntry(test_lifecycle_second_observer),
    TestEntry(test_lifecycle_reservation_covers_hook_removal),
    TestEntry(test_lifecycle_hook_failure_rollback),
    TestEntry(test_lifecycle_unhook_failure),
    TestEntry(test_lifecycle_start_rollback_unhook_retry),
    TestEntry(test_lifecycle_destroy_started),
    TestEntry(test_destroy_stopped_without_graph_resources),
    TestEntry(test_lifecycle_wrong_thread),
    TestEntry(test_hint_filter_table),
    TestEntry(test_hint_wrong_thread),
    TestEntry(test_release_slot_waits_for_inflight_hint),
    TestEntry(test_dirty_coalesce_order_budget),
    TestEntry(test_dirty_overflow_recovery),
    TestEntry(test_flood_child_hints_no_churn),
    TestEntry(test_overflow_recovery_gone_and_recycled),
    TestEntry(test_overflow_pump_statuses),
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
    TestEntry(test_refresh_reopens_existing_transaction),
    TestEntry(test_refresh_repeated_destroy_retry_bound),
    TestEntry(test_pump_repeated_destroy_retry_bound),
    TestEntry(test_bind_rejects_used_stamp_clock_before_mutation),
    TestEntry(test_refresh_defers_child_before_parent),
    TestEntry(test_refresh_failed_window_kept),
    TestEntry(test_refresh_unenumerated_survives),
    TestEntry(test_refresh_vanished_retired),
    TestEntry(test_refresh_enum_failure),
    TestEntry(test_refresh_truncated_degrades),
    TestEntry(test_refresh_cadence_latch),
    TestEntry(test_refresh_queue_full_abort),
    TestEntry(test_refresh_commit_queue_full),
    TestEntry(test_refresh_replacement_observer),
    TestEntry(test_owner_hidden_promotion),
    TestEntry(test_owner_hidden_gone_and_failed),
    TestEntry(test_hidden_window_skips_owner_query),
    TestEntry(test_owner_hidden_uac),
    TestEntry(test_preedit_parenting),
    TestEntry(test_preedit_from_config),
    TestEntry(test_own_process_excluded),
    TestEntry(test_teardown_with_hints),
    TestEntry(test_protected_destroy_handoff_contract),
    TestEntry(test_untracked_destroy_capacity_fault),
    TestEntry(test_protected_admission_reserve_boundary),
    TestEntry(test_protected_stop_queue_full_retry),
    TestEntry(test_protected_drain_queue_full_retry),
    TestEntry(test_protected_destroy_exact_stamp_replacement),
    TestEntry(test_repeated_destroy_after_tracked_removal),
    TestEntry(test_candidate_transition_protects_proposed_key),
    TestEntry(test_full_candidate_transition_retains_both_keys),
    TestEntry(test_full_candidate_transition_bounded_pending),
    TestEntry(test_overflow_retains_untracked_destroy_payloads),
    TestEntry(test_protected_destroy_during_start_rollback),
    TestEntry(test_protected_destroy_during_unhook_retry),
    TestEntry(test_start_final_clock_fault_rolls_back),
    TestEntry(test_tick_callback_fault_prevents_graph_mutation),
    TestEntry(test_query_callback_fault_prevents_graph_mutation),
    TestEntry(test_protected_destroy_during_deferred_promotion),
    TestEntry(test_stats_accounting),
    TestEntry(test_entry_arguments_and_threads),
    TestEntry(test_o5_public_guard_matrix),
    TestEntry(test_start_zero_token),
    TestEntry(test_failed_lifecycle_rejects_sync_and_configuration),
    TestEntry(test_hint_during_install),
    TestEntry(test_hint_destroy_coalesces),
    TestEntry(test_facts_overlay_corners),
    TestEntry(test_putback_merge_queue_full),
    TestEntry(test_putback_merge_fatal),
    TestEntry(test_putback_merge_retry),
    TestEntry(test_pump_destroy_status_paths),
    TestEntry(test_refresh_enumeration_oddities),
    TestEntry(test_refresh_graph_refresh_lost),
    TestEntry(test_refresh_unseen_stamp_exhausted),
    TestEntry(test_cadence_corners),
    TestEntry(test_fuzz_convergence),
    TestEntry(test_fuzz_convergence_stressed),
};

/** Run every test; stop at the first failure and return nonzero. */
#if defined(WgObsPrivateGuards) && WgObsPrivateGuards
/** @brief Run exact-production scalar/guard probes; zero means every boundary passed. */
extern int window_observer_private_guard_tests(void);
#endif
int main(void) {
#if defined(WgObsPrivateGuards) && WgObsPrivateGuards
    if (window_observer_private_guard_tests() != 0) return 1;
#endif
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
