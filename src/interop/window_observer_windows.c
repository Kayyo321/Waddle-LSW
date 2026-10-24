/**
 * @file window_observer_windows.c
 * @brief Win32 observation adapter feeding the session window graph (TODO #10).
 *
 * The file has two layers. The portable core (lifecycle, busy flag, dirty-hint table, pump,
 * refresh, cadence and latch, counters) compiles on every platform and reaches the operating
 * system only through the injected window_observer_api_t table. The native section, compiled only
 * under _WIN32, provides that table for the real Win32 desktop (three WinEvent hooks, window
 * queries, enumeration, the Unsupported check of create). The contract of every public function
 * lives in window_observer_windows.h and is authoritative; this file only repeats what an
 * implementer needs to see and records the decisions taken where the header is silent.
 *
 * Contract summary (see the header for the full text):
 *  - Busy flag. Every entry point except window_observer_note_hint refuses with Reentrant (0 for
 *    refresh_due, WaitForever for next_timeout_ms) and touches nothing while another entry point
 *    of the observer is active. Mutating entry points hold the flag for their whole duration; the
 *    read-only ones (status, refresh_due, next_timeout_ms) only test it. note_hint never reads or
 *    writes it.
 *  - Hooks. start() installs three out-of-context hooks in this order: the object range
 *    [ObjectCreate, ObjectParentChange], the system range [SystemMinimizeStart, SystemMinimizeEnd]
 *    and the cloak range [ObjectCloaked, ObjectUncloaked]. Rollback and stop() remove them in
 *    reverse order. The hint filter accepts exactly those three ranges.
 *  - Dirty table. While pump holds a popped key, one slot stays reserved for its put-back
 *    (popped_in_flight), so a put-back never forces an overflow.
 *  - Facts. The core overlays stamp, captured_ms, AgentOwned, AgentPreedit and OnInputDesktop,
 *    forces title_length 0 on AgentOwned snapshots and computes OwnerHidden itself with a second
 *    api->query_facts on owner_root plus interop_window_graph_classify (config uac_mode).
 *  - Refresh. enumerate; begin; query and offer every window (a Failed query is kept with
 *    refresh_keep, a Gone one stays unseen); list the unseen Active windows and requery each
 *    individually (Exists offers, Gone notes the destruction, Failed keeps); commit. A refresh with
 *    failed queries commits and returns Ok.
 *  - Cadence. last_attempt_ms is stamped before the desktop check; latch_ms is stamped whenever the
 *    full-refresh latch is set. refresh_due is true iff Started and (no attempt yet, or 250 ms
 *    elapsed, or a latch event not yet consumed by an attempt, tracked by latch_unconsumed), so a frozen clock with a persistently
 *    failing refresh runs at most one refresh per latch event.
 *  - Overflow. A full dirty table of ordinary hints latches overflow; protected Destroy evidence is
 *    copied to its bounded mailbox and applied before recovery. Pump then discards only ordinary
 *    dirty hints and runs one full refresh (never invalidate_all). A Truncated enumeration is
 *    applied but aborted instead of committed.
 *  - Native table. create returns Unsupported when GetDpiForWindow is missing or the calling
 *    thread is not DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2. The native query clamps width and
 *    height to InteropWindowGraphMaxExtent, never reads the title of a window of the current
 *    process and falls back to a Toolhelp32 process snapshot for the image basename.
 *
 * Decisions where the header is silent:
 *  - Hints and dirty entries: hints_received counts every accepted hint (new entry or merge),
 *    hints_coalesced the merged subset. A hint that cannot obtain a stamp (clock exhausted)
 *    latches overflow. During Starting, Started, and Stopping callbacks, matching Destroy events
 *    for tracked keys or the reserved candidate enter the protected mailbox. Other hints are
 *    ignored outside Started; successful start latches a full refresh.
 *  - Destroy information already applied to the graph is cleared from a key before the key is
 *    put back, so a retry never repeats an applied destroy. The put-back helpers keep a defensive
 *    overflow latch for a full table; with the popped_in_flight reservation it cannot be reached.
 *  - graph_errors counts every graph status that is neither Ok nor None (dropped, stopping or
 *    Reentrant). refresh_keep answering None (nothing to mark) is benign and counts as Ok.
 *  - A drop-class status answering refresh_begin, refresh_unseen, refresh_commit or refresh_abort
 *    (not meaningful for those calls) is handled as a call-level status: GraphError.
 *  - Desktop gauges: desktop_blocked (latest check returned non-Default) only suppresses the
 *    hint-backlog term of next_timeout_ms so that the loop does not spin while the desktop is
 *    inactive. A refusal (desktop_refuse) sets it, the next Default check clears it, start() clears it.
 *  - A graph Suspended disposition (defensive; the core always sets OnInputDesktop) puts the pump
 *    key back at the head and returns DesktopInactive; in refresh it aborts the refresh and
 *    returns DesktopInactive. In both cases desktop_refuse stamps the attempt and the latch.
 *  - The overflow recovery in pump reads the clock once and uses that value for both
 *    latch_ms and the attempt stamp, so the recovery refresh consumes its own latch event.
 *  - The unseen list lives on the stack (InteropMaxWindows entries); a count above the capacity is
 *    clamped (the graph never exceeds it). Windows listed by refresh_unseen are requeried even when
 *    they were in the enumeration (a Gone answer there skipped them in the first pass).
 *  - A query whose api answer is outside the three result values, or whose facts name another
 *    window, counts as Failed.
 *  - The Toolhelp snapshot of the native table is per outermost pump or refresh call, scoped by
 *    two private functions (native_call_begin, native_call_end) that the core calls on every
 *    platform; they are no-ops off Windows. If QueryFullProcessImageName fails on an opened
 *    process, the snapshot fallback still supplies the basename and the real start time is kept.
 *  - The optional stats snapshot of pump and refresh is skipped for every Reentrant result, also one
 *    that comes from the graph (the header lists Reentrant among the statuses without a snapshot);
 *    window_observer_status reads the counters afterwards, including last_graph_status.
 *  - set_preedit_window called while another entry point is active returns Reentrant and the
 *    change is not applied; the caller repeats it after the outer call returns.
 *  - Stamp exhaustion: UINT64_MAX is the sentinel for "no stamp left"; first_stamp must be
 *    nonzero and below it, and stats.next_stamp reads UINT64_MAX once the clock is exhausted.
 *  - Lifetime safety of process-wide hook ownership: a separate lifecycle reservation blocks a
 *    second start while the active callback slot is cleared and hooks are removed. note_hint
 *    registers itself in a process-wide in-flight counter before loading that slot, and teardown
 *    waits for the counter to reach zero before it may free the observer.
 */
#include "window_observer_windows.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sched.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#endif

/** @brief Number of out-of-context hooks the observer installs (object, system, cloak). */
#define ObserverHookCount 3u

/** @brief Bits recorded per dirty entry; only DirtyKindDestroy carries authority. */
typedef enum dirty_kind_t {
    DirtyKindCreate = 1u << 0,
    DirtyKindDestroy = 1u << 1,
    DirtyKindShow = 1u << 2,
    DirtyKindHide = 1u << 3,
    DirtyKindObjectOther = 1u << 4,
    DirtyKindMinimizeStart = 1u << 5,
    DirtyKindMinimizeEnd = 1u << 6,
    DirtyKindCloak = 1u << 7
} dirty_kind_t;

/** @brief How a graph status is handled by pump and refresh. */
typedef enum fold_t {
    FoldContinue = 0, /**< Ok or None: carry on. */
    FoldDrop = 1, /**< Counted; the key or offer is dropped and processing continues. */
    FoldQueueFull = 2, /**< Counted; processing stops with GraphQueueFull. */
    FoldReentrant = 3, /**< Counted; processing stops with Reentrant. */
    FoldFatal = 4 /**< Counted; processing stops with GraphError. */
} fold_t;

/** @brief Inclusive WinEvent id range of one hook. */
typedef struct hook_range_t {
    uint32_t event_min; /**< Lowest event id delivered. */
    uint32_t event_max; /**< Highest event id delivered (inclusive). */
} hook_range_t;

/** The three hooks in installation order; removal runs in reverse order. */
static const hook_range_t HookRanges[ObserverHookCount] = {
    {WindowObserverEventObjectCreate, WindowObserverEventObjectParentChange},
    {WindowObserverEventSystemMinimizeStart, WindowObserverEventSystemMinimizeEnd},
    {WindowObserverEventObjectCloaked, WindowObserverEventObjectUncloaked},
};

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

/** @brief Protected Destroy event retained until graph acceptance. */
typedef window_observer_destroy_entry_t destroy_entry_t;

/** @brief Private observer state; one calloc'd block owns everything below. */
struct window_observer_t {
    window_observer_api_t api; /**< Copy of the native or injected table. */
    void *api_context; /**< Passed to every api entry. */
    uint32_t owner_thread; /**< Thread id captured at create. */
    uint32_t state; /**< window_observer_state_t. */
    uintptr_t hooks[ObserverHookCount]; /**< Owned hook tokens; 0 = removed or never installed. */
    uint32_t dirty_count; /**< Entries in dirty[0..dirty_count), first-seen order. */
    uint32_t own_process_id; /**< Agent process id. */
    uint32_t uac_mode; /**< interop_window_uac_mode_t used to classify owner windows. */
    interop_window_graph_t *bound_graph; /**< Borrowed immutable graph binding, or NULL. */
    uint64_t bound_epoch; /**< Immutable graph epoch after bind, or zero. */
    uint32_t graph_bound; /**< 1 after successful binding. */
    uint32_t tracking_fault; /**< Sticky fail-closed ownership/evidence fault. */
    uint32_t evidence_incomplete; /**< 1 when a delivered Destroy could not be retained. */
    uint32_t tracked_snapshot_valid; /**< 1 when tracked_windows is the last complete graph snapshot. */
    uint32_t tracked_count; /**< Active/Retiring unique HWND count, at most 64. */
    uint64_t tracked_windows[WindowObserverTrackedCapacity]; /**< Sorted Active/Retiring HWND keys. */
    uint32_t destroy_count; /**< Unique protected Destroy entries. */
    uint32_t mailbox_inflight_index; /**< UINT32_MAX outside graph note_destroyed. */
    destroy_entry_t destroys[WindowObserverDestroyCapacity]; /**< Protected mailbox. */
    uint64_t candidate_window; /**< Current callback-reserved candidate, or zero. */
    uint64_t transition_candidate_window; /**< Temporary second key while changing candidate reservations. */
    uint32_t stop_invalidation_done; /**< Stop graph invalidation committed before hook retry. */
    uint32_t popped_in_flight; /**< 1 while pump holds a popped key (one slot stays reserved for it). */
    uint64_t next_stamp; /**< Next logical stamp; UINT64_MAX = exhausted. */
    uint64_t preedit_window; /**< Preedit HWND or 0. */
    uint32_t overflow; /**< Overflow latch. */
    uint32_t refresh_requested; /**< Full-refresh latch. */
    uint32_t desktop_active; /**< Gauge: latest desktop check returned Default. */
    uint32_t desktop_blocked; /**< Latest desktop check returned non-Default (drives the next_timeout rules). */
    uint32_t busy; /**< Owner-thread busy flag: an entry point is active. */
    uint32_t have_attempt; /**< Nonzero once last_attempt_ms is valid (a refresh attempt happened since start). */
    uint64_t last_attempt_ms; /**< Cadence clock value of the latest refresh attempt. */
    uint64_t latch_ms; /**< Cadence clock value when the refresh latch was last set. */
    uint32_t latch_unconsumed; /**< Nonzero from a latch event until the next refresh attempt starts; makes a refresh due once per event even when the clock did not move. */
    _Atomic uint64_t hints_wrong_thread; /**< Off-thread hint counter (the only cross-thread write). */
    window_observer_stats_t stats; /**< Counters; owner thread only. */
    dirty_entry_t dirty[WindowObserverDirtyCapacity]; /**< Dirty table. */
    uint64_t enum_buffer[WindowObserverMaxEnumerated]; /**< Enumeration scratch for refresh. */
};

