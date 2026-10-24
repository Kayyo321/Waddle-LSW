#ifndef WaddleWindowObserverWindowsH
#define WaddleWindowObserverWindowsH
/**
 * @file window_observer_windows.h
 * @brief Win32 observation adapter feeding the session window graph (TODO #10).
 *
 * The observer watches the interactive desktop session, not one process tree. WinEvent
 * callbacks are only hints: they mark a window handle dirty in a bounded table and never
 * carry facts. The observer's owner thread later pops dirty handles, re-queries the current
 * native facts through window_observer_api_t, and feeds them to a caller-owned
 * interop_window_graph_t (observe, note_destroyed, refresh_*). All Win32 access goes through
 * the api table so a test double can replace it on any platform. The header contains no
 * Win32 types; only window_observer_native_api() exists under _WIN32.
 *
 * Boundaries. The observer owns its hooks, its dirty table and its enumeration buffer and
 * nothing else. It never owns, retires or frees graph state, never produces frame-drain
 * proof, never sends a protocol message (no DesktopSwitch, no Hello), and never sends
 * messages to foreign windows (no WM_GETTEXT, no WM_GETMINMAXINFO). Minimum tracking size is
 * therefore not queried: min_width and min_height are always 0 (remaining work).
 *
 * Threading. An observer is bound to the thread whose api->current_thread_id returned a value
 * at create time (the owner thread, which must outlive the observer and run a message loop so
 * out-of-context hooks are delivered). Every function except window_observer_note_hint must
 * run on that thread; a call from another thread returns WrongThread (or 0 / WaitForever for
 * the two scalar queries) without touching any state. Only one observer per process may be
 * Started at a time because hook procedures carry no context: a process-wide atomic slot names
 * the active observer, and start() on a second observer returns AlreadyActive.
 *
 * Identity continuity. The observer does not stamp foreign windows with properties (SetProp
 * would mutate other processes' windows, can clash with unrelated properties, and is blocked by
 * UIPI for elevated windows). Continuity is proven by the graph from the identity tuple
 * (process id, thread id, process start time, class hash) plus the destroy-hint rule of
 * interop_window_graph_note_destroyed(). Residual risk: a window destroyed and recreated with
 * the identical tuple and the same HWND, with the destroy hint lost by the OS and no
 * hint-table overflow, is indistinguishable from the old incarnation.
 *
 * Access denied. When the process image or start time cannot be read (typical for elevated or
 * protected processes under UIPI) the native query reports ImageUnknown, image_length 0 and
 * process_start_time 0; classification then never matches a UAC or CoreWindow rule for that
 * window. Nothing here claims UIAccess or secure-desktop visibility.
 *
 * Desktop switch. When the input desktop is not Default, or cannot be determined, pump and
 * refresh process nothing, keep queued hints, count the event, and latch a full refresh for
 * the moment the desktop returns. The observer produces no protocol message for it.
 *
 * Graph status mapping used by pump and refresh for every graph call:
 *  - Ok: continue.
 *  - InvalidFacts, StaleFacts, Cycle, AtCapacity, GenerationExhausted, LookupFailed,
 *    AppKeyInvalid, Stale: counted in graph_errors, the key is dropped, processing continues;
 *    the next refresh re-offers the window.
 *  - QueueFull: the key is put back (with its destroy information), processing stops, the
 *    call returns GraphQueueFull; the coordinator drains graph events and calls again.
 *  - anything else (StaleEpoch, WrongThread, Reentrant, InvalidArgument, NoRefresh, ...): the
 *    key is put back, processing stops, the call returns GraphError; last_graph_status holds
 *    the exact status.
 */
#include <stddef.h>
#include <stdint.h>

#include "waddle/interop_window_graph.h"

/** @brief Capacity of the dirty-handle table (distinct HWNDs awaiting a requery). */
#define WindowObserverDirtyCapacity 512u
/** @brief Capacity of the enumeration buffer; more top-level windows make a refresh fail closed. */
#define WindowObserverMaxEnumerated 2048u
/** @brief Extra requery attempts after a Failed query before the key is dropped (until the next refresh). */
#define WindowObserverQueryRetries 1u
/** @brief Value of next_timeout_ms when nothing is scheduled (Win32 INFINITE). */
#define WindowObserverWaitForever 0xFFFFFFFFu

