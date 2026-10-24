#ifndef WaddleWindowObserverApiDoubleH
#define WaddleWindowObserverApiDoubleH
/**
 * @file window_observer_api_double.h
 * @brief Scriptable window_observer_api_t implementation for the observer tests (TODO #10).
 *
 * The double replaces every native operation of the Win32 observer with deterministic state that a
 * test edits between calls: a table of fake windows (facts template, Exists/Gone/Failed mode, query
 * counter, scripted transient failures), install/remove hook counters with scripted failure at the
 * Nth call, an enumeration list with Ok/Truncated/Failed modes, a scripted input desktop, frozen
 * monotonic and tick clocks that only move when the test moves them, a fake thread id, a way to run
 * a function on a second real thread that reports a different thread id (run-and-join, or start and
 * join separately with an optional park inside current_thread_id), a hook-procedure emulator
 * (api_double_hint*) that calls window_observer_note_hint, and two re-entry mechanisms that fire
 * inside query_facts (a scripted hint, and an arbitrary callback).
 *
 * The header is included by exactly one translation unit (window_observer_windows.c in this
 * directory); every function is static. It is portable: pthread off Windows, CreateThread under
 * _WIN32. It never calls the graph and owns no heap memory itself: api_double_new / api_double_free
 * are the only allocation pair.
 *
 * Threading: all state is plain data owned by the test thread. The cross-thread data is the
 * helper-thread identity and the parked flag (atomics published with release/acquire ordering) used
 * by api_double_current_thread_id. api_double_run_on_other_thread joins the helper before it returns,
 * so the test thread and the helper never run concurrently there. api_double_helper_start /
 * api_double_helper_join let the test thread run observer entry points while the helper is parked
 * inside current_thread_id (park_ms); that is the only concurrent use, and the test must touch
 * nothing but the parked flag and observer entry points that are safe against note_hint meanwhile.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif

#include "window_observer_windows.h"

/** @brief Capacity of the fake window table. */
#define ApiDoubleMaxWindows 128u
/** @brief Capacity of the hook bookkeeping arrays (the observer installs three). */
#define ApiDoubleMaxHooks 8u
/** @brief Capacity of the query log (hwnd of every query_facts call, in order). */
#define ApiDoubleQueryLogCapacity 4096u
/** @brief Default process id of fake windows: a foreign process. */
#define ApiDoubleForeignPid 4242u
/** @brief Process id the tests pass as own_process_id; fake windows with it are AgentOwned. */
#define ApiDoubleOwnPid 900u
/** @brief Fake thread id of the owner (test) thread. */
#define ApiDoubleOwnerTid 1u
/** @brief Window style WS_OVERLAPPEDWINDOW | WS_VISIBLE, the default of fake windows. */
#define ApiDoubleStyleOverlapped 0x10CF0000u

/** @brief How query_facts answers for one fake window. */
typedef enum api_double_mode_t {
    ApiDoubleModeExists = 1, /**< Return Exists with the facts template. */
    ApiDoubleModeGone = 2, /**< Return Gone (the OS reports an invalid handle). */
    ApiDoubleModeFailed = 3 /**< Return Failed (state could not be determined). */
} api_double_mode_t;

/** @brief One fake window: facts template plus scripting knobs. */
typedef struct api_double_window_t {
    uint32_t in_use; /**< 1 while the slot holds a window. */
    uint32_t fail_count; /**< Number of upcoming queries that answer Failed before mode applies. */
    uint64_t hwnd; /**< Window handle value (nonzero while in use). */
    uint64_t report_window; /**< When nonzero, query_facts reports this value in facts->window (mismatch scripting). */
    uint32_t query_count; /**< Number of query_facts calls that named this hwnd (owner sub-queries included). */
    api_double_mode_t mode; /**< Answer when fail_count is 0. */
    interop_window_facts_t facts; /**< Template copied to the caller on Exists; window is overwritten with hwnd. */
} api_double_window_t;

typedef struct api_double_t api_double_t;