/** Process-wide active observer; hook procedures carry no context. */
static _Atomic(window_observer_t *) g_active_observer = NULL;
/** Lifecycle reservation held across hook installation and complete hook teardown. */
static _Atomic(window_observer_t *) g_lifecycle_observer = NULL;
/** Number of note_hint calls currently between slot load and completion. */
static _Atomic uint32_t g_hint_inflight = 0;

#ifdef _WIN32
/* Return nonzero when the native table can run here (GetDpiForWindow resolvable, PMv2 thread). */
static int native_table_runnable(void);
/* Scope of the per-call process snapshot used for image basenames. */
static void native_call_begin(void);
static void native_call_end(void);
#else
#define native_call_begin() ((void)0)
#define native_call_end() ((void)0)
#endif

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

static int dirty_find(const window_observer_t *observer, uint64_t window);

/** @brief Latch a permanent tracking failure without touching graph/native ownership. */
static void latch_tracking_fault(window_observer_t *observer, int evidence_incomplete) {
    observer->tracking_fault = 1u;
    if (evidence_incomplete) observer->evidence_incomplete = 1u;
    observer->state = WindowObserverStateTrackingFault;
}

/** @brief Return whether one HWND is in the last complete tracked snapshot. */
static int tracked_contains(const window_observer_t *observer, uint64_t window) {
    for (uint32_t index = 0; index < observer->tracked_count; ++index) {
        if (observer->tracked_windows[index] == window) return 1;
    }
    return 0;
}

/** @brief Find one protected mailbox key. */
static int destroy_find(const window_observer_t *observer, uint64_t window) {
    for (uint32_t index = 0; index < observer->destroy_count; ++index) {
        if (observer->destroys[index].window == window) return (int)index;
    }
    return -1;
}

/** @brief Remove one mailbox record while preserving FIFO order. */
static void destroy_remove(window_observer_t *observer, uint32_t index) {
    if (index >= observer->destroy_count) return;
    if (index + 1u < observer->destroy_count) {
        memmove(&observer->destroys[index], &observer->destroys[index + 1u],
                (observer->destroy_count - index - 1u) * sizeof(observer->destroys[0]));
    }
    observer->destroy_count -= 1u;
    memset(&observer->destroys[observer->destroy_count], 0, sizeof(observer->destroys[0]));
    if (observer->mailbox_inflight_index != UINT32_MAX) {
        if (observer->mailbox_inflight_index == index) observer->mailbox_inflight_index = UINT32_MAX;
        else if (observer->mailbox_inflight_index > index) observer->mailbox_inflight_index -= 1u;
    }
}

/** @brief Validate and atomically install the frozen Active/Retiring graph snapshot. */
static int refresh_tracked_snapshot_ex(window_observer_t *observer, int fault_on_error,
                                       interop_window_graph_status_t *out_status) {
    uint64_t windows[WindowObserverTrackedCapacity];
    uint32_t count = 0u;
    uint32_t free_count = 0u;
    const interop_window_graph_status_t status = interop_window_graph_list_tracked_windows(
        observer->bound_graph, observer->bound_epoch, windows, WindowObserverTrackedCapacity,
        &count, &free_count);
    if (out_status != NULL) *out_status = status;
    if (status != InteropWindowGraphStatusOk) {
        if (fault_on_error) {
            observer->stats.last_graph_status = (uint32_t)status;
            bump(&observer->stats.graph_errors);
        }
        if (fault_on_error) latch_tracking_fault(observer, 0);
        return 0;
    }
    int valid = count <= WindowObserverTrackedCapacity && free_count <= WindowObserverTrackedCapacity &&
                count + free_count <= WindowObserverTrackedCapacity;
    for (uint32_t index = 0; valid && index < count; ++index) {
        if (windows[index] == 0u || (index != 0u && windows[index - 1u] >= windows[index])) valid = 0;
    }
    if (!valid) {
        /* An Ok result with malformed snapshot facts can never authorize callback admission. */
        latch_tracking_fault(observer, 0);
        return 0;
    }
    memcpy(observer->tracked_windows, windows, (size_t)count * sizeof(windows[0]));
    if (count < WindowObserverTrackedCapacity) {
        memset(observer->tracked_windows + count, 0,
               (WindowObserverTrackedCapacity - count) * sizeof(windows[0]));
    }
    observer->tracked_count = count;
    observer->tracked_snapshot_valid = 1u;
    return 1;
}

static int refresh_tracked_snapshot(window_observer_t *observer, int fault_on_error) {
    return refresh_tracked_snapshot_ex(observer, fault_on_error, NULL);
}

/** @brief Count the unique protected union including up to two candidate keys. */
static uint32_t protected_union_count(const window_observer_t *observer, uint64_t candidate_a,
                                     uint64_t candidate_b) {
    uint32_t count = observer->destroy_count;
    for (uint32_t index = 0; index < observer->tracked_count; ++index) {
        if (destroy_find(observer, observer->tracked_windows[index]) < 0) count += 1u;
    }
    if (candidate_a != 0u && !tracked_contains(observer, candidate_a) &&
        destroy_find(observer, candidate_a) < 0) count += 1u;
    if (candidate_b != 0u && candidate_b != candidate_a &&
        !tracked_contains(observer, candidate_b) && destroy_find(observer, candidate_b) < 0) count += 1u;
    return count;
}

static int candidate_protected(const window_observer_t *observer, uint64_t window) {
    return window != 0u && (observer->candidate_window == window ||
                            observer->transition_candidate_window == window);
}

/** Promote Destroy payloads whose HWND became Active/Retiring after a graph mutation. */
static int promote_tracked_dirty_destroys(window_observer_t *observer, uint64_t *out_windows,
                                         uint32_t out_capacity) {
    int promoted = 0;
    for (uint32_t index = 0u; index < observer->dirty_count;) {
        dirty_entry_t *entry = &observer->dirty[index];
        if (entry->destroy_stamp == 0u ||
            (!tracked_contains(observer, entry->window) && !candidate_protected(observer, entry->window))) {
            index += 1u;
            continue;
        }
        int mailbox_index = destroy_find(observer, entry->window);
        if (mailbox_index < 0) {
            if (observer->destroy_count >= WindowObserverDestroyCapacity) {
                latch_tracking_fault(observer, 1);
                return -1;
            }
            mailbox_index = (int)observer->destroy_count++;
            memset(&observer->destroys[mailbox_index], 0, sizeof(observer->destroys[0]));
            observer->destroys[mailbox_index].window = entry->window;
        }
        destroy_entry_t *protected_entry = &observer->destroys[mailbox_index];
        if (entry->destroy_stamp >= protected_entry->destroy_stamp) {
            protected_entry->destroy_stamp = entry->destroy_stamp;
            protected_entry->event_time_ms = entry->destroy_event_ms;
            protected_entry->reserved_zero = 0u;
        }

        /* Keep the dirty copy until graph acceptance; QueueFull must retain both copies. */
        if ((uint32_t)promoted >= out_capacity) {
            latch_tracking_fault(observer, 1);
            return -1;
        }
        out_windows[promoted] = entry->window;
        promoted += 1;
        index += 1u;
    }
    return promoted;
}

/** @brief Apply one exact mailbox entry, retaining it unless graph acceptance and snapshot succeed. */
static window_observer_status_t apply_protected_destroy(window_observer_t *observer,
                                                        uint32_t index) {
    if (index >= observer->destroy_count || observer->tracking_fault)
        return observer->tracking_fault ? WindowObserverStatusTrackingFault : WindowObserverStatusBadState;
    const destroy_entry_t saved = observer->destroys[index];
    interop_window_observation_t observation;
    memset(&observation, 0, sizeof(observation));
    observer->mailbox_inflight_index = index;
    const interop_window_graph_status_t status = interop_window_graph_note_destroyed(
        observer->bound_graph, observer->bound_epoch, saved.window, saved.destroy_stamp,
        saved.event_time_ms, &observation);
    bump(&observer->stats.destroys_noted);
    if (status != InteropWindowGraphStatusOk) {
        observer->mailbox_inflight_index = UINT32_MAX;
        observer->stats.last_graph_status = (uint32_t)status;
        bump(&observer->stats.graph_errors);
        if (status == InteropWindowGraphStatusQueueFull) return WindowObserverStatusGraphQueueFull;
        if (status == InteropWindowGraphStatusReentrant) return WindowObserverStatusReentrant;
        latch_tracking_fault(observer, 0);
        return WindowObserverStatusTrackingFault;
    }
    if (!refresh_tracked_snapshot(observer, 1)) {
        observer->mailbox_inflight_index = UINT32_MAX;
        return WindowObserverStatusTrackingFault;
    }
    const int live_index = destroy_find(observer, saved.window);
    if (live_index >= 0 && observer->destroys[live_index].destroy_stamp == saved.destroy_stamp)
        destroy_remove(observer, (uint32_t)live_index);
    const int dirty_index = dirty_find(observer, saved.window);
    if (dirty_index >= 0 && observer->dirty[dirty_index].destroy_stamp == saved.destroy_stamp) {
        observer->dirty[dirty_index].destroy_stamp = 0u;
        observer->dirty[dirty_index].destroy_event_ms = 0u;
        observer->dirty[dirty_index].kinds &= ~(uint32_t)DirtyKindDestroy;
    }
    observer->mailbox_inflight_index = UINT32_MAX;
    return WindowObserverStatusOk;
}

