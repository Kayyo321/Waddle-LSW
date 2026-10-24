/**
 * @file window_graph.c
 * @brief C ABI consumer tests for the guest window graph (include/waddle/interop_window_graph.h).
 *
 * Links against the Zig ABI object only; it sees nothing but the public header. Every fixture is a
 * private value. Each test returns 0 on success and 1 on the first failed Check. main() runs every test
 * and exits nonzero when any fails. Tests are single-threaded except where a second thread is the point
 * (run_on_second_thread). Every graph created here is destroyed, so the sanitizer builds report no leak.
 */
#include "waddle/interop_window_graph.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define Check(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "window_graph.c:%d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define StOk InteropWindowGraphStatusOk
#define StNone InteropWindowGraphStatusNone
#define StInvalidArgument InteropWindowGraphStatusInvalidArgument
#define StInvalidFacts InteropWindowGraphStatusInvalidFacts
#define StWrongThread InteropWindowGraphStatusWrongThread
#define StReentrant InteropWindowGraphStatusReentrant
#define StStaleEpoch InteropWindowGraphStatusStaleEpoch
#define StStale InteropWindowGraphStatusStale
#define StStaleFacts InteropWindowGraphStatusStaleFacts
#define StQueueFull InteropWindowGraphStatusQueueFull
#define StAtCapacity InteropWindowGraphStatusAtCapacity
#define StCycle InteropWindowGraphStatusCycle
#define StLookupFailed InteropWindowGraphStatusLookupFailed
#define StAppKeyInvalid InteropWindowGraphStatusAppKeyInvalid
#define StRefreshActive InteropWindowGraphStatusRefreshActive
#define StNoRefresh InteropWindowGraphStatusNoRefresh
#define StProofIncomplete InteropWindowGraphStatusProofIncomplete
#define StNotRetiring InteropWindowGraphStatusNotRetiring
#define StSinkFailed InteropWindowGraphStatusSinkFailed

/** @brief Base captured_ms tick of the fixtures; large so that "tick minus one" never wraps. */
#define BaseTick 100000u
/** @brief Number of operations in the random trace. */
#define TraceOps 24000u
/** @brief Number of distinct HWNDs in the random trace. */
#define TraceWindows 12u
/** @brief First HWND of the random trace; window i is TraceFirstWindow + i. */
#define TraceFirstWindow 0x100u
/** @brief Size of the per-generation model tables of the random trace. */
#define TraceMaxGeneration 131072u
/** @brief Number of calls in the guard battery (destroy is the last one). */
#define BatteryCount 19u
/** @brief Highest event index the shared event buffer holds. */
#define EventBufferSize 512u
/** @brief Byte used to poison output structs so "unchanged" is observable. */
#define PoisonByte 0x5Au

static uint64_t g_stamp = 0;
static uint32_t g_tick = BaseTick;
static interop_window_event_t g_events[EventBufferSize];

/* ---- Fixtures ---------------------------------------------------------------------------- */

/** @brief Copy text into a fixed byte field (no NUL) and store its length. */
static void set_text(uint8_t *dst, size_t capacity, uint32_t *length, const char *text) {
    size_t n = strlen(text);
    if (n > capacity) {
        n = capacity;
    }
    memset(dst, 0, capacity);
    memcpy(dst, text, n);
    *length = (uint32_t)n;
}

/** @brief Set the window class of a facts struct. */
static void set_class(interop_window_facts_t *f, const char *text) {
    set_text(f->class_name, sizeof f->class_name, &f->class_length, text);
}

/** @brief Set the image basename and keep the ImageUnknown flag consistent with its length. */
static void set_image(interop_window_facts_t *f, const char *text) {
    set_text(f->image_name, sizeof f->image_name, &f->image_length, text);
    if (f->image_length == 0u) {
        f->flags |= InteropWindowFactImageUnknown;
    } else {
        f->flags &= ~(uint32_t)InteropWindowFactImageUnknown;
    }
}

/** @brief Set the title of a facts struct (an empty string is a valid title). */
static void set_title(interop_window_facts_t *f, const char *text) {
    set_text(f->title, sizeof f->title, &f->title_length, text);
}

/** @brief Give a facts struct a fresh stamp and a fresh tick (what a new observer query would do). */
static void restamp(interop_window_facts_t *f) {
    f->stamp = ++g_stamp;
    f->captured_ms = ++g_tick;
}

/** @brief Build a valid visible top-level window snapshot with a fresh stamp and tick. */
static void fill_toplevel(interop_window_facts_t *f, uint64_t window) {
    memset(f, 0, sizeof *f);
    f->window = window;
    f->root = window;
    f->stamp = ++g_stamp;
    f->style = InteropWindowStyleCaption | InteropWindowStyleThickFrame |
               InteropWindowStyleMaximizeBox | InteropWindowStyleMinimizeBox;
    f->ex_style = 0u;
    f->flags = InteropWindowFactVisible | InteropWindowFactEnabled | InteropWindowFactOnInputDesktop;
    f->process_id = 1000u;
    f->thread_id = 2000u;
    f->process_start_time = 5000u;
    f->captured_ms = ++g_tick;
    f->x = 10;
    f->y = 20;
    f->width = 800u;
    f->height = 600u;
    f->dpi = 96u;
    set_class(f, "TestWindow");
    set_image(f, "app.exe");
    set_title(f, "title");
}

/** @brief Build a valid owned (dialog) snapshot: owner and owner_root are the given window. */
static void fill_dialog(interop_window_facts_t *f, uint64_t window, uint64_t owner) {
    fill_toplevel(f, window);
    f->owner = owner;
    f->owner_root = owner;
    f->style = InteropWindowStyleCaption;
}

/** @brief Fill an output struct with the poison byte. */
static void poison(void *p, size_t n) {
    memset(p, PoisonByte, n);
}

/** @brief Return 1 when every byte of the object still holds the poison byte. */
static int untouched(const void *p, size_t n) {
    const uint8_t *bytes = p;
    for (size_t i = 0; i < n; i++) {
        if (bytes[i] != PoisonByte) {
            return 0;
        }
    }
    return 1;
}

/* ---- Fake clock and stubs ----------------------------------------------------------------- */

/** @brief Fake monotonic clock: the context points at a uint64_t millisecond value. */
static uint64_t fake_now_ms(void *context) {
    return *(const uint64_t *)context;
}

/** @brief Statuses of one guard battery run (every call that has guard rules). */
typedef struct battery_t {
    interop_window_graph_t *graph;
    uint64_t epoch;
    interop_window_graph_status_t status[BatteryCount];
    int slot_kept;
    interop_window_graph_status_t classify_status;
} battery_t;

/** @brief Sink that accepts everything; used only as a valid argument. */
static int accept_sink(void *context, const interop_window_event_t *event) {
    (void)context;
    (void)event;
    return 0;
}

/** @brief Call every graph function once with otherwise valid arguments and record the statuses. */
static void run_battery(battery_t *b) {
    interop_window_graph_t *g = b->graph;
    uint64_t ep = b->epoch;
    interop_window_facts_t f;
    interop_window_verdict_t verdict;
    interop_window_observation_t o;
    interop_window_event_t ev;
    interop_window_retire_proof_t proof = {1u, 1u};
    interop_window_capacity_diagnostic_t diag;
    interop_window_graph_stats_t stats;
    interop_window_live_t live;
    interop_window_retiring_entry_t entries[4];
    uint64_t windows[4];
    uint64_t stamp = 0;
    uint32_t count = 0;
    uint32_t delivered = 0;
    uint32_t i = 0;
    interop_window_graph_t *slot = g;

    fill_toplevel(&f, 0x9990u);
    b->status[i++] = interop_window_graph_observe(g, ep, &f, &o);
    b->status[i++] = interop_window_graph_note_destroyed(g, ep, 0x9990u, ++g_stamp, g_tick, &o);
    b->status[i++] = interop_window_graph_complete_retire(g, ep, 0x9990u, 1u, &proof);
    b->status[i++] = interop_window_graph_check_action(g, ep, 0x9990u, 1u);
    b->status[i++] = interop_window_graph_invalidate_all(g, ep, NULL);
    b->status[i++] = interop_window_graph_refresh_begin(g, ep);
    b->status[i++] = interop_window_graph_refresh_offer(g, ep, &f, &o);
    b->status[i++] = interop_window_graph_refresh_keep(g, ep, 0x9990u);
    b->status[i++] = interop_window_graph_refresh_unseen(g, ep, windows, 4u, &count);
    b->status[i++] = interop_window_graph_refresh_commit(g, ep, NULL);
    b->status[i++] = interop_window_graph_refresh_abort(g, ep);
    b->status[i++] = interop_window_graph_next_event(g, ep, &ev);
    b->status[i++] = interop_window_graph_drain(g, ep, accept_sink, NULL, 1u, &delivered);
    b->status[i++] = interop_window_graph_take_capacity_diagnostic(g, &diag);
    b->status[i++] = interop_window_graph_get_stats(g, &stats);
    b->status[i++] = interop_window_graph_stamp_high_water(g, &stamp);
    b->status[i++] = interop_window_graph_find_live(g, 0x9990u, &live);
    b->status[i++] = interop_window_graph_list_retiring(g, entries, 4u, &count);
    b->status[i++] = interop_window_graph_destroy(&slot);
    b->slot_kept = (slot == g);
    b->classify_status = interop_window_graph_classify(&f, InteropWindowUacWindow, &verdict);
}

/** @brief Configurable application-key lookup stub. */
typedef struct lookup_stub_t {
    int result;                 /**< Value the callback returns (0 = success). */
    uint8_t key[256];           /**< Bytes written on success. */
    uint32_t key_length;        /**< Number of bytes of key to write (clamped to capacity). */
    int use_override;           /**< When nonzero, report length_override instead of key_length. */
    uint32_t length_override;   /**< Length reported through *length when use_override is set. */
    uint32_t calls;             /**< Number of callback invocations. */
    uint64_t last_window;       /**< window of the facts passed to the last call. */
    const interop_window_facts_t *last_facts; /**< Pointer passed to the last call. */
    uint32_t last_preset_length; /**< Value of *length seen on entry to the last call. */
    interop_window_facts_t *mutate; /**< When non-NULL, the callback scribbles over this caller struct. */
    battery_t *battery;         /**< When non-NULL, the callback runs the guard battery (re-entry). */
    interop_window_graph_t *graph; /**< Graph the battery targets. */
    uint64_t epoch;             /**< Epoch the battery passes. */
} lookup_stub_t;

/** @brief Set the stub key from text. */
static void stub_set_key(lookup_stub_t *s, const char *text) {
    size_t n = strlen(text);
    memset(s->key, 0, sizeof s->key);
    memcpy(s->key, text, n);
    s->key_length = (uint32_t)n;
    s->use_override = 0;
}

/** @brief Reset the stub to "succeed with the key a-test-app". */
static void stub_reset(lookup_stub_t *s) {
    memset(s, 0, sizeof *s);
    stub_set_key(s, "a-test-app");
}

/** @brief The app_key_lookup callback implemented by lookup_stub_t. */
static int stub_lookup(void *context, const interop_window_facts_t *facts, uint8_t *key,
                       uint32_t capacity, uint32_t *length) {
    lookup_stub_t *s = context;
    uint32_t n;
    s->calls++;
    s->last_window = facts->window;
    s->last_facts = facts;
    s->last_preset_length = *length;
    if (s->mutate != NULL) {
        s->mutate->width = 1u;
        s->mutate->window = 0u;
        s->mutate->title[0] = 'Z';
    }
    if (s->battery != NULL) {
        s->battery->graph = s->graph;
        s->battery->epoch = s->epoch;
        run_battery(s->battery);
    }
    if (s->result != 0) {
        return s->result;
    }
    n = s->key_length;
    if (n > capacity) {
        n = capacity;
    }
    memcpy(key, s->key, n);
    *length = s->use_override ? s->length_override : s->key_length;
    return 0;
}

/** @brief Test harness: one graph plus its stub, fake clock and epoch. */
typedef struct harness_t {
    interop_window_graph_t *graph;
    uint64_t epoch;
    lookup_stub_t lookup;
    uint64_t clock_ms;
} harness_t;

/** @brief Create a graph in the harness (the harness must not move afterwards). Returns 0 on success. */
static int h_init(harness_t *h, uint64_t epoch, uint32_t uac_mode) {
    interop_window_graph_config_t config;
    memset(h, 0, sizeof *h);
    h->epoch = epoch;
    h->clock_ms = 1000u;
    stub_reset(&h->lookup);
    memset(&config, 0, sizeof config);
    config.session_epoch = epoch;
    config.uac_mode = uac_mode;
    config.app_key_lookup = stub_lookup;
    config.app_key_context = &h->lookup;
    config.now_ms = fake_now_ms;
    config.now_ms_context = &h->clock_ms;
    Check(interop_window_graph_create(&config, &h->graph) == StOk);
    Check(h->graph != NULL);
    return 0;
}

/** @brief Destroy the harness graph and check the handle is cleared. Returns 0 on success. */
static int h_free(harness_t *h) {
    Check(interop_window_graph_destroy(&h->graph) == StOk);
    Check(h->graph == NULL);
    return 0;
}

static interop_window_graph_status_t h_observe(harness_t *h, const interop_window_facts_t *f,
                                               interop_window_observation_t *o) {
    return interop_window_graph_observe(h->graph, h->epoch, f, o);
}

static interop_window_graph_status_t h_offer(harness_t *h, const interop_window_facts_t *f,
                                             interop_window_observation_t *o) {
    return interop_window_graph_refresh_offer(h->graph, h->epoch, f, o);
}

static interop_window_graph_status_t h_destroyed(harness_t *h, uint64_t window, uint64_t stamp,
                                                 uint32_t tick, interop_window_observation_t *o) {
    return interop_window_graph_note_destroyed(h->graph, h->epoch, window, stamp, tick, o);
}

static interop_window_graph_status_t h_complete(harness_t *h, uint64_t window, uint64_t generation,
                                                uint32_t capture, uint32_t pins) {
    interop_window_retire_proof_t proof;
    proof.capture_stopped = capture;
    proof.pins_drained = pins;
    return interop_window_graph_complete_retire(h->graph, h->epoch, window, generation, &proof);
}

static interop_window_graph_status_t h_next(harness_t *h, interop_window_event_t *ev) {
    return interop_window_graph_next_event(h->graph, h->epoch, ev);
}

/** @brief Read the stats or terminate the process (a failing get_stats makes later checks meaningless). */
static interop_window_graph_stats_t stats_of(harness_t *h) {
    interop_window_graph_stats_t s;
    memset(&s, 0, sizeof s);
    if (interop_window_graph_get_stats(h->graph, &s) != StOk) {
        fprintf(stderr, "window_graph.c: get_stats failed\n");
        exit(1);
    }
    return s;
}

/** @brief Pop all queued events into out; terminate if more than capacity are queued. */
static uint32_t drain_events(harness_t *h, interop_window_event_t *out, uint32_t capacity) {
    uint32_t n = 0;
    for (;;) {
        interop_window_event_t ev;
        interop_window_graph_status_t st = h_next(h, &ev);
        if (st == StNone) {
            return n;
        }
        if (st != StOk || n >= capacity) {
            fprintf(stderr, "window_graph.c: drain_events failed\n");
            exit(1);
        }
        out[n++] = ev;
    }
}

/** @brief Pop and discard all queued events; returns how many there were. */
static uint32_t drop_events(harness_t *h) {
    return drain_events(h, g_events, EventBufferSize);
}