/** @brief WinEvent id EVENT_OBJECT_CREATE. */
#define WindowObserverEventObjectCreate 0x8000u
/** @brief WinEvent id EVENT_OBJECT_DESTROY; the only event that carries destroy authority. */
#define WindowObserverEventObjectDestroy 0x8001u
/** @brief WinEvent id EVENT_OBJECT_SHOW. */
#define WindowObserverEventObjectShow 0x8002u
/** @brief WinEvent id EVENT_OBJECT_HIDE. */
#define WindowObserverEventObjectHide 0x8003u
/** @brief WinEvent id EVENT_OBJECT_NAMECHANGE; upper end of the object range (REORDER, STATECHANGE and LOCATIONCHANGE lie inside it). */
#define WindowObserverEventObjectNameChange 0x800Cu
/** @brief WinEvent id EVENT_SYSTEM_MINIMIZESTART; lower end of the system range. */
#define WindowObserverEventSystemMinimizeStart 0x0016u
/** @brief WinEvent id EVENT_SYSTEM_MINIMIZEEND; upper end of the system range. */
#define WindowObserverEventSystemMinimizeEnd 0x0017u
/** @brief OBJID_WINDOW: the only accessible-object id a hint may carry. */
#define WindowObserverObjectIdWindow 0
/** @brief CHILDID_SELF: the only child id a hint may carry. */
#define WindowObserverChildIdSelf 0

/** @brief Observer lifecycle state, reported in window_observer_stats_t.state. */
typedef enum window_observer_state_t {
    WindowObserverStateCreated = 1, /**< Created, never started; no hooks exist. */
    WindowObserverStateStarted = 2, /**< Hooks installed; hints accepted; pump and refresh allowed. */
    WindowObserverStateStopped = 3, /**< Stopped; no hooks; may be started again or destroyed. */
    WindowObserverStateFailed = 4 /**< Hook install or removal failed; terminal, only destroy is allowed. */
} window_observer_state_t;

/** @brief Result codes of the observer functions. Only Ok and More are fully successful. */
typedef enum window_observer_status_t {
    WindowObserverStatusOk = 0, /**< Completed; for pump, no hint remains queued. */
    WindowObserverStatusMore = 1, /**< pump only: budget exhausted with hints still queued; call again. */
    WindowObserverStatusDesktopInactive = 2, /**< Input desktop is not Default or unknown; nothing processed, graph untouched, hints kept, full refresh latched. */
    WindowObserverStatusGraphQueueFull = 3, /**< Graph reported QueueFull; the coordinator must drain graph events and call again. */
    WindowObserverStatusInvalidArgument = 4, /**< NULL pointer, zero budget/stamp/process id, malformed api table or config; nothing changed. */
    WindowObserverStatusWrongThread = 5, /**< Not the owner thread; nothing touched (a destroy does not free). */
    WindowObserverStatusNoMemory = 6, /**< create could not allocate; no observer exists. */
    WindowObserverStatusAlreadyActive = 7, /**< Another observer in this process is Started; this one stays unchanged. */
    WindowObserverStatusBadState = 8, /**< The call is illegal in the current lifecycle state; nothing changed. */
    WindowObserverStatusHookFailed = 9, /**< A hook could not be installed; installed hooks were removed, state Failed. */
    WindowObserverStatusUnhookFailed = 10, /**< A hook could not be removed; state Failed, restart refused. */
    WindowObserverStatusGraphError = 11, /**< Graph returned an unexpected status (see last_graph_status); processing stopped. */
    WindowObserverStatusEnumFailed = 12, /**< Enumeration failed or was truncated; no refresh was begun, nothing retired. */
    WindowObserverStatusQueryFailed = 13, /**< A fact query failed during refresh; the refresh was aborted, nothing retired. */
    WindowObserverStatusStampExhausted = 14 /**< The logical clock cannot issue another stamp; the observer must be replaced. */
} window_observer_status_t;

/** @brief Outcome of one native fact query. */
typedef enum window_observer_query_result_t {
    WindowObserverQueryExists = 1, /**< The window currently exists and facts is fully populated. */
    WindowObserverQueryGone = 2, /**< The OS affirmatively reports the handle is not a valid window (the only result that may lead to retirement). */
    WindowObserverQueryFailed = 3 /**< The state could not be determined (transient error, desktop mismatch, partial read); never causes retirement. */
} window_observer_query_result_t;

