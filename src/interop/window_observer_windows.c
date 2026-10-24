/**
 * @file window_observer_windows.c
 * @brief Win32 observation adapter feeding the session window graph (TODO #10).
 *
 * The file has two layers. The portable core (lifecycle, dirty-hint table, pump, refresh,
 * cadence, counters) compiles on every platform and reaches the operating system only through
 * the injected window_observer_api_t table. The native section, compiled only under _WIN32,
 * provides that table for the real Win32 desktop (WinEvent hooks, window queries, enumeration).
 * The contract of every public function lives in window_observer_windows.h; this file only
 * repeats what an implementer needs to see and records the decisions taken where the header is
 * silent.
 *
 * Decisions where the header is silent:
 *  - Hints and dirty entries: hints_received counts every accepted hint (new entry or merge),
 *    hints_coalesced the merged subset. A hint that cannot obtain a stamp (clock exhausted)
 *    latches overflow. A hint delivered while the observer is not Started (including from inside
 *    install_hook) is ignored without a counter; start() latches a full refresh that covers it.
 *  - Destroy information already applied to the graph is cleared from a key before the key is
 *    put back, so a retry never repeats an applied destroy. Put-back keys that find the table
 *    full (a hint filled the freed slot) latch overflow, which makes pump recover by invalidation.
 *  - graph_errors counts every graph status that is neither Ok nor None (dropped or stopping).
 *  - A refresh offer answered by a "drop" status (InvalidFacts, StaleFacts, Cycle, AtCapacity,
 *    GenerationExhausted, LookupFailed, AppKeyInvalid, Stale) is counted and skipped and the
 *    refresh continues, exactly as the file-comment mapping in the header says; only QueueFull
 *    and the "anything else" statuses abort the refresh.
 *  - Cadence versus desktop: the refresh latch counts as "due" only while the desktop_active gauge
 *    is 1 (the latest check returned Default), otherwise refresh_due would be permanently 1 and
 *    the owner loop would spin. A pump or refresh that finds the desktop inactive stamps the
 *    cadence clock, so the next desktop poll happens one InteropWindowGraphRefreshIntervalMs
 *    later. start() clears the gauge and the cadence clock, so a freshly started observer is due
 *    immediately through the cadence rule.
 *  - A graph Suspended disposition returned to pump puts the key back and returns DesktopInactive;
 *    in refresh it aborts the refresh and returns DesktopInactive. QueueFull put-backs go to the
 *    tail of the dirty table, other stops (GraphError, StampExhausted, Suspended) to the head.
 *  - OnInputDesktop is set on every offered snapshot because the desktop was checked Default at
 *    the start of the call (pump) or before and after the whole enumeration (refresh).
 *  - AgentPreedit additionally requires the window to belong to own_process_id (the graph demands
 *    AgentPreedit implies AgentOwned).
 *  - Stamp exhaustion: UINT64_MAX is the sentinel for "no stamp left"; first_stamp must be
 *    nonzero and below it, and stats.next_stamp reads UINT64_MAX once the clock is exhausted.
 *  - Lifetime safety of the process-wide slot: note_hint registers itself in a process-wide
 *    in-flight counter before it loads the slot, and the code that clears the slot waits for that
 *    counter to reach zero, so an off-thread hint can never touch a freed observer.
 */
#include "window_observer_windows.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

/** @brief Bits recorded per dirty entry; only DirtyKindDestroy carries authority. */
typedef enum dirty_kind_t {
    DirtyKindCreate = 1u << 0,
    DirtyKindDestroy = 1u << 1,
    DirtyKindShow = 1u << 2,
    DirtyKindHide = 1u << 3,
    DirtyKindObjectOther = 1u << 4,
    DirtyKindMinimizeStart = 1u << 5,
    DirtyKindMinimizeEnd = 1u << 6
} dirty_kind_t;

/** @brief How a graph status is handled by pump and refresh. */
typedef enum fold_t {
    FoldContinue = 0, /**< Ok or None: carry on. */
    FoldDrop = 1, /**< Counted; the key or offer is dropped and processing continues. */
    FoldQueueFull = 2, /**< Counted; processing stops with GraphQueueFull. */
    FoldFatal = 3 /**< Counted; processing stops with GraphError. */
} fold_t;

/** @brief One coalesced dirty-handle entry. */
typedef struct dirty_entry_t {
    uint64_t window; /**< HWND value, nonzero. */
    uint32_t kinds; /**< OR of dirty_kind_t bits. */
    uint32_t retries; /**< Failed-query retries already spent. */
    uint64_t first_stamp; /**< Stamp of the first hint of this entry. */
    uint64_t last_stamp; /**< Stamp of the latest hint. */
    uint64_t destroy_stamp; /**< Stamp of the latest Destroy hint; 0 = none. */
    uint32_t destroy_event_ms; /**< Event time of that Destroy hint. */
    uint32_t reserved0; /**< Padding, zero. */
} dirty_entry_t;

/** @brief Private observer state; one calloc'd block owns everything below. */
struct window_observer_t {
    window_observer_api_t api; /**< Copy of the native or injected table. */
    void *api_context; /**< Passed to every api entry. */
    uint32_t owner_thread; /**< Thread id captured at create. */
    uint32_t state; /**< window_observer_state_t. */
    uintptr_t hook_object; /**< Token of the object-range hook; 0 = none. */
    uintptr_t hook_system; /**< Token of the system-range hook; 0 = none. */
    uint32_t dirty_count; /**< Entries in dirty[0..dirty_count), first-seen order. */
    uint32_t own_process_id; /**< Agent process id. */
    uint64_t next_stamp; /**< Next logical stamp; UINT64_MAX = exhausted. */
    uint64_t preedit_window; /**< Preedit HWND or 0. */
    uint32_t overflow; /**< Overflow latch. */
    uint32_t refresh_requested; /**< Full-refresh latch. */
    uint32_t desktop_active; /**< Gauge: latest desktop check returned Default. */
    uint32_t desktop_blocked; /**< Latest desktop check returned non-Default (drives the cadence rules). */
    uint64_t last_refresh_ms; /**< Cadence clock value of the latest attempt. */
    uint32_t have_refresh_ms; /**< Nonzero once last_refresh_ms is valid. */
    uint32_t reserved0; /**< Padding, zero. */
    _Atomic uint64_t hints_wrong_thread; /**< Off-thread hint counter (the only cross-thread write). */
    window_observer_stats_t stats; /**< Counters; owner thread only. */
    dirty_entry_t dirty[WindowObserverDirtyCapacity]; /**< Dirty table. */
    uint64_t enum_buffer[WindowObserverMaxEnumerated]; /**< Enumeration scratch for refresh. */
};

/** Process-wide active observer; hook procedures carry no context. */
static _Atomic(window_observer_t *) g_active_observer = NULL;
/** Number of note_hint calls currently between slot load and completion. */
static _Atomic uint32_t g_hint_inflight = 0;

/* ------------------------------------------------------------------------------------------ */
/* Small helpers                                                                              */
/* ------------------------------------------------------------------------------------------ */

/* Saturating increment of a plain counter. */
static void bump(uint64_t *counter) {
    if (*counter != UINT64_MAX) {
        *counter += 1u;
    }
}