/** @brief Establish callback/admission headroom; may apply retained mailbox entries. */
static window_observer_status_t prepare_callbacks_locked(window_observer_t *observer,
                                                        uint64_t candidate, int may_admit,
                                                        int drain_all) {
    if (!observer->graph_bound) return WindowObserverStatusBadState;
    if (observer->tracking_fault) return WindowObserverStatusTrackingFault;
    const uint64_t previous_candidate = observer->candidate_window;
    observer->transition_candidate_window = 0u;
    uint32_t processed = 0u;
    for (;;) {
        interop_window_graph_status_t read_status = InteropWindowGraphStatusOk;
        if (!refresh_tracked_snapshot_ex(observer, 0, &read_status)) {
            observer->transition_candidate_window = 0u;
            if (observer->tracking_fault) return WindowObserverStatusTrackingFault;
            if (read_status == InteropWindowGraphStatusReentrant) return WindowObserverStatusReentrant;
            if (read_status == InteropWindowGraphStatusWrongThread) return WindowObserverStatusWrongThread;
            return WindowObserverStatusGraphError;
        }
        const uint32_t old_union = protected_union_count(observer, previous_candidate, 0u);
        if (old_union > WindowObserverDestroyCapacity) {
            observer->transition_candidate_window = 0u;
            latch_tracking_fault(observer, 1);
            return WindowObserverStatusTrackingFault;
        }
        const uint32_t transition_union = protected_union_count(observer, previous_candidate, candidate);
        if (candidate != previous_candidate && transition_union > WindowObserverDestroyCapacity) {
            /* The new candidate is not published yet; callback work here is protected by old only. */
            if (observer->destroy_count == 0u || processed == WindowObserverDestroyCapacity) {
                observer->transition_candidate_window = 0u;
                return WindowObserverStatusProtectedStatePending;
            }
            const window_observer_status_t applied = apply_protected_destroy(observer, 0u);
            if (applied != WindowObserverStatusOk) {
                observer->transition_candidate_window = 0u;
                return applied;
            }
            processed += 1u;
            continue;
        }
        if (candidate != previous_candidate) observer->transition_candidate_window = candidate;

        uint64_t promoted_windows[WindowObserverTrackedCapacity + 2u];
        const int promoted = promote_tracked_dirty_destroys(observer, promoted_windows,
                                                             WindowObserverTrackedCapacity + 2u);
        if (promoted < 0) {
            observer->transition_candidate_window = 0u;
            return WindowObserverStatusTrackingFault;
        }
        const uint32_t union_count = protected_union_count(observer,
            candidate != previous_candidate ? candidate : previous_candidate, 0u);
        uint32_t free_count;
        /* The snapshot's exact Free count can be less than 64-count when Retiring entries occupy slots. */
        uint64_t snapshot[WindowObserverTrackedCapacity];
        uint32_t snapshot_count = 0u;
        uint32_t snapshot_free = 0u;
        const interop_window_graph_status_t status = interop_window_graph_list_tracked_windows(
            observer->bound_graph, observer->bound_epoch, snapshot, WindowObserverTrackedCapacity,
            &snapshot_count, &snapshot_free);
        if (status != InteropWindowGraphStatusOk) {
            observer->transition_candidate_window = 0u;
            if (status == InteropWindowGraphStatusReentrant) return WindowObserverStatusReentrant;
            if (status == InteropWindowGraphStatusWrongThread) return WindowObserverStatusWrongThread;
            return WindowObserverStatusGraphError;
        }
        if (snapshot_count != observer->tracked_count || snapshot_free > WindowObserverTrackedCapacity ||
            snapshot_count + snapshot_free > WindowObserverTrackedCapacity) {
            observer->transition_candidate_window = 0u;
            latch_tracking_fault(observer, 1);
            return WindowObserverStatusTrackingFault;
        }
        free_count = snapshot_free;
        const uint32_t reserve = union_count + (may_admit ? free_count : 0u);
        if (promoted != 0) {
            for (int index = 0; index < promoted; ++index) {
                const int mailbox_index = destroy_find(observer, promoted_windows[index]);
                if (mailbox_index < 0) continue; /* an earlier exact apply consumed this key */
                if (processed == WindowObserverDestroyCapacity) {
                    observer->transition_candidate_window = 0u;
                    return WindowObserverStatusProtectedStatePending;
                }
                const window_observer_status_t applied = apply_protected_destroy(
                    observer, (uint32_t)mailbox_index);
                if (applied != WindowObserverStatusOk) {
                    observer->transition_candidate_window = 0u;
                    return applied;
                }
                processed += 1u;
            }
            continue;
        }
        if (drain_all && observer->destroy_count != 0u) {
            if (processed == WindowObserverDestroyCapacity) {
                observer->transition_candidate_window = 0u;
                return WindowObserverStatusProtectedStatePending;
            }
            const window_observer_status_t applied = apply_protected_destroy(observer, 0u);
            if (applied != WindowObserverStatusOk) {
                observer->transition_candidate_window = 0u;
                return applied;
            }
            processed += 1u;
            continue;
        }
        if (union_count <= WindowObserverDestroyCapacity && reserve <= WindowObserverDestroyCapacity) {
            observer->candidate_window = candidate;
            observer->transition_candidate_window = 0u;
            return WindowObserverStatusOk;
        }
        if (observer->destroy_count == 0u) {
            observer->transition_candidate_window = 0u;
            return WindowObserverStatusProtectedStatePending;
        }
        if (processed == WindowObserverDestroyCapacity) {
            observer->transition_candidate_window = 0u;
            return WindowObserverStatusProtectedStatePending;
        }
        const window_observer_status_t applied = apply_protected_destroy(observer, 0u);
        if (applied != WindowObserverStatusOk) {
            observer->transition_candidate_window = 0u;
            return applied;
        }
        processed += 1u;
    }
}

/* Gate of every owner-thread entry point: Ok, WrongThread or Reentrant; changes nothing. */
static window_observer_status_t entry_gate(const window_observer_t *observer) {
    if (!on_owner_thread(observer)) {
        return WindowObserverStatusWrongThread;
    }
    if (observer->busy != 0u) {
        return WindowObserverStatusReentrant;
    }
    return WindowObserverStatusOk;
}

static int observer_span_valid(const void *pointer, size_t size, size_t alignment) {
    const uintptr_t start = (uintptr_t)pointer;
    return pointer != NULL && (start % alignment) == 0u && start <= UINTPTR_MAX - size;
}

static int observer_spans_overlap(const void *left, size_t left_size, const void *right,
                                 size_t right_size) {
    const uintptr_t left_start = (uintptr_t)left;
    const uintptr_t right_start = (uintptr_t)right;
    return left_start < right_start + right_size && right_start < left_start + left_size;
}

window_observer_status_t window_observer_bind_graph(window_observer_t *observer,
                                                    interop_window_graph_t *graph,
                                                    uint64_t session_epoch) {
    if (observer == NULL || graph == NULL || session_epoch == 0u) return WindowObserverStatusInvalidArgument;
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) return gate;
    if (observer->state != WindowObserverStateCreated || observer->graph_bound || observer->tracking_fault)
        return WindowObserverStatusBadState;
    observer->busy = 1u;
    uint64_t high_water = 0u;
    interop_window_graph_stats_t graph_stats;
    interop_window_graph_status_t graph_status = interop_window_graph_stamp_high_water(graph, &high_water);
    if (graph_status == InteropWindowGraphStatusOk)
        graph_status = interop_window_graph_get_stats(graph, &graph_stats);
    if (graph_status != InteropWindowGraphStatusOk) {
        observer->busy = 0u;
        if (graph_status == InteropWindowGraphStatusWrongThread) return WindowObserverStatusWrongThread;
        if (graph_status == InteropWindowGraphStatusReentrant) return WindowObserverStatusReentrant;
        return WindowObserverStatusGraphError;
    }
    if (observer->next_stamp <= high_water || graph_stats.session_epoch != session_epoch) {
        observer->busy = 0u;
        return WindowObserverStatusInvalidArgument;
    }
    uint32_t retired = 0u;
    graph_status = interop_window_graph_invalidate_all(graph, session_epoch, &retired);
    if (graph_status == InteropWindowGraphStatusQueueFull) {
        observer->busy = 0u;
        return WindowObserverStatusGraphQueueFull;
    }
    observer->bound_graph = graph;
    observer->bound_epoch = session_epoch;
    if (graph_status != InteropWindowGraphStatusOk) {
        observer->graph_bound = 0u;
        observer->tracking_fault = 1u;
        observer->state = WindowObserverStateTrackingFault;
        observer->stats.last_graph_status = (uint32_t)graph_status;
        bump(&observer->stats.graph_errors);
        observer->busy = 0u;
        return WindowObserverStatusTrackingFault;
    }
    if (!refresh_tracked_snapshot(observer, 1)) {
        observer->busy = 0u;
        return WindowObserverStatusTrackingFault;
    }
    observer->graph_bound = 1u;
    observer->busy = 0u;
    return WindowObserverStatusOk;
}

window_observer_status_t window_observer_sync_tracked(window_observer_t *observer) {
    if (observer == NULL) return WindowObserverStatusInvalidArgument;
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) return gate;
    if (!observer->graph_bound) return WindowObserverStatusBadState;
    if (observer->tracking_fault) return WindowObserverStatusTrackingFault;
    if (observer->state == WindowObserverStateFailed) return WindowObserverStatusBadState;
    observer->busy = 1u;
    const int ok = refresh_tracked_snapshot(observer, 1);
    observer->busy = 0u;
    return ok ? WindowObserverStatusOk : WindowObserverStatusTrackingFault;
}

window_observer_status_t window_observer_prepare_callbacks(window_observer_t *observer,
                                                           uint64_t candidate_window,
                                                           uint32_t admission_may_add_entries) {
    if (observer == NULL || admission_may_add_entries > 1u) return WindowObserverStatusInvalidArgument;
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) return gate;
    if (!observer->graph_bound) return WindowObserverStatusBadState;
    if (observer->tracking_fault) return WindowObserverStatusTrackingFault;
    if (observer->state != WindowObserverStateStarted && observer->state != WindowObserverStateStopped)
        return WindowObserverStatusBadState;
    observer->busy = 1u;
    const window_observer_status_t result = prepare_callbacks_locked(observer, candidate_window,
                                                                     admission_may_add_entries != 0u, 0);
    observer->busy = 0u;
    return result;
}

window_observer_status_t window_observer_destroy_pending(const window_observer_t *observer,
                                                         uint64_t window,
                                                         uint32_t *out_pending) {
    if (observer == NULL || window == 0u || !observer_span_valid(out_pending, sizeof(*out_pending),
                                                                  _Alignof(uint32_t)) ||
        observer_spans_overlap(observer, sizeof(*observer), out_pending, sizeof(*out_pending)))
        return WindowObserverStatusInvalidArgument;
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) return gate;
    if (!observer->graph_bound) return WindowObserverStatusBadState;
    *out_pending = destroy_find(observer, window) >= 0 ? 1u : 0u;
    return WindowObserverStatusOk;
}

window_observer_status_t window_observer_drain_protected_destroys(window_observer_t *observer) {
    if (observer == NULL) return WindowObserverStatusInvalidArgument;
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) return gate;
    if (observer->tracking_fault) return WindowObserverStatusTrackingFault;
    if (!observer->graph_bound || (observer->state != WindowObserverStateStarted &&
                                   observer->state != WindowObserverStateStopped))
        return WindowObserverStatusBadState;
    observer->busy = 1u;
    const window_observer_status_t reserved = prepare_callbacks_locked(observer, 0u, 0, 1);
    observer->busy = 0u;
    return reserved;
}

window_observer_status_t window_observer_quarantine_handoff(
    window_observer_t **observer_slot, window_observer_quarantine_record_t *record) {
    if (!observer_span_valid(observer_slot, sizeof(*observer_slot), _Alignof(window_observer_t *)) ||
        !observer_span_valid(record, sizeof(*record), _Alignof(window_observer_quarantine_record_t)) ||
        observer_spans_overlap(observer_slot, sizeof(*observer_slot), record, sizeof(*record)))
        return WindowObserverStatusInvalidArgument;
    window_observer_t *observer = *observer_slot;
    if (observer == NULL) return WindowObserverStatusInvalidArgument;
    if (observer_spans_overlap(observer, sizeof(*observer), record, sizeof(*record)) ||
        observer_spans_overlap(observer, sizeof(*observer), observer_slot, sizeof(*observer_slot)))
        return WindowObserverStatusInvalidArgument;
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) return gate;
    if ((observer->state != WindowObserverStateStopped && observer->state != WindowObserverStateTrackingFault) ||
        observer->hooks[0] != 0u || observer->hooks[1] != 0u || observer->hooks[2] != 0u ||
        atomic_load(&g_active_observer) == observer || atomic_load(&g_hint_inflight) != 0u)
        return WindowObserverStatusProtectedStatePending;
    if (observer->dirty_count > WindowObserverDirtyCapacity ||
        observer->destroy_count > WindowObserverDestroyCapacity ||
        observer->tracked_count > WindowObserverTrackedCapacity)
        return WindowObserverStatusTrackingFault;
    window_observer_quarantine_record_t staged;
    memset(&staged, 0, sizeof(staged));
    staged.version = WindowObserverQuarantineVersion;
    staged.size = (uint32_t)sizeof(staged);
    staged.borrowed_graph = observer->bound_graph;
    staged.session_epoch = observer->bound_epoch;
    staged.observer_state = observer->state;
    staged.last_graph_status = observer->stats.last_graph_status;
    staged.tracking_fault = observer->tracking_fault;
    staged.evidence_incomplete = observer->evidence_incomplete;
    staged.protected_count = observer->destroy_count;
    staged.dirty_count = observer->dirty_count;
    staged.tracked_count = observer->tracked_count;
    staged.tracked_snapshot_valid = observer->tracked_snapshot_valid;
    staged.candidate_window = observer->candidate_window;
    staged.mailbox_inflight_index = observer->mailbox_inflight_index;
    memcpy(staged.protected_destroys, observer->destroys, sizeof(observer->destroys));
    for (uint32_t index = 0; index < observer->dirty_count; ++index) {
        const dirty_entry_t *source = &observer->dirty[index];
        window_observer_dirty_record_t *target = &staged.pending_dirty[index];
        target->window = source->window;
        target->kinds = source->kinds;
        target->retries = source->retries;
        target->first_stamp = source->first_stamp;
        target->last_stamp = source->last_stamp;
        target->destroy_stamp = source->destroy_stamp;
        target->destroy_event_ms = source->destroy_event_ms;
        target->reserved_zero = 0u;
    }
    memcpy(staged.last_known_tracked_windows, observer->tracked_windows, sizeof(observer->tracked_windows));
    memcpy(record, &staged, sizeof(staged));
    *observer_slot = NULL;
    free(observer);
    return WindowObserverStatusOk;
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