/** @brief Outcome of one native top-level enumeration. */
typedef enum window_observer_enum_result_t {
    WindowObserverEnumOk = 1, /**< The list is complete. */
    WindowObserverEnumTruncated = 2, /**< More windows exist than capacity; the list is partial and must not drive retirement. */
    WindowObserverEnumFailed = 3 /**< The enumeration failed; count is meaningless. */
} window_observer_enum_result_t;

/** @brief Classification of the current input desktop. */
typedef enum window_observer_desktop_t {
    WindowObserverDesktopDefault = 1, /**< The input desktop is the interactive Default desktop. */
    WindowObserverDesktopOther = 2, /**< Another desktop (for example Winlogon) has input. */
    WindowObserverDesktopDenied = 3 /**< The desktop could not be opened or named; treated like Other. */
} window_observer_desktop_t;

/** @brief Opaque observer handle; allocated by create, freed by destroy; layout is private to the C file. */
typedef struct window_observer_t window_observer_t;

/**
 * @brief Table of every native operation the observer needs. All pointers are required (non-NULL)
 *        and every entry is called only on the owner thread, except current_thread_id, which the
 *        hint entry point also calls from the hook procedure.
 *
 * The table is copied into the observer at create; api_context (from the config) is passed to
 * every entry as its first argument and must stay valid until destroy.
 */
typedef struct window_observer_api_t {
    /**
     * @brief Install one out-of-context WinEvent hook for all processes and threads whose events
     *        reach window_observer_note_hint.
     * @param[in,out] context The config's api_context; may be NULL.
     * @param[in] event_min Lowest WinEvent id delivered.
     * @param[in] event_max Highest WinEvent id delivered (inclusive).
     * @param[out] hook Non-NULL; receives a nonzero opaque token on success, untouched on failure.
     * @return 0 on success, nonzero on failure. start() calls this exactly twice, in order:
     *         (WindowObserverEventObjectCreate, WindowObserverEventObjectNameChange) then
     *         (WindowObserverEventSystemMinimizeStart, WindowObserverEventSystemMinimizeEnd).
     */
    int (*install_hook)(void *context, uint32_t event_min, uint32_t event_max, uintptr_t *hook);
    /**
     * @brief Remove a hook previously installed by install_hook.
     * @param[in,out] context The config's api_context; may be NULL.
     * @param[in] hook Token from install_hook; never 0.
     * @return 0 on success, nonzero on failure (the hook state is then unknown and is not retried).
     *         stop() and failed-start cleanup call it once per installed hook, in reverse order.
     */
    int (*remove_hook)(void *context, uintptr_t hook);
    /**
     * @brief Read the current native facts of one window without sending messages to its owner.
     *
     * The observer zero-fills facts before the call and, after Exists, overwrites stamp,
     * captured_ms and the AgentOwned, AgentPreedit and OnInputDesktop flags. The callee fills
     * everything else: window, owner (GW_OWNER), owner_root (GA_ROOT of owner), root (GA_ROOT of
     * window), style and ex_style (low 32 bits), Enabled, Iconic, Zoomed, Layered,
     * LayeredAlphaKnown and alpha, Fullscreen, ClassTruncated, ImageUnknown, rect (DWM extended
     * frame bounds with GetWindowRect fallback; restored geometry when iconic), dpi, process id,
     * thread id, process start time (0 if unreadable), class_name, image_name (basename only) and
     * title. Visible must be set only if the window is visible and not DWM-cloaked. Titles and
     * class names are converted to UTF-8, cut at a code-point boundary, and unpaired surrogates
     * become U+FFFD. min_width and min_height stay 0.
     *
     * @param[in,out] context The config's api_context; may be NULL.
     * @param[in] window Nonzero HWND value.
     * @param[out] facts Non-NULL pre-zeroed struct; contents are meaningful only for Exists.
     * @return Exists, Gone (only when the OS reports an invalid handle) or Failed. A facts->window
     *         different from the requested window is treated by the observer as Failed.
     */
    window_observer_query_result_t (*query_facts)(void *context, uint64_t window,
                                                  interop_window_facts_t *facts);
    /**
     * @brief Snapshot the top-level windows in z-order, front to back.
     * @param[in,out] context The config's api_context; may be NULL.
     * @param[out] windows Non-NULL array of capacity elements (always WindowObserverMaxEnumerated).
     * @param[in] capacity Elements available.
     * @param[out] count Non-NULL; number of valid entries written (at most capacity) for Ok and Truncated.
     * @return Ok, Truncated (partial list) or Failed. Entries are nonzero; duplicates are tolerated.
     */
    window_observer_enum_result_t (*enum_top_level_windows)(void *context, uint64_t *windows,
                                                            uint32_t capacity, uint32_t *count);
    /**
     * @brief Classify the current input desktop.
     * @param[in,out] context The config's api_context; may be NULL.
     * @return Default only if the desktop is positively identified as Default; Denied on access
     *         failure; Other for any other desktop.
     */
    window_observer_desktop_t (*query_input_desktop)(void *context);
    /**
     * @brief Identify the calling thread; also used inside the hook procedure, so it must be
     *        trivial, non-blocking and callable on any thread.
     * @param[in,out] context The config's api_context; may be NULL.
     * @return A thread id that is stable and unique among live threads.
     */
    uint32_t (*current_thread_id)(void *context);
    /**
     * @brief Monotonic millisecond clock for the refresh cadence.
     * @param[in,out] context The config's api_context; may be NULL.
     * @return Milliseconds, never decreasing.
     */
    uint64_t (*monotonic_ms)(void *context);
    /**
     * @brief 32-bit tick clock in the same domain as the WinEvent event-time field (wraps; compared
     *        with wrap-safe arithmetic by the graph).
     * @param[in,out] context The config's api_context; may be NULL.
     * @return Current tick in milliseconds.
     */
    uint32_t (*tick_ms)(void *context);
} window_observer_api_t;