/**
 * @brief Callback fired inside query_facts, before the window table is consulted.
 * @param[in,out] dbl The double that is answering; never NULL.
 * @param[in] window The hwnd being queried.
 * @param[in,out] arg Opaque pointer from api_double_t.query_hook_arg; may be NULL.
 */
typedef void (*api_double_query_hook_t)(api_double_t *dbl, uint64_t window, void *arg);

/** @brief Complete scriptable state of the API double. Heap object: use api_double_new. */
struct api_double_t {
    api_double_window_t windows[ApiDoubleMaxWindows]; /**< Fake window table. */

    uint64_t enum_list[WindowObserverMaxEnumerated]; /**< Windows returned by enum_top_level_windows, front to back. */
    uint32_t enum_count; /**< Valid entries in enum_list. */
    window_observer_enum_result_t enum_result; /**< Result returned by the next enumerations. */
    uint32_t enum_calls; /**< Number of enum_top_level_windows calls. */

    window_observer_desktop_t desktop; /**< Answer of query_input_desktop. */
    uint32_t desktop_calls; /**< Number of query_input_desktop calls. */

    uint32_t install_calls; /**< Number of install_hook calls (failed ones included). */
    uint32_t install_fail_at; /**< 1-based install call number that fails; 0 = none. */
    uint32_t install_min[ApiDoubleMaxHooks]; /**< event_min of install call i (0-based). */
    uint32_t install_max[ApiDoubleMaxHooks]; /**< event_max of install call i (0-based). */
    uintptr_t install_tokens[ApiDoubleMaxHooks]; /**< Token returned by install call i (0 if it failed). */
    uintptr_t next_token; /**< Token the next successful install returns; incremented per success. */
    uint32_t remove_calls; /**< Number of remove_hook calls (failed ones included). */
    uint32_t remove_fail_at; /**< 1-based remove call number that fails; 0 = none. */
    uintptr_t removed_tokens[ApiDoubleMaxHooks]; /**< Token passed to remove call i (0-based). */

    uint32_t tid; /**< Fake thread id returned for every thread except a registered helper. */
    uint64_t monotonic; /**< Value returned by monotonic_ms. */
    uint32_t tick; /**< Value returned by tick_ms; also the event time of emulated hints. */
    uint32_t monotonic_calls; /**< Number of monotonic_ms calls. */
    uint32_t tick_calls; /**< Number of tick_ms calls. */

    uint64_t query_log[ApiDoubleQueryLogCapacity]; /**< hwnd of every query_facts call, in call order. */
    uint32_t query_total; /**< Total query_facts calls (may exceed the log capacity). */

    uint32_t hint_pending; /**< 1 = emit hint_event for hint_window at the next query_facts call, once. */
    uint32_t hint_event; /**< WinEvent id of the scripted hint. */
    uint64_t hint_window; /**< Window of the scripted hint. */

    api_double_query_hook_t query_hook; /**< Callback fired inside query_facts; NULL = none. */
    void *query_hook_arg; /**< Passed to query_hook. */
    uint64_t query_hook_window; /**< Fire only for this hwnd; 0 = any. */
    uint32_t query_hook_once; /**< 1 = clear query_hook before the first firing. */

    void (*install_hook_fn)(api_double_t *dbl, void *arg); /**< Callback fired at the start of every install_hook call; NULL = none. */
    void *install_hook_arg; /**< Passed to install_hook_fn. */
    uint32_t enum_report_count; /**< Nonzero: enum_top_level_windows reports this count (with result Ok) regardless of the list. */

    uint32_t park_ms; /**< Nonzero: the next current_thread_id call of the helper thread parks for this many ms (one shot, then cleared). */
    _Atomic int parked; /**< 1 once the helper entered its park, 0 otherwise. */
    uint64_t park_deadline_ms; /**< api_double_wall_ms() value at which the park ends; valid once parked is 1 (read after an acquire load of parked). */
    _Atomic int helper_active; /**< 1 while a helper thread registered its identity. */
    uint32_t helper_tid; /**< Fake thread id reported to the helper thread. */
#ifdef _WIN32
    DWORD helper_native; /**< Native id of the helper thread. */
#else
    pthread_t helper_native; /**< Native id of the helper thread. */
#endif
};