/** @brief Admit a fresh top-level window and return its generation. Returns 0 on success. */
static int admit_top(harness_t *h, uint64_t window, uint64_t *generation) {
    interop_window_facts_t f;
    interop_window_observation_t o;
    fill_toplevel(&f, window);
    Check(h_observe(h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionAdmitted);
    Check(o.generation != 0u);
    if (generation != NULL) {
        *generation = o.generation;
    }
    return 0;
}

/** @brief Offer a destroy hint that is newer than any fixture of the window. Returns 0 on success. */
static int kill_window(harness_t *h, uint64_t window, interop_window_observation_t *o) {
    Check(h_destroyed(h, window, ++g_stamp, ++g_tick, o) == StOk);
    return 0;
}

/** @brief Complete a retirement with a full proof. Returns 0 on success. */
static int finish_retire(harness_t *h, uint64_t window, uint64_t generation) {
    Check(h_complete(h, window, generation, 1u, 1u) == StOk);
    return 0;
}

/** @brief Zero the diagnostic-only counters so two stats snapshots can be compared for state. */
static void mask_diagnostics(interop_window_graph_stats_t *s) {
    s->rejected_at_capacity = 0u;
    s->rejected_queue_full = 0u;
    s->rejected_stale_epoch = 0u;
    s->rejected_stale_facts = 0u;
    s->rejected_invalid_facts = 0u;
    s->rejected_cycle = 0u;
    s->rejected_generation_exhausted = 0u;
    s->lookup_failures = 0u;
    s->app_key_invalid = 0u;
    s->sink_failures = 0u;
    s->rejected_stale = 0u;
    s->proof_incomplete = 0u;
    s->suspended = 0u;
}

/** @brief Return 1 when two stats snapshots differ only in diagnostic counters. */
static int same_state(const interop_window_graph_stats_t *a, const interop_window_graph_stats_t *b) {
    interop_window_graph_stats_t x = *a;
    interop_window_graph_stats_t y = *b;
    mask_diagnostics(&x);
    mask_diagnostics(&y);
    return memcmp(&x, &y, sizeof x) == 0;
}

/* ---- Second-thread helper ------------------------------------------------------------------- */

typedef void (*thread_fn_t)(void *arg);

typedef struct thread_start_t {
    thread_fn_t fn;
    void *arg;
} thread_start_t;

#ifdef _WIN32
static DWORD WINAPI thread_entry(LPVOID raw) {
    thread_start_t *start = raw;
    start->fn(start->arg);
    return 0;
}

/** @brief Run fn(arg) on a new thread and join it. Returns 0 on success. */
static int run_on_second_thread(thread_fn_t fn, void *arg) {
    thread_start_t start;
    HANDLE thread;
    start.fn = fn;
    start.arg = arg;
    thread = CreateThread(NULL, 0, thread_entry, &start, 0, NULL);
    if (thread == NULL) {
        return 1;
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return 0;
}
#else
static void *thread_entry(void *raw) {
    thread_start_t *start = raw;
    start->fn(start->arg);
    return NULL;
}

/** @brief Run fn(arg) on a new thread and join it. Returns 0 on success. */
static int run_on_second_thread(thread_fn_t fn, void *arg) {
    thread_start_t start;
    pthread_t thread;
    start.fn = fn;
    start.arg = arg;
    if (pthread_create(&thread, NULL, thread_entry, &start) != 0) {
        return 1;
    }
    return pthread_join(thread, NULL) != 0;
}
#endif

/** @brief Thread body: run the guard battery against the graph in the battery struct. */
static void thread_battery(void *arg) {
    run_battery((battery_t *)arg);
}

/** @brief Thread body: destroy the graph through a slot copy and record whether the slot was kept. */
typedef struct destroy_job_t {
    interop_window_graph_t *graph;
    interop_window_graph_t *slot;
    interop_window_graph_status_t status;
} destroy_job_t;

static void thread_destroy(void *arg) {
    destroy_job_t *job = arg;
    job->slot = job->graph;
    job->status = interop_window_graph_destroy(&job->slot);
}

/* ---- Test: runtime sizes and constants ------------------------------------------------------ */

static int test_runtime_sizes(void) {
    Check(sizeof(interop_window_facts_t) == 1656u);
    Check(sizeof(interop_window_verdict_t) == 24u);
    Check(sizeof(interop_window_event_t) == 1328u);
    Check(sizeof(interop_window_observation_t) == 56u);
    Check(sizeof(interop_window_live_t) == 48u);
    Check(sizeof(interop_window_retiring_entry_t) == 24u);
    Check(sizeof(interop_window_retire_proof_t) == 8u);
    Check(sizeof(interop_window_capacity_diagnostic_t) == 56u);
    Check(sizeof(interop_window_graph_stats_t) == 256u);
    Check(sizeof(interop_window_graph_config_t) == 48u);
    Check(sizeof(interop_window_graph_status_t) == 4u);
    Check(InteropMaxWindows == 64u);
    Check(InteropWindowGraphMaxDeferred == 64u);
    Check(InteropWindowGraphEventCapacity == 256u);
    Check(InteropWindowGraphMaxCallEvents == 192u);
    Check(InteropWindowGraphDiagnosticIntervalMs == 60000u);
    Check(InteropWindowFactMask == 0x1FFFu);
    Check(InteropWindowChangeMask == 0x1Fu);
    Check(InteropWindowGraphStatusOk == 0 && InteropWindowGraphStatusNone == 1);
    Check(InteropWindowGraphStatusInvalidArgument == 2 && InteropWindowGraphStatusWrongThread == 4);
    Check(InteropWindowGraphStatusReentrant == 5 && InteropWindowGraphStatusStaleEpoch == 7);
    Check(InteropWindowGraphStatusQueueFull == 10 && InteropWindowGraphStatusAtCapacity == 11);
    Check(InteropWindowGraphStatusSinkFailed == 20);
    Check(InteropWindowEventAnnounce == 1 && InteropWindowEventUpdate == 2 &&
          InteropWindowEventRetireIntent == 3);
    Check(InteropWindowDispositionUnchanged == 1 && InteropWindowDispositionSuspended == 8);
    Check(InteropWindowReasonParentPending == 10 && InteropWindowReasonInvalidated == 16);
    Check(InteropWindowVerdictEligible == 1 && InteropWindowVerdictSuspended == 3);
    return 0;
}

/* ---- Test: create and destroy --------------------------------------------------------------- */

static int test_create_destroy(void) {
    lookup_stub_t stub;
    interop_window_graph_config_t config;
    interop_window_graph_t *graph = NULL;
    int dummy = 0;
    interop_window_graph_t *junk = (interop_window_graph_t *)&dummy;
    harness_t h;
    battery_t battery;
    destroy_job_t job;
    interop_window_graph_stats_t before;
    interop_window_capacity_diagnostic_t diag;
    uint32_t i;

    stub_reset(&stub);
    memset(&config, 0, sizeof config);
    config.session_epoch = 5u;
    config.uac_mode = InteropWindowUacAuto;
    config.app_key_lookup = stub_lookup;
    config.app_key_context = &stub;

    /* NULL slot and NULL config. */
    Check(interop_window_graph_create(&config, NULL) == StInvalidArgument);
    graph = junk;
    Check(interop_window_graph_create(NULL, &graph) == StInvalidArgument);
    Check(graph == NULL);

    /* Every config field, one at a time; the slot is NULL after each failure. */
    config.session_epoch = 0u;
    graph = junk;
    Check(interop_window_graph_create(&config, &graph) == StInvalidArgument);
    Check(graph == NULL);
    config.session_epoch = 5u;
    config.uac_mode = 3u;
    graph = junk;
    Check(interop_window_graph_create(&config, &graph) == StInvalidArgument);
    Check(graph == NULL);
    config.uac_mode = InteropWindowUacAuto;
    config.reserved0 = 1u;
    graph = junk;
    Check(interop_window_graph_create(&config, &graph) == StInvalidArgument);
    Check(graph == NULL);
    config.reserved0 = 0u;
    config.app_key_lookup = NULL;
    graph = junk;
    Check(interop_window_graph_create(&config, &graph) == StInvalidArgument);
    Check(graph == NULL);
    config.app_key_lookup = stub_lookup;

    /* Valid config with the default clock (now_ms NULL); the slot receives a handle. */
    graph = NULL;
    Check(interop_window_graph_create(&config, &graph) == StOk);
    Check(graph != NULL);
    Check(interop_window_graph_take_capacity_diagnostic(graph, &diag) == StNone);
    for (i = 0; i <= 2u; i++) {
        config.uac_mode = i;
        {
            interop_window_graph_t *other = NULL;
            Check(interop_window_graph_create(&config, &other) == StOk);
            Check(interop_window_graph_destroy(&other) == StOk);
            Check(other == NULL);
        }
    }

    /* destroy: NULL slot, no-op on a NULL handle, real destroy, double destroy. */
    Check(interop_window_graph_destroy(NULL) == StInvalidArgument);
    {
        interop_window_graph_t *empty = NULL;
        Check(interop_window_graph_destroy(&empty) == StOk);
        Check(empty == NULL);
    }
    Check(interop_window_graph_destroy(&graph) == StOk);
    Check(graph == NULL);
    Check(interop_window_graph_destroy(&graph) == StOk);
    Check(graph == NULL);

    /* A NULL graph makes every guarded call InvalidArgument; destroy of a NULL handle is Ok. */
    memset(&battery, 0, sizeof battery);
    battery.graph = NULL;
    battery.epoch = 5u;
    run_battery(&battery);
    for (i = 0; i + 1u < BatteryCount; i++) {
        Check(battery.status[i] == StInvalidArgument);
    }
    Check(battery.status[BatteryCount - 1u] == StOk);
    Check(battery.classify_status == StOk);

    /* Destroy from another thread must not free; the real destroy then frees. */
    Check(h_init(&h, 9u, InteropWindowUacWindow) == 0);
    {
        interop_window_facts_t f;
        interop_window_observation_t o;
        fill_toplevel(&f, 0x10u);
        Check(h_observe(&h, &f, &o) == StOk);
    }
    before = stats_of(&h);
    job.graph = h.graph;
    job.slot = NULL;
    job.status = StOk;
    Check(run_on_second_thread(thread_destroy, &job) == 0);
    Check(job.status == StWrongThread);
    Check(job.slot == h.graph);
    {
        interop_window_graph_stats_t after = stats_of(&h);
        Check(memcmp(&before, &after, sizeof before) == 0);
    }
    Check(h_free(&h) == 0);

    /* Destroying a graph that still holds events, a deferred record and a quarantine is fine. */
    Check(h_init(&h, 10u, InteropWindowUacWindow) == 0);
    {
        interop_window_facts_t f;
        interop_window_observation_t o;
        fill_toplevel(&f, 0x20u);
        Check(h_observe(&h, &f, &o) == StOk);
        fill_toplevel(&f, 0x21u);
        Check(h_observe(&h, &f, &o) == StOk);
        Check(kill_window(&h, 0x21u, &o) == 0);
        fill_dialog(&f, 0x22u, 0x23u);
        Check(h_observe(&h, &f, &o) == StOk);
        Check(o.disposition == InteropWindowDispositionDeferred);
    }
    Check(h_free(&h) == 0);
    return 0;
}

/* ---- Test: create rejects config/output aliases without writing either range ---------------- */

static int test_create_alias_preserves_inputs(void) {
    union alias_storage_t {
        max_align_t alignment;
        uint8_t bytes[sizeof(interop_window_graph_config_t) + 16u];
    } storage;
    uint8_t before[sizeof storage.bytes];
    interop_window_graph_status_t status;

    /* Aligned config; output aliases callback-context bytes. */
    memset(&storage, 0xA5, sizeof storage.bytes);
    memcpy(before, storage.bytes, sizeof before);
    status = interop_window_graph_create(
        (const interop_window_graph_config_t *)(const void *)storage.bytes,
        (interop_window_graph_t **)(void *)(storage.bytes + offsetof(interop_window_graph_config_t, app_key_context)));
    Check(status == StInvalidArgument);
    Check(memcmp(storage.bytes, before, sizeof before) == 0);

    /* Misaligned config still overlaps the aligned output slot; reject before any write. */
    memset(&storage, 0x5A, sizeof storage.bytes);
    memcpy(before, storage.bytes, sizeof before);
    status = interop_window_graph_create(
        (const interop_window_graph_config_t *)(const void *)(storage.bytes + 1u),
        (interop_window_graph_t **)(void *)(storage.bytes + 16u));
    Check(status == StInvalidArgument);
    Check(memcmp(storage.bytes, before, sizeof before) == 0);
    return 0;
}

/* ---- Test: invalid arguments ---------------------------------------------------------------- */

static int test_invalid_arguments(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_event_t ev;
    interop_window_retire_proof_t proof = {1u, 1u};
    interop_window_live_t live;
    interop_window_retiring_entry_t entries[2];
    interop_window_capacity_diagnostic_t diag;
    interop_window_graph_stats_t stats;
    uint64_t windows[2];
    uint64_t stamp = 77u;
    uint32_t count = 0x1234u;
    uint32_t delivered = 0x1234u;
    uint32_t retired = 0x1234u;

    Check(h_init(&h, 3u, InteropWindowUacWindow) == 0);
    fill_toplevel(&f, 0x40u);
    Check(h_observe(&h, &f, &o) == StOk);
    poison(&o, sizeof o);
    Check(h_observe(&h, NULL, &o) == StInvalidArgument);
    Check(untouched(&o, sizeof o));
    Check(h_observe(&h, &f, NULL) == StInvalidArgument);
    Check(h_destroyed(&h, 0u, 5u, 5u, &o) == StInvalidArgument);
    Check(h_destroyed(&h, 0x40u, 0u, 5u, &o) == StInvalidArgument);
    Check(h_destroyed(&h, 0x40u, 5u, 5u, NULL) == StInvalidArgument);
    Check(h_complete(&h, 0u, 1u, 1u, 1u) == StInvalidArgument);
    Check(h_complete(&h, 0x40u, 0u, 1u, 1u) == StInvalidArgument);
    Check(interop_window_graph_complete_retire(h.graph, h.epoch, 0x40u, 1u, NULL) == StInvalidArgument);
    Check(interop_window_graph_check_action(h.graph, h.epoch, 0u, 1u) == StInvalidArgument);
    Check(interop_window_graph_check_action(h.graph, h.epoch, 0x40u, 0u) == StInvalidArgument);
    poison(&ev, sizeof ev);
    Check(interop_window_graph_next_event(h.graph, h.epoch, NULL) == StInvalidArgument);
    Check(interop_window_graph_drain(h.graph, h.epoch, NULL, NULL, 1u, &delivered) == StInvalidArgument);
    Check(interop_window_graph_drain(h.graph, h.epoch, accept_sink, NULL, 0u, &delivered) == StInvalidArgument);
    Check(interop_window_graph_drain(h.graph, h.epoch, accept_sink, NULL, 1u, NULL) == StInvalidArgument);
    Check(delivered == 0x1234u);
    Check(interop_window_graph_refresh_keep(h.graph, h.epoch, 0u) == StInvalidArgument);
    Check(interop_window_graph_get_stats(h.graph, NULL) == StInvalidArgument);
    Check(interop_window_graph_stamp_high_water(h.graph, NULL) == StInvalidArgument);
    Check(interop_window_graph_take_capacity_diagnostic(h.graph, NULL) == StInvalidArgument);
    poison(&live, sizeof live);
    Check(interop_window_graph_find_live(h.graph, 0u, &live) == StInvalidArgument);
    Check(interop_window_graph_find_live(h.graph, 0x40u, NULL) == StInvalidArgument);
    Check(untouched(&live, sizeof live));
    Check(interop_window_graph_list_retiring(h.graph, entries, 2u, NULL) == StInvalidArgument);
    Check(interop_window_graph_list_retiring(h.graph, NULL, 2u, &count) == StInvalidArgument);
    Check(count == 0x1234u);
    Check(interop_window_graph_list_retiring(h.graph, NULL, 0u, &count) == StOk);
    Check(count == 0u);

    /* Refresh pointer arguments: NULL pointers are InvalidArgument even with no open refresh. */
    Check(h_offer(&h, NULL, &o) == StInvalidArgument);
    Check(h_offer(&h, &f, NULL) == StInvalidArgument);
    Check(h_offer(&h, &f, &o) == StNoRefresh);
    Check(interop_window_graph_refresh_begin(h.graph, h.epoch) == StOk);
    count = 0x1234u;
    Check(interop_window_graph_refresh_unseen(h.graph, h.epoch, windows, 2u, NULL) == StInvalidArgument);
    Check(interop_window_graph_refresh_unseen(h.graph, h.epoch, NULL, 2u, &count) == StInvalidArgument);
    Check(count == 0x1234u);
    Check(interop_window_graph_refresh_unseen(h.graph, h.epoch, NULL, 0u, &count) == StOk);
    Check(count == 1u);
    Check(interop_window_graph_refresh_abort(h.graph, h.epoch) == StOk);

    /* Wrong epoch is reported before argument errors, with no output written. */
    Check(interop_window_graph_complete_retire(h.graph, h.epoch + 1u, 0u, 0u, NULL) == StStaleEpoch);
    Check(interop_window_graph_check_action(h.graph, 0u, 0x40u, 1u) == StStaleEpoch);
    Check(interop_window_graph_invalidate_all(h.graph, h.epoch + 1u, &retired) == StStaleEpoch);
    Check(retired == 0x1234u);
    Check(interop_window_graph_next_event(h.graph, h.epoch + 1u, &ev) == StStaleEpoch);
    Check(untouched(&ev, sizeof ev));
    Check(interop_window_graph_refresh_begin(h.graph, h.epoch + 1u) == StStaleEpoch);
    Check(interop_window_graph_refresh_keep(h.graph, h.epoch + 1u, 0x40u) == StStaleEpoch);
    Check(interop_window_graph_refresh_commit(h.graph, h.epoch + 1u, NULL) == StStaleEpoch);
    Check(interop_window_graph_refresh_abort(h.graph, h.epoch + 1u) == StStaleEpoch);
    Check(interop_window_graph_drain(h.graph, h.epoch + 1u, accept_sink, NULL, 1u, &delivered) == StStaleEpoch);
    Check(delivered == 0x1234u);
    Check(interop_window_graph_complete_retire(h.graph, h.epoch, 0x40u, 99u, &proof) == StStale);

    /* Read-only getters leave outputs unchanged on error and fill them on success. */
    poison(&stats, sizeof stats);
    Check(interop_window_graph_get_stats(h.graph, &stats) == StOk);
    Check(stats.session_epoch == 3u);
    Check(interop_window_graph_stamp_high_water(h.graph, &stamp) == StOk);
    Check(stamp == f.stamp);
    Check(interop_window_graph_take_capacity_diagnostic(h.graph, &diag) == StNone);
    return h_free(&h);
}

/* ---- Test: full lifecycle ------------------------------------------------------------------- */

static int test_lifecycle(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_event_t ev;
    interop_window_graph_stats_t s;
    uint32_t expected_state = InteropStateResizable | InteropStateMaximizable | InteropStateMinimizable;

    Check(h_init(&h, 42u, InteropWindowUacWindow) == 0);
    fill_toplevel(&f, 0x1000u);
    f.x = 10;
    f.y = -20;
    f.width = 800u;
    f.height = 600u;
    f.dpi = 120u;
    f.min_width = 100u;
    f.min_height = 50u;
    set_title(&f, "Hello");
    poison(&o, sizeof o);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.window == 0x1000u);
    Check(o.generation == 1u);
    Check(o.retired_generation == 0u);
    Check(o.disposition == InteropWindowDispositionAdmitted);
    Check(o.role == InteropRoleToplevel);
    Check(o.reason == InteropWindowReasonNone);
    Check(o.events_queued == 1u);
    Check(o.entries_admitted == 1u);
    Check(o.entries_retired == 0u);
    Check(o.deferred_dropped == 0u);
    Check(o.reserved0 == 0u);
    Check(h.lookup.calls == 1u);
    Check(h.lookup.last_window == 0x1000u);
    Check(h.lookup.last_preset_length == 0u);
    Check(h.lookup.last_facts != &f);
    s = stats_of(&h);
    Check(s.generations_issued == 1u && s.admitted == 1u && s.events_queued == 1u);
    Check(s.events_pending == 1u && s.active_count == 1u && s.retiring_count == 0u);
    Check(s.stamp_high_water == f.stamp);

    poison(&ev, sizeof ev);
    Check(h_next(&h, &ev) == StOk);
    Check(ev.session_epoch == 42u && ev.window == 0x1000u && ev.generation == 1u);
    Check(ev.parent_window == 0u && ev.parent_generation == 0u);
    Check(ev.kind == InteropWindowEventAnnounce && ev.role == InteropRoleToplevel);
    Check(ev.x == 10 && ev.y == -20 && ev.width == 800u && ev.height == 600u);
    Check(ev.state == expected_state);
    Check((ev.state & InteropStateActivated) == 0u);
    Check(ev.dpi == 120u && ev.min_width == 100u && ev.min_height == 50u);
    Check(ev.process_id == 1000u);
    Check(ev.changed_mask == InteropWindowChangeMask);
    Check(ev.reason == 0u && ev.reserved0 == 0u);
    Check(ev.title_length == 5u && memcmp(ev.title, "Hello", 5) == 0);
    Check(ev.app_key_length == 10u && memcmp(ev.app_key, "a-test-app", 10) == 0);
    poison(&ev, sizeof ev);
    Check(h_next(&h, &ev) == StNone);
    Check(untouched(&ev, sizeof ev));

    /* An identical offer is Unchanged and queues nothing. */
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionUnchanged);
    Check(o.generation == 1u && o.events_queued == 0u && o.retired_generation == 0u);
    Check(h.lookup.calls == 1u);
    Check(h_next(&h, &ev) == StNone);

    /* State bits come from the flags. */
    fill_toplevel(&f, 0x1001u);
    f.style = 0u;
    f.flags |= InteropWindowFactIconic | InteropWindowFactZoomed | InteropWindowFactFullscreen;
    Check(h_observe(&h, &f, &o) == StOk);
    Check(h_next(&h, &ev) == StOk);
    Check(ev.state == (InteropStateMinimized | InteropStateMaximized | InteropStateFullscreen));
    return h_free(&h);
}