/** @brief Observer creation parameters. The struct is copied; pointers inside are borrowed. */
typedef struct window_observer_config_t {
    const window_observer_api_t *api; /**< Native table; NULL selects the native table under _WIN32 and is InvalidArgument elsewhere. */
    void *api_context; /**< Passed to every api entry; must outlive the observer; may be NULL. */
    uint64_t first_stamp; /**< First logical-clock value to issue; nonzero and above the graph's stamp_high_water. */
    uint64_t preedit_window; /**< HWND of the agent's IME preedit window; 0 = none yet. */
    uint32_t own_process_id; /**< Nonzero id of the agent's own process; its windows are AgentOwned. */
    uint32_t reserved0; /**< Must be zero. */
} window_observer_config_t;

/** @brief Cumulative observer counters and gauges. Counters start at 0 at create, saturate, never wrap. */
typedef struct window_observer_stats_t {
    uint64_t next_stamp; /**< Next logical-clock value that will be issued. */
    uint32_t state; /**< window_observer_state_t. */
    uint32_t last_graph_status; /**< interop_window_graph_status_t of the latest graph call that returned neither Ok nor None; Ok if none yet. */
    uint32_t dirty_pending; /**< Gauge: distinct HWNDs queued for requery. */
    uint32_t desktop_active; /**< Gauge: 1 if the latest desktop check returned Default. */
    uint32_t overflow_latched; /**< Gauge: 1 while a hint-table overflow awaits recovery. */
    uint32_t refresh_requested; /**< Gauge: 1 while a full refresh is latched as due. */
    uint64_t hints_received; /**< Hints accepted into the table. */
    uint64_t hints_filtered; /**< Hints ignored: wrong object/child id, handle 0, or event outside the hooked ranges. */
    uint64_t hints_wrong_thread; /**< Hints ignored because they arrived off the owner thread (the one counter updated atomically from other threads). */
    uint64_t hints_coalesced; /**< Hints merged into an existing table entry. */
    uint64_t hint_overflows; /**< Hints that found the table full and latched overflow. */
    uint64_t keys_processed; /**< Table entries popped by pump. */
    uint64_t queries_exists; /**< Queries that returned Exists. */
    uint64_t queries_gone; /**< Queries that returned Gone. */
    uint64_t queries_failed; /**< Queries that returned Failed (or a mismatching window). */
    uint64_t query_retries_exhausted; /**< Keys dropped after WindowObserverQueryRetries failed retries. */
    uint64_t destroys_noted; /**< note_destroyed calls issued. */
    uint64_t graph_errors; /**< Graph statuses handled by dropping a key or stopping (see the mapping in the file comment). */
    uint64_t pumps_desktop_inactive; /**< pump or refresh calls refused because the desktop was not Default. */
    uint64_t refreshes_started; /**< refresh_begin calls issued. */
    uint64_t refreshes_committed; /**< Refreshes committed. */
    uint64_t refreshes_aborted; /**< Refreshes aborted by the observer. */
    uint64_t invalidations; /**< invalidate_all calls issued after an overflow. */
    uint64_t uac_native_hidden; /**< Offers whose result reason was UacNativeHidden (a consent window exists but is hidden from export). */
} window_observer_stats_t;