/**
 * @brief Allocate a double and reset it.
 * @return Zero-initialized object ready for use, or NULL when allocation fails. The caller owns it
 *         and releases it with api_double_free.
 */
static api_double_t *api_double_new(void);

/**
 * @brief Release a double.
 * @param[in] dbl Object from api_double_new; may be NULL. Dangling after the call.
 */
static void api_double_free(api_double_t *dbl);

/**
 * @brief Copy ASCII text into a facts buffer.
 * @param[out] dest Destination buffer of capacity bytes (not NUL-terminated by this function).
 * @param[in] capacity Bytes available in dest.
 * @param[out] length Receives strlen(text), at most capacity.
 * @param[in] text NUL-terminated ASCII string; non-NULL.
 */
static void api_double_set_text(uint8_t *dest, uint32_t capacity, uint32_t *length, const char *text) {
    size_t count = strlen(text);
    if (count > capacity) {
        count = capacity;
    }
    memcpy(dest, text, count);
    *length = (uint32_t)count;
}

/**
 * @brief Fill a facts struct with a valid visible top-level application window.
 *
 * Values: style WS_OVERLAPPEDWINDOW | WS_VISIBLE, no owner, root == window, flags Visible | Enabled,
 * rect (10, 20) 800 x 600, dpi 96, foreign process 4242 / thread 77 / start time 1000, class
 * "TestWindow", image "app.exe", title "title". stamp and captured_ms stay 0 (the observer overlays
 * them).
 *
 * @param[out] facts Struct to zero and fill; non-NULL.
 * @param[in] window Window handle value (nonzero).
 */
static void api_double_default_facts(interop_window_facts_t *facts, uint64_t window) {
    memset(facts, 0, sizeof(*facts));
    facts->window = window;
    facts->root = window;
    facts->style = ApiDoubleStyleOverlapped;
    facts->ex_style = 0u;
    facts->flags = (uint32_t)InteropWindowFactVisible | (uint32_t)InteropWindowFactEnabled;
    facts->process_id = ApiDoubleForeignPid;
    facts->thread_id = 77u;
    facts->process_start_time = 1000u;
    facts->x = 10;
    facts->y = 20;
    facts->width = 800u;
    facts->height = 600u;
    facts->dpi = 96u;
    api_double_set_text(facts->class_name, InteropWindowGraphMaxClassBytes, &facts->class_length, "TestWindow");
    api_double_set_text(facts->image_name, InteropWindowGraphMaxImageBytes, &facts->image_length, "app.exe");
    api_double_set_text(facts->title, InteropWindowGraphMaxTitleBytes, &facts->title_length, "title");
}

/**
 * @brief Reset a double to its initial state: no windows, empty enumeration (Ok), Default desktop,
 *        no scripted failure, tid ApiDoubleOwnerTid, monotonic 1000, tick 1000.
 * @param[in,out] dbl Non-NULL object.
 */
static void api_double_init(api_double_t *dbl) {
    memset(dbl, 0, sizeof(*dbl));
    dbl->enum_result = WindowObserverEnumOk;
    dbl->desktop = WindowObserverDesktopDefault;
    dbl->next_token = 0x100u;
    dbl->tid = ApiDoubleOwnerTid;
    dbl->monotonic = 1000u;
    dbl->tick = 1000u;
}

static api_double_t *api_double_new(void) {
    api_double_t *dbl = (api_double_t *)calloc(1u, sizeof(*dbl));
    if (dbl != NULL) {
        api_double_init(dbl);
    }
    return dbl;
}

static void api_double_free(api_double_t *dbl) {
    free(dbl);
}