/* ---- Test: update coalescing ---------------------------------------------------------------- */

static int test_update_coalescing(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_event_t ev;
    interop_window_graph_stats_t s0;
    interop_window_graph_stats_t s1;

    Check(h_init(&h, 11u, InteropWindowUacWindow) == 0);
    fill_toplevel(&f, 0x2000u);
    Check(h_observe(&h, &f, &o) == StOk);

    /* A change before the Announce is consumed is merged into the Announce. */
    f.x = 99;
    set_title(&f, "merged");
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionUpdated);
    Check(o.events_queued == 0u);
    s1 = stats_of(&h);
    Check(s1.events_coalesced == 1u && s1.events_pending == 1u && s1.updates_queued == 0u);
    Check(h_next(&h, &ev) == StOk);
    Check(ev.kind == InteropWindowEventAnnounce && ev.x == 99);
    Check(ev.changed_mask == InteropWindowChangeMask);
    Check(ev.title_length == 6u && memcmp(ev.title, "merged", 6) == 0);
    Check(h_next(&h, &ev) == StNone);

    /* After the drain a change is a real Update, with the right changed_mask per group. */
    f.width = 801u;
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionUpdated && o.events_queued == 1u);
    Check(h_next(&h, &ev) == StOk);
    Check(ev.kind == InteropWindowEventUpdate && ev.changed_mask == InteropWindowChangeRect);
    Check(ev.width == 801u);

    set_title(&f, "other");
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(h_next(&h, &ev) == StOk);
    Check(ev.kind == InteropWindowEventUpdate && ev.changed_mask == InteropWindowChangeTitle);

    set_title(&f, "");
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(h_next(&h, &ev) == StOk);
    Check(ev.changed_mask == InteropWindowChangeTitle && ev.title_length == 0u);

    f.flags |= InteropWindowFactZoomed;
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(h_next(&h, &ev) == StOk);
    Check(ev.changed_mask == InteropWindowChangeState && (ev.state & InteropStateMaximized) != 0u);

    f.dpi = 144u;
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(h_next(&h, &ev) == StOk);
    Check(ev.changed_mask == InteropWindowChangeDpi && ev.dpi == 144u);

    f.min_width = 200u;
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(h_next(&h, &ev) == StOk);
    Check(ev.changed_mask == InteropWindowChangeMinSize && ev.min_width == 200u);

    /* Two unconsumed Updates collapse into one with the OR of the masks. */
    s0 = stats_of(&h);
    f.y = 7;
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.events_queued == 1u);
    f.dpi = 192u;
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.events_queued == 0u);
    s1 = stats_of(&h);
    Check(s1.updates_queued == s0.updates_queued + 1u);
    Check(s1.events_coalesced == s0.events_coalesced + 1u);
    Check(s1.events_pending == 1u);
    Check(h_next(&h, &ev) == StOk);
    Check(ev.kind == InteropWindowEventUpdate);
    Check(ev.changed_mask == (InteropWindowChangeRect | InteropWindowChangeDpi));
    Check(ev.y == 7 && ev.dpi == 192u);
    Check(h_next(&h, &ev) == StNone);
    return h_free(&h);
}

/* ---- Test: parents before children ---------------------------------------------------------- */

static int test_child_before_parent(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_graph_stats_t s;
    interop_window_live_t live;
    uint64_t parent_gen;
    uint32_t n;

    Check(h_init(&h, 12u, InteropWindowUacWindow) == 0);

    /* A child offered before its parent is deferred: no generation, no event, not live. */
    fill_dialog(&f, 0x3001u, 0x3000u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionDeferred);
    Check(o.generation == 0u && o.retired_generation == 0u);
    Check(o.reason == InteropWindowReasonParentPending && o.role == InteropRoleDialog);
    Check(o.events_queued == 0u && o.entries_admitted == 0u);
    Check(h.lookup.calls == 1u);
    s = stats_of(&h);
    Check(s.deferred_count == 1u && s.deferred_added == 1u && s.events_pending == 0u);
    Check(s.active_count == 0u && s.generations_issued == 0u);
    Check(interop_window_graph_find_live(h.graph, 0x3001u, &live) == StNone);

    /* Admitting the parent flushes the child; Announce order is parent then child. */
    fill_toplevel(&f, 0x3000u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionAdmitted);
    Check(o.entries_admitted == 2u && o.events_queued == 2u);
    parent_gen = o.generation;
    Check(h.lookup.calls == 2u);
    n = drain_events(&h, g_events, EventBufferSize);
    Check(n == 2u);
    Check(g_events[0].kind == InteropWindowEventAnnounce && g_events[0].window == 0x3000u);
    Check(g_events[0].generation == parent_gen && g_events[0].parent_window == 0u);
    Check(g_events[1].kind == InteropWindowEventAnnounce && g_events[1].window == 0x3001u);
    Check(g_events[1].parent_window == 0x3000u && g_events[1].parent_generation == parent_gen);
    Check(g_events[1].role == InteropRoleDialog && g_events[1].generation > parent_gen);
    Check(interop_window_graph_find_live(h.graph, 0x3001u, &live) == StOk);
    Check(live.parent_window == 0x3000u && live.parent_generation == parent_gen);
    s = stats_of(&h);
    Check(s.deferred_count == 0u && s.active_count == 2u);

    /* A chain offered leaf first flushes transitively in order. */
    fill_dialog(&f, 0x3012u, 0x3011u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionDeferred);
    fill_dialog(&f, 0x3011u, 0x3010u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionDeferred);
    Check(stats_of(&h).deferred_count == 2u);
    fill_toplevel(&f, 0x3010u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.entries_admitted == 3u && o.events_queued == 3u);
    n = drain_events(&h, g_events, EventBufferSize);
    Check(n == 3u);
    Check(g_events[0].window == 0x3010u && g_events[1].window == 0x3011u && g_events[2].window == 0x3012u);
    Check(g_events[1].parent_window == 0x3010u && g_events[1].parent_generation == g_events[0].generation);
    Check(g_events[2].parent_window == 0x3011u && g_events[2].parent_generation == g_events[1].generation);
    Check(g_events[0].generation < g_events[1].generation && g_events[1].generation < g_events[2].generation);

    /* A child whose parent is Retiring is deferred too. */
    Check(kill_window(&h, 0x3000u, &o) == 0);
    fill_dialog(&f, 0x3021u, 0x3000u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionDeferred);
    Check(o.reason == InteropWindowReasonParentPending);
    Check(interop_window_graph_find_live(h.graph, 0x3021u, &live) == StNone);
    return h_free(&h);
}

/* ---- Test: capacity ------------------------------------------------------------------------- */

static int test_capacity(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_graph_stats_t before;
    interop_window_graph_stats_t after;
    interop_window_live_t live;
    uint64_t gens[64];
    uint32_t i;
    uint32_t calls;

    Check(h_init(&h, 13u, InteropWindowUacWindow) == 0);
    for (i = 0; i < 64u; i++) {
        Check(admit_top(&h, 0x1000u + i, &gens[i]) == 0);
        Check(gens[i] == (uint64_t)i + 1u);
    }
    before = stats_of(&h);
    Check(before.active_count == 64u && before.events_pending == 64u);
    calls = h.lookup.calls;

    fill_toplevel(&f, 0x2000u);
    poison(&o, sizeof o);
    Check(h_observe(&h, &f, &o) == StAtCapacity);
    Check(untouched(&o, sizeof o));
    after = stats_of(&h);
    Check(same_state(&before, &after));
    Check(after.rejected_at_capacity == before.rejected_at_capacity + 1u);
    Check(h.lookup.calls == calls);
    for (i = 0; i < 64u; i++) {
        Check(interop_window_graph_find_live(h.graph, 0x1000u + i, &live) == StOk);
        Check(live.generation == gens[i]);
    }
    Check(drain_events(&h, g_events, EventBufferSize) == 64u);
    for (i = 0; i < 64u; i++) {
        Check(g_events[i].kind == InteropWindowEventAnnounce && g_events[i].generation == gens[i]);
    }

    /* A retiring entry still occupies its slot until complete_retire. */
    Check(kill_window(&h, 0x100Au, &o) == 0);
    Check(o.disposition == InteropWindowDispositionRetired);
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StAtCapacity);
    Check(finish_retire(&h, 0x100Au, gens[10]) == 0);
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionAdmitted && o.generation == 65u);
    after = stats_of(&h);
    Check(after.active_count == 64u && after.retiring_count == 0u);
    Check(interop_window_graph_find_live(h.graph, 0x100Au, &live) == StNone);
    return h_free(&h);
}

/* ---- Test: deferral overflow ---------------------------------------------------------------- */

static int test_deferral_overflow(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_graph_stats_t s;
    interop_window_capacity_diagnostic_t diag;
    interop_window_live_t live;
    uint32_t i;

    Check(h_init(&h, 14u, InteropWindowUacWindow) == 0);
    for (i = 0; i < 64u; i++) {
        fill_dialog(&f, 0x3000u + i, 0x4000u + i);
        Check(h_observe(&h, &f, &o) == StOk);
        Check(o.disposition == InteropWindowDispositionDeferred && o.deferred_dropped == 0u);
    }
    s = stats_of(&h);
    Check(s.deferred_count == 64u && s.deferred_added == 64u && s.deferred_evicted == 0u);
    fill_dialog(&f, 0x3000u + 64u, 0x4000u + 64u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionDeferred);
    Check(o.deferred_dropped == 1u && o.generation == 0u);
    s = stats_of(&h);
    Check(s.deferred_count == 64u && s.deferred_added == 65u && s.deferred_evicted == 1u);
    Check(s.events_pending == 0u && s.active_count == 0u);
    Check(h.lookup.calls == 65u);

    /* The oldest deferral (child 0) is gone, the next one (child 1) survives. */
    Check(admit_top(&h, 0x4000u, NULL) == 0);
    Check(stats_of(&h).events_pending == 1u);
    Check(interop_window_graph_find_live(h.graph, 0x3000u, &live) == StNone);
    fill_toplevel(&f, 0x4001u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.entries_admitted == 2u && o.events_queued == 2u);
    Check(interop_window_graph_find_live(h.graph, 0x3001u, &live) == StOk);

    Check(interop_window_graph_take_capacity_diagnostic(h.graph, &diag) == StOk);
    Check(diag.deferred_evicted == 1u && diag.at_capacity == 0u && diag.queue_full == 0u);
    return h_free(&h);
}

/* ---- Test: classification ------------------------------------------------------------------- */