_Static_assert(sizeof(window_observer_state_t) == 4, "observer state enum ABI");
_Static_assert(sizeof(window_observer_status_t) == 4, "observer status enum ABI");
_Static_assert(sizeof(window_observer_query_result_t) == 4, "query result enum ABI");
_Static_assert(sizeof(window_observer_enum_result_t) == 4, "enum result enum ABI");
_Static_assert(sizeof(window_observer_desktop_t) == 4, "desktop enum ABI");
_Static_assert(WindowObserverDirtyCapacity == 512u, "dirty table bound is frozen by the design contract");
_Static_assert(WindowObserverMaxEnumerated >= 4u * InteropMaxWindows, "enumeration buffer must exceed the exported bound");

_Static_assert(sizeof(window_observer_api_t) == 64, "observer api table ABI");
_Static_assert(offsetof(window_observer_api_t, install_hook) == 0, "api install_hook ABI");
_Static_assert(offsetof(window_observer_api_t, remove_hook) == 8, "api remove_hook ABI");
_Static_assert(offsetof(window_observer_api_t, query_facts) == 16, "api query_facts ABI");
_Static_assert(offsetof(window_observer_api_t, enum_top_level_windows) == 24, "api enum ABI");
_Static_assert(offsetof(window_observer_api_t, query_input_desktop) == 32, "api desktop ABI");
_Static_assert(offsetof(window_observer_api_t, current_thread_id) == 40, "api thread ABI");
_Static_assert(offsetof(window_observer_api_t, monotonic_ms) == 48, "api monotonic ABI");
_Static_assert(offsetof(window_observer_api_t, tick_ms) == 56, "api tick ABI");

_Static_assert(sizeof(window_observer_config_t) == 40, "observer config ABI");
_Static_assert(offsetof(window_observer_config_t, api) == 0, "config api ABI");
_Static_assert(offsetof(window_observer_config_t, api_context) == 8, "config context ABI");
_Static_assert(offsetof(window_observer_config_t, first_stamp) == 16, "config first_stamp ABI");
_Static_assert(offsetof(window_observer_config_t, preedit_window) == 24, "config preedit ABI");
_Static_assert(offsetof(window_observer_config_t, own_process_id) == 32, "config process ABI");
_Static_assert(offsetof(window_observer_config_t, reserved0) == 36, "config reserved ABI");

_Static_assert(sizeof(window_observer_stats_t) == 176, "observer stats ABI");
_Static_assert(offsetof(window_observer_stats_t, next_stamp) == 0, "stats next_stamp ABI");
_Static_assert(offsetof(window_observer_stats_t, state) == 8, "stats state ABI");
_Static_assert(offsetof(window_observer_stats_t, last_graph_status) == 12, "stats graph status ABI");
_Static_assert(offsetof(window_observer_stats_t, dirty_pending) == 16, "stats dirty ABI");
_Static_assert(offsetof(window_observer_stats_t, desktop_active) == 20, "stats desktop ABI");
_Static_assert(offsetof(window_observer_stats_t, overflow_latched) == 24, "stats overflow ABI");
_Static_assert(offsetof(window_observer_stats_t, refresh_requested) == 28, "stats refresh requested ABI");
_Static_assert(offsetof(window_observer_stats_t, hints_received) == 32, "stats hints received ABI");
_Static_assert(offsetof(window_observer_stats_t, hints_filtered) == 40, "stats hints filtered ABI");
_Static_assert(offsetof(window_observer_stats_t, hints_wrong_thread) == 48, "stats hints wrong thread ABI");
_Static_assert(offsetof(window_observer_stats_t, hints_coalesced) == 56, "stats hints coalesced ABI");
_Static_assert(offsetof(window_observer_stats_t, hint_overflows) == 64, "stats hint overflows ABI");
_Static_assert(offsetof(window_observer_stats_t, keys_processed) == 72, "stats keys ABI");
_Static_assert(offsetof(window_observer_stats_t, queries_exists) == 80, "stats exists ABI");
_Static_assert(offsetof(window_observer_stats_t, queries_gone) == 88, "stats gone ABI");
_Static_assert(offsetof(window_observer_stats_t, queries_failed) == 96, "stats failed ABI");
_Static_assert(offsetof(window_observer_stats_t, query_retries_exhausted) == 104, "stats retries ABI");
_Static_assert(offsetof(window_observer_stats_t, destroys_noted) == 112, "stats destroys ABI");
_Static_assert(offsetof(window_observer_stats_t, graph_errors) == 120, "stats graph errors ABI");
_Static_assert(offsetof(window_observer_stats_t, pumps_desktop_inactive) == 128, "stats inactive ABI");
_Static_assert(offsetof(window_observer_stats_t, refreshes_started) == 136, "stats refreshes started ABI");
_Static_assert(offsetof(window_observer_stats_t, refreshes_committed) == 144, "stats refreshes committed ABI");
_Static_assert(offsetof(window_observer_stats_t, refreshes_aborted) == 152, "stats refreshes aborted ABI");
_Static_assert(offsetof(window_observer_stats_t, invalidations) == 160, "stats invalidations ABI");
_Static_assert(offsetof(window_observer_stats_t, uac_native_hidden) == 168, "stats uac ABI");