/**
 * @brief Find a fake window.
 * @param[in] dbl Non-NULL object.
 * @param[in] hwnd Handle to look for.
 * @return Pointer into dbl->windows (valid until api_double_init/free), or NULL when absent.
 */
static api_double_window_t *api_double_find(api_double_t *dbl, uint64_t hwnd) {
    for (uint32_t i = 0; i < ApiDoubleMaxWindows; i++) {
        if (dbl->windows[i].in_use != 0u && dbl->windows[i].hwnd == hwnd) {
            return &dbl->windows[i];
        }
    }
    return NULL;
}

/**
 * @brief Add (or reset) a fake window with default facts and mode Exists.
 * @param[in,out] dbl Non-NULL object.
 * @param[in] hwnd Nonzero handle.
 * @return The window slot (stable pointer), or NULL when the table is full.
 */
static api_double_window_t *api_double_add(api_double_t *dbl, uint64_t hwnd) {
    api_double_window_t *slot = api_double_find(dbl, hwnd);
    for (uint32_t i = 0; slot == NULL && i < ApiDoubleMaxWindows; i++) {
        if (dbl->windows[i].in_use == 0u) {
            slot = &dbl->windows[i];
        }
    }
    if (slot == NULL) {
        return NULL;
    }
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1u;
    slot->hwnd = hwnd;
    slot->mode = ApiDoubleModeExists;
    api_double_default_facts(&slot->facts, hwnd);
    return slot;
}

/**
 * @brief Remove a fake window; later queries for it answer Gone.
 * @param[in,out] dbl Non-NULL object.
 * @param[in] hwnd Handle to remove; absent handles are ignored.
 */
static void api_double_remove(api_double_t *dbl, uint64_t hwnd) {
    api_double_window_t *slot = api_double_find(dbl, hwnd);
    if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
}

/**
 * @brief Replace the enumeration list.
 * @param[in,out] dbl Non-NULL object.
 * @param[in] hwnds Array of count handles, front to back; may be NULL when count is 0.
 * @param[in] count Entries to copy, at most WindowObserverMaxEnumerated.
 */
static void api_double_enum_set(api_double_t *dbl, const uint64_t *hwnds, uint32_t count) {
    for (uint32_t i = 0; i < count && i < WindowObserverMaxEnumerated; i++) {
        dbl->enum_list[i] = hwnds[i];
    }
    dbl->enum_count = count < WindowObserverMaxEnumerated ? count : WindowObserverMaxEnumerated;
}

/**
 * @brief Number of query_facts calls recorded for one hwnd.
 * @param[in] dbl Non-NULL object.
 * @param[in] hwnd Handle.
 * @return query_count of the window, 0 when the window is absent.
 */
static uint32_t api_double_queries_of(api_double_t *dbl, uint64_t hwnd) {
    const api_double_window_t *slot = api_double_find(dbl, hwnd);
    return slot != NULL ? slot->query_count : 0u;
}

/** @brief Native identity of the calling thread (helper registration). */
#ifdef _WIN32
static int api_double_native_is_helper(const api_double_t *dbl) {
    return dbl->helper_native == GetCurrentThreadId();
}
#else
static int api_double_native_is_helper(const api_double_t *dbl) {
    return pthread_equal(dbl->helper_native, pthread_self()) != 0;
}
#endif