/** @brief Return 1 when classify succeeds with exactly the given verdict. */
static int verdict_is(const interop_window_facts_t *f, uint32_t uac, uint32_t kind, uint32_t role,
                      uint32_t reason, uint64_t parent) {
    interop_window_verdict_t v;
    poison(&v, sizeof v);
    if (interop_window_graph_classify(f, uac, &v) != StOk) {
        return 0;
    }
    return v.kind == kind && v.role == role && v.reason == reason && v.parent_window == parent &&
           v.reserved0 == 0u;
}

/** @brief Return 1 when classify rejects the facts as InvalidFacts and leaves the verdict alone. */
static int facts_rejected(const interop_window_facts_t *f) {
    interop_window_verdict_t v;
    poison(&v, sizeof v);
    return interop_window_graph_classify(f, InteropWindowUacWindow, &v) == StInvalidFacts &&
           untouched(&v, sizeof v);
}

#define Eligible(f, uac, role, parent) \
    Check(verdict_is(&(f), (uac), InteropWindowVerdictEligible, (role), 0u, (parent)))
#define NotExported(f, uac, reason) \
    Check(verdict_is(&(f), (uac), InteropWindowVerdictNotExported, 0u, (reason), 0u))

static int test_classification(void) {
    interop_window_facts_t f;
    interop_window_verdict_t v;
    uint32_t mode;
    const uint32_t w = InteropWindowUacWindow;
    const uint32_t a = InteropWindowUacAuto;
    const uint32_t n = InteropWindowUacNative;
    static const char *excluded[] = {"Shell_TrayWnd", "Shell_SecondaryTrayWnd", "Progman", "WorkerW",
                                     "NotifyIconOverflowWindow", "TaskListThumbnailWnd",
                                     "MSCTFIME UI", "IME"};
    size_t i;

    /* Toplevel and dialog. */
    for (mode = 0; mode <= 2u; mode++) {
        fill_toplevel(&f, 0x500u);
        Eligible(f, mode, InteropRoleToplevel, 0u);
    }
    fill_dialog(&f, 0x500u, 0x600u);
    Eligible(f, w, InteropRoleDialog, 0x600u);
    f.ex_style |= InteropWindowExStyleAppWindow;
    Eligible(f, w, InteropRoleToplevel, 0u);
    f.flags |= InteropWindowFactOwnerHidden;
    Eligible(f, w, InteropRoleToplevel, 0u);

    /* Menus: class rules are unconditional, the style rule needs a popup, an owner and no title bar. */
    fill_toplevel(&f, 0x500u);
    set_class(&f, "#32768");
    Eligible(f, w, InteropRoleMenu, 0u);
    f.owner = 0x600u;
    f.owner_root = 0x601u;
    Eligible(f, w, InteropRoleMenu, 0x601u);
    f.flags |= InteropWindowFactOwnerHidden;
    Eligible(f, w, InteropRoleMenu, 0u);
    fill_dialog(&f, 0x500u, 0x600u);
    set_class(&f, "ComboLBox");
    Eligible(f, w, InteropRoleMenu, 0x600u);
    fill_dialog(&f, 0x500u, 0x600u);
    f.style = InteropWindowStylePopup;
    Eligible(f, w, InteropRoleMenu, 0x600u);
    f.style = InteropWindowStylePopup | 0x00800000u;
    Eligible(f, w, InteropRoleMenu, 0x600u);
    f.style = InteropWindowStylePopup | InteropWindowStyleCaption;
    Eligible(f, w, InteropRoleDialog, 0x600u);
    f.style = InteropWindowStylePopup;
    f.ex_style = InteropWindowExStyleNoActivate;
    Eligible(f, w, InteropRoleTooltip, 0x600u);
    f.ex_style = 0u;
    f.flags |= InteropWindowFactOwnerHidden;
    Eligible(f, w, InteropRoleToplevel, 0u);
    f.flags &= ~(uint32_t)InteropWindowFactOwnerHidden;
    f.owner = 0u;
    f.owner_root = 0u;
    Eligible(f, w, InteropRoleToplevel, 0u);

    /* Tooltips. */
    fill_toplevel(&f, 0x500u);
    set_class(&f, "tooltips_class32");
    Eligible(f, w, InteropRoleTooltip, 0u);
    fill_toplevel(&f, 0x500u);
    f.ex_style = InteropWindowExStyleNoActivate;
    Eligible(f, w, InteropRoleTooltip, 0u);
    fill_toplevel(&f, 0x500u);
    f.ex_style = InteropWindowExStyleToolWindow;
    Eligible(f, w, InteropRoleToplevel, 0u);
    fill_dialog(&f, 0x500u, 0x600u);
    f.ex_style = InteropWindowExStyleToolWindow;
    Eligible(f, w, InteropRoleTooltip, 0x600u);
    f.flags |= InteropWindowFactOwnerHidden;
    Eligible(f, w, InteropRoleToplevel, 0u);

    /* UAC: dummy window is excluded in every mode; the credential dialog depends on the mode. */
    fill_toplevel(&f, 0x500u);
    set_image(&f, "consent.exe");
    set_class(&f, "$$$Secure UAP Dummy Window Class For Interim Dialog");
    for (mode = 0; mode <= 2u; mode++) {
        NotExported(f, mode, InteropWindowReasonExcludedUacDummy);
    }
    set_class(&f, "Credential Dialog Xaml Host");
    Eligible(f, w, InteropRoleUacConsent, 0u);
    Eligible(f, a, InteropRoleUacConsent, 0u);
    NotExported(f, n, InteropWindowReasonUacNativeHidden);
    set_class(&f, "SomeOtherClass");
    for (mode = 0; mode <= 2u; mode++) {
        Eligible(f, mode, InteropRoleToplevel, 0u);
    }
    set_class(&f, "Credential Dialog Xaml Host");
    set_image(&f, "notepad.exe");
    Eligible(f, n, InteropRoleToplevel, 0u);
    set_image(&f, "");
    Eligible(f, n, InteropRoleToplevel, 0u);

    /* Excluded classes, case-insensitive, full length, never when truncated. */
    for (i = 0; i < sizeof excluded / sizeof excluded[0]; i++) {
        fill_toplevel(&f, 0x500u);
        set_class(&f, excluded[i]);
        NotExported(f, w, InteropWindowReasonExcludedClass);
        f.flags |= InteropWindowFactClassTruncated;
        Eligible(f, w, InteropRoleToplevel, 0u);
    }
    fill_toplevel(&f, 0x500u);
    set_class(&f, "SHELL_TRAYWND");
    NotExported(f, w, InteropWindowReasonExcludedClass);
    set_class(&f, "msctfime ui");
    NotExported(f, w, InteropWindowReasonExcludedClass);
    set_class(&f, "Shell_TrayWndX");
    Eligible(f, w, InteropRoleToplevel, 0u);
    set_class(&f, "Shell_TrayWn");
    Eligible(f, w, InteropRoleToplevel, 0u);

    /* CoreWindow is excluded only for the three shell host images. */
    fill_toplevel(&f, 0x500u);
    set_class(&f, "Windows.UI.Core.CoreWindow");
    set_image(&f, "ShellExperienceHost.exe");
    NotExported(f, w, InteropWindowReasonExcludedCoreWindow);
    set_image(&f, "STARTMENUEXPERIENCEHOST.EXE");
    NotExported(f, w, InteropWindowReasonExcludedCoreWindow);
    set_image(&f, "searchhost.exe");
    NotExported(f, w, InteropWindowReasonExcludedCoreWindow);
    set_image(&f, "calc.exe");
    Eligible(f, w, InteropRoleToplevel, 0u);
    set_image(&f, "");
    Eligible(f, w, InteropRoleToplevel, 0u);

    /* Agent windows. */
    fill_toplevel(&f, 0x500u);
    f.flags |= InteropWindowFactAgentOwned;
    NotExported(f, w, InteropWindowReasonExcludedAgent);
    f.flags |= InteropWindowFactAgentPreedit;
    Eligible(f, w, InteropRoleImePreedit, 0u);
    f.owner = 0x600u;
    f.owner_root = 0x601u;
    Eligible(f, w, InteropRoleImePreedit, 0x601u);
    f.flags |= InteropWindowFactOwnerHidden;
    Eligible(f, w, InteropRoleImePreedit, 0u);
    f.width = 0u;
    NotExported(f, w, InteropWindowReasonZeroArea);
    f.width = 10u;
    f.flags &= ~(uint32_t)InteropWindowFactVisible;
    NotExported(f, w, InteropWindowReasonNotVisible);

    /* Visibility, desktop, child, area and transparency gates. */
    fill_toplevel(&f, 0x500u);
    f.flags &= ~(uint32_t)InteropWindowFactVisible;
    NotExported(f, w, InteropWindowReasonNotVisible);
    f.flags &= ~(uint32_t)InteropWindowFactOnInputDesktop;
    Check(verdict_is(&f, w, InteropWindowVerdictSuspended, 0u, 0u, 0u));
    fill_toplevel(&f, 0x500u);
    f.flags &= ~(uint32_t)InteropWindowFactOnInputDesktop;
    f.width = 0u;
    Check(verdict_is(&f, n, InteropWindowVerdictSuspended, 0u, 0u, 0u));
    fill_toplevel(&f, 0x500u);
    f.root = 0x999u;
    NotExported(f, w, InteropWindowReasonChild);
    set_class(&f, "Progman");
    NotExported(f, w, InteropWindowReasonExcludedClass);
    fill_toplevel(&f, 0x500u);
    f.width = 0u;
    NotExported(f, w, InteropWindowReasonZeroArea);
    f.width = 5u;
    f.height = 0u;
    NotExported(f, w, InteropWindowReasonZeroArea);
    fill_toplevel(&f, 0x500u);
    f.flags |= InteropWindowFactLayered | InteropWindowFactLayeredAlphaKnown;
    f.alpha = 0u;
    NotExported(f, w, InteropWindowReasonTransparent);
    f.alpha = 1u;
    Eligible(f, w, InteropRoleToplevel, 0u);
    f.flags &= ~(uint32_t)InteropWindowFactLayeredAlphaKnown;
    f.alpha = 0u;
    Eligible(f, w, InteropRoleToplevel, 0u);
    fill_toplevel(&f, 0x500u);
    set_class(&f, "#32768");
    f.width = 0u;
    NotExported(f, w, InteropWindowReasonZeroArea);

    /* Bytes after the stated lengths are ignored. */
    fill_toplevel(&f, 0x500u);
    memset(f.class_name + f.class_length, 0xFF, sizeof f.class_name - f.class_length);
    memset(f.image_name + f.image_length, 0xFF, sizeof f.image_name - f.image_length);
    memset(f.title + f.title_length, 0xFF, sizeof f.title - f.title_length);
    Eligible(f, w, InteropRoleToplevel, 0u);

    /* Invalid arguments and invalid facts leave the verdict untouched. */
    fill_toplevel(&f, 0x500u);
    poison(&v, sizeof v);
    Check(interop_window_graph_classify(NULL, w, &v) == StInvalidArgument);
    Check(interop_window_graph_classify(&f, w, NULL) == StInvalidArgument);
    Check(interop_window_graph_classify(&f, 3u, &v) == StInvalidArgument);
    Check(untouched(&v, sizeof v));
    f.window = 0u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.root = 0u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.stamp = 0u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.owner = 1u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.owner_root = 1u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.flags |= 1u << 13; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.flags |= InteropWindowFactAgentPreedit; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.flags |= InteropWindowFactLayeredAlphaKnown; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.flags |= InteropWindowFactImageUnknown; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); set_image(&f, ""); f.flags &= ~(uint32_t)InteropWindowFactImageUnknown;
    Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.flags |= InteropWindowFactOwnerHidden; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.dpi = 95u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.dpi = 961u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.width = 16385u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.height = 16385u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.min_width = 16385u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.min_height = 16385u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.class_length = 0u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.class_length = 257u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.image_length = 261u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.title_length = 1025u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.reserved0[1] = 1u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.title[0] = 0xC0u; f.title[1] = 0x80u; f.title_length = 2u;
    Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.title[0] = 0xEDu; f.title[1] = 0xA0u; f.title[2] = 0x80u; f.title_length = 3u;
    Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.title[0] = 0xF4u; f.title[1] = 0x90u; f.title[2] = 0x80u; f.title[3] = 0x80u;
    f.title_length = 4u;
    Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.title[0] = 'a'; f.title[1] = 0u; f.title_length = 2u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.class_name[0] = 0xFFu; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u); f.image_name[0] = 0x80u; Check(facts_rejected(&f));
    fill_toplevel(&f, 0x500u);
    f.title[0] = 0xC3u; f.title[1] = 0xA9u; f.title_length = 2u;
    Eligible(f, w, InteropRoleToplevel, 0u);
    fill_toplevel(&f, 0x500u);
    f.width = 16384u; f.height = 16384u; f.min_width = 16384u; f.dpi = 960u; f.title_length = 1024u;
    memset(f.title, 'x', sizeof f.title);
    f.class_length = 256u;
    memset(f.class_name, 'c', sizeof f.class_name);
    f.image_length = 260u;
    memset(f.image_name, 'i', sizeof f.image_name);
    Eligible(f, w, InteropRoleToplevel, 0u);
    return 0;
}

/* ---- Test: observe error statuses ----------------------------------------------------------- */