/**
 * @brief Create an observer in state Created. No hook is installed and no hint is accepted yet.
 *
 * @param[in] config Non-NULL borrowed config (copied). api NULL means the native table under _WIN32
 *                   and InvalidArgument elsewhere; a non-NULL api must have every entry non-NULL.
 *                   first_stamp and own_process_id nonzero, reserved0 zero.
 * @param[out] out_observer Non-NULL caller slot. On Ok it receives the handle (owned by the caller
 *                          until destroy). On every failure it is set to NULL once the slot itself was valid.
 * @return Ok; InvalidArgument; NoMemory (a single allocation holds the observer, dirty table and
 *         enumeration buffer; nothing leaks on failure).
 * @note The owner thread is the thread id returned by api->current_thread_id during this call.
 *       Does not install hooks, query the OS, or touch any graph.
 */
window_observer_status_t window_observer_create(const window_observer_config_t *config,
                                                window_observer_t **out_observer);

/**
 * @brief Stop (if started), free the observer and set the caller's handle to NULL.
 *
 * Frees even when removing a hook fails; the process-wide slot is released first, so a hook that
 * could not be removed delivers only ignored hints afterwards. The observer never touches the
 * graph, so destroy order relative to the graph is free.
 *
 * @param[in,out] observer_slot Non-NULL caller slot. If *observer_slot is NULL this is a successful
 *                              no-op. On Ok and UnhookFailed the observer is freed and *observer_slot
 *                              is NULL; on any other status nothing is freed and the slot is unchanged.
 * @return Ok; UnhookFailed (freed, but a hook leaked until process exit); InvalidArgument for a NULL
 *         slot; WrongThread.
 * @note Owner thread only. Not safe against concurrent use of the same observer.
 */
window_observer_status_t window_observer_destroy(window_observer_t **observer_slot);

/**
 * @brief Claim the process-wide active slot, clear the dirty table, install both hooks, and latch a
 *        full refresh as due.
 *
 * Allowed from Created and Stopped (restart; the logical clock keeps counting up). On HookFailed the
 * hooks already installed are removed, the slot is released, and the state becomes Failed.
 *
 * @param[in,out] observer Non-NULL handle.
 * @return Ok; InvalidArgument; WrongThread; BadState (already Started, or Failed); AlreadyActive
 *         (another observer holds the slot; this one unchanged); HookFailed (state Failed, nothing
 *         installed that could be removed).
 * @note Owner thread only. Calls api->install_hook; may dispatch hint callbacks only after it returns.
 */
window_observer_status_t window_observer_start(window_observer_t *observer);

/**
 * @brief Cease hint production: release the active slot, remove every hook (reverse order), clear
 *        the dirty table, the overflow latch and the refresh latch.
 *
 * Never touches the graph: nothing is retired, freed or proven; the coordinator decides what happens
 * to the graph. Stopping a Created or Stopped observer succeeds without effect.
 *
 * @param[in,out] observer Non-NULL handle.
 * @return Ok; UnhookFailed (every hook was still attempted, state becomes Failed and restart is
 *         refused); InvalidArgument; WrongThread; BadState (observer is Failed).
 * @note Owner thread only. Calls api->remove_hook.
 */
window_observer_status_t window_observer_stop(window_observer_t *observer);

/**
 * @brief Tell the observer which window is the agent's IME preedit window.
 *
 * The change applies to the next query of each window. A change while Started latches a full refresh
 * so the old and new preedit windows are reclassified.
 *
 * @param[in,out] observer Non-NULL handle.
 * @param[in] window HWND value, or 0 for none.
 * @return Ok; InvalidArgument; WrongThread.
 * @note Owner thread only; no OS or graph access.
 */