/* Give up the rest of the time slice of the calling thread (SwitchToThread / sched_yield). */
static void yield_thread(void) {
#ifdef _WIN32
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

/*
 * Release the process-wide slot if this observer holds it, then wait out in-flight hints. The wait
 * yields the processor on every iteration, so a preempted foreign-thread hint on a single-CPU guest
 * is scheduled instead of burning the waiter's quantum. note_hint never blocks, so the wait ends as
 * soon as every hint that loaded the slot before the clear has returned.
 */
static void release_slot(window_observer_t *observer) {
    window_observer_t *expected = observer;
    if (atomic_compare_exchange_strong(&g_active_observer, &expected, NULL)) {
        while (atomic_load(&g_hint_inflight) != 0u) {
            yield_thread();
        }
    }
}

/* Reserve the one process-wide observer lifecycle, including periods with no published callback target. */
static int reserve_lifecycle(window_observer_t *observer) {
    window_observer_t *expected = NULL;
    return atomic_compare_exchange_strong(&g_lifecycle_observer, &expected, observer);
}

/* Release the lifecycle reservation only after the observer is stopped and owns no hook tokens. */
static void release_lifecycle(window_observer_t *observer) {
    window_observer_t *expected = observer;
    (void)atomic_compare_exchange_strong(&g_lifecycle_observer, &expected, NULL);
}

/* Remove every retained hook in reverse install order; preserve each token whose removal failed. */
static int remove_owned_hooks(window_observer_t *observer) {
    int failed = 0;
    for (uint32_t i = ObserverHookCount; i-- > 0u;) {
        if (observer->hooks[i] != 0u) {
            if (observer->api.remove_hook(observer->api_context, observer->hooks[i]) != 0) {
                failed = 1;
            } else {
                observer->hooks[i] = 0u;
            }
        }
    }
    return failed;
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
    case InteropWindowGraphStatusReentrant:
        return FoldReentrant;
    default:
        return FoldFatal;
    }
}

/* Convert a stopping fold into a pump/refresh status (Drop is not meaningful there and maps to GraphError). */
static window_observer_status_t stop_status(fold_t fold) {
    switch (fold) {
    case FoldQueueFull:
        return WindowObserverStatusGraphQueueFull;
    case FoldReentrant:
        return WindowObserverStatusReentrant;
    default:
        return WindowObserverStatusGraphError;
    }
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

/*
 * Put an entry back at the head (processing stopped); merges into a newer entry of the same window.
 * Only called for the key pump has popped, whose slot is reserved, so the full-table branch is
 * defensive and cannot be reached while popped_in_flight is honored by note_hint.
 */
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

/* Put a retried entry back at the tail; merges into a newer entry of the same window (same reservation rule). */
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
    case WindowObserverEventObjectCloaked:
    case WindowObserverEventObjectUncloaked:
        return DirtyKindCloak;
    default:
        return DirtyKindObjectOther;
    }
}