/* Saturating increment of the cross-thread atomic counter. */
static void bump_atomic(_Atomic uint64_t *counter) {
    uint64_t value = atomic_load_explicit(counter, memory_order_relaxed);
    while (value != UINT64_MAX &&
           !atomic_compare_exchange_weak_explicit(counter, &value, value + 1u, memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

/* Return nonzero when the caller runs on the observer's owner thread. */
static int on_owner_thread(const window_observer_t *observer) {
    return observer->api.current_thread_id(observer->api_context) == observer->owner_thread;
}

/* Issue the next logical stamp; returns 0 when the clock reached the UINT64_MAX sentinel. */
static int stamp_next(window_observer_t *observer, uint64_t *stamp) {
    if (observer->next_stamp == UINT64_MAX) {
        return 0;
    }
    *stamp = observer->next_stamp;
    observer->next_stamp += 1u;
    return 1;
}

/* Copy counters and gauges into a caller struct. */
static void fill_stats(const window_observer_t *observer, window_observer_stats_t *out) {
    *out = observer->stats;
    out->next_stamp = observer->next_stamp;
    out->state = observer->state;
    out->dirty_pending = observer->dirty_count;
    out->desktop_active = observer->desktop_active;
    out->overflow_latched = observer->overflow;
    out->refresh_requested = observer->refresh_requested;
    out->hints_wrong_thread = atomic_load_explicit(&observer->hints_wrong_thread, memory_order_relaxed);
}

/* Snapshot into the optional stats out-parameter. */
static void snapshot_stats(const window_observer_t *observer, window_observer_stats_t *stats) {
    if (stats != NULL) {
        fill_stats(observer, stats);
    }
}

/* Release the process-wide slot if this observer holds it, then wait out in-flight hints. */
static void release_slot(window_observer_t *observer) {
    window_observer_t *expected = observer;
    if (atomic_compare_exchange_strong(&g_active_observer, &expected, NULL)) {
        while (atomic_load(&g_hint_inflight) != 0u) {
            /* An off-thread note_hint is finishing; it never blocks, so this ends promptly. */
        }
    }
}

/* Map one graph status to a handling class and account for it. */
static fold_t fold_status(window_observer_t *observer, interop_window_graph_status_t status) {
    if (status == InteropWindowGraphStatusOk || status == InteropWindowGraphStatusNone) {
        return FoldContinue;
    }
    observer->stats.last_graph_status = (uint32_t)status;
    bump(&observer->stats.graph_errors);
    switch (status) {
    case InteropWindowGraphStatusInvalidFacts:
    case InteropWindowGraphStatusStaleFacts:
    case InteropWindowGraphStatusCycle:
    case InteropWindowGraphStatusAtCapacity:
    case InteropWindowGraphStatusGenerationExhausted:
    case InteropWindowGraphStatusLookupFailed:
    case InteropWindowGraphStatusAppKeyInvalid:
    case InteropWindowGraphStatusStale:
        return FoldDrop;
    case InteropWindowGraphStatusQueueFull:
        return FoldQueueFull;
    default:
        return FoldFatal;
    }
}

/* Convert a non-offer graph fold into a pump/refresh status (Drop is not meaningful there). */
static window_observer_status_t stop_status(fold_t fold) {
    return fold == FoldQueueFull ? WindowObserverStatusGraphQueueFull : WindowObserverStatusGraphError;
}

/* ------------------------------------------------------------------------------------------ */
/* Dirty table                                                                                */
/* ------------------------------------------------------------------------------------------ */

/* Find the table index of a window, or -1. */
static int dirty_find(const window_observer_t *observer, uint64_t window) {
    for (uint32_t i = 0; i < observer->dirty_count; i++) {
        if (observer->dirty[i].window == window) {
            return (int)i;
        }
    }
    return -1;
}

/* Fold src into dst: OR kinds, widen stamps, keep the newest destroy information. */
static void dirty_merge(dirty_entry_t *dst, const dirty_entry_t *src) {
    dst->kinds |= src->kinds;
    if (src->first_stamp != 0u && (dst->first_stamp == 0u || src->first_stamp < dst->first_stamp)) {
        dst->first_stamp = src->first_stamp;
    }
    if (src->last_stamp > dst->last_stamp) {
        dst->last_stamp = src->last_stamp;
    }
    if (src->destroy_stamp > dst->destroy_stamp) {
        dst->destroy_stamp = src->destroy_stamp;
        dst->destroy_event_ms = src->destroy_event_ms;
    }
}

/* Remove and return the oldest entry; returns 0 when the table is empty. */
static int dirty_pop_front(window_observer_t *observer, dirty_entry_t *out) {
    if (observer->dirty_count == 0u) {
        return 0;
    }
    *out = observer->dirty[0];
    observer->dirty_count -= 1u;
    if (observer->dirty_count != 0u) {
        memmove(&observer->dirty[0], &observer->dirty[1], (size_t)observer->dirty_count * sizeof(dirty_entry_t));
    }
    return 1;
}

/* Put an entry back at the head (processing stopped); merges into a newer entry of the same window. */
static void dirty_push_front(window_observer_t *observer, const dirty_entry_t *entry) {
    int index = dirty_find(observer, entry->window);
    if (index >= 0) {
        dirty_merge(&observer->dirty[index], entry);
        return;
    }
    if (observer->dirty_count >= WindowObserverDirtyCapacity) {
        observer->overflow = 1u;
        return;
    }
    memmove(&observer->dirty[1], &observer->dirty[0], (size_t)observer->dirty_count * sizeof(dirty_entry_t));
    observer->dirty[0] = *entry;
    observer->dirty_count += 1u;
}

/* Put a retried entry back at the tail; merges into a newer entry of the same window. */
static void dirty_push_back_retry(window_observer_t *observer, const dirty_entry_t *entry) {
    int index = dirty_find(observer, entry->window);
    if (index >= 0) {
        dirty_merge(&observer->dirty[index], entry);
        return;
    }
    if (observer->dirty_count >= WindowObserverDirtyCapacity) {
        observer->overflow = 1u;
        return;
    }
    observer->dirty[observer->dirty_count] = *entry;
    observer->dirty_count += 1u;
}

/* Map a WinEvent id to its dirty kind bit. */
static uint32_t dirty_kind_for_event(uint32_t event) {
    switch (event) {
    case WindowObserverEventObjectCreate:
        return DirtyKindCreate;
    case WindowObserverEventObjectDestroy:
        return DirtyKindDestroy;
    case WindowObserverEventObjectShow:
        return DirtyKindShow;
    case WindowObserverEventObjectHide:
        return DirtyKindHide;
    case WindowObserverEventSystemMinimizeStart:
        return DirtyKindMinimizeStart;
    case WindowObserverEventSystemMinimizeEnd:
        return DirtyKindMinimizeEnd;
    default:
        return DirtyKindObjectOther;
    }
}

/* Return nonzero when the event id lies in one of the two hooked ranges. */
static int event_in_hooked_range(uint32_t event) {
    return (event >= WindowObserverEventObjectCreate && event <= WindowObserverEventObjectNameChange) ||
           (event >= WindowObserverEventSystemMinimizeStart && event <= WindowObserverEventSystemMinimizeEnd);
}

/* ------------------------------------------------------------------------------------------ */
/* Lifecycle                                                                                  */
/* ------------------------------------------------------------------------------------------ */

#ifdef _WIN32
/* Return nonzero when the native table can run on this OS (GetDpiForWindow is resolvable). */
static int native_dpi_available(void);
#endif

/** @copydoc window_observer_create */
window_observer_status_t window_observer_create(const window_observer_config_t *config,
                                                window_observer_t **out_observer) {
    if (out_observer == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    *out_observer = NULL;
    if (config == NULL || config->reserved0 != 0u || config->first_stamp == 0u || config->first_stamp == UINT64_MAX ||
        config->own_process_id == 0u) {
        return WindowObserverStatusInvalidArgument;
    }
    const window_observer_api_t *api = config->api;
    if (api == NULL) {
#ifdef _WIN32
        api = window_observer_native_api();
#else
        return WindowObserverStatusInvalidArgument;
#endif
    }
    if (api->install_hook == NULL || api->remove_hook == NULL || api->query_facts == NULL ||
        api->enum_top_level_windows == NULL || api->query_input_desktop == NULL ||
        api->current_thread_id == NULL || api->monotonic_ms == NULL || api->tick_ms == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
#ifdef _WIN32
    if (api == window_observer_native_api() && !native_dpi_available()) {
        return WindowObserverStatusInvalidArgument; /* Windows older than 10 1607: no GetDpiForWindow */
    }
#endif
    window_observer_t *observer = calloc(1u, sizeof(*observer));
    if (observer == NULL) {
        return WindowObserverStatusNoMemory;
    }
    observer->api = *api;
    observer->api_context = config->api_context;
    observer->owner_thread = api->current_thread_id(config->api_context);
    observer->state = WindowObserverStateCreated;
    observer->next_stamp = config->first_stamp;
    observer->preedit_window = config->preedit_window;
    observer->own_process_id = config->own_process_id;
    *out_observer = observer;
    return WindowObserverStatusOk;
}

/** @copydoc window_observer_stop */
window_observer_status_t window_observer_stop(window_observer_t *observer) {
    if (observer == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    if (!on_owner_thread(observer)) {
        return WindowObserverStatusWrongThread;
    }
    if (observer->state == WindowObserverStateFailed) {
        return WindowObserverStatusBadState;
    }
    if (observer->state != WindowObserverStateStarted) {
        return WindowObserverStatusOk;
    }
    release_slot(observer);
    int failed = 0;
    if (observer->hook_system != 0u) {
        failed |= observer->api.remove_hook(observer->api_context, observer->hook_system) != 0;
        observer->hook_system = 0u;
    }
    if (observer->hook_object != 0u) {
        failed |= observer->api.remove_hook(observer->api_context, observer->hook_object) != 0;
        observer->hook_object = 0u;
    }
    observer->dirty_count = 0u;
    observer->overflow = 0u;
    observer->refresh_requested = 0u;
    if (failed) {
        observer->state = WindowObserverStateFailed;
        return WindowObserverStatusUnhookFailed;
    }
    observer->state = WindowObserverStateStopped;
    return WindowObserverStatusOk;
}

/** @copydoc window_observer_destroy */
window_observer_status_t window_observer_destroy(window_observer_t **observer_slot) {
    if (observer_slot == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    window_observer_t *observer = *observer_slot;
    if (observer == NULL) {
        return WindowObserverStatusOk;
    }
    if (!on_owner_thread(observer)) {
        return WindowObserverStatusWrongThread;
    }
    window_observer_status_t result = WindowObserverStatusOk;
    if (observer->state == WindowObserverStateStarted) {
        result = window_observer_stop(observer);
    }
    release_slot(observer);
    free(observer);
    *observer_slot = NULL;
    return result == WindowObserverStatusUnhookFailed ? WindowObserverStatusUnhookFailed
                                                      : WindowObserverStatusOk;
}

/* Remove the hooks installed so far (reverse order) after a failed start. */
static void start_unwind(window_observer_t *observer, uintptr_t object_hook, uintptr_t system_hook) {
    if (system_hook != 0u) {
        (void)observer->api.remove_hook(observer->api_context, system_hook);
    }
    if (object_hook != 0u) {
        (void)observer->api.remove_hook(observer->api_context, object_hook);
    }
}

/** @copydoc window_observer_start */
window_observer_status_t window_observer_start(window_observer_t *observer) {
    if (observer == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    if (!on_owner_thread(observer)) {
        return WindowObserverStatusWrongThread;
    }
    if (observer->state != WindowObserverStateCreated && observer->state != WindowObserverStateStopped) {
        return WindowObserverStatusBadState;
    }
    window_observer_t *expected = NULL;
    if (!atomic_compare_exchange_strong(&g_active_observer, &expected, observer)) {
        return WindowObserverStatusAlreadyActive;
    }
    observer->dirty_count = 0u;
    observer->overflow = 0u;

    uintptr_t object_hook = 0u;
    uintptr_t system_hook = 0u;
    int rc = observer->api.install_hook(observer->api_context, WindowObserverEventObjectCreate,
                                        WindowObserverEventObjectNameChange, &object_hook);
    if (rc != 0 || object_hook == 0u) {
        release_slot(observer);
        observer->state = WindowObserverStateFailed;
        return WindowObserverStatusHookFailed;
    }
    rc = observer->api.install_hook(observer->api_context, WindowObserverEventSystemMinimizeStart,
                                    WindowObserverEventSystemMinimizeEnd, &system_hook);
    if (rc != 0 || system_hook == 0u) {
        start_unwind(observer, object_hook, 0u);
        release_slot(observer);
        observer->state = WindowObserverStateFailed;
        return WindowObserverStatusHookFailed;
    }
    observer->hook_object = object_hook;
    observer->hook_system = system_hook;
    observer->dirty_count = 0u; /* hints that raced the install were ignored; the latch below covers them */
    observer->overflow = 0u;
    observer->refresh_requested = 1u;
    observer->desktop_active = 0u;
    observer->desktop_blocked = 0u;
    observer->have_refresh_ms = 0u;
    observer->state = WindowObserverStateStarted;
    return WindowObserverStatusOk;
}

/** @copydoc window_observer_set_preedit_window */
window_observer_status_t window_observer_set_preedit_window(window_observer_t *observer, uint64_t window) {
    if (observer == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    if (!on_owner_thread(observer)) {
        return WindowObserverStatusWrongThread;
    }
    if (observer->preedit_window != window) {
        observer->preedit_window = window;
        if (observer->state == WindowObserverStateStarted) {
            observer->refresh_requested = 1u;
        }
    }
    return WindowObserverStatusOk;
}

/** @copydoc window_observer_status */
window_observer_status_t window_observer_status(const window_observer_t *observer,
                                                window_observer_stats_t *stats) {
    if (observer == NULL || stats == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    if (!on_owner_thread(observer)) {
        return WindowObserverStatusWrongThread;
    }
    fill_stats(observer, stats);
    return WindowObserverStatusOk;
}

/* ------------------------------------------------------------------------------------------ */
/* Hints                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/* Record one hint on the active observer (runs in the hook procedure; touches no api but the thread id). */
static void note_hint_active(window_observer_t *observer, uint32_t event, uint64_t window, int32_t object_id,
                             int32_t child_id, uint32_t event_time_ms) {
    if (observer->api.current_thread_id(observer->api_context) != observer->owner_thread) {
        bump_atomic(&observer->hints_wrong_thread);
        return;
    }
    if (observer->state != WindowObserverStateStarted) {
        return;
    }
    if (object_id != WindowObserverObjectIdWindow || child_id != WindowObserverChildIdSelf || window == 0u ||
        !event_in_hooked_range(event)) {
        bump(&observer->stats.hints_filtered);
        return;
    }
    uint64_t stamp = 0;
    if (!stamp_next(observer, &stamp)) {
        observer->overflow = 1u;
        return;
    }
    const uint32_t kind = dirty_kind_for_event(event);
    int index = dirty_find(observer, window);
    if (index >= 0) {
        dirty_entry_t *entry = &observer->dirty[index];
        entry->kinds |= kind;
        entry->last_stamp = stamp;
        if (event == WindowObserverEventObjectDestroy) {
            entry->destroy_stamp = stamp;
            entry->destroy_event_ms = event_time_ms;
        }
        bump(&observer->stats.hints_received);
        bump(&observer->stats.hints_coalesced);
        return;
    }
    if (observer->dirty_count >= WindowObserverDirtyCapacity) {
        observer->overflow = 1u;
        bump(&observer->stats.hint_overflows);
        return;
    }
    dirty_entry_t *entry = &observer->dirty[observer->dirty_count];
    memset(entry, 0, sizeof(*entry));
    entry->window = window;
    entry->kinds = kind;
    entry->first_stamp = stamp;
    entry->last_stamp = stamp;
    if (event == WindowObserverEventObjectDestroy) {
        entry->destroy_stamp = stamp;
        entry->destroy_event_ms = event_time_ms;
    }
    observer->dirty_count += 1u;
    bump(&observer->stats.hints_received);
}

/** @copydoc window_observer_note_hint */
void window_observer_note_hint(uint32_t event, uint64_t window, int32_t object_id, int32_t child_id,
                               uint32_t event_thread_id, uint32_t event_time_ms) {
    (void)event_thread_id;
    atomic_fetch_add(&g_hint_inflight, 1u);
    window_observer_t *observer = atomic_load(&g_active_observer);
    if (observer != NULL) {
        note_hint_active(observer, event, window, object_id, child_id, event_time_ms);
    }
    atomic_fetch_sub(&g_hint_inflight, 1u);
}

/* ------------------------------------------------------------------------------------------ */
/* Query plumbing shared by pump and refresh                                                  */
/* ------------------------------------------------------------------------------------------ */

/* Record an inactive desktop: latch a refresh, stamp the cadence clock and count the refusal. */
static void desktop_mark_inactive(window_observer_t *observer) {
    observer->desktop_active = 0u;
    observer->desktop_blocked = 1u;
    observer->refresh_requested = 1u;
    observer->last_refresh_ms = observer->api.monotonic_ms(observer->api_context);
    observer->have_refresh_ms = 1u;
    bump(&observer->stats.pumps_desktop_inactive);
}

/* Check the input desktop; returns 1 for Default, otherwise records the inactive desktop and returns 0. */
static int desktop_check(window_observer_t *observer) {
    const window_observer_desktop_t desktop = observer->api.query_input_desktop(observer->api_context);
    if (desktop == WindowObserverDesktopDefault) {
        observer->desktop_active = 1u;
        observer->desktop_blocked = 0u;
        return 1;
    }
    desktop_mark_inactive(observer);
    return 0;
}

/* Zero facts, run the native query, validate the answer and account for it. */
static window_observer_query_result_t query_one(window_observer_t *observer, uint64_t window,
                                                interop_window_facts_t *facts) {
    memset(facts, 0, sizeof(*facts));
    window_observer_query_result_t result = observer->api.query_facts(observer->api_context, window, facts);
    if (result == WindowObserverQueryExists && facts->window != window) {
        result = WindowObserverQueryFailed;
    }
    switch (result) {
    case WindowObserverQueryExists:
        bump(&observer->stats.queries_exists);
        return result;
    case WindowObserverQueryGone:
        bump(&observer->stats.queries_gone);
        return result;
    default:
        bump(&observer->stats.queries_failed);
        return WindowObserverQueryFailed;
    }
}

/* Overwrite the fields the core owns: stamp, captured time and the three observer flags. */
static void overlay_facts(const window_observer_t *observer, interop_window_facts_t *facts, uint64_t stamp,
                          uint32_t captured_ms) {
    facts->stamp = stamp;
    facts->captured_ms = captured_ms;
    facts->flags &= ~((uint32_t)InteropWindowFactAgentOwned | (uint32_t)InteropWindowFactAgentPreedit |
                      (uint32_t)InteropWindowFactOnInputDesktop);
    facts->flags |= (uint32_t)InteropWindowFactOnInputDesktop;
    if (facts->process_id == observer->own_process_id) {
        facts->flags |= (uint32_t)InteropWindowFactAgentOwned;
        if (observer->preedit_window != 0u && facts->window == observer->preedit_window) {
            facts->flags |= (uint32_t)InteropWindowFactAgentPreedit;
        }
    }
}

/* Count a result whose reason says the UAC consent window is hidden by mode Native. */
static void count_uac_hidden(window_observer_t *observer, const interop_window_observation_t *observation) {
    if (observation->reason == InteropWindowReasonUacNativeHidden) {
        bump(&observer->stats.uac_native_hidden);
    }
}

/* Abort the open refresh, ignoring the status (the original failure is what the caller reports). */
static void refresh_abort_quiet(window_observer_t *observer, interop_window_graph_t *graph, uint64_t epoch) {
    if (interop_window_graph_refresh_abort(graph, epoch) == InteropWindowGraphStatusOk) {
        bump(&observer->stats.refreshes_aborted);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Pump                                                                                       */
/* ------------------------------------------------------------------------------------------ */

/* Apply a note_destroyed result to a key; Ok means "continue with this key", other values stop or drop. */
static window_observer_status_t apply_destroy(window_observer_t *observer, interop_window_graph_t *graph,
                                              uint64_t epoch, uint64_t window, uint64_t stamp,
                                              uint32_t event_ms, fold_t *fold) {
    interop_window_observation_t observation;
    memset(&observation, 0, sizeof(observation));
    const interop_window_graph_status_t status =
        interop_window_graph_note_destroyed(graph, epoch, window, stamp, event_ms, &observation);
    bump(&observer->stats.destroys_noted);
    *fold = fold_status(observer, status);
    return WindowObserverStatusOk;
}

/* Put a stopped key back: QueueFull goes to the tail, any other stop to the head (first-seen order). */
static void put_back(window_observer_t *observer, const dirty_entry_t *entry, fold_t fold) {
    if (fold == FoldQueueFull) {
        dirty_push_back_retry(observer, entry);
    } else {
        dirty_push_front(observer, entry);
    }
}

/* Process one popped key; Ok = continue, anything else = stop the pump with that status. */
static window_observer_status_t process_key(window_observer_t *observer, interop_window_graph_t *graph,
                                            uint64_t epoch, dirty_entry_t *entry) {
    fold_t fold = FoldContinue;
    if (entry->destroy_stamp != 0u) {
        (void)apply_destroy(observer, graph, epoch, entry->window, entry->destroy_stamp, entry->destroy_event_ms,
                            &fold);
        if (fold == FoldQueueFull || fold == FoldFatal) {
            put_back(observer, entry, fold);
            return stop_status(fold);
        }
        if (fold == FoldDrop) {
            return WindowObserverStatusOk; /* key dropped */
        }
        entry->destroy_stamp = 0u;
        entry->destroy_event_ms = 0u;
        entry->kinds &= ~(uint32_t)DirtyKindDestroy;
    }

    uint64_t stamp = 0;
    if (!stamp_next(observer, &stamp)) {
        dirty_push_front(observer, entry);
        return WindowObserverStatusStampExhausted;
    }
    const uint32_t captured_ms = observer->api.tick_ms(observer->api_context);
    interop_window_facts_t facts;
    const window_observer_query_result_t result = query_one(observer, entry->window, &facts);

    if (result == WindowObserverQueryExists) {
        overlay_facts(observer, &facts, stamp, captured_ms);
        interop_window_observation_t observation;
        memset(&observation, 0, sizeof(observation));
        const interop_window_graph_status_t status = interop_window_graph_observe(graph, epoch, &facts, &observation);
        fold = fold_status(observer, status);
        if (fold == FoldContinue) {
            count_uac_hidden(observer, &observation);
            if (observation.disposition == InteropWindowDispositionSuspended) {
                dirty_push_front(observer, entry);
                desktop_mark_inactive(observer);
                return WindowObserverStatusDesktopInactive;
            }
            return WindowObserverStatusOk;
        }
    } else if (result == WindowObserverQueryGone) {
        (void)apply_destroy(observer, graph, epoch, entry->window, stamp, captured_ms, &fold);
        if (fold == FoldContinue) {
            return WindowObserverStatusOk;
        }
    } else {
        if (entry->retries < WindowObserverQueryRetries) {
            entry->retries += 1u;
            dirty_push_back_retry(observer, entry);
        } else {
            bump(&observer->stats.query_retries_exhausted);
        }
        return WindowObserverStatusOk;
    }

    if (fold == FoldDrop) {
        return WindowObserverStatusOk;
    }
    put_back(observer, entry, fold);
    return stop_status(fold);
}

/* Forward declaration: full refresh used by the pump's overflow recovery (defined below). */
static window_observer_status_t refresh_run(window_observer_t *observer, interop_window_graph_t *graph,
                                            uint64_t epoch, int desktop_known_default);

/* Body of pump after argument, thread and state checks. */
static window_observer_status_t pump_run(window_observer_t *observer, interop_window_graph_t *graph,
                                         uint64_t epoch, uint32_t budget) {
    if (!desktop_check(observer)) {
        return WindowObserverStatusDesktopInactive;
    }
    if (observer->overflow != 0u) {
        uint32_t retired = 0;
        const fold_t fold = fold_status(observer, interop_window_graph_invalidate_all(graph, epoch, &retired));
        if (fold != FoldContinue) {
            return stop_status(fold);
        }
        bump(&observer->stats.invalidations);
        observer->dirty_count = 0u;
        observer->overflow = 0u;
        observer->refresh_requested = 1u;
        return refresh_run(observer, graph, epoch, 1);
    }
    const uint32_t available = observer->dirty_count;
    const uint32_t limit = budget < available ? budget : available;
    for (uint32_t i = 0; i < limit; i++) {
        dirty_entry_t entry;
        if (!dirty_pop_front(observer, &entry)) {
            break;
        }
        bump(&observer->stats.keys_processed);
        const window_observer_status_t status = process_key(observer, graph, epoch, &entry);
        if (status != WindowObserverStatusOk) {
            return status;
        }
    }
    return observer->dirty_count == 0u ? WindowObserverStatusOk : WindowObserverStatusMore;
}

/** @copydoc window_observer_pump */
window_observer_status_t window_observer_pump(window_observer_t *observer, interop_window_graph_t *graph,
                                              uint64_t session_epoch, uint32_t budget,
                                              window_observer_stats_t *stats) {
    if (observer == NULL || graph == NULL || budget == 0u) {
        return WindowObserverStatusInvalidArgument;
    }
    if (!on_owner_thread(observer)) {
        return WindowObserverStatusWrongThread;
    }
    if (observer->state != WindowObserverStateStarted) {
        snapshot_stats(observer, stats);
        return WindowObserverStatusBadState;
    }
    const window_observer_status_t status = pump_run(observer, graph, session_epoch, budget);
    snapshot_stats(observer, stats);
    return status;
}

/* ------------------------------------------------------------------------------------------ */
/* Refresh                                                                                    */
/* ------------------------------------------------------------------------------------------ */

/* Open a graph refresh, aborting and reopening one that a failed earlier call left open. */
static window_observer_status_t refresh_open(window_observer_t *observer, interop_window_graph_t *graph,
                                             uint64_t epoch) {
    bump(&observer->stats.refreshes_started);
    interop_window_graph_status_t status = interop_window_graph_refresh_begin(graph, epoch);
    if (status == InteropWindowGraphStatusRefreshActive) {
        observer->stats.last_graph_status = (uint32_t)status;
        const interop_window_graph_status_t abort_status = interop_window_graph_refresh_abort(graph, epoch);
        if (abort_status != InteropWindowGraphStatusOk) {
            return stop_status(fold_status(observer, abort_status));
        }
        bump(&observer->stats.refreshes_aborted);
        bump(&observer->stats.refreshes_started);
        status = interop_window_graph_refresh_begin(graph, epoch);
    }
    const fold_t fold = fold_status(observer, status);
    return fold == FoldContinue ? WindowObserverStatusOk : stop_status(fold);
}

/* Query and offer every enumerated window, then recheck the desktop; the refresh is open on entry. */
static window_observer_status_t refresh_offer_all(window_observer_t *observer, interop_window_graph_t *graph,
                                                  uint64_t epoch, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const uint64_t window = observer->enum_buffer[i];
        if (window == 0u) {
            continue;
        }
        uint64_t stamp = 0;
        if (!stamp_next(observer, &stamp)) {
            refresh_abort_quiet(observer, graph, epoch);
            return WindowObserverStatusStampExhausted;
        }
        const uint32_t captured_ms = observer->api.tick_ms(observer->api_context);
        interop_window_facts_t facts;
        const window_observer_query_result_t result = query_one(observer, window, &facts);
        if (result == WindowObserverQueryGone) {
            continue; /* stays unseen; the commit retires it */
        }
        if (result != WindowObserverQueryExists) {
            refresh_abort_quiet(observer, graph, epoch);
            return WindowObserverStatusQueryFailed;
        }
        overlay_facts(observer, &facts, stamp, captured_ms);
        interop_window_observation_t observation;
        memset(&observation, 0, sizeof(observation));
        const interop_window_graph_status_t status =
            interop_window_graph_refresh_offer(graph, epoch, &facts, &observation);
        const fold_t fold = fold_status(observer, status);
        if (fold == FoldDrop) {
            continue;
        }
        if (fold != FoldContinue) {
            refresh_abort_quiet(observer, graph, epoch);
            return stop_status(fold);
        }
        count_uac_hidden(observer, &observation);
        if (observation.disposition == InteropWindowDispositionSuspended) {
            refresh_abort_quiet(observer, graph, epoch);
            desktop_mark_inactive(observer);
            return WindowObserverStatusDesktopInactive;
        }
    }
    if (!desktop_check(observer)) {
        refresh_abort_quiet(observer, graph, epoch);
        return WindowObserverStatusDesktopInactive;
    }
    return WindowObserverStatusOk;
}

/* Body of refresh after argument, thread and state checks. */
static window_observer_status_t refresh_run(window_observer_t *observer, interop_window_graph_t *graph,
                                            uint64_t epoch, int desktop_known_default) {
    observer->last_refresh_ms = observer->api.monotonic_ms(observer->api_context);
    observer->have_refresh_ms = 1u;
    if (!desktop_known_default && !desktop_check(observer)) {
        return WindowObserverStatusDesktopInactive;
    }
    uint32_t count = 0;
    const window_observer_enum_result_t enumerated = observer->api.enum_top_level_windows(
        observer->api_context, observer->enum_buffer, WindowObserverMaxEnumerated, &count);
    if (enumerated != WindowObserverEnumOk || count > WindowObserverMaxEnumerated) {
        return WindowObserverStatusEnumFailed;
    }
    window_observer_status_t status = refresh_open(observer, graph, epoch);
    if (status != WindowObserverStatusOk) {
        return status;
    }
    status = refresh_offer_all(observer, graph, epoch, count);
    if (status != WindowObserverStatusOk) {
        return status;
    }
    uint32_t retired = 0;
    const interop_window_graph_status_t commit = interop_window_graph_refresh_commit(graph, epoch, &retired);
    const fold_t fold = fold_status(observer, commit);
    if (fold == FoldContinue) {
        bump(&observer->stats.refreshes_committed);
        observer->refresh_requested = 0u;
        return WindowObserverStatusOk;
    }
    if (fold != FoldQueueFull) {
        refresh_abort_quiet(observer, graph, epoch); /* QueueFull alone leaves the refresh open for a retry */
    }
    return stop_status(fold);
}

/** @copydoc window_observer_refresh */
window_observer_status_t window_observer_refresh(window_observer_t *observer, interop_window_graph_t *graph,
                                                 uint64_t session_epoch, window_observer_stats_t *stats) {
    if (observer == NULL || graph == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    if (!on_owner_thread(observer)) {
        return WindowObserverStatusWrongThread;
    }
    if (observer->state != WindowObserverStateStarted) {
        snapshot_stats(observer, stats);
        return WindowObserverStatusBadState;
    }
    const window_observer_status_t status = refresh_run(observer, graph, session_epoch, 0);
    snapshot_stats(observer, stats);
    return status;
}

/* ------------------------------------------------------------------------------------------ */
/* Cadence                                                                                    */
/* ------------------------------------------------------------------------------------------ */

/* Return nonzero when the 250 ms cadence has elapsed (or no attempt was made yet). */
static int cadence_elapsed(const window_observer_t *observer, uint64_t now_ms) {
    if (observer->have_refresh_ms == 0u) {
        return 1;
    }
    const uint64_t elapsed = now_ms >= observer->last_refresh_ms ? now_ms - observer->last_refresh_ms : 0u;
    return elapsed >= InteropWindowGraphRefreshIntervalMs;
}

/** @copydoc window_observer_refresh_due */
int window_observer_refresh_due(const window_observer_t *observer, uint64_t now_ms) {
    if (observer == NULL || observer->state != WindowObserverStateStarted || !on_owner_thread(observer)) {
        return 0;
    }
    if (observer->refresh_requested != 0u && observer->desktop_active != 0u) {
        return 1;
    }
    return cadence_elapsed(observer, now_ms);
}

/** @copydoc window_observer_next_timeout_ms */
uint32_t window_observer_next_timeout_ms(const window_observer_t *observer, uint64_t now_ms) {
    if (observer == NULL || observer->state != WindowObserverStateStarted || !on_owner_thread(observer)) {
        return WindowObserverWaitForever;
    }
    if (window_observer_refresh_due(observer, now_ms)) {
        return 0u;
    }
    if ((observer->dirty_count != 0u || observer->overflow != 0u) && observer->desktop_blocked == 0u) {
        return 0u;
    }
    const uint64_t elapsed = now_ms >= observer->last_refresh_ms ? now_ms - observer->last_refresh_ms : 0u;
    const uint64_t remaining = InteropWindowGraphRefreshIntervalMs > elapsed ? InteropWindowGraphRefreshIntervalMs - elapsed : 1u;
    return (uint32_t)(remaining < 1u ? 1u : remaining);
}

/* ------------------------------------------------------------------------------------------ */
/* Native Win32 section                                                                       */
/* ------------------------------------------------------------------------------------------ */

#ifdef _WIN32

/** Signature of GetDpiForWindow, resolved at run time because it exists only on Windows 10 1607+. */
typedef UINT(WINAPI *get_dpi_for_window_fn_t)(HWND);

/** Resolved GetDpiForWindow; NULL until native_dpi_available() succeeds. */
static _Atomic(get_dpi_for_window_fn_t) g_get_dpi_for_window = NULL;

/** Bytes of UTF-8 a title may keep (equals InteropWindowGraphMaxTitleBytes). */
#define NativeTitleBytes 1024
/** WCHAR capacity of the title read buffer including the terminator. */
#define NativeTitleWchars 1025
/** WCHAR capacity of the class-name buffer including the terminator. */
#define NativeClassWchars 258
/** WCHAR capacity of the full process image path buffer. */
#define NativeImagePathWchars 32768
/** Capacity of the desktop-name buffer in WCHARs. */
#define NativeDesktopNameWchars 64

/* Resolve GetDpiForWindow once; returns NULL when the OS lacks it. */
static get_dpi_for_window_fn_t native_resolve_dpi(void) {
    get_dpi_for_window_fn_t resolved = atomic_load(&g_get_dpi_for_window);
    if (resolved != NULL) {
        return resolved;
    }
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == NULL) {
        return NULL;
    }
    FARPROC raw = GetProcAddress(user32, "GetDpiForWindow");
    if (raw == NULL) {
        return NULL;
    }
    memcpy(&resolved, &raw, sizeof(resolved));
    atomic_store(&g_get_dpi_for_window, resolved);
    return resolved;
}

static int native_dpi_available(void) {
    return native_resolve_dpi() != NULL;
}

/* HWND to the zero-extended 32-bit significant value used by the graph. */
static uint64_t hwnd_to_u64(HWND window) {
    return (uint64_t)((uintptr_t)window & 0xFFFFFFFFu);
}

/* Value back to an HWND (sign-extended like the OS does); 0 when the value is not a 32-bit handle. */
static HWND u64_to_hwnd(uint64_t window) {
    if (window == 0u || window > 0xFFFFFFFFu) {
        return NULL;
    }
    return (HWND)(LONG_PTR)(LONG)(uint32_t)window;
}

/* WinEvent procedure: forwards the event to the portable hint entry point and does nothing else. */
static VOID CALLBACK win_event_proc(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object_id, LONG child_id,
                                    DWORD event_thread, DWORD event_time_ms) {
    (void)hook;
    window_observer_note_hint((uint32_t)event, hwnd_to_u64(window), (int32_t)object_id, (int32_t)child_id,
                              (uint32_t)event_thread, (uint32_t)event_time_ms);
}

/* api.install_hook: one out-of-context hook for all processes and threads of this desktop. */
static int native_install_hook(void *context, uint32_t event_min, uint32_t event_max, uintptr_t *hook) {
    (void)context;
    HWINEVENTHOOK installed =
        SetWinEventHook((DWORD)event_min, (DWORD)event_max, NULL, win_event_proc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (installed == NULL) {
        return 1;
    }
    *hook = (uintptr_t)installed;
    return 0;
}

/* api.remove_hook: UnhookWinEvent on the installing thread. */
static int native_remove_hook(void *context, uintptr_t hook) {
    (void)context;
    return UnhookWinEvent((HWINEVENTHOOK)hook) ? 0 : 1;
}

/* api.query_input_desktop: Default only when the input desktop is positively named Default. */
static window_observer_desktop_t native_query_desktop(void *context) {
    (void)context;
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (desktop == NULL) {
        return WindowObserverDesktopDenied;
    }
    WCHAR name[NativeDesktopNameWchars];
    DWORD needed = 0;
    memset(name, 0, sizeof(name));
    const BOOL named = GetUserObjectInformationW(desktop, UOI_NAME, name, (DWORD)sizeof(name), &needed);
    CloseDesktop(desktop);
    if (!named) {
        return WindowObserverDesktopDenied;
    }
    name[NativeDesktopNameWchars - 1] = L'\0';
    return _wcsicmp(name, L"Default") == 0 ? WindowObserverDesktopDefault : WindowObserverDesktopOther;
}

/* api.current_thread_id. */
static uint32_t native_thread_id(void *context) {
    (void)context;
    return (uint32_t)GetCurrentThreadId();
}

/* api.monotonic_ms. */
static uint64_t native_monotonic_ms(void *context) {
    (void)context;
    return (uint64_t)GetTickCount64();
}

/* api.tick_ms: same base as the WinEvent event-time field. */
static uint32_t native_tick_ms(void *context) {
    (void)context;
    return (uint32_t)GetTickCount();
}

/* State shared with the EnumWindows callback. */
typedef struct enum_state_t {
    uint64_t *windows;
    uint32_t capacity;
    uint32_t count;
    int truncated;
} enum_state_t;

/* EnumWindows callback: append in z-order; stop and flag truncation when full. */
static BOOL CALLBACK enum_callback(HWND window, LPARAM param) {
    enum_state_t *state = (enum_state_t *)param;
    if (window == NULL) {
        return TRUE;
    }
    if (state->count >= state->capacity) {
        state->truncated = 1;
        return FALSE;
    }
    state->windows[state->count] = hwnd_to_u64(window);
    state->count += 1u;
    return TRUE;
}

/* api.enum_top_level_windows. */
static window_observer_enum_result_t native_enum(void *context, uint64_t *windows, uint32_t capacity,
                                                 uint32_t *count) {
    (void)context;
    enum_state_t state;
    state.windows = windows;
    state.capacity = capacity;
    state.count = 0u;
    state.truncated = 0;
    const BOOL ok = EnumWindows(enum_callback, (LPARAM)&state);
    *count = state.count;
    if (state.truncated) {
        return WindowObserverEnumTruncated;
    }
    return ok ? WindowObserverEnumOk : WindowObserverEnumFailed;
}

/* Decide Gone versus Failed for a handle that IsWindow rejected; Gone only on an affirmative OS answer. */
static window_observer_query_result_t native_gone_probe(HWND window) {
    SetLastError(0);
    (void)GetWindowLongPtrW(window, GWL_STYLE);
    if (GetLastError() != ERROR_INVALID_WINDOW_HANDLE || IsWindow(window)) {
        return WindowObserverQueryFailed;
    }
    if (native_query_desktop(NULL) != WindowObserverDesktopDefault) {
        return WindowObserverQueryFailed; /* a handle from another desktop looks invalid too */
    }
    return WindowObserverQueryGone;
}

/* A read failed: Gone if the window vanished affirmatively, otherwise Failed. */
static window_observer_query_result_t native_read_failed(HWND window) {
    if (IsWindow(window)) {
        return WindowObserverQueryFailed;
    }
    return native_gone_probe(window);
}

/* Convert UTF-16 to UTF-8; with allow_cut the result is cut at a code-point boundary to capacity. */
static uint32_t utf8_from_wide(const WCHAR *source, int source_length, DWORD flags, int allow_cut, uint8_t *dest,
                               uint32_t capacity, int *was_cut) {
    *was_cut = 0;
    if (source_length <= 0) {
        return 0u;
    }
    int written = WideCharToMultiByte(CP_UTF8, flags, source, source_length, (LPSTR)dest, (int)capacity, NULL, NULL);
    if (written > 0) {
        return (uint32_t)written;
    }
    if (!allow_cut || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return 0u;
    }
    uint8_t scratch[4096];
    const int total = WideCharToMultiByte(CP_UTF8, flags, source, source_length, (LPSTR)scratch, (int)sizeof(scratch),
                                          NULL, NULL);
    if (total <= 0) {
        return 0u;
    }
    uint32_t length = capacity;
    if ((uint32_t)total <= capacity) {
        length = (uint32_t)total;
    } else {
        while (length > 0u && (scratch[length] & 0xC0u) == 0x80u) {
            length -= 1u;
        }
    }
    memcpy(dest, scratch, length);
    *was_cut = 1;
    return length;
}

/* Fill class name and the ClassTruncated flag; returns 0 when the class cannot be read. */
static int native_fill_class(HWND window, interop_window_facts_t *facts) {
    WCHAR buffer[NativeClassWchars];
    const int length = GetClassNameW(window, buffer, NativeClassWchars);
    if (length <= 0) {
        return 0;
    }
    int cut = 0;
    uint32_t bytes = utf8_from_wide(buffer, length, WC_ERR_INVALID_CHARS, 0, facts->class_name,
                                    InteropWindowGraphMaxClassBytes, &cut);
    if (bytes == 0u) {
        bytes = utf8_from_wide(buffer, length, 0, 1, facts->class_name, InteropWindowGraphMaxClassBytes, &cut);
        if (bytes == 0u) {
            return 0;
        }
        facts->flags |= (uint32_t)InteropWindowFactClassTruncated;
    }
    if (length >= NativeClassWchars - 1) {
        facts->flags |= (uint32_t)InteropWindowFactClassTruncated;
    }
    facts->class_length = bytes;
    return 1;
}

/* Fill the title; an unreadable title becomes empty, only an invalid handle reports failure. */
static int native_fill_title(HWND window, interop_window_facts_t *facts) {
    WCHAR buffer[NativeTitleWchars];
    SetLastError(0);
    const int length = GetWindowTextW(window, buffer, NativeTitleWchars);
    if (length <= 0) {
        return GetLastError() != ERROR_INVALID_WINDOW_HANDLE;
    }
    int cut = 0;
    facts->title_length = utf8_from_wide(buffer, length, 0, 1, facts->title, NativeTitleBytes, &cut);
    return 1;
}

/* Fill image basename and process start time from a limited-rights process handle (best effort). */
static int native_fill_process(DWORD process_id, interop_window_facts_t *facts) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == NULL) {
        facts->flags |= (uint32_t)InteropWindowFactImageUnknown;
        return 1;
    }
    WCHAR *path = malloc((size_t)NativeImagePathWchars * sizeof(WCHAR));
    if (path == NULL) {
        CloseHandle(process);
        return 0; /* Failed: a missing image would silently disable the UAC rules */
    }
    DWORD path_length = NativeImagePathWchars;
    uint32_t image_bytes = 0u;
    if (QueryFullProcessImageNameW(process, 0, path, &path_length) && path_length > 0u) {
        DWORD base = 0;
        for (DWORD i = 0; i < path_length; i++) {
            if (path[i] == L'\\' || path[i] == L'/') {
                base = i + 1u;
            }
        }
        int cut = 0;
        image_bytes = utf8_from_wide(path + base, (int)(path_length - base), WC_ERR_INVALID_CHARS, 0,
                                     facts->image_name, InteropWindowGraphMaxImageBytes, &cut);
    }
    free(path);
    path = NULL;
    facts->image_length = image_bytes;
    if (image_bytes == 0u) {
        facts->flags |= (uint32_t)InteropWindowFactImageUnknown;
    }
    FILETIME created;
    FILETIME exited;
    FILETIME kernel;
    FILETIME user;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        facts->process_start_time = ((uint64_t)created.dwHighDateTime << 32) | (uint64_t)created.dwLowDateTime;
    }
    CloseHandle(process);
    return 1;
}

/* Fill rect, honoring the iconic rule; returns 0 on failure. Reports the screen rect for fullscreen tests. */
static int native_fill_rect(HWND window, int iconic, interop_window_facts_t *facts, RECT *screen_rect) {
    RECT rect;
    memset(&rect, 0, sizeof(rect));
    if (iconic) {
        WINDOWPLACEMENT placement;
        memset(&placement, 0, sizeof(placement));
        placement.length = (UINT)sizeof(placement);
        if (!GetWindowPlacement(window, &placement)) {
            return 0;
        }
        rect = placement.rcNormalPosition;
    } else if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, (DWORD)sizeof(rect)))) {
        if (!GetWindowRect(window, &rect)) {
            return 0;
        }
    }
    if (rect.right < rect.left || rect.bottom < rect.top) {
        return 0;
    }
    facts->x = (int32_t)rect.left;
    facts->y = (int32_t)rect.top;
    facts->width = (uint32_t)(rect.right - rect.left);
    facts->height = (uint32_t)(rect.bottom - rect.top);
    *screen_rect = rect;
    return 1;
}

/* Compute the Fullscreen flag: not iconic, rect covers its monitor, title bar absent. */
static int native_is_fullscreen(HWND window, int iconic, uint32_t style, const RECT *rect) {
    if (iconic || (style & WS_CAPTION) == WS_CAPTION) {
        return 0;
    }
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    MONITORINFO info;
    memset(&info, 0, sizeof(info));
    info.cbSize = (DWORD)sizeof(info);
    if (monitor == NULL || !GetMonitorInfoW(monitor, &info)) {
        return 0;
    }
    return rect->left <= info.rcMonitor.left && rect->top <= info.rcMonitor.top &&
           rect->right >= info.rcMonitor.right && rect->bottom >= info.rcMonitor.bottom;
}

/* Read the handle graph (root, owner, owner root); returns 0 on any ambiguity. */
static int native_fill_relations(HWND window, interop_window_facts_t *facts) {
    SetLastError(0);
    HWND root = GetAncestor(window, GA_ROOT);
    if (root == NULL) {
        return 0;
    }
    SetLastError(0);
    HWND owner = GetWindow(window, GW_OWNER);
    if (owner == NULL && GetLastError() != 0) {
        return 0;
    }
    HWND owner_root = NULL;
    if (owner != NULL) {
        SetLastError(0);
        owner_root = GetAncestor(owner, GA_ROOT);
        if (owner_root == NULL) {
            return 0;
        }
    }
    facts->root = hwnd_to_u64(root);
    facts->owner = hwnd_to_u64(owner);
    facts->owner_root = hwnd_to_u64(owner_root);
    return 1;
}

/* Read raw style bits; returns 0 when the call is ambiguous. */
static int native_fill_styles(HWND window, interop_window_facts_t *facts) {
    SetLastError(0);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (style == 0 && GetLastError() != 0) {
        return 0;
    }
    SetLastError(0);
    const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (ex_style == 0 && GetLastError() != 0) {
        return 0;
    }
    facts->style = (uint32_t)((ULONG_PTR)style & 0xFFFFFFFFu);
    facts->ex_style = (uint32_t)((ULONG_PTR)ex_style & 0xFFFFFFFFu);
    return 1;
}

/* api.query_facts: read every fact of one window without sending it a message. */
static window_observer_query_result_t native_query_facts(void *context, uint64_t window,
                                                         interop_window_facts_t *facts) {
    (void)context;
    HWND handle = u64_to_hwnd(window);
    if (handle == NULL) {
        return WindowObserverQueryFailed;
    }
    if (!IsWindow(handle)) {
        return native_gone_probe(handle);
    }
    get_dpi_for_window_fn_t get_dpi = native_resolve_dpi();
    if (get_dpi == NULL) {
        return WindowObserverQueryFailed;
    }
    facts->window = window;
    if (!native_fill_relations(handle, facts) || !native_fill_styles(handle, facts)) {
        return native_read_failed(handle);
    }
    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(handle, &process_id);
    if (thread_id == 0u || process_id == 0u) {
        return native_read_failed(handle);
    }
    facts->process_id = (uint32_t)process_id;
    facts->thread_id = (uint32_t)thread_id;

    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(handle, DWMWA_CLOAKED, &cloaked, (DWORD)sizeof(cloaked)))) {
        return native_read_failed(handle);
    }
    const int iconic = IsIconic(handle) ? 1 : 0;
    uint32_t flags = 0u;
    if (IsWindowVisible(handle) && cloaked == 0u) {
        flags |= (uint32_t)InteropWindowFactVisible;
    }
    if (IsWindowEnabled(handle)) {
        flags |= (uint32_t)InteropWindowFactEnabled;
    }
    if (iconic) {
        flags |= (uint32_t)InteropWindowFactIconic;
    }
    if (IsZoomed(handle)) {
        flags |= (uint32_t)InteropWindowFactZoomed;
    }
    if ((facts->ex_style & WS_EX_LAYERED) != 0u) {
        flags |= (uint32_t)InteropWindowFactLayered;
        BYTE alpha = 0;
        DWORD layered_flags = 0;
        if (GetLayeredWindowAttributes(handle, NULL, &alpha, &layered_flags) && (layered_flags & LWA_ALPHA) != 0u) {
            flags |= (uint32_t)InteropWindowFactLayeredAlphaKnown;
            facts->alpha = (uint8_t)alpha;
        }
    }
    facts->flags = flags;

    RECT rect;
    if (!native_fill_rect(handle, iconic, facts, &rect)) {
        return native_read_failed(handle);
    }
    if (native_is_fullscreen(handle, iconic, facts->style, &rect)) {
        facts->flags |= (uint32_t)InteropWindowFactFullscreen;
    }
    const UINT dpi = get_dpi(handle);
    if (dpi == 0u) {
        return native_read_failed(handle);
    }
    facts->dpi = (uint32_t)dpi;
    if (!native_fill_class(handle, facts) || !native_fill_title(handle, facts) ||
        !native_fill_process(process_id, facts)) {
        return native_read_failed(handle);
    }
    if (!IsWindow(handle)) {
        return native_gone_probe(handle); /* vanished mid-read: the partial facts are discarded */
    }
    return WindowObserverQueryExists;
}

/** Immutable native api table; context is NULL. */
static const window_observer_api_t NativeApiTable = {
    native_install_hook,
    native_remove_hook,
    native_query_facts,
    native_enum,
    native_query_desktop,
    native_thread_id,
    native_monotonic_ms,
    native_tick_ms,
};

/** @copydoc window_observer_native_api */
const window_observer_api_t *window_observer_native_api(void) {
    return &NativeApiTable;
}

#endif