window_observer_status_t window_observer_set_preedit_window(window_observer_t *observer, uint64_t window);

/**
 * @brief Process queued hints against the graph.
 *
 * Order: if the input desktop is not Default, process nothing and return DesktopInactive. If the
 * overflow latch is set, call invalidate_all (QueueFull returns GraphQueueFull with the latch kept),
 * clear the table and latch, and run a full refresh in this call, whose status is returned. Otherwise
 * pop at most min(budget, entries present at call start) keys in first-seen order; each key is removed
 * from the table before its facts are queried, so a hint arriving during the query re-adds it. Per key:
 * if it carries a destroy stamp, call note_destroyed with that stamp and event time; then draw a fresh
 * stamp (and a tick sample) before querying; Exists calls observe, Gone calls note_destroyed with the
 * fresh stamp and tick, and Failed puts the key back (at the tail, with its attempt count) up to
 * WindowObserverQueryRetries times and then drops it: a failed query never retires anything. Graph
 * statuses follow the mapping in the file comment.
 *
 * @param[in,out] observer Non-NULL Started handle.
 * @param[in,out] graph Non-NULL graph owned by the caller, mutated only on its owner thread (the
 *                      owner threads of graph and observer must be the same thread).
 * @param[in] session_epoch Captured epoch of the graph; passed to every graph call, so a mismatch
 *                          yields GraphError with last_graph_status StaleEpoch and no graph effect.
 * @param[in] budget At least 1; maximum keys popped in this call.
 * @param[out] stats Nullable; receives a snapshot of the cumulative counters after the call
 *                   (also on non-Ok statuses other than InvalidArgument and WrongThread).
 * @return Ok (the table is empty on return); More (hints remain: budget exhausted, or a key was put
 *         back after a failed query); DesktopInactive;
 *         GraphQueueFull; GraphError; StampExhausted; BadState (not Started); InvalidArgument;
 *         WrongThread.
 * @note Owner thread only; calls api query entries and the graph; does not allocate. Graph
 *       callbacks (app-key lookup) run inside observe.
 */
window_observer_status_t window_observer_pump(window_observer_t *observer, interop_window_graph_t *graph,
                                              uint64_t session_epoch, uint32_t budget,
                                              window_observer_stats_t *stats);

/**
 * @brief Run one full refresh: enumerate, requery, offer every window, commit.
 *
 * Steps: desktop must be Default (else DesktopInactive, graph untouched); enumerate (Truncated or
 * Failed returns EnumFailed before any graph call); refresh_begin (if the graph reports a refresh
 * left open, abort it and begin again); for each enumerated window in order draw a stamp and tick, then
 * query; Exists offers via refresh_offer, Gone skips the window (it stays unseen and is retired by the
 * commit), Failed aborts the refresh and returns QueryFailed (a missing offer would otherwise retire a
 * live window); re-check the desktop and abort with DesktopInactive if it changed; refresh_commit.
 * Any failure before the commit (QueueFull or another graph status on an offer, a failed query, a
 * desktop change, a Suspended offer result) aborts the open refresh first, so nothing is retired. A
 * QueueFull on the commit itself returns GraphQueueFull and leaves the refresh open in the graph; the
 * next refresh call detects that (RefreshActive), aborts it and starts over. The cadence timestamp is
 * set when a refresh is attempted, so a persistently failing refresh retries at the normal 250 ms
 * cadence, not in a loop; the refresh latch clears only on commit. Pending hints are not consumed.
 * A persistently Failed query for any enumerated window therefore blocks commits (fail closed: no
 * retirement from missing information); destroy hints still retire.
 *
 * @param[in,out] observer Non-NULL Started handle.
 * @param[in,out] graph Non-NULL graph (same thread rule as pump).
 * @param[in] session_epoch Captured epoch of the graph.
 * @param[out] stats Nullable; snapshot of the cumulative counters after the call.
 * @return Ok (committed); DesktopInactive; EnumFailed; QueryFailed; GraphQueueFull; GraphError;
 *         StampExhausted; BadState; InvalidArgument; WrongThread.
 * @note Owner thread only; calls api entries and the graph; uses the observer's enumeration buffer.
 */
window_observer_status_t window_observer_refresh(window_observer_t *observer, interop_window_graph_t *graph,
                                                 uint64_t session_epoch, window_observer_stats_t *stats);