static int test_observe_errors(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_facts_t good;
    interop_window_observation_t o;
    interop_window_graph_stats_t before;
    interop_window_graph_stats_t after;
    interop_window_live_t live;
    uint32_t calls;
    uint8_t long_key[200];

    Check(h_init(&h, 15u, InteropWindowUacWindow) == 0);
    Check(admit_top(&h, 0xA0u, NULL) == 0);
    before = stats_of(&h);

    /* InvalidFacts. */
    fill_toplevel(&f, 0xA1u);
    f.width = 16385u;
    poison(&o, sizeof o);
    Check(h_observe(&h, &f, &o) == StInvalidFacts);
    Check(untouched(&o, sizeof o));
    after = stats_of(&h);
    Check(same_state(&before, &after) && after.rejected_invalid_facts == before.rejected_invalid_facts + 1u);
    Check(h.lookup.calls == 1u);

    /* StaleFacts: older stamp than the newest accepted for the window. */
    fill_toplevel(&good, 0xA2u);
    Check(h_observe(&h, &good, &o) == StOk);
    before = stats_of(&h);
    f = good;
    f.stamp = good.stamp - 1u;
    f.x = 500;
    Check(h_observe(&h, &f, &o) == StStaleFacts);
    after = stats_of(&h);
    Check(same_state(&before, &after) && after.rejected_stale_facts == before.rejected_stale_facts + 1u);

    /* StaleEpoch. */
    Check(interop_window_graph_observe(h.graph, h.epoch + 1u, &good, &o) == StStaleEpoch);
    Check(interop_window_graph_observe(h.graph, 0u, &good, &o) == StStaleEpoch);
    after = stats_of(&h);
    Check(same_state(&before, &after) && after.rejected_stale_epoch == before.rejected_stale_epoch + 2u);

    /* LookupFailed: nothing is fabricated, and a retry with the same facts then succeeds. */
    fill_toplevel(&f, 0xA3u);
    before = stats_of(&h);
    h.lookup.result = 7;
    Check(h_observe(&h, &f, &o) == StLookupFailed);
    after = stats_of(&h);
    Check(same_state(&before, &after) && after.lookup_failures == before.lookup_failures + 1u);
    Check(interop_window_graph_find_live(h.graph, 0xA3u, &live) == StNone);
    h.lookup.result = 0;
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionAdmitted);

    /* AppKeyInvalid: every malformed key shape. */
    before = stats_of(&h);
    fill_toplevel(&f, 0xA4u);
    stub_set_key(&h.lookup, "x-bad");
    Check(h_observe(&h, &f, &o) == StAppKeyInvalid);
    stub_set_key(&h.lookup, "a-");
    Check(h_observe(&h, &f, &o) == StAppKeyInvalid);
    stub_set_key(&h.lookup, "a-has space");
    Check(h_observe(&h, &f, &o) == StAppKeyInvalid);
    stub_set_key(&h.lookup, "a-\xC3\xA9");
    Check(h_observe(&h, &f, &o) == StAppKeyInvalid);
    stub_set_key(&h.lookup, "a-ok");
    h.lookup.use_override = 1;
    h.lookup.length_override = 0u;
    Check(h_observe(&h, &f, &o) == StAppKeyInvalid);
    h.lookup.length_override = 201u;
    Check(h_observe(&h, &f, &o) == StAppKeyInvalid);
    h.lookup.use_override = 0;
    after = stats_of(&h);
    Check(same_state(&before, &after) && after.app_key_invalid == before.app_key_invalid + 6u);
    Check(interop_window_graph_find_live(h.graph, 0xA4u, &live) == StNone);

    /* Valid key boundaries: 3 bytes, mixed alphabet, and exactly 200 bytes. */
    stub_set_key(&h.lookup, "a-x");
    Check(h_observe(&h, &f, &o) == StOk);
    drop_events(&h);
    fill_toplevel(&f, 0xA5u);
    stub_set_key(&h.lookup, "a-Z_9-q");
    Check(h_observe(&h, &f, &o) == StOk);
    fill_toplevel(&f, 0xA6u);
    long_key[0] = 'p';
    long_key[1] = '-';
    memset(long_key + 2, 'k', sizeof long_key - 2u);
    memcpy(h.lookup.key, long_key, sizeof long_key);
    h.lookup.key_length = 200u;
    Check(h_observe(&h, &f, &o) == StOk);
    {
        uint32_t n = drain_events(&h, g_events, EventBufferSize);
        Check(n == 2u);
        Check(g_events[0].app_key_length == 7u && memcmp(g_events[0].app_key, "a-Z_9-q", 7) == 0);
        Check(g_events[1].app_key_length == 200u && memcmp(g_events[1].app_key, long_key, 200) == 0);
    }
    stub_set_key(&h.lookup, "a-test-app");

    /* Cycle: self ownership changes nothing. */
    fill_dialog(&f, 0xB0u, 0xB0u);
    before = stats_of(&h);
    calls = h.lookup.calls;
    Check(h_observe(&h, &f, &o) == StCycle);
    after = stats_of(&h);
    Check(same_state(&before, &after) && after.rejected_cycle == before.rejected_cycle + 1u);
    Check(h.lookup.calls == calls);

    /* Cycle drops the window's own deferred record. */
    fill_dialog(&f, 0xB1u, 0xBFu);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionDeferred);
    Check(stats_of(&h).deferred_count == 1u);
    fill_dialog(&f, 0xB1u, 0xB1u);
    Check(h_observe(&h, &f, &o) == StCycle);
    Check(stats_of(&h).deferred_count == 0u);

    /* A two-window cycle through deferred records is rejected; the first record stays. */
    fill_dialog(&f, 0xB2u, 0xB3u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionDeferred);
    fill_dialog(&f, 0xB3u, 0xB2u);
    Check(h_observe(&h, &f, &o) == StCycle);
    Check(stats_of(&h).deferred_count == 1u);
    fill_toplevel(&f, 0xB3u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.entries_admitted == 2u);
    return h_free(&h);
}

/* ---- Test: queue full ----------------------------------------------------------------------- */

static int test_queue_full(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_graph_stats_t before;
    interop_window_graph_stats_t after;
    interop_window_event_t ev;
    interop_window_live_t live;
    uint64_t gen;
    uint32_t calls;
    uint32_t retired = 0x77u;
    uint32_t i;

    Check(h_init(&h, 16u, InteropWindowUacWindow) == 0);
    /* Each cycle queues an Announce and a RetireIntent that cannot coalesce: 128 cycles fill 256 slots. */
    for (i = 0; i < 128u; i++) {
        Check(admit_top(&h, 0x5000u + i, &gen) == 0);
        Check(kill_window(&h, 0x5000u + i, &o) == 0);
        Check(finish_retire(&h, 0x5000u + i, gen) == 0);
    }
    before = stats_of(&h);
    Check(before.events_pending == 256u && before.active_count == 0u && before.retiring_count == 0u);
    calls = h.lookup.calls;

    fill_toplevel(&f, 0x6000u);
    poison(&o, sizeof o);
    Check(h_observe(&h, &f, &o) == StQueueFull);
    Check(untouched(&o, sizeof o));
    after = stats_of(&h);
    Check(same_state(&before, &after) && after.rejected_queue_full == before.rejected_queue_full + 1u);
    Check(h.lookup.calls == calls);
    Check(interop_window_graph_find_live(h.graph, 0x6000u, &live) == StNone);

    /* Draining one event makes room; the same facts are then admitted. */
    Check(h_next(&h, &ev) == StOk);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionAdmitted);
    Check(stats_of(&h).events_pending == 256u);

    /* Every call that must queue events fails the same way and changes nothing. */
    before = stats_of(&h);
    Check(h_destroyed(&h, 0x6000u, ++g_stamp, ++g_tick, &o) == StQueueFull);
    Check(interop_window_graph_invalidate_all(h.graph, h.epoch, &retired) == StQueueFull);
    Check(retired == 0x77u);
    Check(interop_window_graph_refresh_begin(h.graph, h.epoch) == StOk);
    Check(interop_window_graph_refresh_commit(h.graph, h.epoch, &retired) == StQueueFull);
    Check(retired == 0x77u);
    Check(interop_window_graph_refresh_begin(h.graph, h.epoch) == StRefreshActive);
    after = stats_of(&h);
    Check(after.active_count == 1u && after.events_pending == 256u && after.refresh_active == 1u);
    Check(after.retire_intents == before.retire_intents);
    Check(interop_window_graph_find_live(h.graph, 0x6000u, &live) == StOk);

    /* After draining, the refresh commit retries and retires the unseen window. */
    Check(drop_events(&h) == 256u);
    Check(interop_window_graph_refresh_commit(h.graph, h.epoch, &retired) == StOk);
    Check(retired == 1u);
    Check(drain_events(&h, g_events, EventBufferSize) == 1u);
    Check(g_events[0].kind == InteropWindowEventRetireIntent);
    Check(g_events[0].reason == InteropWindowReasonRefreshUnseen);
    return h_free(&h);
}

/* ---- Test: drain with a refusing sink ------------------------------------------------------- */

typedef struct sink_stub_t {
    uint32_t calls;
    uint32_t refuse_at;     /**< 1-based call number to refuse; 0 accepts all. */
    uint64_t windows[16];
    battery_t *battery;
} sink_stub_t;

static int recording_sink(void *context, const interop_window_event_t *event) {
    sink_stub_t *s = context;
    s->calls++;
    if (s->refuse_at != 0u && s->calls == s->refuse_at) {
        return 5;
    }
    if (s->battery != NULL) {
        run_battery(s->battery);
    }
    if (s->calls <= 16u) {
        s->windows[s->calls - 1u] = event->window;
    }
    return 0;
}

static int test_drain_sink(void) {
    harness_t h;
    sink_stub_t sink;
    uint32_t delivered = 0x99u;
    interop_window_graph_stats_t s;
    interop_window_event_t ev;

    Check(h_init(&h, 17u, InteropWindowUacWindow) == 0);
    Check(admit_top(&h, 0x71u, NULL) == 0);
    Check(admit_top(&h, 0x72u, NULL) == 0);
    Check(admit_top(&h, 0x73u, NULL) == 0);

    memset(&sink, 0, sizeof sink);
    sink.refuse_at = 2u;
    Check(interop_window_graph_drain(h.graph, h.epoch, recording_sink, &sink, 10u, &delivered) == StSinkFailed);
    Check(delivered == 1u && sink.calls == 2u);
    s = stats_of(&h);
    Check(s.events_pending == 2u && s.sink_failures == 1u && s.events_drained == 1u);

    /* The refused event stays at the head of the queue and is delivered next. */
    memset(&sink, 0, sizeof sink);
    Check(interop_window_graph_drain(h.graph, h.epoch, recording_sink, &sink, 1u, &delivered) == StOk);
    Check(delivered == 1u && sink.windows[0] == 0x72u);
    Check(interop_window_graph_drain(h.graph, h.epoch, recording_sink, &sink, 10u, &delivered) == StOk);
    Check(delivered == 1u && sink.windows[1] == 0x73u);
    delivered = 0x99u;
    Check(interop_window_graph_drain(h.graph, h.epoch, recording_sink, &sink, 10u, &delivered) == StNone);
    Check(delivered == 0u);
    Check(h_next(&h, &ev) == StNone);
    s = stats_of(&h);
    Check(s.events_pending == 0u && s.events_drained == 3u);

    /* max_events bounds the batch. */
    Check(admit_top(&h, 0x74u, NULL) == 0);
    Check(admit_top(&h, 0x75u, NULL) == 0);
    memset(&sink, 0, sizeof sink);
    Check(interop_window_graph_drain(h.graph, h.epoch, recording_sink, &sink, 1u, &delivered) == StOk);
    Check(delivered == 1u && stats_of(&h).events_pending == 1u);
    return h_free(&h);
}

/* ---- Test: retirement ----------------------------------------------------------------------- */

static int test_retirement(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_graph_stats_t before;
    interop_window_graph_stats_t after;
    interop_window_retiring_entry_t entries[4];
    uint64_t parent_gen;
    uint64_t child_gen;
    uint32_t count = 0;
    uint32_t n;

    Check(h_init(&h, 18u, InteropWindowUacWindow) == 0);
    Check(admit_top(&h, 0x8000u, &parent_gen) == 0);
    fill_dialog(&f, 0x8001u, 0x8000u);
    Check(h_observe(&h, &f, &o) == StOk);
    child_gen = o.generation;
    Check(child_gen > parent_gen);
    Check(drop_events(&h) == 2u);
    Check(interop_window_graph_check_action(h.graph, h.epoch, 0x8000u, parent_gen) == StOk);
    Check(interop_window_graph_check_action(h.graph, h.epoch, 0x8000u, parent_gen + 100u) == StStale);

    /* Destroying the parent retires the child first, both with a RetireIntent. */
    before = stats_of(&h);
    Check(kill_window(&h, 0x8000u, &o) == 0);
    Check(o.disposition == InteropWindowDispositionRetired);
    Check(o.retired_generation == parent_gen && o.generation == 0u);
    Check(o.reason == InteropWindowReasonDestroyed);
    Check(o.entries_retired == 2u && o.events_queued == 2u);
    n = drain_events(&h, g_events, EventBufferSize);
    Check(n == 2u);
    Check(g_events[0].kind == InteropWindowEventRetireIntent && g_events[0].window == 0x8001u);
    Check(g_events[0].generation == child_gen && g_events[0].reason == InteropWindowReasonParentRetired);
    Check(g_events[1].kind == InteropWindowEventRetireIntent && g_events[1].window == 0x8000u);
    Check(g_events[1].generation == parent_gen && g_events[1].reason == InteropWindowReasonDestroyed);
    Check(g_events[0].changed_mask == 0u && g_events[1].changed_mask == 0u);
    Check(g_events[1].width == 800u && g_events[1].role == InteropRoleToplevel);
    Check(g_events[1].app_key_length == 10u);
    after = stats_of(&h);
    Check(after.retire_intents == before.retire_intents + 2u);
    Check(after.active_count == 0u && after.retiring_count == 2u);

    /* Quarantine blocks actions and is listed in ascending generation order. */
    before = stats_of(&h);
    Check(interop_window_graph_check_action(h.graph, h.epoch, 0x8000u, parent_gen) == StStale);
    Check(interop_window_graph_check_action(h.graph, h.epoch, 0x8001u, child_gen) == StStale);
    after = stats_of(&h);
    Check(after.rejected_stale == before.rejected_stale + 2u);
    Check(interop_window_graph_list_retiring(h.graph, entries, 4u, &count) == StOk);
    Check(count == 2u);
    Check(entries[0].window == 0x8000u && entries[0].generation == parent_gen);
    Check(entries[0].reason == InteropWindowReasonDestroyed && entries[0].role == InteropRoleToplevel);
    Check(entries[1].window == 0x8001u && entries[1].generation == child_gen);
    Check(entries[1].reason == InteropWindowReasonParentRetired && entries[1].role == InteropRoleDialog);
    count = 0;
    Check(interop_window_graph_list_retiring(h.graph, entries, 1u, &count) == StOk);
    Check(count == 2u && entries[0].generation == parent_gen);

    /* A second destroy hint touches nothing. */
    Check(kill_window(&h, 0x8000u, &o) == 0);
    Check(o.disposition == InteropWindowDispositionUnchanged && o.events_queued == 0u);

    /* Not-exported facts retire a live window with the classification reason. */
    Check(admit_top(&h, 0x8100u, &parent_gen) == 0);
    drop_events(&h);
    fill_toplevel(&f, 0x8100u);
    f.flags &= ~(uint32_t)InteropWindowFactVisible;
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionRetired);
    Check(o.retired_generation == parent_gen && o.reason == InteropWindowReasonNotVisible);
    Check(drain_events(&h, g_events, EventBufferSize) == 1u);
    Check(g_events[0].kind == InteropWindowEventRetireIntent && g_events[0].reason == InteropWindowReasonNotVisible);

    /* Nothing live and not exported: Excluded. Suspended offers have no effect. */
    fill_toplevel(&f, 0x8200u);
    set_class(&f, "Progman");
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionExcluded && o.reason == InteropWindowReasonExcludedClass);
    Check(o.generation == 0u && o.events_queued == 0u);
    fill_toplevel(&f, 0x8201u);
    f.flags &= ~(uint32_t)InteropWindowFactOnInputDesktop;
    before = stats_of(&h);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionSuspended && o.events_queued == 0u);
    after = stats_of(&h);
    Check(after.suspended == before.suspended + 1u);
    Check(after.events_pending == before.events_pending && after.active_count == before.active_count);
    return h_free(&h);
}

/* ---- Test: complete_retire matrix ----------------------------------------------------------- */

static int test_complete_retire_matrix(void) {
    harness_t h;
    interop_window_observation_t o;
    interop_window_graph_stats_t before;
    interop_window_graph_stats_t after;
    interop_window_retiring_entry_t entries[4];
    interop_window_live_t live;
    uint64_t g1;
    uint64_t g2;
    uint32_t count = 0;

    Check(h_init(&h, 19u, InteropWindowUacWindow) == 0);
    Check(admit_top(&h, 0x9100u, &g1) == 0);

    /* Active entry: NotRetiring, whatever the proof is. */
    Check(h_complete(&h, 0x9100u, g1, 1u, 1u) == StNotRetiring);
    Check(h_complete(&h, 0x9100u, g1, 0u, 0u) == StNotRetiring);
    Check(interop_window_graph_check_action(h.graph, h.epoch, 0x9100u, g1) == StOk);
    Check(kill_window(&h, 0x9100u, &o) == 0);

    /* Unknown pair: Stale even with a bad proof. */
    Check(h_complete(&h, 0x9100u, g1 + 50u, 0u, 0u) == StStale);
    Check(h_complete(&h, 0x9999u, g1, 1u, 1u) == StStale);

    /* Proof flags must each be exactly 1. */
    before = stats_of(&h);
    Check(h_complete(&h, 0x9100u, g1, 0u, 1u) == StProofIncomplete);
    Check(h_complete(&h, 0x9100u, g1, 2u, 1u) == StProofIncomplete);
    Check(h_complete(&h, 0x9100u, g1, 1u, 0u) == StProofIncomplete);
    Check(h_complete(&h, 0x9100u, g1, 1u, 2u) == StProofIncomplete);
    Check(h_complete(&h, 0x9100u, g1, 0u, 0u) == StProofIncomplete);
    Check(h_complete(&h, 0x9100u, g1, 0xFFFFFFFFu, 1u) == StProofIncomplete);
    after = stats_of(&h);
    Check(same_state(&before, &after) && after.proof_incomplete == before.proof_incomplete + 6u);
    Check(after.retiring_count == 1u && after.retire_completed == 0u);

    /* A newer incarnation of the same HWND is not released by completing the old generation. */
    Check(admit_top(&h, 0x9100u, &g2) == 0);
    Check(g2 > g1);
    Check(h_complete(&h, 0x9100u, g1, 1u, 1u) == StOk);
    Check(h_complete(&h, 0x9100u, g1, 1u, 1u) == StStale);
    Check(interop_window_graph_find_live(h.graph, 0x9100u, &live) == StOk && live.generation == g2);
    Check(kill_window(&h, 0x9100u, &o) == 0);
    Check(h_complete(&h, 0x9100u, g1, 1u, 1u) == StStale);
    Check(interop_window_graph_list_retiring(h.graph, entries, 4u, &count) == StOk);
    Check(count == 1u && entries[0].generation == g2);
    Check(h_complete(&h, 0x9100u, g2, 1u, 1u) == StOk);
    Check(interop_window_graph_list_retiring(h.graph, entries, 4u, &count) == StOk && count == 0u);
    after = stats_of(&h);
    Check(after.retire_completed == 2u && after.retiring_count == 0u);
    return h_free(&h);
}