/* Return nonzero when the event id lies in one of the three hooked ranges (and nowhere else). */
static int event_in_hooked_range(uint32_t event) {
    for (uint32_t i = 0; i < ObserverHookCount; i++) {
        if (event >= HookRanges[i].event_min && event <= HookRanges[i].event_max) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Lifecycle                                                                                  */
/* ------------------------------------------------------------------------------------------ */

/** @copydoc window_observer_create */
window_observer_status_t window_observer_create(const window_observer_config_t *config,
                                                window_observer_t **out_observer) {
    if (out_observer == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    /*
     * The config is copied BEFORE the out slot is written: a caller whose config storage overlaps the
     * slot would otherwise have the config clobbered by the NULL store below.
     */
    window_observer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (config != NULL) {
        cfg = *config;
    }
    *out_observer = NULL;
    if (config == NULL || cfg.uac_mode > (uint32_t)InteropWindowUacNative || cfg.first_stamp == 0u ||
        cfg.first_stamp == UINT64_MAX || cfg.own_process_id == 0u) {
        return WindowObserverStatusInvalidArgument;
    }
    const window_observer_api_t *api = cfg.api;
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
    if (api == window_observer_native_api() && !native_table_runnable()) {
        return WindowObserverStatusUnsupported; /* checked before any allocation */
    }
#endif
    window_observer_t *observer = calloc(1u, sizeof(*observer));
    if (observer == NULL) {
        return WindowObserverStatusNoMemory;
    }
    observer->api = *api;
    observer->api_context = cfg.api_context;
    observer->owner_thread = api->current_thread_id(cfg.api_context);
    observer->state = WindowObserverStateCreated;
    observer->next_stamp = cfg.first_stamp;
    observer->preedit_window = cfg.preedit_window;
    observer->own_process_id = cfg.own_process_id;
    observer->uac_mode = cfg.uac_mode;
    observer->mailbox_inflight_index = UINT32_MAX;
    *out_observer = observer;
    return WindowObserverStatusOk;
}

/* Body of stop; the caller holds the busy flag and has checked thread and argument. */
static window_observer_status_t stop_locked(window_observer_t *observer) {
    if (observer->state == WindowObserverStateCreated) {
        if (!observer->graph_bound) return WindowObserverStatusBadState;
        observer->state = WindowObserverStateStopped;
        return WindowObserverStatusOk;
    }
    if (observer->state == WindowObserverStateStopped) return WindowObserverStatusOk;
    if (observer->state != WindowObserverStateStarted && observer->state != WindowObserverStateFailed &&
        observer->state != WindowObserverStateStopping && observer->state != WindowObserverStateTrackingFault)
        return WindowObserverStatusBadState;
    if (!observer->tracking_fault && !observer->stop_invalidation_done) {
        uint32_t retired = 0u;
        const interop_window_graph_status_t graph_status = interop_window_graph_invalidate_all(
            observer->bound_graph, observer->bound_epoch, &retired);
        if (graph_status == InteropWindowGraphStatusQueueFull) {
            observer->stats.last_graph_status = (uint32_t)graph_status;
            bump(&observer->stats.graph_errors);
            return WindowObserverStatusGraphQueueFull;
        }
        if (graph_status != InteropWindowGraphStatusOk) {
            observer->stats.last_graph_status = (uint32_t)graph_status;
            bump(&observer->stats.graph_errors);
            latch_tracking_fault(observer, 0);
        } else {
            observer->stop_invalidation_done = 1u;
            if (!refresh_tracked_snapshot(observer, 1)) observer->tracking_fault = 1u;
        }
    }
    observer->state = observer->tracking_fault ? WindowObserverStateTrackingFault : WindowObserverStateStopping;
    const int failed = remove_owned_hooks(observer);
    if (failed) {
        if (!observer->tracking_fault) observer->state = WindowObserverStateFailed;
        return WindowObserverStatusUnhookFailed;
    }
    release_slot(observer);
    release_lifecycle(observer);
    observer->state = observer->tracking_fault ? WindowObserverStateTrackingFault : WindowObserverStateStopped;
    return observer->tracking_fault ? WindowObserverStatusTrackingFault : WindowObserverStatusOk;
}

/** @copydoc window_observer_stop */
window_observer_status_t window_observer_stop(window_observer_t *observer) {
    if (observer == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) {
        return gate;
    }
    observer->busy = 1u;
    const window_observer_status_t result = stop_locked(observer);
    observer->busy = 0u;
    return result;
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
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) {
        return gate;
    }
    observer->busy = 1u;
    if (observer->tracking_fault) { observer->busy = 0u; return WindowObserverStatusTrackingFault; }
    if (observer->hooks[0] != 0u || observer->hooks[1] != 0u || observer->hooks[2] != 0u ||
        atomic_load(&g_active_observer) == observer || atomic_load(&g_lifecycle_observer) == observer ||
        atomic_load(&g_hint_inflight) != 0u) {
        observer->busy = 0u;
        return WindowObserverStatusProtectedStatePending;
    }
    if (observer->state == WindowObserverStateStarted || observer->state == WindowObserverStateFailed ||
        observer->state == WindowObserverStateStopping || observer->state == WindowObserverStateTrackingFault ||
        observer->destroy_count != 0u) {
        observer->busy = 0u;
        return WindowObserverStatusProtectedStatePending;
    }
    if (observer->graph_bound) {
        if (observer->state != WindowObserverStateStopped) {
            observer->busy = 0u;
            return WindowObserverStatusProtectedStatePending;
        }
        uint64_t windows[WindowObserverTrackedCapacity];
        uint32_t count = 0u, free_count = 0u;
        const interop_window_graph_status_t status = interop_window_graph_list_tracked_windows(
            observer->bound_graph, observer->bound_epoch, windows, WindowObserverTrackedCapacity,
            &count, &free_count);
        if (status != InteropWindowGraphStatusOk || count != 0u) {
            observer->busy = 0u;
            return WindowObserverStatusProtectedStatePending;
        }
    } else if (observer->state != WindowObserverStateCreated || observer->dirty_count != 0u) {
        observer->busy = 0u;
        return WindowObserverStatusProtectedStatePending;
    }
    free(observer);
    *observer_slot = NULL;
    return WindowObserverStatusOk;
}

/* Body of start; the caller holds the busy flag and has checked thread and argument. */
static window_observer_status_t start_locked(window_observer_t *observer) {
    if (!observer->graph_bound || observer->tracking_fault ||
        (observer->state != WindowObserverStateCreated && observer->state != WindowObserverStateStopped)) {
        return WindowObserverStatusBadState;
    }
    if (!reserve_lifecycle(observer)) return WindowObserverStatusAlreadyActive;
    const window_observer_status_t reservation = prepare_callbacks_locked(observer, 0u, 0, 1);
    if (reservation != WindowObserverStatusOk) {
        release_lifecycle(observer);
        return reservation;
    }
    const window_observer_state_t prior_state = (window_observer_state_t)observer->state;
    observer->state = WindowObserverStateStarting;
    window_observer_t *expected = NULL;
    if (!atomic_compare_exchange_strong(&g_active_observer, &expected, observer)) {
        observer->state = prior_state;
        release_lifecycle(observer);
        return WindowObserverStatusAlreadyActive;
    }
    observer->stop_invalidation_done = 0u;

    for (uint32_t i = 0; i < ObserverHookCount; i++) {
        uintptr_t token = 0u;
        const int rc = observer->api.install_hook(observer->api_context, HookRanges[i].event_min,
                                                  HookRanges[i].event_max, &token);
        if (token != 0u) {
            observer->hooks[i] = token;
        }
        if (rc != 0 || token == 0u) {
            observer->stop_invalidation_done = 1u; /* failed-start rollback never mutates the graph */
            if (remove_owned_hooks(observer)) {
                observer->state = observer->tracking_fault ? WindowObserverStateTrackingFault : WindowObserverStateFailed;
            } else {
                release_slot(observer);
                observer->state = observer->tracking_fault ? WindowObserverStateTrackingFault : WindowObserverStateStopped;
                release_lifecycle(observer);
            }
            return observer->tracking_fault ? WindowObserverStatusTrackingFault : WindowObserverStatusHookFailed;
        }
        if (observer->tracking_fault) break;
    }
    if (observer->tracking_fault) {
        observer->state = WindowObserverStateTrackingFault;
        return stop_locked(observer);
    }
    observer->refresh_requested = 1u;
    observer->latch_ms = observer->api.monotonic_ms(observer->api_context);
    if (observer->tracking_fault) {
        observer->state = WindowObserverStateTrackingFault;
        return stop_locked(observer);
    }
    observer->latch_unconsumed = 1u;
    observer->have_attempt = 0u;
    observer->last_attempt_ms = 0u;
    observer->desktop_active = 0u;
    observer->desktop_blocked = 0u;
    observer->state = WindowObserverStateStarted;
    return WindowObserverStatusOk;
}

/** @copydoc window_observer_start */
window_observer_status_t window_observer_start(window_observer_t *observer) {
    if (observer == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) {
        return gate;
    }
    observer->busy = 1u;
    const window_observer_status_t result = start_locked(observer);
    observer->busy = 0u;
    return result;
}

/** @copydoc window_observer_set_preedit_window */
window_observer_status_t window_observer_set_preedit_window(window_observer_t *observer, uint64_t window) {
    if (observer == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) {
        return gate;
    }
    /* Starting/Stopping are serialized by busy; Failed permits cleanup/diagnostics only. */
    if (observer->tracking_fault) return WindowObserverStatusTrackingFault;
    if (observer->state == WindowObserverStateFailed) return WindowObserverStatusBadState;
    observer->busy = 1u;
    if (observer->preedit_window != window) {
        uint64_t now_ms = 0u;
        if (observer->state == WindowObserverStateStarted) {
            now_ms = observer->api.monotonic_ms(observer->api_context);
            if (observer->tracking_fault) {
                observer->busy = 0u;
                return WindowObserverStatusTrackingFault;
            }
        }
        observer->preedit_window = window;
        if (observer->state == WindowObserverStateStarted) {
            observer->refresh_requested = 1u;
            observer->latch_ms = now_ms;
            observer->latch_unconsumed = 1u;
        }
    }
    observer->busy = 0u;
    return WindowObserverStatusOk;
}

/** @copydoc window_observer_status */
window_observer_status_t window_observer_status(const window_observer_t *observer,
                                                window_observer_stats_t *stats) {
    if (observer == NULL || stats == NULL) {
        return WindowObserverStatusInvalidArgument;
    }
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) {
        return gate;
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
    if (observer->state != WindowObserverStateStarted && observer->state != WindowObserverStateStarting &&
        observer->state != WindowObserverStateStopping && observer->state != WindowObserverStateFailed &&
        observer->state != WindowObserverStateTrackingFault) {
        return;
    }
    if (object_id != WindowObserverObjectIdWindow || child_id != WindowObserverChildIdSelf || window == 0u ||
        !event_in_hooked_range(event)) {
        bump(&observer->stats.hints_filtered);
        return;
    }
    const uint32_t kind = dirty_kind_for_event(event);
    if (observer->state != WindowObserverStateStarted && event != WindowObserverEventObjectDestroy) {
        observer->refresh_requested = 1u;
        return;
    }
    uint64_t stamp = 0;
    if (!stamp_next(observer, &stamp)) {
        if (observer->graph_bound) latch_tracking_fault(observer, 1);
        else observer->overflow = 1u;
        return;
    }
    if (event == WindowObserverEventObjectDestroy && observer->graph_bound &&
        (destroy_find(observer, window) >= 0 || tracked_contains(observer, window) ||
         candidate_protected(observer, window))) {
        int protected_index = destroy_find(observer, window);
        if (protected_index < 0) {
            if (observer->destroy_count >= WindowObserverDestroyCapacity) {
                latch_tracking_fault(observer, 1);
                return;
            }
            protected_index = (int)observer->destroy_count++;
            memset(&observer->destroys[protected_index], 0, sizeof(observer->destroys[0]));
            observer->destroys[protected_index].window = window;
        } else {
            bump(&observer->stats.hints_coalesced);
        }
        observer->destroys[protected_index].destroy_stamp = stamp;
        observer->destroys[protected_index].event_time_ms = event_time_ms;
        int dirty_index = dirty_find(observer, window);
        if (dirty_index >= 0) {
            dirty_entry_t *entry = &observer->dirty[dirty_index];
            entry->kinds |= kind;
            entry->last_stamp = stamp;
            entry->destroy_stamp = stamp;
            entry->destroy_event_ms = event_time_ms;
            bump(&observer->stats.hints_coalesced);
        } else if (observer->dirty_count + observer->popped_in_flight < WindowObserverDirtyCapacity) {
            dirty_entry_t *entry = &observer->dirty[observer->dirty_count++];
            memset(entry, 0, sizeof(*entry));
            entry->window = window;
            entry->kinds = kind;
            entry->first_stamp = stamp;
            entry->last_stamp = stamp;
            entry->destroy_stamp = stamp;
            entry->destroy_event_ms = event_time_ms;
        }
        bump(&observer->stats.hints_received);
        return;
    }
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
    /* A popped key keeps one slot reserved for its put-back, so a put-back never forces an overflow. */
    if (observer->dirty_count + observer->popped_in_flight >= WindowObserverDirtyCapacity) {
        if (event == WindowObserverEventObjectDestroy && observer->graph_bound) {
            latch_tracking_fault(observer, 1);
        } else {
            observer->overflow = 1u;
        }
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
/* Desktop and query plumbing shared by pump and refresh                                      */
/* ------------------------------------------------------------------------------------------ */

/* Check the input desktop; returns 1 for Default (gauges updated), 0 otherwise (caller refuses). */
static int desktop_is_default(window_observer_t *observer) {
    const window_observer_desktop_t desktop = observer->api.query_input_desktop(observer->api_context);
    if (desktop == WindowObserverDesktopDefault) {
        observer->desktop_active = 1u;
        observer->desktop_blocked = 0u;
        return 1;
    }
    return 0;
}

/* Record a refused call: gauges, latch (latch_ms = now), attempt stamp (= now) and the counter. */
static void desktop_refuse(window_observer_t *observer, uint64_t now_ms) {
    observer->desktop_active = 0u;
    observer->desktop_blocked = 1u;
    observer->refresh_requested = 1u;
    observer->latch_ms = now_ms;
    observer->latch_unconsumed = 0u;
    observer->last_attempt_ms = now_ms;
    observer->have_attempt = 1u;
    bump(&observer->stats.pumps_desktop_inactive);
}

/* Zero facts, run the native query and sanitize the answer (no accounting). */
static window_observer_query_result_t query_raw(window_observer_t *observer, uint64_t window,
                                                interop_window_facts_t *facts) {
    memset(facts, 0, sizeof(*facts));
    const window_observer_query_result_t result = observer->api.query_facts(observer->api_context, window, facts);
    if (result == WindowObserverQueryGone) {
        return WindowObserverQueryGone;
    }
    if (result == WindowObserverQueryExists && facts->window == window) {
        return WindowObserverQueryExists;
    }
    return WindowObserverQueryFailed;
}

/*
 * Overwrite the fields the core owns: stamp, captured time, the three observer flags and the title of
 * AgentOwned windows. OwnerHidden is cleared here; only resolve_owner may set it.
 */
static void overlay_facts(const window_observer_t *observer, interop_window_facts_t *facts, uint64_t stamp,
                          uint32_t captured_ms) {
    facts->stamp = stamp;
    facts->captured_ms = captured_ms;
    facts->flags &= ~((uint32_t)InteropWindowFactAgentOwned | (uint32_t)InteropWindowFactAgentPreedit |
                      (uint32_t)InteropWindowFactOnInputDesktop | (uint32_t)InteropWindowFactOwnerHidden);
    facts->flags |= (uint32_t)InteropWindowFactOnInputDesktop;
    if (facts->process_id == observer->own_process_id) {
        facts->flags |= (uint32_t)InteropWindowFactAgentOwned;
        facts->title_length = 0u; /* no title for the agent's own windows */
        if (observer->preedit_window != 0u && facts->window == observer->preedit_window) {
            facts->flags |= (uint32_t)InteropWindowFactAgentPreedit;
        }
    }
}

/*
 * Owner sub-query: decide OwnerHidden for a window whose overlay is complete. Returns Exists when the
 * window's facts are usable (flag set or clear) and Failed when the owner could not be judged.
 * No recursion: the owner is classified with OwnerHidden clear and its own owner is never queried.
 */
static window_observer_query_result_t resolve_owner(window_observer_t *observer, interop_window_facts_t *facts,
                                                    uint64_t stamp, uint32_t captured_ms) {
    if (facts->owner_root == 0u || facts->owner_root == facts->window) {
        return WindowObserverQueryExists;
    }
    if ((facts->flags & (uint32_t)InteropWindowFactVisible) == 0u) {
        /* Classification stops at NotVisible before OwnerHidden is consulted: no owner sub-query. */
        return WindowObserverQueryExists;
    }
    interop_window_facts_t owner;
    const window_observer_query_result_t result = query_raw(observer, facts->owner_root, &owner);
    if (result == WindowObserverQueryGone) {
        facts->flags |= (uint32_t)InteropWindowFactOwnerHidden;
        return WindowObserverQueryExists;
    }
    if (result != WindowObserverQueryExists) {
        return WindowObserverQueryFailed;
    }
    overlay_facts(observer, &owner, stamp, captured_ms);
    interop_window_verdict_t verdict;
    memset(&verdict, 0, sizeof(verdict));
    if (interop_window_graph_classify(&owner, observer->uac_mode, &verdict) != InteropWindowGraphStatusOk) {
        return WindowObserverQueryFailed;
    }
    if (verdict.kind == (uint32_t)InteropWindowVerdictNotExported) {
        facts->flags |= (uint32_t)InteropWindowFactOwnerHidden;
    }
    return WindowObserverQueryExists;
}

/*
 * Facts assembly for one window: query, overlay the core fields, resolve OwnerHidden, count one
 * result. Exists leaves facts ready for observe or refresh_offer.
 */
static window_observer_query_result_t query_window(window_observer_t *observer, uint64_t window,
                                                   interop_window_facts_t *facts, uint64_t stamp,
                                                   uint32_t captured_ms) {
    window_observer_query_result_t result = query_raw(observer, window, facts);
    if (result == WindowObserverQueryExists) {
        overlay_facts(observer, facts, stamp, captured_ms);
        result = resolve_owner(observer, facts, stamp, captured_ms);
    }
    switch (result) {
    case WindowObserverQueryExists:
        bump(&observer->stats.queries_exists);
        break;
    case WindowObserverQueryGone:
        bump(&observer->stats.queries_gone);
        break;
    default:
        bump(&observer->stats.queries_failed);
        result = WindowObserverQueryFailed;
        break;
    }
    return result;
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

/* Apply a query-confirmed Gone through the same protected, exact-stamp mailbox path as a hint. */
static window_observer_status_t apply_fresh_destroy(window_observer_t *observer, uint64_t window,
                                                   uint64_t stamp, uint32_t event_ms) {
    int index = destroy_find(observer, window);
    if (index < 0) {
        if (observer->destroy_count >= WindowObserverDestroyCapacity) {
            latch_tracking_fault(observer, 1);
            return WindowObserverStatusTrackingFault;
        }
        index = (int)observer->destroy_count++;
        memset(&observer->destroys[index], 0, sizeof(observer->destroys[0]));
        observer->destroys[index].window = window;
    }
    observer->destroys[index].destroy_stamp = stamp;
    observer->destroys[index].event_time_ms = event_ms;
    return apply_protected_destroy(observer, (uint32_t)index);
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
        /* Reserve the popped Destroy key before its graph clock callback can reenter note_hint. */
        const window_observer_status_t transition = prepare_callbacks_locked(observer, entry->window, 0, 0);
        if (transition != WindowObserverStatusOk) {
            put_back(observer, entry, transition == WindowObserverStatusGraphQueueFull ?
                     FoldQueueFull : FoldFatal);
            return transition;
        }
        int destroy_index = destroy_find(observer, entry->window);
        if (destroy_index < 0) {
            if (observer->destroy_count >= WindowObserverDestroyCapacity) {
                latch_tracking_fault(observer, 1);
                put_back(observer, entry, FoldFatal);
                return WindowObserverStatusTrackingFault;
            }
            destroy_index = (int)observer->destroy_count++;
            observer->destroys[destroy_index].window = entry->window;
            observer->destroys[destroy_index].reserved_zero = 0u;
        }
        if (entry->destroy_stamp >= observer->destroys[destroy_index].destroy_stamp) {
            observer->destroys[destroy_index].destroy_stamp = entry->destroy_stamp;
            observer->destroys[destroy_index].event_time_ms = entry->destroy_event_ms;
        }
        const window_observer_status_t drained = apply_protected_destroy(observer, (uint32_t)destroy_index);
        if (drained != WindowObserverStatusOk) {
            put_back(observer, entry, drained == WindowObserverStatusGraphQueueFull ? FoldQueueFull : FoldFatal);
            return drained;
        }
        entry->destroy_stamp = 0u;
        entry->destroy_event_ms = 0u;
        entry->kinds &= ~(uint32_t)DirtyKindDestroy;
    }

    const window_observer_status_t reserve = prepare_callbacks_locked(observer, entry->window, 1, 0);
    if (reserve != WindowObserverStatusOk) {
        put_back(observer, entry, reserve == WindowObserverStatusGraphQueueFull ? FoldQueueFull : FoldFatal);
        return reserve;
    }
    interop_window_facts_t facts;
    window_observer_query_result_t result = WindowObserverQueryFailed;
    uint64_t stamp = 0u;
    uint32_t captured_ms = 0u;
    for (uint32_t attempt = 0; attempt <= WindowObserverDestroyCapacity; ++attempt) {
        if (!stamp_next(observer, &stamp)) {
            latch_tracking_fault(observer, 1);
            dirty_push_front(observer, entry);
            return WindowObserverStatusTrackingFault;
        }
        captured_ms = observer->api.tick_ms(observer->api_context);
        if (observer->tracking_fault) {
            dirty_push_front(observer, entry);
            return WindowObserverStatusTrackingFault;
        }
        result = query_window(observer, entry->window, &facts, stamp, captured_ms);
        if (observer->tracking_fault) {
            dirty_push_front(observer, entry);
            return WindowObserverStatusTrackingFault;
        }
        const int raced_destroy = destroy_find(observer, entry->window);
        if (raced_destroy < 0) break;
        if (attempt == WindowObserverDestroyCapacity) {
            latch_tracking_fault(observer, 1);
            dirty_push_front(observer, entry);
            return WindowObserverStatusTrackingFault;
        }
        const window_observer_status_t drained = apply_protected_destroy(observer, (uint32_t)raced_destroy);
        if (drained != WindowObserverStatusOk) {
            put_back(observer, entry, drained == WindowObserverStatusGraphQueueFull ? FoldQueueFull : FoldFatal);
            return drained;
        }
    }

    if (result == WindowObserverQueryExists) {
        interop_window_observation_t observation;
        memset(&observation, 0, sizeof(observation));
        const interop_window_graph_status_t status = interop_window_graph_observe(graph, epoch, &facts, &observation);
        fold = fold_status(observer, status);
        if (fold == FoldContinue && observer->tracking_fault) {
            dirty_push_front(observer, entry);
            return WindowObserverStatusTrackingFault;
        }
        if (fold == FoldContinue) {
            if (!refresh_tracked_snapshot(observer, 1)) return WindowObserverStatusTrackingFault;
            const window_observer_status_t drained = prepare_callbacks_locked(observer, entry->window, 0, 1);
            if (drained != WindowObserverStatusOk) return drained;
            count_uac_hidden(observer, &observation);
            if (observation.disposition == InteropWindowDispositionSuspended) {
                /* Defensive: the core always sets OnInputDesktop, so the graph cannot answer Suspended. */
                dirty_push_front(observer, entry);
                desktop_refuse(observer, observer->api.monotonic_ms(observer->api_context));
                return WindowObserverStatusDesktopInactive;
            }
            return WindowObserverStatusOk;
        }
    } else if (result == WindowObserverQueryGone) {
        const window_observer_status_t destroyed = apply_fresh_destroy(observer, entry->window, stamp, captured_ms);
        if (destroyed == WindowObserverStatusOk) {
            return WindowObserverStatusOk;
        }
        if (destroyed == WindowObserverStatusGraphQueueFull) fold = FoldQueueFull;
        else if (destroyed == WindowObserverStatusReentrant) fold = FoldReentrant;
        else return destroyed;
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

/* Forward declaration: refresh steps 2 to 6, shared with the pump's overflow recovery (defined below). */
static window_observer_status_t refresh_body(window_observer_t *observer, interop_window_graph_t *graph,
                                             uint64_t epoch);

/* Body of pump after argument, thread, busy and state checks; the busy flag is held. */
static window_observer_status_t pump_locked(window_observer_t *observer, interop_window_graph_t *graph,
                                            uint64_t epoch, uint32_t budget) {
    const int desktop_default = desktop_is_default(observer);
    if (observer->tracking_fault) return WindowObserverStatusTrackingFault;
    if (!desktop_default) {
        const uint64_t now_ms = observer->api.monotonic_ms(observer->api_context);
        if (observer->tracking_fault) return WindowObserverStatusTrackingFault;
        desktop_refuse(observer, now_ms);
        return WindowObserverStatusDesktopInactive;
    }
    const window_observer_status_t pending = prepare_callbacks_locked(observer, 0u, 0, 1);
    if (pending != WindowObserverStatusOk) return pending;
    if (observer->overflow != 0u) {
        /*
         * Overflow recovery is a full refresh, never invalidate_all: the refresh offers every enumerated
         * window (unchanged windows produce no event), requeries the unseen ones and retires only what
         * is gone, so a flood of hints for unrelated (child) windows costs one refresh and no churn.
         * Protected Destroy entries are applied before this path. Ordinary dirty hints are discarded;
         * the refresh latch stays set until a refresh commits, so a failed recovery is retried at the
         * cadence.
         */
        bump(&observer->stats.invalidations); /* counts overflow recoveries; no invalidate_all is issued */
        uint32_t retained = 0u;
        for (uint32_t index = 0; index < observer->dirty_count; ++index) {
            if (observer->dirty[index].destroy_stamp != 0u) {
                if (tracked_contains(observer, observer->dirty[index].window) ||
                    observer->dirty[index].window == observer->candidate_window) {
                    const dirty_entry_t saved = observer->dirty[index];
                    int mailbox_index = destroy_find(observer, saved.window);
                    if (mailbox_index < 0 && observer->destroy_count < WindowObserverDestroyCapacity) {
                        mailbox_index = (int)observer->destroy_count++;
                        memset(&observer->destroys[mailbox_index], 0, sizeof(observer->destroys[0]));
                        observer->destroys[mailbox_index].window = saved.window;
                    }
                    if (mailbox_index < 0) {
                        latch_tracking_fault(observer, 1);
                        return WindowObserverStatusTrackingFault;
                    }
                    observer->destroys[mailbox_index].destroy_stamp = saved.destroy_stamp;
                    observer->destroys[mailbox_index].event_time_ms = saved.destroy_event_ms;
                } else {
                    observer->dirty[retained++] = observer->dirty[index];
                }
            }
        }
        observer->dirty_count = retained;
        observer->overflow = 0u;
        for (uint32_t processed = 0u; observer->destroy_count != 0u &&
             processed < WindowObserverDestroyCapacity; ++processed) {
            const window_observer_status_t drained = apply_protected_destroy(observer, 0u);
            if (drained != WindowObserverStatusOk) return drained;
        }
        if (observer->destroy_count != 0u) {
            latch_tracking_fault(observer, 1);
            return WindowObserverStatusTrackingFault;
        }
        /* One clock read stamps both the latch and the attempt, so this refresh consumes its own latch event. */
        const uint64_t now_ms = observer->api.monotonic_ms(observer->api_context);
        if (observer->tracking_fault) return WindowObserverStatusTrackingFault;
        observer->refresh_requested = 1u;
        observer->latch_ms = now_ms;
        observer->latch_unconsumed = 0u;
        observer->last_attempt_ms = now_ms;
        observer->have_attempt = 1u;
        const window_observer_status_t recovered = refresh_body(observer, graph, epoch);
        if (recovered == WindowObserverStatusOk && (observer->dirty_count != 0u || observer->overflow != 0u)) {
            return WindowObserverStatusMore; /* hints arrived while the refresh ran: pump again */
        }
        return recovered;
    }
    const uint32_t available = observer->dirty_count;
    const uint32_t limit = budget < available ? budget : available;
    for (uint32_t i = 0; i < limit; i++) {
        dirty_entry_t entry;
        if (!dirty_pop_front(observer, &entry)) {
            break;
        }
        observer->popped_in_flight = 1u; /* reserves the put-back slot against note_hint */
        bump(&observer->stats.keys_processed);
        const window_observer_status_t status = process_key(observer, graph, epoch, &entry);
        observer->popped_in_flight = 0u;
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
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) {
        return gate;
    }
    if (!observer->graph_bound || observer->bound_graph != graph || observer->bound_epoch != session_epoch) {
        return WindowObserverStatusGraphBindingMismatch;
    }
    if (observer->tracking_fault) {
        return WindowObserverStatusTrackingFault;
    }
    if (observer->state != WindowObserverStateStarted) {
        return WindowObserverStatusBadState;
    }
    observer->busy = 1u;
    const window_observer_status_t reservation = prepare_callbacks_locked(observer, 0u, 0, 0);
    if (reservation != WindowObserverStatusOk) {
        observer->busy = 0u;
        if (reservation != WindowObserverStatusReentrant) snapshot_stats(observer, stats);
        return reservation;
    }
    native_call_begin();
    const window_observer_status_t status = pump_locked(observer, graph, session_epoch, budget);
    native_call_end();
    observer->busy = 0u;
    if (status != WindowObserverStatusReentrant) { /* the header excludes Reentrant from the stats snapshot */
        snapshot_stats(observer, stats);
    }
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

/*
 * Query one window of an open refresh and report it to the graph. unseen_pass is 0 for the enumerated
 * windows (Gone: skipped, the window stays unseen) and 1 for the windows refresh_unseen listed (Gone:
 * note_destroyed with a fresh stamp and tick). Exists is offered, Failed is kept. Ok = continue; any
 * other status means the refresh was aborted (QueueFull, call-level status, Reentrant, stamp
 * exhaustion, defensive Suspended) and is the status of the whole call.
 */
static window_observer_status_t refresh_visit(window_observer_t *observer, interop_window_graph_t *graph,
                                              uint64_t epoch, uint64_t window, int unseen_pass) {
    const window_observer_status_t reserved = prepare_callbacks_locked(observer, window, 1, 0);
    if (reserved != WindowObserverStatusOk) {
        refresh_abort_quiet(observer, graph, epoch);
        return reserved;
    }
    interop_window_facts_t facts;
    window_observer_query_result_t result = WindowObserverQueryFailed;
    uint64_t stamp = 0u;
    uint32_t captured_ms = 0u;
    for (uint32_t attempt = 0u; attempt <= WindowObserverDestroyCapacity; ++attempt) {
        if (!stamp_next(observer, &stamp)) {
            refresh_abort_quiet(observer, graph, epoch);
            latch_tracking_fault(observer, 1);
            return WindowObserverStatusTrackingFault;
        }
        captured_ms = observer->api.tick_ms(observer->api_context);
        if (observer->tracking_fault) {
            refresh_abort_quiet(observer, graph, epoch);
            return WindowObserverStatusTrackingFault;
        }
        result = query_window(observer, window, &facts, stamp, captured_ms);
        if (observer->tracking_fault) {
            refresh_abort_quiet(observer, graph, epoch);
            return WindowObserverStatusTrackingFault;
        }
        const int raced_destroy = destroy_find(observer, window);
        if (raced_destroy < 0) break;
        if (attempt == WindowObserverDestroyCapacity) {
            refresh_abort_quiet(observer, graph, epoch);
            latch_tracking_fault(observer, 1);
            return WindowObserverStatusTrackingFault;
        }
        const window_observer_status_t drained = apply_protected_destroy(observer, (uint32_t)raced_destroy);
        if (drained != WindowObserverStatusOk) {
            refresh_abort_quiet(observer, graph, epoch);
            return drained;
        }
    }
    fold_t fold = FoldContinue;
    if (result == WindowObserverQueryExists) {
        interop_window_observation_t observation;
        memset(&observation, 0, sizeof(observation));
        fold = fold_status(observer, interop_window_graph_refresh_offer(graph, epoch, &facts, &observation));
        if (fold == FoldContinue) {
            if (!refresh_tracked_snapshot(observer, 1)) {
                refresh_abort_quiet(observer, graph, epoch);
                return WindowObserverStatusTrackingFault;
            }
            const window_observer_status_t drained = prepare_callbacks_locked(observer, window, 0, 1);
            if (drained != WindowObserverStatusOk) {
                refresh_abort_quiet(observer, graph, epoch);
                return drained;
            }
            count_uac_hidden(observer, &observation);
            if (observation.disposition == InteropWindowDispositionSuspended) {
                refresh_abort_quiet(observer, graph, epoch);
                desktop_refuse(observer, observer->api.monotonic_ms(observer->api_context));
                return WindowObserverStatusDesktopInactive;
            }
        }
    } else if (result == WindowObserverQueryGone) {
        if (!unseen_pass) {
            return WindowObserverStatusOk; /* stays unseen; the unseen pass requeries it if it is Active */
        }
        const window_observer_status_t destroyed = apply_fresh_destroy(observer, window, stamp, captured_ms);
        if (destroyed == WindowObserverStatusOk) {
            if (!refresh_tracked_snapshot(observer, 1)) return WindowObserverStatusTrackingFault;
            const window_observer_status_t drained = prepare_callbacks_locked(observer, window, 0, 1);
            if (drained != WindowObserverStatusOk) return drained;
        } else if (destroyed == WindowObserverStatusGraphQueueFull) {
            fold = FoldQueueFull;
        } else if (destroyed == WindowObserverStatusReentrant) {
            fold = FoldReentrant;
        } else {
            return destroyed;
        }
    } else {
        fold = fold_status(observer, interop_window_graph_refresh_keep(graph, epoch, window));
    }
    if (fold == FoldContinue || fold == FoldDrop) {
        return WindowObserverStatusOk;
    }
    refresh_abort_quiet(observer, graph, epoch);
    return stop_status(fold);
}

/*
 * Steps 2 to 6 of the refresh: enumerate, begin, query and offer every window, requery the unseen
 * Active windows, commit. The attempt is already stamped and the desktop already checked Default by
 * the caller; the busy flag is held. Failed queries never fail the refresh. A Truncated enumeration
 * runs every step except the commit (the refresh is aborted and EnumFailed is returned).
 */
static window_observer_status_t refresh_body(window_observer_t *observer, interop_window_graph_t *graph,
                                             uint64_t epoch) {
    uint32_t count = 0;
    const window_observer_enum_result_t enumerated = observer->api.enum_top_level_windows(
        observer->api_context, observer->enum_buffer, WindowObserverMaxEnumerated, &count);
    if (observer->tracking_fault) return WindowObserverStatusTrackingFault;
    if ((enumerated != WindowObserverEnumOk && enumerated != WindowObserverEnumTruncated) ||
        count > WindowObserverMaxEnumerated) {
        return WindowObserverStatusEnumFailed; /* nothing was begun */
    }
    /*
     * A truncated list is still processed: the listed windows are offered and the unseen Active windows
     * are requeried one by one below, so admissions, updates and Gone notes keep flowing; only the
     * commit is withheld (absence from a partial list proves nothing).
     */
    const int truncated = enumerated == WindowObserverEnumTruncated;
    window_observer_status_t status = refresh_open(observer, graph, epoch);
    if (status != WindowObserverStatusOk) {
        return status;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint64_t window = observer->enum_buffer[i];
        if (window == 0u) {
            continue;
        }
        status = refresh_visit(observer, graph, epoch, window, 0);
        if (status != WindowObserverStatusOk) {
            return status;
        }
    }

    uint64_t unseen[InteropMaxWindows];
    uint32_t unseen_count = 0;
    const fold_t listed = fold_status(
        observer, interop_window_graph_refresh_unseen(graph, epoch, unseen, InteropMaxWindows, &unseen_count));
    if (listed != FoldContinue) {
        refresh_abort_quiet(observer, graph, epoch);
        return stop_status(listed);
    }
    if (unseen_count > InteropMaxWindows) {
        unseen_count = InteropMaxWindows;
    }
    for (uint32_t i = 0; i < unseen_count; i++) {
        if (unseen[i] == 0u) {
            continue;
        }
        status = refresh_visit(observer, graph, epoch, unseen[i], 1);
        if (status != WindowObserverStatusOk) {
            return status;
        }
    }

    if (truncated) {
        refresh_abort_quiet(observer, graph, epoch);
        return WindowObserverStatusEnumFailed; /* partial list: never committed, the latch stays set */
    }
    uint32_t retired = 0;
    const fold_t fold = fold_status(observer, interop_window_graph_refresh_commit(graph, epoch, &retired));
    if (fold == FoldContinue) {
        bump(&observer->stats.refreshes_committed);
        if (!refresh_tracked_snapshot(observer, 1)) return WindowObserverStatusTrackingFault;
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
    const window_observer_status_t gate = entry_gate(observer);
    if (gate != WindowObserverStatusOk) {
        return gate;
    }
    if (!observer->graph_bound || observer->bound_graph != graph || observer->bound_epoch != session_epoch) {
        return WindowObserverStatusGraphBindingMismatch;
    }
    if (observer->tracking_fault) {
        return WindowObserverStatusTrackingFault;
    }
    if (observer->state != WindowObserverStateStarted) {
        return WindowObserverStatusBadState;
    }
    observer->busy = 1u;
    const window_observer_status_t reservation = prepare_callbacks_locked(observer, 0u, 1, 0);
    if (reservation != WindowObserverStatusOk) {
        observer->busy = 0u;
        if (reservation != WindowObserverStatusReentrant) snapshot_stats(observer, stats);
        return reservation;
    }
    native_call_begin();
    /* Step 1: stamp the attempt BEFORE anything can fail, then check the desktop. */
    const uint64_t now_ms = observer->api.monotonic_ms(observer->api_context);
    window_observer_status_t status;
    if (observer->tracking_fault) {
        status = WindowObserverStatusTrackingFault;
    } else {
        observer->latch_unconsumed = 0u;
        observer->last_attempt_ms = now_ms;
        observer->have_attempt = 1u;
        const int desktop_default = desktop_is_default(observer);
        if (observer->tracking_fault) {
            status = WindowObserverStatusTrackingFault;
        } else if (!desktop_default) {
            desktop_refuse(observer, now_ms);
            status = WindowObserverStatusDesktopInactive;
        } else {
            status = prepare_callbacks_locked(observer, 0u, 0, 1);
            if (status == WindowObserverStatusOk) status = refresh_body(observer, graph, session_epoch);
        }
    }
    native_call_end();
    observer->busy = 0u;
    if (status != WindowObserverStatusReentrant) { /* the header excludes Reentrant from the stats snapshot */
        snapshot_stats(observer, stats);
    }
    return status;
}

/* ------------------------------------------------------------------------------------------ */
/* Cadence                                                                                    */
/* ------------------------------------------------------------------------------------------ */

/* Milliseconds since the latest refresh attempt; a clock that went backwards counts as zero. */
static uint64_t attempt_elapsed(const window_observer_t *observer, uint64_t now_ms) {
    return now_ms >= observer->last_attempt_ms ? now_ms - observer->last_attempt_ms : 0u;
}

/* Cadence rule of refresh_due for a Started observer, without the thread and busy gate. */
static int refresh_due_core(const window_observer_t *observer, uint64_t now_ms) {
    if (observer->have_attempt == 0u) {
        return 1;
    }
    if (attempt_elapsed(observer, now_ms) >= InteropWindowGraphRefreshIntervalMs) {
        return 1;
    }
    /* A latch event is consumed by any attempt stamped at or after it, even a failed one. */
    return observer->refresh_requested != 0u && observer->latch_unconsumed != 0u;
}

/** @copydoc window_observer_refresh_due */
int window_observer_refresh_due(const window_observer_t *observer, uint64_t now_ms) {
    if (observer == NULL || entry_gate(observer) != WindowObserverStatusOk ||
        observer->state != WindowObserverStateStarted) {
        return 0;
    }
    return refresh_due_core(observer, now_ms);
}

/** @copydoc window_observer_next_timeout_ms */
uint32_t window_observer_next_timeout_ms(const window_observer_t *observer, uint64_t now_ms) {
    if (observer == NULL || entry_gate(observer) != WindowObserverStatusOk ||
        observer->state != WindowObserverStateStarted) {
        return WindowObserverWaitForever;
    }
    if (refresh_due_core(observer, now_ms)) {
        return 0u;
    }
    if ((observer->dirty_count != 0u || observer->overflow != 0u) && observer->desktop_blocked == 0u) {
        return 0u;
    }
    /* No refresh is due, so an attempt exists and less than one interval has elapsed since it. */
    const uint64_t elapsed = attempt_elapsed(observer, now_ms);
    const uint64_t remaining = InteropWindowGraphRefreshIntervalMs > elapsed ? InteropWindowGraphRefreshIntervalMs - elapsed : 1u;
    return (uint32_t)(remaining < 1u ? 1u : remaining);
}

/* ------------------------------------------------------------------------------------------ */
/* Native Win32 section                                                                       */
/* ------------------------------------------------------------------------------------------ */

#ifdef _WIN32

/** Signature of GetDpiForWindow, resolved at run time because it exists only on Windows 10 1607+. */
typedef UINT(WINAPI *get_dpi_for_window_fn_t)(HWND);

/** Signature of GetThreadDpiAwarenessContext (Windows 10 1607+), resolved at run time. */
typedef DPI_AWARENESS_CONTEXT(WINAPI *get_thread_dpi_context_fn_t)(void);

/** Signature of AreDpiAwarenessContextsEqual (Windows 10 1607+), resolved at run time. */
typedef BOOL(WINAPI *are_dpi_contexts_equal_fn_t)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);

/** Resolved GetDpiForWindow; NULL until native_resolve_dpi() succeeds. */
static _Atomic(get_dpi_for_window_fn_t) g_get_dpi_for_window = NULL;

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT) - 4)
#endif

/** Maximum rect extent exported by the native query (equals InteropWindowGraphMaxExtent). */
#define NativeMaxExtent InteropWindowGraphMaxExtent

/** Bytes of UTF-8 a title may keep (equals InteropWindowGraphMaxTitleBytes). */
#define NativeTitleBytes 1024
/** WCHAR capacity of the title read buffer including the terminator. */
#define NativeTitleWchars 1025
/** WCHAR capacity of the class-name buffer including the terminator. */
#define NativeClassWchars 258
/** WCHAR capacity of the full process image path buffer (heap fallback for long paths). */
#define NativeImagePathWchars 32768
/** WCHAR capacity of the stack buffer tried first (MAX_PATH); the heap buffer is used only when it is too small. */
#define NativeImagePathSmallWchars 260
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

/*
 * Unsupported check of window_observer_create, run on the creating thread before any allocation:
 * GetDpiForWindow must exist and the thread's DPI awareness context must be per-monitor-aware V2
 * (rects and dpi are then physical pixels of a known scale). The observer never changes awareness.
 * The two awareness entry points arrived with GetDpiForWindow (Windows 10 1607) and are resolved the
 * same way; a missing one counts as unsupported.
 */
static int native_table_runnable(void) {
    if (native_resolve_dpi() == NULL) {
        return 0;
    }
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == NULL) {
        return 0;
    }
    FARPROC raw_get = GetProcAddress(user32, "GetThreadDpiAwarenessContext");
    FARPROC raw_equal = GetProcAddress(user32, "AreDpiAwarenessContextsEqual");
    if (raw_get == NULL || raw_equal == NULL) {
        return 0;
    }
    get_thread_dpi_context_fn_t get_context = NULL;
    are_dpi_contexts_equal_fn_t contexts_equal = NULL;
    memcpy(&get_context, &raw_get, sizeof(get_context));
    memcpy(&contexts_equal, &raw_equal, sizeof(contexts_equal));
    const DPI_AWARENESS_CONTEXT context = get_context();
    return contexts_equal(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ? 1 : 0;
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

/*
 * Per-call process snapshot used when OpenProcess cannot name a process (elevated or protected ones).
 * It is scoped by native_call_begin/native_call_end around one outermost pump or refresh call, is
 * touched only on the owner thread of the single Started observer, holds at most one Toolhelp32
 * handle and creates at most two snapshots per call: the first lazily, the second only to retry a
 * process-id miss against a snapshot taken earlier in the same call.
 */
typedef struct native_scan_t {
    HANDLE snapshot; /**< Current snapshot handle, INVALID_HANDLE_VALUE when none is open. */
    uint32_t created; /**< Snapshots created in this call, 0..2. */
} native_scan_t;

static native_scan_t g_native_scan = {INVALID_HANDLE_VALUE, 0u};
/** @brief One owner-thread pump/refresh or standalone native-query scope is active. */
static uint32_t g_native_scope_active = 0u;

/* Close the snapshot (if any) and reset the per-call state. */
static void native_call_end(void) {
    if (g_native_scan.snapshot != INVALID_HANDLE_VALUE) {
        CloseHandle(g_native_scan.snapshot);
    }
    g_native_scan.snapshot = INVALID_HANDLE_VALUE;
    g_native_scan.created = 0u;
    g_native_scope_active = 0u;
}

/* Start a call scope: no snapshot yet. A leftover handle (a call that never ended) is closed first. */
static void native_call_begin(void) {
    native_call_end();
    g_native_scope_active = 1u;
}

/* Create the next snapshot of the call; INVALID_HANDLE_VALUE when the per-call budget is spent or the OS refuses. */
static HANDLE native_scan_create(void) {
    if (g_native_scan.created >= 2u) {
        return INVALID_HANDLE_VALUE;
    }
    g_native_scan.created += 1u;
    g_native_scan.snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    return g_native_scan.snapshot;
}

/* Walk the current snapshot for process_id; *found tells whether the id exists; returns the basename bytes. */
static uint32_t native_scan_walk(DWORD process_id, uint8_t *dest, int *found) {
    PROCESSENTRY32W entry;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = (DWORD)sizeof(entry);
    *found = 0;
    if (!Process32FirstW(g_native_scan.snapshot, &entry)) {
        return 0u;
    }
    do {
        if (entry.th32ProcessID == process_id) {
            *found = 1;
            entry.szExeFile[MAX_PATH - 1] = L'\0';
            int cut = 0;
            return utf8_from_wide(entry.szExeFile, (int)wcslen(entry.szExeFile), WC_ERR_INVALID_CHARS, 0, dest,
                                  InteropWindowGraphMaxImageBytes, &cut);
        }
    } while (Process32NextW(g_native_scan.snapshot, &entry));
    return 0u;
}

/* Resolve the image basename of process_id from the call's snapshot; 0 when unknown. */
static uint32_t native_scan_image_name(DWORD process_id, uint8_t *dest) {
    int fresh = 0;
    if (g_native_scan.snapshot == INVALID_HANDLE_VALUE) {
        if (g_native_scan.created != 0u || native_scan_create() == INVALID_HANDLE_VALUE) {
            return 0u; /* an earlier creation in this call failed, or this one does */
        }
        fresh = 1;
    }
    int found = 0;
    const uint32_t bytes = native_scan_walk(process_id, dest, &found);
    if (found || fresh || g_native_scan.created >= 2u) {
        return bytes;
    }
    /* Miss against a snapshot from earlier in this call: retake it once. */
    CloseHandle(g_native_scan.snapshot);
    g_native_scan.snapshot = INVALID_HANDLE_VALUE;
    if (native_scan_create() == INVALID_HANDLE_VALUE) {
        return 0u;
    }
    return native_scan_walk(process_id, dest, &found);
}

/* Basename (after the last separator) of a UTF-16 path to UTF-8 image bytes; 0 when empty or not convertible. */
static uint32_t native_image_basename(const WCHAR *path, DWORD path_length, interop_window_facts_t *facts) {
    DWORD base = 0;
    for (DWORD i = 0; i < path_length; i++) {
        if (path[i] == L'\\' || path[i] == L'/') {
            base = i + 1u;
        }
    }
    int cut = 0;
    return utf8_from_wide(path + base, (int)(path_length - base), WC_ERR_INVALID_CHARS, 0, facts->image_name,
                          InteropWindowGraphMaxImageBytes, &cut);
}

/*
 * QueryFullProcessImageName into the facts. The 260-WCHAR stack buffer is tried first; only when the OS
 * answers ERROR_INSUFFICIENT_BUFFER does a 32768-WCHAR (64 KiB) heap buffer get allocated, used once and
 * freed on every path before this function returns. Returns 0 only when that heap buffer cannot be
 * allocated (the caller reports Failed); *image_bytes is 0 when the OS gave no usable basename.
 */
static int native_query_image(HANDLE process, interop_window_facts_t *facts, uint32_t *image_bytes) {
    WCHAR small[NativeImagePathSmallWchars];
    DWORD path_length = NativeImagePathSmallWchars;
    *image_bytes = 0u;
    if (QueryFullProcessImageNameW(process, 0, small, &path_length)) {
        if (path_length > 0u) {
            *image_bytes = native_image_basename(small, path_length, facts);
        }
        return 1;
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return 1;
    }
    WCHAR *path = malloc((size_t)NativeImagePathWchars * sizeof(WCHAR));
    if (path == NULL) {
        return 0;
    }
    path_length = NativeImagePathWchars;
    if (QueryFullProcessImageNameW(process, 0, path, &path_length) && path_length > 0u) {
        *image_bytes = native_image_basename(path, path_length, facts);
    }
    free(path);
    path = NULL;
    return 1;
}

/*
 * Fill image basename and process start time. Step 1: OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)
 * with QueryFullProcessImageName gives the basename and GetProcessTimes the real start time. Step 2,
 * when OpenProcess fails (or yields no basename): the basename comes from the per-call Toolhelp32
 * snapshot and the start time stays what step 1 produced (0 when OpenProcess failed). ImageUnknown is
 * set, with image_length 0, only when both steps fail. With want_image 0 (the window is not visible,
 * so its classification is NotVisible whatever its image) only the start time is read: no path query,
 * no allocation and no snapshot; image_length is 0 and ImageUnknown is set. Returns 0 only when the
 * heap fallback buffer of a long image path cannot be allocated (the caller reports Failed: a missing
 * image would silently disable UAC rules).
 */
static int native_fill_process(DWORD process_id, interop_window_facts_t *facts, int want_image) {
    uint32_t image_bytes = 0u;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process != NULL) {
        if (want_image && !native_query_image(process, facts, &image_bytes)) {
            CloseHandle(process);
            return 0;
        }
        FILETIME created;
        FILETIME exited;
        FILETIME kernel;
        FILETIME user;
        if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
            facts->process_start_time = ((uint64_t)created.dwHighDateTime << 32) | (uint64_t)created.dwLowDateTime;
        }
        CloseHandle(process);
    }
    if (image_bytes == 0u && want_image) {
        image_bytes = native_scan_image_name(process_id, facts->image_name);
    }
    facts->image_length = image_bytes;
    if (image_bytes == 0u) {
        facts->flags |= (uint32_t)InteropWindowFactImageUnknown;
    }
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
    /* Clamp to the graph maximum: an oversized window is exported at the maximum, not rejected. */
    const uint32_t width = (uint32_t)((int64_t)rect.right - (int64_t)rect.left);
    const uint32_t height = (uint32_t)((int64_t)rect.bottom - (int64_t)rect.top);
    facts->width = width > NativeMaxExtent ? NativeMaxExtent : width;
    facts->height = height > NativeMaxExtent ? NativeMaxExtent : height;
    *screen_rect = rect;
    return 1;
}

/* Compute the Fullscreen flag: -1 indeterminate, 0 false, 1 true. */
static int native_is_fullscreen(HWND window, int iconic, uint32_t style, const RECT *rect) {
    if (iconic || (style & WS_CAPTION) == WS_CAPTION) {
        return 0;
    }
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    MONITORINFO info;
    memset(&info, 0, sizeof(info));
    info.cbSize = (DWORD)sizeof(info);
    if (monitor == NULL) {
        return 0;
    }
    if (!GetMonitorInfoW(monitor, &info)) return -1;
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
static window_observer_query_result_t native_query_facts_inner(void *context, uint64_t window,
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
    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(handle, &process_id);
    if (thread_id == 0u || process_id == 0u) {
        return native_read_failed(handle);
    }
    facts->window = window;
    if (!native_fill_relations(handle, facts) || !native_fill_styles(handle, facts)) {
        return native_read_failed(handle);
    }
    facts->process_id = (uint32_t)process_id;
    facts->thread_id = (uint32_t)thread_id;

    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(handle, DWMWA_CLOAKED, &cloaked, (DWORD)sizeof(cloaked)))) {
        return native_read_failed(handle);
    }
    const int iconic = IsIconic(handle) ? 1 : 0;
    /*
     * Visible: IsWindowVisible and not DWM-cloaked. A window without it is classified NotVisible
     * whatever else its facts say, so the reads that exist only for the later classification steps
     * (rect and fullscreen test, layered alpha, title, image path, Toolhelp fallback) are skipped:
     * the 250 ms refresh queries every enumerated top-level window, hidden ones included. What stays is
     * what the identity tuple (process, thread, start time, class hash) and validation need: relations,
     * styles, class, dpi, process id, thread id and process start time. Gone and Failed are decided
     * exactly as for a visible window.
     */
    const int visible = IsWindowVisible(handle) && cloaked == 0u;
    uint32_t flags = 0u;
    if (visible) {
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
        if (visible && GetLayeredWindowAttributes(handle, NULL, &alpha, &layered_flags) &&
            (layered_flags & LWA_ALPHA) != 0u) {
            flags |= (uint32_t)InteropWindowFactLayeredAlphaKnown;
            facts->alpha = (uint8_t)alpha;
        }
    }
    facts->flags = flags;

    if (visible) {
        RECT rect;
        if (!native_fill_rect(handle, iconic, facts, &rect)) {
            return native_read_failed(handle);
        }
        const int fullscreen = native_is_fullscreen(handle, iconic, facts->style, &rect);
        if (fullscreen < 0) {
            return native_read_failed(handle);
        }
        if (fullscreen != 0) {
            facts->flags |= (uint32_t)InteropWindowFactFullscreen;
        }
    }
    const UINT dpi = get_dpi(handle);
    if (dpi == 0u) {
        return native_read_failed(handle);
    }
    facts->dpi = (uint32_t)dpi;
    /*
     * GetWindowTextW on a window of the current process sends WM_GETTEXT, which can deadlock or
     * re-enter the owner thread. Own-process windows (exactly the AgentOwned set) therefore get no
     * title (title_length stays 0) and receive no message at all. Invisible windows get no title either.
     */
    const int own_process = process_id == GetCurrentProcessId();
    if (!native_fill_class(handle, facts) || (visible && !own_process && !native_fill_title(handle, facts)) ||
        !native_fill_process(process_id, facts, visible)) {
        return native_read_failed(handle);
    }
    if (!IsWindow(handle)) {
        return native_gone_probe(handle); /* vanished mid-read: the partial facts are discarded */
    }
    DWORD final_process_id = 0;
    const DWORD final_thread_id = GetWindowThreadProcessId(handle, &final_process_id);
    if (final_thread_id == 0u || final_process_id == 0u || final_thread_id != thread_id ||
        final_process_id != process_id) {
        return native_read_failed(handle);
    }
    return WindowObserverQueryExists;
}

/**
 * @brief Query with a deterministic scan lifetime, including direct native-table calls.
 * @param[in,out] context Borrowed native context; NULL.
 * @param[in] window HWND value; no native ownership transfers.
 * @param[out] facts Caller-owned snapshot, meaningful only on Exists.
 * @return Exists, Gone or Failed; every standalone scan handle is closed on return.
 * @note Owner thread of the Started native observer only. Pump/refresh share their
 *       existing scope; a direct query owns one temporary scope across all returns.
 */
static window_observer_query_result_t native_query_facts(void *context, uint64_t window,
                                                         interop_window_facts_t *facts) {
    const int standalone = g_native_scope_active == 0u;
    if (standalone) native_call_begin();
    const window_observer_query_result_t result = native_query_facts_inner(context, window, facts);
    if (standalone) native_call_end();
    return result;
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