/**
 * @brief Report whether the 250 ms cadence (or a latched resync) calls for a refresh.
 *
 * @param[in] observer Handle; NULL returns 0.
 * @param[in] now_ms Current time from the same clock as api->monotonic_ms.
 * @return 1 when the observer is Started and either a refresh is latched or at least
 *         InteropWindowGraphRefreshIntervalMs have elapsed since the last refresh attempt (a clock that
 *         went backwards counts as zero elapsed); otherwise 0, including off the owner thread.
 * @note Owner thread only; no side effects.
 */
int window_observer_refresh_due(const window_observer_t *observer, uint64_t now_ms);

/**
 * @brief Compute how long the owner thread may block before the observer needs service.
 *
 * @param[in] observer Handle; NULL returns WindowObserverWaitForever.
 * @param[in] now_ms Current time from the same clock as api->monotonic_ms.
 * @return 0 when a refresh is due, or hints are queued and the last desktop check was not inactive;
 *         otherwise the milliseconds until the next refresh is due (1 to
 *         InteropWindowGraphRefreshIntervalMs). While the desktop is inactive a hint backlog does not
 *         force 0, so the loop does not spin; the cadence re-polls the desktop. Returns
 *         WindowObserverWaitForever when the observer is not Started or off the owner thread.
 * @note Owner thread only; no side effects.
 */
uint32_t window_observer_next_timeout_ms(const window_observer_t *observer, uint64_t now_ms);

/**
 * @brief Copy the lifecycle state, gauges and cumulative counters.
 *
 * @param[in] observer Non-NULL handle.
 * @param[out] stats Non-NULL; written fully on Ok, unchanged otherwise.
 * @return Ok; InvalidArgument; WrongThread.
 * @note Owner thread only; no state change.
 */
window_observer_status_t window_observer_status(const window_observer_t *observer, window_observer_stats_t *stats);

/**
 * @brief Record one WinEvent as a dirty hint. Entry point for the hook procedure (native or double).
 *
 * Applies to the process-wide active observer; with none, or one that is not Started, the call is
 * ignored. A call off the owner thread (judged by api->current_thread_id) is ignored except for an
 * atomic hints_wrong_thread increment. A hint is filtered (hints_filtered) when object_id is not
 * WindowObserverObjectIdWindow, child_id is not WindowObserverChildIdSelf, window is 0, or event lies
 * outside [ObjectCreate, ObjectNameChange] and [SystemMinimizeStart, SystemMinimizeEnd]. Otherwise
 * the hint draws a stamp from the logical clock and merges into the dirty table (coalescing per
 * HWND: OR of kind bits, latest stamp, and for the Destroy event the latest destroy stamp and
 * event time, first-seen order kept). A new HWND in a full table latches overflow instead.
 *
 * It MAY: read the active slot, call api->current_thread_id, update the dirty table and counters on
 * the owner thread. It MUST NOT: touch the graph, call any other api entry (no queries, no
 * enumeration, no desktop check), allocate, take locks, block, log, retain pointers, or call any
 * other observer function. Hook procedures can run while the owner thread is inside another observer
 * call that pumps messages, so the observer holds no table iterator or half-written entry across any
 * api call.
 *
 * @param[in] event WinEvent id.
 * @param[in] window HWND value of the event; 0 is filtered.
 * @param[in] object_id Accessible object id; only WindowObserverObjectIdWindow passes.
 * @param[in] child_id Accessible child id; only WindowObserverChildIdSelf passes.
 * @param[in] event_thread_id Thread that generated the event (informational, not used for authority).
 * @param[in] event_time_ms Tick-domain time the OS generated the event; kept as the destroy time for
 *                          Destroy events.
 * @note Callable from any thread, but effective only on the owner thread. No return value, no
 *       errors; all outcomes are visible in the stats counters.
 */
void window_observer_note_hint(uint32_t event, uint64_t window, int32_t object_id, int32_t child_id,
                               uint32_t event_thread_id, uint32_t event_time_ms);

#ifdef _WIN32
/**
 * @brief Return the native Win32 api table.
 *
 * @return Pointer to a static const table valid for the process lifetime; never NULL; ownership stays
 *         with the library. Its context is NULL. Its hook procedure forwards to
 *         window_observer_note_hint and does nothing else.
 * @note Thread-safe (immutable).
 */
const window_observer_api_t *window_observer_native_api(void);
#endif

#endif