/** @brief Yield the processor to another thread (sched_yield / SwitchToThread). */
static void api_double_yield(void) {
#ifdef _WIN32
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

/** @brief Monotonic wall clock in milliseconds (for the park deadline only). */
static uint64_t api_double_wall_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

/** @brief api table entry: current_thread_id. Returns the helper tid on the registered helper thread.
 *         When park_ms is set, the helper thread parks here once (parked = 1) for park_ms and then answers. */
static uint32_t api_double_current_thread_id(void *context) {
    api_double_t *dbl = (api_double_t *)context;
    if (atomic_load_explicit(&dbl->helper_active, memory_order_acquire) != 0 &&
        api_double_native_is_helper(dbl)) {
        if (dbl->park_ms != 0u) {
            const uint64_t deadline = api_double_wall_ms() + dbl->park_ms;
            dbl->park_ms = 0u;
            dbl->park_deadline_ms = deadline;
            atomic_store_explicit(&dbl->parked, 1, memory_order_release);
            while (api_double_wall_ms() < deadline) {
                api_double_yield();
            }
        }
        return dbl->helper_tid;
    }
    return dbl->tid;
}

/** @brief api table entry: monotonic_ms. */
static uint64_t api_double_monotonic_ms(void *context) {
    api_double_t *dbl = (api_double_t *)context;
    dbl->monotonic_calls++;
    return dbl->monotonic;
}

/** @brief api table entry: tick_ms. */
static uint32_t api_double_tick_ms(void *context) {
    api_double_t *dbl = (api_double_t *)context;
    dbl->tick_calls++;
    return dbl->tick;
}

/** @brief api table entry: query_input_desktop. */
static window_observer_desktop_t api_double_query_input_desktop(void *context) {
    api_double_t *dbl = (api_double_t *)context;
    dbl->desktop_calls++;
    return dbl->desktop;
}

/** @brief api table entry: install_hook. Fails at install_fail_at; records ranges and tokens. */
static int api_double_install_hook(void *context, uint32_t event_min, uint32_t event_max, uintptr_t *hook) {
    api_double_t *dbl = (api_double_t *)context;
    if (dbl->install_hook_fn != NULL) {
        dbl->install_hook_fn(dbl, dbl->install_hook_arg);
    }
    const uint32_t index = dbl->install_calls;
    dbl->install_calls++;
    if (index < ApiDoubleMaxHooks) {
        dbl->install_min[index] = event_min;
        dbl->install_max[index] = event_max;
        dbl->install_tokens[index] = 0u;
    }
    if (dbl->install_fail_at != 0u && dbl->install_calls == dbl->install_fail_at) {
        return 1;
    }
    *hook = dbl->next_token;
    if (index < ApiDoubleMaxHooks) {
        dbl->install_tokens[index] = dbl->next_token;
    }
    dbl->next_token++;
    return 0;
}

/** @brief api table entry: remove_hook. Fails at remove_fail_at; records the token. */
static int api_double_remove_hook(void *context, uintptr_t hook) {
    api_double_t *dbl = (api_double_t *)context;
    const uint32_t index = dbl->remove_calls;
    dbl->remove_calls++;
    if (index < ApiDoubleMaxHooks) {
        dbl->removed_tokens[index] = hook;
    }
    return dbl->remove_fail_at != 0u && dbl->remove_calls == dbl->remove_fail_at ? 1 : 0;
}

/** @brief api table entry: enum_top_level_windows. Failed reports a garbage count on purpose. */
static window_observer_enum_result_t api_double_enum(void *context, uint64_t *windows, uint32_t capacity,
                                                     uint32_t *count) {
    api_double_t *dbl = (api_double_t *)context;
    dbl->enum_calls++;
    if (dbl->enum_result == WindowObserverEnumFailed) {
        *count = 0xFFFFFFFFu;
        return WindowObserverEnumFailed;
    }
    uint32_t written = dbl->enum_count < capacity ? dbl->enum_count : capacity;
    for (uint32_t i = 0; i < written; i++) {
        windows[i] = dbl->enum_list[i];
    }
    *count = dbl->enum_report_count != 0u ? dbl->enum_report_count : written;
    return dbl->enum_result;
}

/**
 * @brief Hook-procedure emulator with every argument explicit.
 * @param[in] dbl Non-NULL object (supplies the fake thread id of the event thread).
 * @param[in] event WinEvent id.
 * @param[in] window HWND value.
 * @param[in] object_id Accessible object id.
 * @param[in] child_id Accessible child id.
 * @param[in] event_time_ms Tick-domain event time.
 */
static void api_double_hint_full(api_double_t *dbl, uint32_t event, uint64_t window, int32_t object_id,
                                 int32_t child_id, uint32_t event_time_ms) {
    window_observer_note_hint(event, window, object_id, child_id, dbl->tid, event_time_ms);
}

/**
 * @brief Hook-procedure emulator for a well-formed hint: OBJID_WINDOW, CHILDID_SELF, event time = dbl->tick.
 * @param[in] dbl Non-NULL object.
 * @param[in] event WinEvent id.
 * @param[in] window HWND value.
 */
static void api_double_hint(api_double_t *dbl, uint32_t event, uint64_t window) {
    api_double_hint_full(dbl, event, window, WindowObserverObjectIdWindow, WindowObserverChildIdSelf, dbl->tick);
}

/** @brief api table entry: query_facts (see window_observer_api_t). Fires the scripted re-entry first. */
static window_observer_query_result_t api_double_query_facts(void *context, uint64_t window,
                                                             interop_window_facts_t *facts) {
    api_double_t *dbl = (api_double_t *)context;
    if (dbl->query_total < ApiDoubleQueryLogCapacity) {
        dbl->query_log[dbl->query_total] = window;
    }
    dbl->query_total++;
    if (dbl->hint_pending != 0u) {
        dbl->hint_pending = 0u;
        api_double_hint(dbl, dbl->hint_event, dbl->hint_window);
    }
    if (dbl->query_hook != NULL && (dbl->query_hook_window == 0u || dbl->query_hook_window == window)) {
        const api_double_query_hook_t hook = dbl->query_hook;
        if (dbl->query_hook_once != 0u) {
            dbl->query_hook = NULL;
        }
        hook(dbl, window, dbl->query_hook_arg);
    }
    api_double_window_t *slot = api_double_find(dbl, window);
    if (slot == NULL) {
        return WindowObserverQueryGone;
    }
    slot->query_count++;
    if (slot->fail_count != 0u) {
        slot->fail_count--;
        return WindowObserverQueryFailed;
    }
    if (slot->mode == ApiDoubleModeGone) {
        return WindowObserverQueryGone;
    }
    if (slot->mode == ApiDoubleModeFailed) {
        return WindowObserverQueryFailed;
    }
    *facts = slot->facts;
    facts->window = slot->report_window != 0u ? slot->report_window : window;
    facts->flags &= ~(uint32_t)InteropWindowFactOwnerHidden; /* the callee never sets OwnerHidden */
    return WindowObserverQueryExists;
}

/** The scriptable api table; api_context must be an api_double_t pointer. */
static const window_observer_api_t ApiDoubleTable = {
    api_double_install_hook,
    api_double_remove_hook,
    api_double_query_facts,
    api_double_enum,
    api_double_query_input_desktop,
    api_double_current_thread_id,
    api_double_monotonic_ms,
    api_double_tick_ms,
};

/**
 * @brief Fill an observer config for the double.
 * @param[out] config Struct to zero and fill; non-NULL.
 * @param[in] dbl Context passed to every api entry; non-NULL.
 * @param[in] uac_mode interop_window_uac_mode_t value.
 * @param[in] first_stamp First logical stamp.
 */
static void api_double_config(window_observer_config_t *config, api_double_t *dbl, uint32_t uac_mode,
                              uint64_t first_stamp) {
    memset(config, 0, sizeof(*config));
    config->api = &ApiDoubleTable;
    config->api_context = dbl;
    config->first_stamp = first_stamp;
    config->preedit_window = 0u;
    config->own_process_id = ApiDoubleOwnPid;
    config->uac_mode = uac_mode;
}

/** @brief Work item for api_double_run_on_other_thread. */
typedef struct api_double_job_t {
    api_double_t *dbl; /**< Double that learns the helper identity. */
    uint32_t tid; /**< Fake thread id the helper reports. */
    void (*function)(void *argument); /**< Function run on the helper; non-NULL. */
    void *argument; /**< Passed to function. */
} api_double_job_t;

/** @brief Body of the helper thread: register identity, run the function, unregister. */
static void api_double_job_run(api_double_job_t *job) {
    api_double_t *dbl = job->dbl;
#ifdef _WIN32
    dbl->helper_native = GetCurrentThreadId();
#else
    dbl->helper_native = pthread_self();
#endif
    dbl->helper_tid = job->tid;
    atomic_store_explicit(&dbl->helper_active, 1, memory_order_release);
    job->function(job->argument);
    atomic_store_explicit(&dbl->helper_active, 0, memory_order_release);
}

#ifdef _WIN32
static DWORD WINAPI api_double_thread_entry(LPVOID parameter) {
    api_double_job_run((api_double_job_t *)parameter);
    return 0;
}
#else
static void *api_double_thread_entry(void *parameter) {
    api_double_job_run((api_double_job_t *)parameter);
    return NULL;
}
#endif

/** @brief A started helper thread; opaque to tests, filled by api_double_helper_start. */
typedef struct api_double_helper_t {
    api_double_job_t job; /**< Job description; must stay at a stable address until the join. */
#ifdef _WIN32
    HANDLE thread; /**< Native thread handle. */
#else
    pthread_t thread; /**< Native thread handle. */
#endif
} api_double_helper_t;

/**
 * @brief Start function(argument) on a second real thread whose current_thread_id is tid, without joining.
 *
 * @param[in,out] dbl Double whose api_double_current_thread_id reports tid on the helper; non-NULL.
 * @param[in] tid Fake thread id of the helper; must differ from dbl->tid to look foreign.
 * @param[in] function Non-NULL function to run on the helper.
 * @param[in,out] argument Passed to function; may be NULL.
 * @param[out] helper Non-NULL; must stay valid until api_double_helper_join returns.
 * @return 0 when the thread was created, 1 otherwise (then no join is needed).
 * @note The caller thread runs concurrently with the helper until the join; only the atomic parked flag
 *       and the helper identity are shared safely, everything else is the test's responsibility.
 */
static int api_double_helper_start(api_double_t *dbl, uint32_t tid, void (*function)(void *argument),
                                   void *argument, api_double_helper_t *helper) {
    helper->job.dbl = dbl;
    helper->job.tid = tid;
    helper->job.function = function;
    helper->job.argument = argument;
#ifdef _WIN32
    helper->thread = CreateThread(NULL, 0, api_double_thread_entry, &helper->job, 0, NULL);
    return helper->thread == NULL ? 1 : 0;
#else
    return pthread_create(&helper->thread, NULL, api_double_thread_entry, &helper->job) != 0 ? 1 : 0;
#endif
}

/** @brief Join a helper started by api_double_helper_start. @param[in,out] helper Started helper; invalid afterwards. */
static void api_double_helper_join(api_double_helper_t *helper) {
#ifdef _WIN32
    WaitForSingleObject(helper->thread, INFINITE);
    CloseHandle(helper->thread);
#else
    (void)pthread_join(helper->thread, NULL);
#endif
}

/**
 * @brief Run function(argument) on a second real thread whose current_thread_id is tid, and join it.
 *
 * The test thread is blocked in the join, so the helper has the double to itself. Use it to call
 * observer entry points, or the hint emulator, "from another thread".
 *
 * @param[in,out] dbl Double whose api_double_current_thread_id reports tid on the helper; non-NULL.
 * @param[in] tid Fake thread id of the helper; must differ from dbl->tid to look foreign.
 * @param[in] function Non-NULL function to run on the helper.
 * @param[in,out] argument Passed to function; may be NULL.
 * @return 0 when the thread ran and was joined, 1 when it could not be created.
 */
static int api_double_run_on_other_thread(api_double_t *dbl, uint32_t tid, void (*function)(void *argument),
                                          void *argument) {
    api_double_helper_t helper;
    if (api_double_helper_start(dbl, tid, function, argument, &helper) != 0) {
        return 1;
    }
    api_double_helper_join(&helper);
    return 0;
}

#endif