/* ---- Test: note_destroyed rule -------------------------------------------------------------- */

static int test_note_destroyed(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_graph_stats_t before;
    interop_window_graph_stats_t after;
    interop_window_live_t live;
    uint64_t hw = 0;

    Check(h_init(&h, 20u, InteropWindowUacWindow) == 0);
    fill_toplevel(&f, 0xC000u);
    Check(h_observe(&h, &f, &o) == StOk);
    before = stats_of(&h);

    /* A hint whose stamp is not newer than the admitting facts is ignored. */
    Check(h_destroyed(&h, 0xC000u, f.stamp, f.captured_ms + 5u, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionUnchanged && o.events_queued == 0u);
    Check(o.retired_generation == 0u);
    Check(interop_window_graph_find_live(h.graph, 0xC000u, &live) == StOk);
    /* A hint with an older event time describes an earlier incarnation. */
    Check(h_destroyed(&h, 0xC000u, ++g_stamp, f.captured_ms - 1u, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionUnchanged);
    Check(interop_window_graph_find_live(h.graph, 0xC000u, &live) == StOk);
    after = stats_of(&h);
    Check(after.retire_intents == before.retire_intents && after.events_pending == before.events_pending);

    /* A hint for an unknown window, and a stamp high-water that follows Ok calls. */
    Check(h_destroyed(&h, 0xC0FFu, ++g_stamp, ++g_tick, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionUnchanged && o.generation == 0u);
    Check(interop_window_graph_stamp_high_water(h.graph, &hw) == StOk);
    Check(hw == g_stamp);

    /* Equal ticks count as destroyed. */
    Check(h_destroyed(&h, 0xC000u, ++g_stamp, f.captured_ms, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionRetired && o.retired_generation != 0u);
    Check(o.reason == InteropWindowReasonDestroyed);
    Check(interop_window_graph_find_live(h.graph, 0xC000u, &live) == StNone);

    /* Deferred records follow the same stamp and time rule and are dropped without an event. */
    fill_dialog(&f, 0xC010u, 0xC011u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionDeferred);
    Check(h_destroyed(&h, 0xC010u, f.stamp, f.captured_ms, &o) == StOk);
    Check(o.deferred_dropped == 0u && stats_of(&h).deferred_count == 1u);
    Check(h_destroyed(&h, 0xC010u, ++g_stamp, f.captured_ms - 1u, &o) == StOk);
    Check(o.deferred_dropped == 0u && stats_of(&h).deferred_count == 1u);
    Check(h_destroyed(&h, 0xC010u, ++g_stamp, f.captured_ms, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionUnchanged && o.deferred_dropped == 1u);
    Check(stats_of(&h).deferred_count == 0u);
    return h_free(&h);
}

/* ---- Test: tick wrap-around ----------------------------------------------------------------- */

static int test_tick_wrap(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_live_t live;

    Check(h_init(&h, 21u, InteropWindowUacWindow) == 0);
    fill_toplevel(&f, 0xD000u);
    f.captured_ms = 0xFFFFFF00u;
    Check(h_observe(&h, &f, &o) == StOk);
    /* 256 ms before the admitting facts, across no wrap: older, ignored. */
    Check(h_destroyed(&h, 0xD000u, ++g_stamp, 0xFFFFFE00u, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionUnchanged);
    Check(interop_window_graph_find_live(h.graph, 0xD000u, &live) == StOk);
    /* 272 ms after, past the 32-bit wrap: newer, applies. */
    Check(h_destroyed(&h, 0xD000u, ++g_stamp, 0x00000010u, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionRetired);
    return h_free(&h);
}

/* ---- Test: identity change, reclassification, HWND reuse ------------------------------------ */

static int test_identity_and_reclassify(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_live_t live;
    uint64_t gen;
    uint64_t parent_gen;
    uint32_t calls;
    uint32_t n;
    uint32_t round;

    Check(h_init(&h, 22u, InteropWindowUacWindow) == 0);
    Check(admit_top(&h, 0xE000u, &gen) == 0);
    drop_events(&h);

    /* Each identity field changes the incarnation: old generation retires, a new one is announced. */
    for (round = 0; round < 4u; round++) {
        fill_toplevel(&f, 0xE000u);
        if (round == 0u) {
            f.process_id = 1001u;
        } else if (round == 1u) {
            f.process_id = 1001u;
            f.thread_id = 2001u;
        } else if (round == 2u) {
            f.process_id = 1001u;
            f.thread_id = 2001u;
            f.process_start_time = 5001u;
        } else {
            f.process_id = 1001u;
            f.thread_id = 2001u;
            f.process_start_time = 5001u;
            set_class(&f, "OtherClass");
        }
        calls = h.lookup.calls;
        Check(h_observe(&h, &f, &o) == StOk);
        Check(o.disposition == InteropWindowDispositionAdmitted);
        Check(o.retired_generation == gen && o.generation == gen + 1u);
        Check(o.reason == InteropWindowReasonIncarnationChanged);
        Check(o.events_queued == 2u && o.entries_retired == 1u && o.entries_admitted == 1u);
        Check(h.lookup.calls == calls + 1u);
        n = drain_events(&h, g_events, EventBufferSize);
        Check(n == 2u);
        Check(g_events[0].kind == InteropWindowEventRetireIntent && g_events[0].generation == gen);
        Check(g_events[0].reason == InteropWindowReasonIncarnationChanged);
        Check(g_events[1].kind == InteropWindowEventAnnounce && g_events[1].generation == gen + 1u);
        gen = o.generation;
        /* Without changing the identity again, fixtures of the first three rounds stay put. */
        if (round == 0u) {
            Check(h_complete(&h, 0xE000u, gen - 1u, 1u, 1u) == StOk);
        }
    }

    /* Reclassification (same incarnation): new generation, key reused, no new lookup. */
    fill_toplevel(&f, 0xE100u);
    Check(h_observe(&h, &f, &o) == StOk);
    parent_gen = o.generation;
    drop_events(&h);
    calls = h.lookup.calls;
    f.ex_style = InteropWindowExStyleNoActivate;
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionReclassified);
    Check(o.retired_generation == parent_gen && o.generation > parent_gen);
    Check(o.reason == InteropWindowReasonReclassified && o.role == InteropRoleTooltip);
    Check(h.lookup.calls == calls);
    n = drain_events(&h, g_events, EventBufferSize);
    Check(n == 2u);
    Check(g_events[0].kind == InteropWindowEventRetireIntent && g_events[0].reason == InteropWindowReasonReclassified);
    Check(g_events[1].kind == InteropWindowEventAnnounce && g_events[1].role == InteropRoleTooltip);
    Check(g_events[1].app_key_length == 10u && memcmp(g_events[1].app_key, "a-test-app", 10) == 0);
    Check(stats_of(&h).reclassified == 1u);

    /* A parent reclassification re-announces its child after the new parent Announce. */
    fill_toplevel(&f, 0xE200u);
    Check(h_observe(&h, &f, &o) == StOk);
    parent_gen = o.generation;
    {
        interop_window_facts_t child;
        fill_dialog(&child, 0xE201u, 0xE200u);
        Check(h_observe(&h, &child, &o) == StOk);
    }
    drop_events(&h);
    f.ex_style = InteropWindowExStyleNoActivate;
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionReclassified);
    Check(o.entries_retired == 2u && o.entries_admitted == 2u && o.events_queued == 4u);
    n = drain_events(&h, g_events, EventBufferSize);
    Check(n == 4u);
    Check(g_events[0].kind == InteropWindowEventRetireIntent && g_events[0].window == 0xE201u);
    Check(g_events[1].kind == InteropWindowEventRetireIntent && g_events[1].window == 0xE200u);
    Check(g_events[2].kind == InteropWindowEventAnnounce && g_events[2].window == 0xE200u);
    Check(g_events[3].kind == InteropWindowEventAnnounce && g_events[3].window == 0xE201u);
    Check(g_events[3].parent_window == 0xE200u && g_events[3].parent_generation == g_events[2].generation);
    Check(interop_window_graph_find_live(h.graph, 0xE201u, &live) == StOk);
    Check(live.generation == g_events[3].generation);

    /* OwnerHidden changes that keep role and parent are Unchanged. */
    fill_dialog(&f, 0xE300u, 0xE301u);
    f.ex_style = InteropWindowExStyleAppWindow;
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionAdmitted && o.role == InteropRoleToplevel);
    drop_events(&h);
    f.flags |= InteropWindowFactOwnerHidden;
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionUnchanged && o.events_queued == 0u);
    return h_free(&h);
}

/* ---- Test: refresh -------------------------------------------------------------------------- */

static int test_refresh(void) {
    harness_t h;
    interop_window_facts_t fa;
    interop_window_facts_t fb;
    interop_window_facts_t fc;
    interop_window_facts_t fd;
    interop_window_facts_t fe;
    interop_window_observation_t o;
    interop_window_graph_stats_t s;
    interop_window_retiring_entry_t entries[4];
    uint64_t windows[4];
    uint32_t count = 0;
    uint32_t retired = 0;
    uint64_t gen_c;

    Check(h_init(&h, 23u, InteropWindowUacWindow) == 0);
    fill_toplevel(&fa, 0x1100u);
    Check(h_observe(&h, &fa, &o) == StOk);
    fill_toplevel(&fb, 0x1101u);
    Check(h_observe(&h, &fb, &o) == StOk);
    fill_toplevel(&fc, 0x1102u);
    Check(h_observe(&h, &fc, &o) == StOk);
    gen_c = o.generation;
    fill_dialog(&fd, 0x1200u, 0x1300u);
    Check(h_observe(&h, &fd, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionDeferred);
    drop_events(&h);

    /* No refresh open. */
    Check(h_offer(&h, &fa, &o) == StNoRefresh);
    Check(interop_window_graph_refresh_keep(h.graph, h.epoch, 0x1100u) == StNoRefresh);
    Check(interop_window_graph_refresh_unseen(h.graph, h.epoch, windows, 4u, &count) == StNoRefresh);
    Check(interop_window_graph_refresh_commit(h.graph, h.epoch, &retired) == StNoRefresh);
    Check(interop_window_graph_refresh_abort(h.graph, h.epoch) == StNoRefresh);
    s = stats_of(&h);
    Check(s.refresh_active == 0u);

    /* Open; a second begin continues the same refresh. */
    Check(interop_window_graph_refresh_begin(h.graph, h.epoch) == StOk);
    Check(stats_of(&h).refresh_active == 1u);
    Check(interop_window_graph_refresh_begin(h.graph, h.epoch) == StRefreshActive);
    restamp(&fa);
    Check(h_offer(&h, &fa, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionUnchanged);
    Check(interop_window_graph_refresh_keep(h.graph, h.epoch, 0x1101u) == StOk);
    Check(interop_window_graph_refresh_keep(h.graph, h.epoch, 0x1200u) == StOk);
    Check(interop_window_graph_refresh_keep(h.graph, h.epoch, 0x7777u) == StNone);
    Check(interop_window_graph_refresh_unseen(h.graph, h.epoch, windows, 4u, &count) == StOk);
    Check(count == 1u && windows[0] == 0x1102u);
    Check(interop_window_graph_refresh_commit(h.graph, h.epoch, &retired) == StOk);
    Check(retired == 1u);
    s = stats_of(&h);
    Check(s.refresh_active == 0u && s.refresh_commits == 1u && s.deferred_count == 1u);
    Check(s.active_count == 2u && s.retiring_count == 1u);
    Check(drain_events(&h, g_events, EventBufferSize) == 1u);
    Check(g_events[0].kind == InteropWindowEventRetireIntent && g_events[0].window == 0x1102u);
    Check(g_events[0].reason == InteropWindowReasonRefreshUnseen);
    Check(interop_window_graph_list_retiring(h.graph, entries, 4u, &count) == StOk);
    Check(count == 1u && entries[0].reason == InteropWindowReasonRefreshUnseen);
    Check(interop_window_graph_refresh_commit(h.graph, h.epoch, &retired) == StNoRefresh);
    Check(finish_retire(&h, 0x1102u, gen_c) == 0);

    /* unseen lists in ascending generation order and reports the full count when truncated. */
    Check(interop_window_graph_refresh_begin(h.graph, h.epoch) == StOk);
    poison(windows, sizeof windows);
    Check(interop_window_graph_refresh_unseen(h.graph, h.epoch, windows, 1u, &count) == StOk);
    Check(count == 2u && windows[0] == 0x1100u);
    Check(untouched(&windows[1], 3u * sizeof windows[0]));
    Check(interop_window_graph_refresh_abort(h.graph, h.epoch) == StOk);
    s = stats_of(&h);
    Check(s.refresh_aborts == 1u && s.refresh_active == 0u && s.active_count == 2u);
    Check(s.deferred_count == 1u && s.events_pending == 0u);

    /* Failed offers still mark seen; a new window is born seen; an Ok observe marks seen. */
    Check(interop_window_graph_refresh_begin(h.graph, h.epoch) == StOk);
    restamp(&fa);
    fa.width = 16385u;
    Check(h_offer(&h, &fa, &o) == StInvalidFacts);
    fb.stamp = 1u;
    Check(h_offer(&h, &fb, &o) == StStaleFacts);
    fill_toplevel(&fe, 0x1103u);
    Check(h_offer(&h, &fe, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionAdmitted);
    Check(interop_window_graph_refresh_keep(h.graph, h.epoch, 0x1200u) == StOk);
    Check(interop_window_graph_refresh_unseen(h.graph, h.epoch, windows, 4u, &count) == StOk);
    Check(count == 0u);
    Check(interop_window_graph_refresh_commit(h.graph, h.epoch, &retired) == StOk);
    Check(retired == 0u && stats_of(&h).active_count == 3u && stats_of(&h).deferred_count == 1u);
    drop_events(&h);

    Check(interop_window_graph_refresh_begin(h.graph, h.epoch) == StOk);
    fill_toplevel(&fa, 0x1100u);
    fa.stamp = ++g_stamp;
    Check(h_observe(&h, &fa, &o) == StOk);
    fb.stamp = ++g_stamp;
    Check(h_offer(&h, &fb, &o) == StOk);
    restamp(&fe);
    Check(h_offer(&h, &fe, &o) == StOk);
    Check(interop_window_graph_refresh_keep(h.graph, h.epoch, 0x1200u) == StOk);
    Check(interop_window_graph_refresh_commit(h.graph, h.epoch, &retired) == StOk);
    Check(retired == 0u);

    /* An unseen deferred record is dropped by the commit. */
    Check(interop_window_graph_refresh_begin(h.graph, h.epoch) == StOk);
    restamp(&fa);
    Check(h_offer(&h, &fa, &o) == StOk);
    restamp(&fb);
    Check(h_offer(&h, &fb, &o) == StOk);
    restamp(&fe);
    Check(h_offer(&h, &fe, &o) == StOk);
    Check(interop_window_graph_refresh_commit(h.graph, h.epoch, &retired) == StOk);
    Check(retired == 0u);
    Check(stats_of(&h).deferred_count == 0u && stats_of(&h).active_count == 3u);
    return h_free(&h);
}

/* ---- Test: invalidate_all ------------------------------------------------------------------- */

static int test_invalidate_all(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_graph_stats_t s;
    uint32_t retired = 0x55u;
    uint32_t i;
    int saw_a = 0;

    Check(h_init(&h, 24u, InteropWindowUacWindow) == 0);
    Check(admit_top(&h, 0x1500u, NULL) == 0);
    fill_dialog(&f, 0x1501u, 0x1500u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(admit_top(&h, 0x1510u, NULL) == 0);
    fill_dialog(&f, 0x1520u, 0x1599u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionDeferred);
    drop_events(&h);
    Check(interop_window_graph_refresh_begin(h.graph, h.epoch) == StOk);

    Check(interop_window_graph_invalidate_all(h.graph, h.epoch, &retired) == StOk);
    Check(retired == 3u);
    s = stats_of(&h);
    Check(s.active_count == 0u && s.retiring_count == 3u && s.deferred_count == 0u);
    Check(s.refresh_active == 0u && s.refresh_aborts == 1u && s.events_pending == 3u);
    Check(interop_window_graph_refresh_commit(h.graph, h.epoch, NULL) == StNoRefresh);
    Check(drain_events(&h, g_events, EventBufferSize) == 3u);
    for (i = 0; i < 3u; i++) {
        Check(g_events[i].kind == InteropWindowEventRetireIntent);
        if (g_events[i].window == 0x1500u) {
            Check(g_events[i].reason == InteropWindowReasonInvalidated);
            saw_a = 1;
            /* The child's RetireIntent comes before the parent's. */
            Check(i == 1u || i == 2u);
        }
        if (g_events[i].window == 0x1510u) {
            Check(g_events[i].reason == InteropWindowReasonInvalidated);
        }
    }
    Check(saw_a);
    {
        uint32_t child_index = 0;
        uint32_t parent_index = 0;
        for (i = 0; i < 3u; i++) {
            if (g_events[i].window == 0x1501u) {
                child_index = i;
            }
            if (g_events[i].window == 0x1500u) {
                parent_index = i;
            }
        }
        Check(child_index < parent_index);
    }

    /* Already quarantined entries are not touched again. */
    Check(interop_window_graph_invalidate_all(h.graph, h.epoch, &retired) == StOk);
    Check(retired == 0u);
    Check(interop_window_graph_invalidate_all(h.graph, h.epoch, NULL) == StOk);
    Check(stats_of(&h).events_pending == 0u && stats_of(&h).retiring_count == 3u);
    return h_free(&h);
}

/* ---- Test: capacity diagnostic -------------------------------------------------------------- */

static int test_capacity_diagnostic(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_capacity_diagnostic_t diag;
    uint32_t i;

    Check(h_init(&h, 25u, InteropWindowUacWindow) == 0);
    h.clock_ms = 1000u;
    Check(interop_window_graph_take_capacity_diagnostic(h.graph, &diag) == StNone);
    for (i = 0; i < 64u; i++) {
        Check(admit_top(&h, 0x1000u + i, NULL) == 0);
    }
    fill_toplevel(&f, 0x2000u);
    Check(h_observe(&h, &f, &o) == StAtCapacity);

    /* The first take is immediate and resets the counters. */
    poison(&diag, sizeof diag);
    Check(interop_window_graph_take_capacity_diagnostic(h.graph, &diag) == StOk);
    Check(diag.at_capacity == 1u && diag.deferred_evicted == 0u && diag.queue_full == 0u);
    Check(diag.generation_exhausted == 0u && diag.elapsed_ms == 0u);
    Check(diag.active_count == 64u && diag.retiring_count == 0u);
    Check(diag.deferred_count == 0u && diag.events_pending == 64u);
    poison(&diag, sizeof diag);
    Check(interop_window_graph_take_capacity_diagnostic(h.graph, &diag) == StNone);
    Check(untouched(&diag, sizeof diag));

    /* Counters accumulate; the next take is due only 60000 ms after the previous take. */
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StAtCapacity);
    Check(interop_window_graph_take_capacity_diagnostic(h.graph, &diag) == StNone);
    h.clock_ms = 1000u + InteropWindowGraphDiagnosticIntervalMs - 1u;
    Check(interop_window_graph_take_capacity_diagnostic(h.graph, &diag) == StNone);
    h.clock_ms = 1000u + InteropWindowGraphDiagnosticIntervalMs;
    Check(interop_window_graph_take_capacity_diagnostic(h.graph, &diag) == StOk);
    Check(diag.at_capacity == 1u && diag.elapsed_ms == InteropWindowGraphDiagnosticIntervalMs);
    Check(interop_window_graph_take_capacity_diagnostic(h.graph, &diag) == StNone);

    /* A clock that goes backwards counts as zero elapsed time. */
    restamp(&f);
    Check(h_observe(&h, &f, &o) == StAtCapacity);
    h.clock_ms = 5u;
    Check(interop_window_graph_take_capacity_diagnostic(h.graph, &diag) == StNone);
    return h_free(&h);
}

/* ---- Test: stats, stamp high water, find_live ----------------------------------------------- */

static int test_stats_and_find(void) {
    harness_t h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_event_t ev;
    interop_window_graph_stats_t s;
    interop_window_live_t live;
    uint64_t hw = 99u;
    uint64_t parent_gen;

    Check(h_init(&h, 26u, InteropWindowUacWindow) == 0);
    s = stats_of(&h);
    Check(s.session_epoch == 26u && s.generations_issued == 0u && s.stamp_high_water == 0u);
    Check(interop_window_graph_stamp_high_water(h.graph, &hw) == StOk && hw == 0u);

    fill_toplevel(&f, 0x2100u);
    f.flags |= InteropWindowFactIconic;
    f.process_id = 4321u;
    Check(h_observe(&h, &f, &o) == StOk);
    parent_gen = o.generation;
    Check(interop_window_graph_find_live(h.graph, 0x2100u, &live) == StOk);
    Check(live.window == 0x2100u && live.generation == parent_gen);
    Check(live.parent_window == 0u && live.parent_generation == 0u);
    Check(live.role == InteropRoleToplevel && live.process_id == 4321u && live.reserved0 == 0u);
    Check((live.state & InteropStateMinimized) != 0u && (live.state & InteropStateActivated) == 0u);
    Check(h_next(&h, &ev) == StOk && ev.state == live.state);

    fill_dialog(&f, 0x2101u, 0x2100u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(interop_window_graph_find_live(h.graph, 0x2101u, &live) == StOk);
    Check(live.parent_window == 0x2100u && live.parent_generation == parent_gen);
    Check(live.role == InteropRoleDialog);
    Check(interop_window_graph_stamp_high_water(h.graph, &hw) == StOk && hw == f.stamp);

    /* Rejected offers do not move the high-water mark, even with a higher stamp. */
    fill_toplevel(&f, 0x2102u);
    f.width = 16385u;
    Check(h_observe(&h, &f, &o) == StInvalidFacts);
    Check(interop_window_graph_stamp_high_water(h.graph, &hw) == StOk && hw < f.stamp);

    /* find_live does not find Retiring entries and leaves the output alone. */
    Check(kill_window(&h, 0x2101u, &o) == 0);
    poison(&live, sizeof live);
    Check(interop_window_graph_find_live(h.graph, 0x2101u, &live) == StNone);
    Check(untouched(&live, sizeof live));
    Check(interop_window_graph_stamp_high_water(h.graph, &hw) == StOk && hw == g_stamp);
    s = stats_of(&h);
    Check(s.generations_issued == 2u && s.admitted == 2u && s.retire_intents == 1u);
    Check(s.active_count == 1u && s.retiring_count == 1u && s.reserved0 == 0u);
    return h_free(&h);
}

/* ---- Test: wrong thread --------------------------------------------------------------------- */

static int test_wrong_thread(void) {
    harness_t h;
    battery_t battery;
    interop_window_graph_stats_t before;
    interop_window_graph_stats_t after;
    uint32_t i;

    Check(h_init(&h, 27u, InteropWindowUacWindow) == 0);
    Check(admit_top(&h, 0x2200u, NULL) == 0);
    before = stats_of(&h);
    memset(&battery, 0, sizeof battery);
    battery.graph = h.graph;
    battery.epoch = h.epoch;
    Check(run_on_second_thread(thread_battery, &battery) == 0);
    for (i = 0; i < BatteryCount; i++) {
        Check(battery.status[i] == StWrongThread);
    }
    Check(battery.slot_kept);
    Check(battery.classify_status == StOk);
    after = stats_of(&h);
    Check(memcmp(&before, &after, sizeof before) == 0);
    Check(drop_events(&h) == 1u);
    Check(admit_top(&h, 0x2201u, NULL) == 0);
    return h_free(&h);
}

/* ---- Test: reentrancy ----------------------------------------------------------------------- */

static int test_reentrancy(void) {
    harness_t h;
    battery_t battery;
    sink_stub_t sink;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_graph_stats_t s;
    uint32_t delivered = 0;
    uint32_t i;

    Check(h_init(&h, 28u, InteropWindowUacWindow) == 0);

    /* From inside the application-key lookup. */
    memset(&battery, 0, sizeof battery);
    h.lookup.battery = &battery;
    h.lookup.graph = h.graph;
    h.lookup.epoch = h.epoch;
    fill_toplevel(&f, 0x2300u);
    Check(h_observe(&h, &f, &o) == StOk);
    Check(o.disposition == InteropWindowDispositionAdmitted);
    for (i = 0; i < BatteryCount; i++) {
        Check(battery.status[i] == StReentrant);
    }
    Check(battery.slot_kept);
    Check(battery.classify_status == StOk);
    h.lookup.battery = NULL;
    s = stats_of(&h);
    Check(s.active_count == 1u && s.admitted == 1u && s.events_pending == 1u);

    /* From inside the drain sink. */
    Check(admit_top(&h, 0x2301u, NULL) == 0);
    memset(&battery, 0, sizeof battery);
    battery.graph = h.graph;
    battery.epoch = h.epoch;
    memset(&sink, 0, sizeof sink);
    sink.battery = &battery;
    Check(interop_window_graph_drain(h.graph, h.epoch, recording_sink, &sink, 1u, &delivered) == StOk);
    Check(delivered == 1u);
    for (i = 0; i < BatteryCount; i++) {
        Check(battery.status[i] == StReentrant);
    }
    Check(battery.slot_kept);
    Check(stats_of(&h).events_pending == 1u);

    /* Facts mutated by the lookup callback in the caller's struct do not affect the call. */
    {
        interop_window_live_t live;
        interop_window_facts_t caller;
        fill_toplevel(&caller, 0x2302u);
        h.lookup.mutate = &caller;
        Check(h_observe(&h, &caller, &o) == StOk);
        h.lookup.mutate = NULL;
        Check(o.window == 0x2302u && o.disposition == InteropWindowDispositionAdmitted);
        Check(caller.width == 1u);
        drop_events(&h);
        Check(interop_window_graph_find_live(h.graph, 0x2302u, &live) == StOk);
    }
    return h_free(&h);
}

/* ---- Test: epoch isolation ------------------------------------------------------------------ */

static int test_epoch_isolation(void) {
    harness_t old_h;
    harness_t new_h;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_event_t ev;
    interop_window_graph_stats_t before;
    interop_window_graph_stats_t after;
    interop_window_live_t live;
    uint64_t old_gen;
    uint64_t new_gen;

    Check(h_init(&old_h, 7u, InteropWindowUacWindow) == 0);
    Check(h_init(&new_h, 8u, InteropWindowUacWindow) == 0);
    Check(admit_top(&old_h, 0x2400u, &old_gen) == 0);
    Check(kill_window(&old_h, 0x2400u, &o) == 0);
    Check(admit_top(&new_h, 0x2400u, &new_gen) == 0);
    Check(new_gen == old_gen);

    Check(h_next(&old_h, &ev) == StOk && ev.session_epoch == 7u);
    Check(h_next(&new_h, &ev) == StOk && ev.session_epoch == 8u);
    before = stats_of(&new_h);

    /* A call carrying the old epoch cannot touch the new graph, even for an identical pair. */
    fill_toplevel(&f, 0x2401u);
    Check(interop_window_graph_observe(new_h.graph, 7u, &f, &o) == StStaleEpoch);
    Check(interop_window_graph_note_destroyed(new_h.graph, 7u, 0x2400u, ++g_stamp, ++g_tick, &o) == StStaleEpoch);
    {
        interop_window_retire_proof_t proof = {1u, 1u};
        Check(interop_window_graph_complete_retire(new_h.graph, 7u, 0x2400u, new_gen, &proof) == StStaleEpoch);
    }
    Check(interop_window_graph_check_action(new_h.graph, 7u, 0x2400u, new_gen) == StStaleEpoch);
    Check(interop_window_graph_invalidate_all(new_h.graph, 7u, NULL) == StStaleEpoch);
    Check(interop_window_graph_refresh_begin(new_h.graph, 7u) == StStaleEpoch);
    Check(interop_window_graph_next_event(new_h.graph, 7u, &ev) == StStaleEpoch);
    after = stats_of(&new_h);
    Check(same_state(&before, &after) && after.rejected_stale_epoch == before.rejected_stale_epoch + 7u);
    Check(interop_window_graph_find_live(new_h.graph, 0x2400u, &live) == StOk && live.generation == new_gen);
    Check(interop_window_graph_check_action(new_h.graph, 8u, 0x2400u, new_gen) == StOk);

    /* The old graph keeps its own state and still accepts its own epoch. */
    Check(h_complete(&old_h, 0x2400u, old_gen, 1u, 1u) == StOk);
    Check(h_free(&new_h) == 0);
    return h_free(&old_h);
}

/* ---- Test: generation monotonic ------------------------------------------------------------- */

static int test_generation_monotonic(void) {
    harness_t h;
    interop_window_observation_t o;
    interop_window_event_t ev;
    interop_window_graph_stats_t s;
    uint64_t gen = 0;
    uint64_t last = 0;
    uint32_t i;

    Check(h_init(&h, 29u, InteropWindowUacWindow) == 0);
    for (i = 0; i < 200u; i++) {
        Check(admit_top(&h, 0x7000u, &gen) == 0);
        Check(gen > last && gen == last + 1u);
        Check(h_next(&h, &ev) == StOk && ev.kind == InteropWindowEventAnnounce && ev.generation == gen);
        Check(kill_window(&h, 0x7000u, &o) == 0);
        Check(o.retired_generation == gen);
        Check(h_next(&h, &ev) == StOk && ev.kind == InteropWindowEventRetireIntent && ev.generation == gen);
        Check(finish_retire(&h, 0x7000u, gen) == 0);
        last = gen;
    }
    s = stats_of(&h);
    Check(s.generations_issued == 200u && s.admitted == 200u && s.retire_completed == 200u);
    Check(s.active_count == 0u && s.retiring_count == 0u && s.events_pending == 0u);
    return h_free(&h);
}

/* ---- Test: random trace --------------------------------------------------------------------- */

typedef struct rng_t {
    uint64_t state;
} rng_t;

static uint64_t rng_next(rng_t *r) {
    uint64_t x = r->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->state = x;
    return x;
}

static uint32_t rng_below(rng_t *r, uint32_t n) {
    return (uint32_t)(rng_next(r) % n);
}

/** @brief Model of everything the ABI-visible event stream has said so far. */
typedef struct trace_t {
    harness_t *h;
    rng_t rng;
    uint64_t max_announce;
    uint64_t announces;
    uint64_t retires;
    uint64_t events_seen;
    uint64_t current[TraceWindows];
    uint8_t announced[TraceMaxGeneration];
    uint8_t retired[TraceMaxGeneration];
    uint8_t completed[TraceMaxGeneration];
    uint64_t window_of[TraceMaxGeneration];
    uint64_t parent_gen_of[TraceMaxGeneration];
    uint32_t live_children[TraceMaxGeneration];
} trace_t;

static trace_t g_trace;

/** @brief Check one drained event against the model and update the model. Returns 0 on success. */
static int trace_event(trace_t *t, const interop_window_event_t *ev) {
    uint64_t g = ev->generation;
    uint64_t pg = ev->parent_generation;
    uint64_t idx;
    Check(ev->session_epoch == t->h->epoch);
    Check(g != 0u && g < TraceMaxGeneration);
    Check(ev->window >= TraceFirstWindow && ev->window < TraceFirstWindow + TraceWindows);
    idx = ev->window - TraceFirstWindow;
    t->events_seen++;
    if (ev->kind == InteropWindowEventAnnounce) {
        Check(!t->announced[g] && !t->retired[g]);
        Check(g > t->max_announce);
        Check(ev->changed_mask == InteropWindowChangeMask);
        Check(t->current[idx] == 0u);
        if (ev->parent_window != 0u) {
            Check(pg != 0u && pg < TraceMaxGeneration);
            Check(t->announced[pg] && !t->retired[pg]);
            Check(t->window_of[pg] == ev->parent_window);
            t->live_children[pg]++;
        } else {
            Check(pg == 0u);
        }
        t->max_announce = g;
        t->announced[g] = 1u;
        t->window_of[g] = ev->window;
        t->parent_gen_of[g] = pg;
        t->current[idx] = g;
        t->announces++;
    } else if (ev->kind == InteropWindowEventUpdate) {
        Check(t->announced[g] && !t->retired[g]);
        Check(t->window_of[g] == ev->window);
        Check(ev->changed_mask != 0u && (ev->changed_mask & ~(uint32_t)InteropWindowChangeMask) == 0u);
    } else {
        Check(ev->kind == InteropWindowEventRetireIntent);
        Check(t->announced[g] && !t->retired[g]);
        Check(t->window_of[g] == ev->window);
        Check(t->live_children[g] == 0u);
        Check(ev->reason >= 1u && ev->reason <= 16u);
        Check(ev->changed_mask == 0u);
        if (t->parent_gen_of[g] != 0u) {
            t->live_children[t->parent_gen_of[g]]--;
        }
        t->retired[g] = 1u;
        if (t->current[idx] == g) {
            t->current[idx] = 0u;
        }
        t->retires++;
    }
    return 0;
}

/** @brief Pop up to max_events events through next_event and feed them to the model. */
static int trace_pop(trace_t *t, uint32_t max_events) {
    uint32_t i;
    for (i = 0; i < max_events; i++) {
        interop_window_event_t ev;
        interop_window_graph_status_t st = h_next(t->h, &ev);
        if (st == StNone) {
            return 0;
        }
        Check(st == StOk);
        if (trace_event(t, &ev) != 0) {
            return 1;
        }
    }
    return 0;
}

/** @brief Drain everything and compare the graph's live/retiring view with the event model. */
static int trace_sync(trace_t *t) {
    interop_window_graph_stats_t s;
    interop_window_live_t live;
    interop_window_retiring_entry_t entries[64];
    uint32_t count = 0;
    uint32_t live_count = 0;
    uint32_t expect_retiring = 0;
    uint64_t g;
    uint32_t i;

    Check(trace_pop(t, 1000u) == 0);
    s = stats_of(t->h);
    Check(s.events_pending == 0u);
    for (i = 0; i < TraceWindows; i++) {
        interop_window_graph_status_t st = interop_window_graph_find_live(t->h->graph, TraceFirstWindow + i, &live);
        if (st == StOk) {
            live_count++;
            Check(live.generation == t->current[i]);
            Check(live.window == TraceFirstWindow + i);
            Check(live.parent_generation == t->parent_gen_of[live.generation]);
            Check((live.parent_window == 0u) == (live.parent_generation == 0u));
            Check((live.state & InteropStateActivated) == 0u);
        } else {
            Check(st == StNone);
            Check(t->current[i] == 0u);
        }
    }
    Check(s.active_count == live_count);
    for (g = 1; g <= t->max_announce; g++) {
        if (t->retired[g] && !t->completed[g]) {
            expect_retiring++;
        }
    }
    Check(s.retiring_count == expect_retiring);
    Check(interop_window_graph_list_retiring(t->h->graph, entries, 64u, &count) == StOk);
    Check(count == expect_retiring);
    for (i = 0; i < count; i++) {
        Check(t->retired[entries[i].generation] && !t->completed[entries[i].generation]);
        Check(t->window_of[entries[i].generation] == entries[i].window);
        Check(i == 0u || entries[i - 1u].generation < entries[i].generation);
    }
    return 0;
}

/** @brief Build random valid facts for a trace window. */
static void trace_facts(trace_t *t, interop_window_facts_t *f) {
    static const char *titles[] = {"", "alpha", "beta window", "gamma"};
    uint32_t idx = rng_below(&t->rng, TraceWindows);
    uint32_t variant = rng_below(&t->rng, 10u);
    uint32_t other = rng_below(&t->rng, TraceWindows);
    uint64_t owner = TraceFirstWindow + other;
    fill_toplevel(f, TraceFirstWindow + idx);
    if (rng_below(&t->rng, 20u) == 0u) {
        uint64_t back = 1u + rng_below(&t->rng, 40u);
        f->stamp = g_stamp > back ? g_stamp - back : 1u;
    }
    if (variant >= 4u && variant <= 5u) {
        f->owner = owner;
        f->owner_root = owner;
        f->style = InteropWindowStyleCaption;
    } else if (variant == 6u) {
        set_class(f, "#32768");
        if (rng_below(&t->rng, 2u) == 0u) {
            f->owner = owner;
            f->owner_root = owner;
        }
    } else if (variant == 7u) {
        f->ex_style = InteropWindowExStyleNoActivate;
        if (rng_below(&t->rng, 2u) == 0u) {
            f->owner = owner;
            f->owner_root = owner;
        }
    } else if (variant == 8u) {
        f->flags &= ~(uint32_t)InteropWindowFactVisible;
    } else if (variant == 9u) {
        f->owner = owner;
        f->owner_root = owner;
        f->ex_style = InteropWindowExStyleAppWindow;
        if (rng_below(&t->rng, 2u) == 0u) {
            f->flags |= InteropWindowFactOwnerHidden;
        }
    }
    f->process_id = 1000u + (rng_below(&t->rng, 12u) == 0u ? 1u : 0u);
    f->width = rng_below(&t->rng, 25u) == 0u ? 0u : 300u + 100u * rng_below(&t->rng, 4u);
    f->x = (int32_t)rng_below(&t->rng, 50u);
    set_title(f, titles[rng_below(&t->rng, 4u)]);
}

/** @brief Configure the lookup stub for the next call: mostly fine, sometimes failing or invalid. */
static void trace_stub(trace_t *t) {
    uint32_t dice = rng_below(&t->rng, 100u);
    lookup_stub_t *s = &t->h->lookup;
    s->result = dice < 3u ? 1 : 0;
    s->use_override = 0;
    if (dice >= 3u && dice < 5u) {
        stub_set_key(s, "bad key");
    } else {
        stub_set_key(s, "a-trace");
    }
}

/** @brief Return 1 when status is one the trace tolerates from observe and refresh_offer. */
static int observe_status_allowed(interop_window_graph_status_t st) {
    return st == StOk || st == StStaleFacts || st == StCycle || st == StAtCapacity ||
           st == StQueueFull || st == StLookupFailed || st == StAppKeyInvalid;
}

/** @brief Bounds that must hold after every operation. */
static int trace_bounds(trace_t *t) {
    interop_window_graph_stats_t s = stats_of(t->h);
    Check(s.events_pending <= InteropWindowGraphEventCapacity);
    Check(s.active_count + s.retiring_count <= InteropMaxWindows);
    Check(s.deferred_count <= InteropWindowGraphMaxDeferred);
    Check(s.generations_issued < TraceMaxGeneration);
    return 0;
}

/** @brief One refresh cycle with random offers, keeps and an abort or commit. */
static int trace_refresh(trace_t *t) {
    interop_window_observation_t o;
    interop_window_facts_t f;
    uint64_t windows[64];
    uint32_t count = 0;
    uint32_t retired = 0;
    uint32_t offers = rng_below(&t->rng, 8u);
    uint32_t i;
    interop_window_graph_status_t st = interop_window_graph_refresh_begin(t->h->graph, t->h->epoch);
    if (st == StRefreshActive) {
        Check(interop_window_graph_refresh_abort(t->h->graph, t->h->epoch) == StOk);
        st = interop_window_graph_refresh_begin(t->h->graph, t->h->epoch);
    }
    Check(st == StOk);
    for (i = 0; i < offers; i++) {
        trace_facts(t, &f);
        trace_stub(t);
        st = h_offer(t->h, &f, &o);
        Check(observe_status_allowed(st));
        if (rng_below(&t->rng, 4u) == 0u) {
            Check(interop_window_graph_refresh_keep(t->h->graph, t->h->epoch,
                                                    TraceFirstWindow + rng_below(&t->rng, TraceWindows)) != StNoRefresh);
        }
    }
    Check(interop_window_graph_refresh_unseen(t->h->graph, t->h->epoch, windows, 64u, &count) == StOk);
    Check(count <= 64u);
    for (i = 0; i < count; i++) {
        interop_window_live_t live;
        Check(interop_window_graph_find_live(t->h->graph, windows[i], &live) == StOk);
    }
    if (rng_below(&t->rng, 3u) == 0u) {
        Check(interop_window_graph_refresh_abort(t->h->graph, t->h->epoch) == StOk);
        return 0;
    }
    st = interop_window_graph_refresh_commit(t->h->graph, t->h->epoch, &retired);
    if (st == StQueueFull) {
        Check(interop_window_graph_refresh_abort(t->h->graph, t->h->epoch) == StOk);
    } else {
        Check(st == StOk);
        Check(retired <= 64u);
    }
    Check(stats_of(t->h).refresh_active == 0u);
    return 0;
}

static int test_random_trace(void) {
    harness_t h;
    trace_t *t = &g_trace;
    interop_window_facts_t f;
    interop_window_observation_t o;
    interop_window_retiring_entry_t entries[64];
    interop_window_live_t live;
    interop_window_graph_stats_t s;
    interop_window_capacity_diagnostic_t diag;
    uint32_t count = 0;
    uint32_t retired = 0;
    uint32_t op;
    uint32_t i;

    memset(t, 0, sizeof *t);
    Check(h_init(&h, 31u, InteropWindowUacWindow) == 0);
    t->h = &h;
    t->rng.state = UINT64_C(0x9E3779B97F4A7C15);
    for (op = 0; op < TraceOps; op++) {
        uint32_t dice = rng_below(&t->rng, 100u);
        interop_window_graph_status_t st;
        if (dice < 55u) {
            trace_facts(t, &f);
            trace_stub(t);
            st = h_observe(&h, &f, &o);
            Check(observe_status_allowed(st));
            if (st == StOk) {
                Check(o.disposition >= InteropWindowDispositionUnchanged &&
                      o.disposition <= InteropWindowDispositionSuspended);
                Check(o.events_queued <= InteropWindowGraphMaxCallEvents);
                Check(o.reserved0 == 0u);
            }
        } else if (dice < 65u) {
            st = h_destroyed(&h, TraceFirstWindow + rng_below(&t->rng, TraceWindows), ++g_stamp, ++g_tick, &o);
            Check(st == StOk || st == StQueueFull);
        } else if (dice < 77u) {
            Check(interop_window_graph_list_retiring(h.graph, entries, 64u, &count) == StOk);
            if (count > 0u) {
                uint32_t pick = rng_below(&t->rng, count);
                uint32_t mode = rng_below(&t->rng, 6u);
                if (mode == 0u) {
                    Check(h_complete(&h, entries[pick].window, entries[pick].generation, 0u, 1u) == StProofIncomplete);
                } else if (mode == 1u) {
                    Check(h_complete(&h, entries[pick].window, entries[pick].generation + 1000000u, 1u, 1u) == StStale);
                } else {
                    Check(h_complete(&h, entries[pick].window, entries[pick].generation, 1u, 1u) == StOk);
                    Check(entries[pick].generation < TraceMaxGeneration);
                    t->completed[entries[pick].generation] = 1u;
                    Check(h_complete(&h, entries[pick].window, entries[pick].generation, 1u, 1u) == StStale);
                }
            }
        } else if (dice < 81u) {
            Check(trace_refresh(t) == 0);
        } else if (dice < 82u) {
            if (rng_below(&t->rng, 3u) == 0u) {
                st = interop_window_graph_invalidate_all(h.graph, h.epoch, &retired);
                Check(st == StOk || st == StQueueFull);
            }
        } else if (dice < 93u) {
            Check(trace_pop(t, 1u + rng_below(&t->rng, 10u)) == 0);
        } else if (dice < 95u) {
            uint32_t w = rng_below(&t->rng, TraceWindows);
            if (interop_window_graph_find_live(h.graph, TraceFirstWindow + w, &live) == StOk) {
                Check(interop_window_graph_check_action(h.graph, h.epoch, live.window, live.generation) == StOk);
                Check(interop_window_graph_check_action(h.graph, h.epoch, live.window, live.generation + 1000000u) == StStale);
            }
        } else if (dice < 96u) {
            st = interop_window_graph_take_capacity_diagnostic(h.graph, &diag);
            Check(st == StOk || st == StNone);
            h.clock_ms += 30000u;
        } else {
            Check(trace_sync(t) == 0);
        }
        Check(trace_bounds(t) == 0);
    }

    /* Wind down: retire everything, complete every quarantine, then nothing may remain. */
    Check(trace_sync(t) == 0);
    Check(interop_window_graph_invalidate_all(h.graph, h.epoch, &retired) == StOk);
    Check(trace_sync(t) == 0);
    Check(interop_window_graph_list_retiring(h.graph, entries, 64u, &count) == StOk);
    for (i = 0; i < count; i++) {
        Check(h_complete(&h, entries[i].window, entries[i].generation, 1u, 1u) == StOk);
        t->completed[entries[i].generation] = 1u;
    }
    Check(trace_sync(t) == 0);
    s = stats_of(&h);
    Check(s.active_count == 0u && s.retiring_count == 0u && s.deferred_count == 0u);
    Check(s.events_pending == 0u && s.refresh_active == 0u);
    Check(s.admitted == t->announces);
    Check(s.retire_intents == t->retires && s.retire_intents == s.admitted);
    Check(s.retire_completed == s.retire_intents);
    Check(s.events_drained == t->events_seen && s.events_queued == s.events_drained);
    Check(s.generations_issued >= t->max_announce);
    Check(t->announces > 100u);
    return h_free(&h);
}

/* ---- Runner --------------------------------------------------------------------------------- */

typedef struct test_case_t {
    const char *name;
    int (*run)(void);
} test_case_t;

int main(void) {
    static const test_case_t tests[] = {
        {"runtime_sizes", test_runtime_sizes},
        {"create_destroy", test_create_destroy},
        {"create_alias_preserves_inputs", test_create_alias_preserves_inputs},
        {"invalid_arguments", test_invalid_arguments},
        {"lifecycle", test_lifecycle},
        {"update_coalescing", test_update_coalescing},
        {"child_before_parent", test_child_before_parent},
        {"capacity", test_capacity},
        {"deferral_overflow", test_deferral_overflow},
        {"classification", test_classification},
        {"observe_errors", test_observe_errors},
        {"queue_full", test_queue_full},
        {"drain_sink", test_drain_sink},
        {"retirement", test_retirement},
        {"complete_retire_matrix", test_complete_retire_matrix},
        {"note_destroyed", test_note_destroyed},
        {"tick_wrap", test_tick_wrap},
        {"identity_and_reclassify", test_identity_and_reclassify},
        {"refresh", test_refresh},
        {"invalidate_all", test_invalidate_all},
        {"capacity_diagnostic", test_capacity_diagnostic},
        {"stats_and_find", test_stats_and_find},
        {"wrong_thread", test_wrong_thread},
        {"reentrancy", test_reentrancy},
        {"epoch_isolation", test_epoch_isolation},
        {"generation_monotonic", test_generation_monotonic},
        {"random_trace", test_random_trace},
    };
    size_t count = sizeof tests / sizeof tests[0];
    size_t i;
    for (i = 0; i < count; i++) {
        if (tests[i].run() != 0) {
            fprintf(stderr, "window_graph: FAILED %s\n", tests[i].name);
            return 1;
        }
        printf("window_graph: ok %s\n", tests[i].name);
    }
    printf("window_graph: %zu tests passed\n", count);
    return 0;
}
