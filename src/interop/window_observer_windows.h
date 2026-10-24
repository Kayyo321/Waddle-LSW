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
 * messages to foreign windows (no WM_GETTEXT, no WM_GETMINMAXINFO). It also never sends a
 * message of any kind to a window of the agent's own process: the title of an AgentOwned
 * window is not read (title_length 0). Minimum tracking size is
 * therefore not queried: min_width and min_height are always 0 (remaining work).
 *
 * Threading. An observer is bound to the thread whose api->current_thread_id returned a value
 * at create time (the owner thread, which must outlive the observer and run a message loop so
 * out-of-context hooks are delivered). Every function except window_observer_note_hint must
 * run on that thread; a call from another thread returns WrongThread (or 0 / WaitForever for
 * the two scalar queries) without touching any state. Only one observer per process may be
 * Started at a time because hook procedures carry no context: a process-wide atomic slot names
 * the active observer, and start() on a second observer returns AlreadyActive. The code that
 * clears the slot (stop, destroy, failed-start rollback) waits until the process-wide in-flight
 * counter, which every window_observer_note_hint call increments before it loads the slot and
 * decrements when it finishes, is zero, so a hint on another thread can never touch an observer
 * that stop or destroy has returned from.
 *
 * Re-entrancy. A Win32 call made by an observer function (enumeration, queries) can dispatch a
 * message to the owner thread, and the handler may call back into this observer. The observer
 * therefore keeps a busy flag on the owner thread. Every function that takes an observer handle,
 * except window_observer_note_hint, returns WindowObserverStatusReentrant (0 for
 * window_observer_refresh_due, WindowObserverWaitForever for window_observer_next_timeout_ms)
 * and touches nothing while another entry point of that observer is active, including an entry
 * through a message dispatched inside a Win32 call. Functions that mutate or call the api hold
 * the flag for their whole duration; read-only functions only test it. note_hint never reads or
 * writes the flag. The enumeration buffer and the popped-key state are therefore touched only by
 * the outermost pump or refresh.
 *
 * Native requirements. The native table (api NULL under _WIN32) needs GetDpiForWindow (Windows 10
 * 1607 or later) and an owner thread whose DPI awareness context is
 * DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, because rects and dpi must be physical pixels of a
 * known scale and the observer never changes the awareness of a thread. window_observer_create
 * checks both on the calling thread (GetDpiForWindow resolved at run time;
 * GetThreadDpiAwarenessContext compared with AreDpiAwarenessContextsEqual) before it allocates
 * anything and returns WindowObserverStatusUnsupported when either check fails. This is the only
 * place the condition is reported: after a successful create the native table is runnable and no
 * later call returns Unsupported. A caller-supplied api table is never checked this way.
 *
 * Identity continuity. The observer does not stamp foreign windows with properties (SetProp
 * would mutate other processes' windows, can clash with unrelated properties, and is blocked by
 * UIPI for elevated windows). Continuity is proven by the graph from the identity tuple
 * (process id, thread id, process start time, class hash) plus the destroy-hint rule of
 * interop_window_graph_note_destroyed(). Residual risk: a window destroyed and recreated with
 * the identical tuple and the same HWND, with the destroy hint lost by the OS and no
 * hint-table overflow, is indistinguishable from the old incarnation.
 *
 * Access denied. The image basename is resolved in two steps. First OpenProcess
 * (PROCESS_QUERY_LIMITED_INFORMATION) with QueryFullProcessImageName, which also yields the process
 * start time. When OpenProcess fails (typical for elevated or protected processes under UIPI), the
 * basename is taken from a CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS) walk by process id
 * (the executable file name of the PROCESSENTRY32 entry); the snapshot is taken at most once per pump or refresh call (retaken only on a
 * process-id miss) and its handle is closed on every path. In that case process_start_time stays 0
 * (unknown). Only when both steps fail does the native query report ImageUnknown with image_length 0
 * and process_start_time 0; classification then never matches a UAC or CoreWindow rule for that
 * window. A known image with start time 0 and the same process with a known start time are
 * different identities for the graph (the flip is an incarnation change, one surface of churn).
 * Nothing here claims UIAccess or secure-desktop visibility.
 *
 * Desktop switch. The input desktop is checked once per pump or refresh call, before any query.
 * When it is not Default, or cannot be determined, the call processes nothing, keeps queued hints,
 * counts the event, and latches a full refresh for the moment the desktop returns. When it is
 * Default, the core sets OnInputDesktop on every snapshot of that call; no per-window desktop
 * detection exists. A graph Suspended disposition therefore cannot occur; if it does (defensive),
 * pump puts the key back and returns DesktopInactive, and refresh aborts and returns
 * DesktopInactive. The observer produces no protocol message for it.
 *
 * Refresh cadence and latch. The observer keeps last_refresh_attempt_ms and latch_ms, both read from
 * api->monotonic_ms. last_refresh_attempt_ms is stamped at the start of every refresh attempt
 * (window_observer_refresh, or the refresh that pump runs after an overflow), BEFORE the desktop is
 * checked, so a refresh that fails or is refused still counts as an attempt; it is also stamped when
 * pump finds the desktop inactive. It is unset (no attempt) from start() until the first attempt.
 * latch_ms is stamped whenever a full-refresh latch is set: start, set_preedit_window that changes
 * the window while Started, overflow recovery in pump, and a refusal because the desktop is
 * inactive; a refusal reuses the clock read of its own attempt stamp, so both values are equal. The
 * latch itself (stats.refresh_requested) is cleared only by a committed refresh.
 * window_observer_refresh_due(now) is true iff the observer is Started and (no refresh attempt has
 * been made since start, or now - last_refresh_attempt_ms >= InteropWindowGraphRefreshIntervalMs
 * with a backwards clock counted as zero elapsed, or the latch is set and last_refresh_attempt_ms <
 * latch_ms). A refresh attempt therefore consumes every latch event that preceded it even when it
 * fails, and a frozen clock with a persistently failing refresh runs at most one refresh per latch
 * event.
 *
 * Facts assembly (portable core). For each window it queries, the core zero-fills a facts struct,
 * calls api->query_facts and, on Exists, overlays stamp, captured_ms, AgentOwned (process_id equals
 * own_process_id), AgentPreedit (window equals the preedit window and AgentOwned) and OnInputDesktop
 * (always set). It forces title_length 0 on every AgentOwned snapshot. The core then computes
 * OwnerHidden itself; the api never sets it. When owner_root is neither 0 nor the window itself, the
 * core calls api->query_facts(owner_root) into a second zero-filled struct (the owner sub-query):
 *  - Exists: overlay the same core flags (same stamp and captured_ms), call
 *    interop_window_graph_classify with the config's uac_mode, and set OwnerHidden on the window iff
 *    the verdict kind is NotExported. An owner that is Eligible or Suspended leaves it clear. A
 *    classify status other than Ok (unusable owner facts) makes the window's query Failed.
 *  - Gone: set OwnerHidden.
 *  - Failed: the window's query is Failed.
 * There is no recursion: the owner is classified with OwnerHidden clear and its own owner is never
 * queried. The query counters count one result per window query, with the owner sub-query folded in.
 *
 * Graph status mapping used by pump and refresh for every graph call:
 *  - Ok: continue.
 *  - InvalidFacts, StaleFacts, Cycle, AtCapacity, GenerationExhausted, LookupFailed,
 *    AppKeyInvalid, Stale: counted in graph_errors, the key is dropped, processing continues;
 *    the next refresh re-offers the window.
 *  - QueueFull: the key is put back at the tail of the dirty table (with its destroy
 *    information), processing stops, the call returns GraphQueueFull; the coordinator drains graph
 *    events and calls again.
 *  - Reentrant: the key is put back at the head, processing stops, the call returns Reentrant (not
 *    GraphError); last_graph_status holds Reentrant. A refresh in this state also tries to abort the
 *    graph refresh it opened; a failed abort is ignored and the next refresh recovers (RefreshActive).
 *  - anything else (StaleEpoch, WrongThread, InvalidArgument, NoRefresh, ...): the
 *    key is put back at the head, processing stops, the call returns GraphError; last_graph_status
 *    holds the exact status.
 * The None status counts as Ok. Within a refresh, where there is no popped key, "dropped" means the
 * window was already marked seen by refresh_offer and the refresh continues, "put back" means the
 * refresh is aborted first (nothing is retired), and the Reentrant/GraphError/GraphQueueFull
 * result is returned.
 */
#include <stddef.h>
#include <stdint.h>

#include "waddle/interop_window_graph.h"

/** @brief Capacity of the dirty-handle table (distinct HWNDs awaiting a requery); while pump holds a popped key one slot is reserved for its put-back, so a new HWND is accepted only while dirty_pending plus the popped key count stays below this value. */
#define WindowObserverDirtyCapacity 512u
/** @brief Capacity of the enumeration buffer; more top-level windows make a refresh fail closed. */
#define WindowObserverMaxEnumerated 2048u
/** @brief Extra requery attempts after a Failed query before the key is dropped (until the next refresh). */
#define WindowObserverQueryRetries 1u
/** @brief Value of next_timeout_ms when nothing is scheduled (Win32 INFINITE). */
#define WindowObserverWaitForever 0xFFFFFFFFu

/** @brief WinEvent id EVENT_OBJECT_CREATE; lower end of the object range. */
#define WindowObserverEventObjectCreate 0x8000u
/** @brief WinEvent id EVENT_OBJECT_DESTROY; the only event that carries destroy authority. */
#define WindowObserverEventObjectDestroy 0x8001u
/** @brief WinEvent id EVENT_OBJECT_SHOW. */
#define WindowObserverEventObjectShow 0x8002u
/** @brief WinEvent id EVENT_OBJECT_HIDE. */
#define WindowObserverEventObjectHide 0x8003u
/** @brief WinEvent id EVENT_OBJECT_NAMECHANGE; lies inside the object range [Create, ParentChange] (as do REORDER, STATECHANGE and LOCATIONCHANGE); no longer a range bound. */
#define WindowObserverEventObjectNameChange 0x800Cu
/** @brief WinEvent id EVENT_OBJECT_PARENTCHANGE; upper end of the object range. */
#define WindowObserverEventObjectParentChange 0x800Fu
/** @brief WinEvent id EVENT_SYSTEM_MINIMIZESTART; lower end of the system range. */
#define WindowObserverEventSystemMinimizeStart 0x0016u
/** @brief WinEvent id EVENT_SYSTEM_MINIMIZEEND; upper end of the system range. */
#define WindowObserverEventSystemMinimizeEnd 0x0017u
/** @brief WinEvent id EVENT_OBJECT_CLOAKED; lower end of the cloak range. */
#define WindowObserverEventObjectCloaked 0x8017u
/** @brief WinEvent id EVENT_OBJECT_UNCLOAKED; upper end of the cloak range. */
#define WindowObserverEventObjectUncloaked 0x8018u
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
    WindowObserverStatusInvalidArgument = 4, /**< NULL pointer, zero budget/stamp/process id, uac_mode above 2, malformed api table or config; nothing changed. */
    WindowObserverStatusWrongThread = 5, /**< Not the owner thread; nothing touched (a destroy does not free). */
    WindowObserverStatusNoMemory = 6, /**< create could not allocate; no observer exists. */
    WindowObserverStatusAlreadyActive = 7, /**< Another observer in this process is Started; this one stays unchanged. */
    WindowObserverStatusBadState = 8, /**< The call is illegal in the current lifecycle state; nothing changed. */
    WindowObserverStatusHookFailed = 9, /**< A hook could not be installed; installed hooks were removed, state Failed. */
    WindowObserverStatusUnhookFailed = 10, /**< A hook could not be removed; state Failed, restart refused. */
    WindowObserverStatusGraphError = 11, /**< Graph returned an unexpected status (see last_graph_status); processing stopped. */
    WindowObserverStatusEnumFailed = 12, /**< Enumeration failed or was truncated; no refresh was begun, nothing retired. */
    WindowObserverStatusStampExhausted = 13, /**< The logical clock cannot issue another stamp; the observer must be replaced. */
    WindowObserverStatusReentrant = 14, /**< Any function with an observer handle except note_hint: another entry point of this observer is active (busy flag); nothing touched (a destroy does not free). pump and refresh: also returned when the graph reports Reentrant (key put back, processing stopped, last_graph_status Reentrant). */
    WindowObserverStatusUnsupported = 15 /**< create only: the native table cannot run here (GetDpiForWindow missing, or the calling thread is not DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); no observer exists. */
} window_observer_status_t;

/** @brief Outcome of one native fact query. */
typedef enum window_observer_query_result_t {
    WindowObserverQueryExists = 1, /**< The window currently exists and facts is fully populated. */
    WindowObserverQueryGone = 2, /**< The OS affirmatively reports the handle is not a valid window (the only result that may lead to retirement). */
    WindowObserverQueryFailed = 3 /**< The state could not be determined (transient error, access failure, partial read, or a failed owner sub-query); never causes retirement. */
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
     * @return 0 on success, nonzero on failure. start() calls this exactly three times, in order:
     *         (WindowObserverEventObjectCreate, WindowObserverEventObjectParentChange), then
     *         (WindowObserverEventSystemMinimizeStart, WindowObserverEventSystemMinimizeEnd), then
     *         (WindowObserverEventObjectCloaked, WindowObserverEventObjectUncloaked). The first range
     *         covers every object event in 0x8000..0x800F (create, destroy, show, hide, reorder,
     *         statechange, locationchange, namechange, parentchange); the third delivers DWM cloak
     *         changes, which flip the Visible fact without a show or hide event.
     */
    int (*install_hook)(void *context, uint32_t event_min, uint32_t event_max, uintptr_t *hook);
    /**
     * @brief Remove a hook previously installed by install_hook.
     * @param[in,out] context The config's api_context; may be NULL.
     * @param[in] hook Token from install_hook; never 0.
     * @return 0 on success, nonzero on failure (the hook state is then unknown and is not retried).
     *         stop() and failed-start cleanup call it once per installed hook (up to three), in
     *         reverse order of installation.
     */
    int (*remove_hook)(void *context, uintptr_t hook);
    /**
     * @brief Read the current native facts of one window without sending messages to its owner.
     *
     * No message is ever sent to a window of the agent's own process, and none to a foreign window
     * (no WM_GETTEXT, no WM_GETMINMAXINFO). The core calls this entry once for the window and, when
     * the window's owner_root is neither 0 nor the window itself, once more for owner_root (the owner
     * sub-query of Facts assembly in the file comment); each call must be independent and stateless.
     *
     * The core zero-fills facts before the call and, after Exists, overwrites stamp, captured_ms and
     * the AgentOwned, AgentPreedit and OnInputDesktop flags, forces title_length 0 on AgentOwned
     * snapshots and computes OwnerHidden itself: the callee must never set OwnerHidden. The callee
     * fills everything else: window, owner (GW_OWNER), owner_root (GA_ROOT of owner), root (GA_ROOT of
     * window), style and ex_style (low 32 bits), Enabled, Iconic, Zoomed, Layered,
     * LayeredAlphaKnown and alpha, Fullscreen, ClassTruncated, ImageUnknown, rect (DWM extended
     * frame bounds with GetWindowRect fallback; restored geometry when iconic), dpi, process id,
     * thread id, process start time (0 if unreadable), class_name, image_name (basename only) and
     * title. Visible must be set only if the window is visible and not DWM-cloaked. Titles and
     * class names are converted to UTF-8, cut at a code-point boundary, and unpaired surrogates
     * become U+FFFD. min_width and min_height stay 0.
     *
     * Native rules. Title: never read for a window of the current process (title_length 0 and no
     * message sent), which is exactly the AgentOwned set of the native table. Size: the native adapter
     * clamps width and height to InteropWindowGraphMaxExtent (16384), so an oversized window is
     * exported at the maximum instead of being rejected by the graph (the graph itself still rejects
     * larger values). Image: basename from OpenProcess + QueryFullProcessImageName with the real
     * process start time; when OpenProcess fails, the basename from a CreateToolhelp32Snapshot
     * (TH32CS_SNAPPROCESS) walk by process id (the executable file name of the PROCESSENTRY32 entry; the snapshot is refreshed at most once per
     * pump or refresh call, on a process-id miss) with process_start_time left 0; ImageUnknown and
     * image_length 0 only when both fail.
     *
     * @param[in,out] context The config's api_context; may be NULL.
     * @param[in] window Nonzero HWND value.
     * @param[out] facts Non-NULL pre-zeroed struct; contents are meaningful only for Exists.
     * @return Exists, Gone (only when the OS reports an invalid handle) or Failed. A facts->window
     *         different from the requested window is treated by the observer as Failed.
     * @note Owner thread only. The native implementation makes one bounded transient heap allocation
     *       per call (the image-path buffer), freed on every path before it returns.
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
     * @brief Monotonic millisecond clock for the refresh cadence and the latch.
     * @param[in,out] context The config's api_context; may be NULL.
     * @return Milliseconds, never decreasing.
     */
    uint64_t (*monotonic_ms)(void *context);
    /**
     * @brief 32-bit tick clock in the same domain as the WinEvent event-time field (wraps every 2^32
     *        ms; the graph unwraps it against its own 64-bit base, see Tick clock in the graph header).
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
    uint32_t uac_mode; /**< interop_window_uac_mode_t, 0..2; must equal the graph's uac_mode. Used only to classify owner windows (interop_window_graph_classify) when computing OwnerHidden. Occupies the former reserved0 slot (same offset and size). */
} window_observer_config_t;

/** @brief Cumulative observer counters and gauges. Counters start at 0 at create, saturate, never wrap. */
typedef struct window_observer_stats_t {
    uint64_t next_stamp; /**< Next logical-clock value that will be issued. */
    uint32_t state; /**< window_observer_state_t. */
    uint32_t last_graph_status; /**< interop_window_graph_status_t of the latest graph call that returned neither Ok nor None; Ok if none yet. */
    uint32_t dirty_pending; /**< Gauge: distinct HWNDs queued for requery (a key popped by a running pump is not counted; it holds one reserved slot). */
    uint32_t desktop_active; /**< Gauge: 1 if the latest desktop check returned Default. */
    uint32_t overflow_latched; /**< Gauge: 1 while a hint-table overflow awaits recovery. */
    uint32_t refresh_requested; /**< Gauge: 1 while a full refresh is latched as due. */
    uint64_t hints_received; /**< Hints accepted into the table. */
    uint64_t hints_filtered; /**< Hints ignored: wrong object/child id, handle 0, or event outside the hooked ranges. */
    uint64_t hints_wrong_thread; /**< Hints ignored because they arrived off the owner thread (the one counter updated atomically from other threads). */
    uint64_t hints_coalesced; /**< Hints merged into an existing table entry. */
    uint64_t hint_overflows; /**< Hints for a new HWND that found the table full (counting the slot reserved for a popped key) and latched overflow. */
    uint64_t keys_processed; /**< Table entries popped by pump. */
    uint64_t queries_exists; /**< Window queries that returned Exists (one count per window query, the owner sub-query folded in). */
    uint64_t queries_gone; /**< Window queries that returned Gone (a Gone owner sub-query sets OwnerHidden and is not counted here). */
    uint64_t queries_failed; /**< Window queries that returned Failed: a failed query, a mismatching window, a failed or unusable owner sub-query. A refresh keeps such a window with refresh_keep. */
    uint64_t query_retries_exhausted; /**< Keys dropped after WindowObserverQueryRetries failed retries. */
    uint64_t destroys_noted; /**< note_destroyed calls issued. */
    uint64_t graph_errors; /**< Graph statuses handled by dropping a key, skipping a refresh offer or stopping (see the mapping in the file comment). */
    uint64_t pumps_desktop_inactive; /**< pump or refresh calls refused because the desktop was not Default. */
    uint64_t refreshes_started; /**< refresh_begin calls issued. */
    uint64_t refreshes_committed; /**< Refreshes committed. */
    uint64_t refreshes_aborted; /**< Refreshes aborted by the observer (before the commit, on QueueFull, a call-level graph status, Reentrant or a defensive Suspended). */
    uint64_t invalidations; /**< invalidate_all calls issued after an overflow. */
    uint64_t uac_native_hidden; /**< Offers whose result reason was UacNativeHidden (a consent window exists but is hidden from export). */
} window_observer_stats_t;

_Static_assert(sizeof(window_observer_state_t) == 4, "observer state enum ABI");
_Static_assert(sizeof(window_observer_status_t) == 4, "observer status enum ABI");
_Static_assert(WindowObserverStatusStampExhausted == 13, "status numbering: StampExhausted");
_Static_assert(WindowObserverStatusReentrant == 14, "status numbering: Reentrant");
_Static_assert(WindowObserverStatusUnsupported == 15, "status numbering: Unsupported");
_Static_assert(sizeof(window_observer_query_result_t) == 4, "query result enum ABI");
_Static_assert(sizeof(window_observer_enum_result_t) == 4, "enum result enum ABI");
_Static_assert(sizeof(window_observer_desktop_t) == 4, "desktop enum ABI");
_Static_assert(WindowObserverDirtyCapacity == 512u, "dirty table bound is frozen by the design contract");
_Static_assert(WindowObserverMaxEnumerated >= 4u * InteropMaxWindows, "enumeration buffer must exceed the exported bound");
_Static_assert(WindowObserverEventObjectCreate < WindowObserverEventObjectParentChange, "object hook range is ordered");
_Static_assert(WindowObserverEventSystemMinimizeStart < WindowObserverEventSystemMinimizeEnd, "system hook range is ordered");
_Static_assert(WindowObserverEventObjectCloaked < WindowObserverEventObjectUncloaked, "cloak hook range is ordered");
_Static_assert(WindowObserverEventObjectNameChange > WindowObserverEventObjectCreate &&
                   WindowObserverEventObjectNameChange < WindowObserverEventObjectParentChange,
               "NameChange lies inside the object hook range");

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
_Static_assert(offsetof(window_observer_config_t, uac_mode) == 36, "config uac_mode ABI");

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
 *                   first_stamp and own_process_id nonzero, uac_mode 0..2 (InvalidArgument otherwise).
 * @param[out] out_observer Non-NULL caller slot. On Ok it receives the handle (owned by the caller
 *                          until destroy). On every failure it is set to NULL once the slot itself was valid.
 * @return Ok; InvalidArgument; Unsupported (native table selected and it cannot run: GetDpiForWindow
 *         is missing, or the calling thread's DPI awareness context is not
 *         DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 per GetThreadDpiAwarenessContext and
 *         AreDpiAwarenessContextsEqual; never returned for a caller-supplied api table; checked before
 *         any allocation); NoMemory (a single allocation holds the observer, dirty table and
 *         enumeration buffer; nothing leaks on failure).
 * @note The owner thread is the thread id returned by api->current_thread_id during this call.
 *       Does not install hooks, query the OS beyond the Unsupported check, or touch any graph.
 *       There is no busy flag to test yet, so create never returns Reentrant.
 */
window_observer_status_t window_observer_create(const window_observer_config_t *config,
                                                window_observer_t **out_observer);

/**
 * @brief Stop (if started), free the observer and set the caller's handle to NULL.
 *
 * Frees even when removing a hook fails; the process-wide slot is released first and the call then
 * waits until the process-wide in-flight hint counter is zero, so a hook that could not be removed
 * delivers only ignored hints afterwards and no hint can touch the freed observer. The observer never
 * touches the graph, so destroy order relative to the graph is free.
 *
 * @param[in,out] observer_slot Non-NULL caller slot. If *observer_slot is NULL this is a successful
 *                              no-op. On Ok and UnhookFailed the observer is freed and *observer_slot
 *                              is NULL; on any other status nothing is freed and the slot is unchanged.
 * @return Ok; UnhookFailed (freed, but a hook leaked until process exit); InvalidArgument for a NULL
 *         slot; WrongThread; Reentrant (another entry point is active; nothing freed).
 * @note Owner thread only. Not safe against concurrent use of the same observer. Holds the busy flag.
 */
window_observer_status_t window_observer_destroy(window_observer_t **observer_slot);

/**
 * @brief Claim the process-wide active slot, clear the dirty table, install the three hooks, and latch
 *        a full refresh as due.
 *
 * Allowed from Created and Stopped (restart; the logical clock keeps counting up). Installs, in order,
 * the object range [ObjectCreate, ObjectParentChange], the system range [SystemMinimizeStart,
 * SystemMinimizeEnd] and the cloak range [ObjectCloaked, ObjectUncloaked]. On HookFailed the hooks
 * already installed are removed in reverse order, the slot is released (waiting for in-flight hints),
 * and the state becomes Failed. Clears the cadence clock (no refresh attempt yet) and stamps latch_ms.
 *
 * @param[in,out] observer Non-NULL handle.
 * @return Ok; InvalidArgument; WrongThread; Reentrant; BadState (already Started, or Failed);
 *         AlreadyActive (another observer holds the slot; this one unchanged); HookFailed (state Failed,
 *         every hook that was installed has been removed).
 * @note Owner thread only. Calls api->install_hook; may dispatch hint callbacks only after it returns.
 *       Holds the busy flag.
 */
window_observer_status_t window_observer_start(window_observer_t *observer);

/**
 * @brief Cease hint production: release the active slot, remove every hook (reverse order), clear
 *        the dirty table, the overflow latch and the refresh latch.
 *
 * Releasing the slot waits until the process-wide in-flight hint counter is zero, so no hint is inside
 * the observer when the call returns. Never touches the graph: nothing is retired, freed or proven;
 * the coordinator decides what happens to the graph. Stopping a Created or Stopped observer succeeds
 * without effect.
 *
 * @param[in,out] observer Non-NULL handle.
 * @return Ok; UnhookFailed (every hook was still attempted, state becomes Failed and restart is
 *         refused); InvalidArgument; WrongThread; Reentrant; BadState (observer is Failed).
 * @note Owner thread only. Calls api->remove_hook up to three times. Holds the busy flag.
 */
window_observer_status_t window_observer_stop(window_observer_t *observer);

/**
 * @brief Tell the observer which window is the agent's IME preedit window.
 *
 * The change applies to the next query of each window. A change while Started latches a full refresh
 * (stamping latch_ms from api->monotonic_ms) so the old and new preedit windows are reclassified.
 *
 * @param[in,out] observer Non-NULL handle.
 * @param[in] window HWND value, or 0 for none.
 * @return Ok; InvalidArgument; WrongThread; Reentrant.
 * @note Owner thread only; no OS or graph access (the only api call is the monotonic clock read, and
 *       only when the window changes while Started).
 */
window_observer_status_t window_observer_set_preedit_window(window_observer_t *observer, uint64_t window);

/**
 * @brief Process queued hints against the graph.
 *
 * Order: take the busy flag (Reentrant if taken); if the input desktop is not Default, process nothing,
 * latch a refresh, stamp the cadence clock and return DesktopInactive. If the overflow latch is set,
 * call invalidate_all (QueueFull returns GraphQueueFull with the latch kept), clear the table and
 * latch, latch a refresh and run a full refresh (steps 2 to 6 of window_observer_refresh; pump has just
 * checked the desktop and stamped the attempt) in this call, whose status is returned. Otherwise
 * pop at most min(budget, entries present at call start) keys in first-seen order; each key is removed
 * from the table before its facts are queried, so a hint arriving during the query re-adds it. While a
 * key is popped one dirty slot stays reserved for it: note_hint treats the table as full when
 * dirty_pending plus the popped key reaches WindowObserverDirtyCapacity, so a put-back always fits and
 * never forces an overflow. Per key:
 * if it carries a destroy stamp, call note_destroyed with that stamp and event time; then draw a fresh
 * stamp (and a tick sample) before querying (the window query plus, when owner_root is neither 0 nor
 * the window, the owner sub-query that decides OwnerHidden; see Facts assembly); Exists calls observe,
 * Gone calls note_destroyed with the fresh stamp and tick, and Failed puts the key back (at the tail,
 * with its attempt count) up to WindowObserverQueryRetries times and then drops it: a failed query
 * never retires anything. Graph statuses follow the mapping in the file comment (a QueueFull
 * put-back goes to the tail of the table, any other stop to the head).
 *
 * @param[in,out] observer Non-NULL Started handle.
 * @param[in,out] graph Non-NULL graph owned by the caller, mutated only on its owner thread (the
 *                      owner threads of graph and observer must be the same thread).
 * @param[in] session_epoch Captured epoch of the graph; passed to every graph call, so a mismatch
 *                          yields GraphError with last_graph_status StaleEpoch and no graph effect.
 * @param[in] budget At least 1; maximum keys popped in this call.
 * @param[out] stats Nullable; receives a snapshot of the cumulative counters after the call
 *                   (also on non-Ok statuses other than InvalidArgument, WrongThread and Reentrant).
 * @return Ok (the table is empty on return); More (hints remain: budget exhausted, or a key was put
 *         back after a failed query); DesktopInactive;
 *         GraphQueueFull; GraphError; StampExhausted; Reentrant (busy flag, or the graph reported
 *         Reentrant); BadState (not Started); InvalidArgument; WrongThread.
 * @note Owner thread only; calls api query entries and the graph; the portable core does not allocate
 *       (two facts structs live on the stack; the native query_facts makes one bounded transient
 *       allocation per query, freed on every path). Graph callbacks (app-key lookup) run inside observe.
 *       Holds the busy flag.
 */
window_observer_status_t window_observer_pump(window_observer_t *observer, interop_window_graph_t *graph,
                                              uint64_t session_epoch, uint32_t budget,
                                              window_observer_stats_t *stats);

/**
 * @brief Run one full refresh: enumerate, offer every window, reconcile the unseen, commit.
 *
 * Steps (the busy flag is held throughout; the enumeration buffer is touched only by this outermost
 * call):
 *  1. Stamp last_refresh_attempt_ms from api->monotonic_ms (before anything can fail), then check the
 *     desktop: it must be Default, else DesktopInactive with the graph untouched and the latch kept
 *     (latch_ms equals the attempt stamp).
 *  2. Enumerate; Truncated or Failed returns EnumFailed before any graph call.
 *  3. refresh_begin (if the graph reports a refresh left open, abort it and begin again).
 *  4. For each enumerated window in order draw a stamp and a tick, then query (with the owner
 *     sub-query of Facts assembly). Exists: offer with refresh_offer. Gone: skip the window (it stays
 *     unseen). Failed: refresh_keep(window), count the failed query, continue.
 *  5. refresh_unseen lists the Active windows still unseen; requery each listed window individually
 *     (fresh stamp and tick). Exists: refresh_offer. Gone: note_destroyed(window, fresh stamp, tick)
 *     (if its destroy rule rejects the hint the window stays unseen and the commit retires it).
 *     Failed: refresh_keep(window).
 *  6. refresh_commit retires what is still unseen (reason RefreshUnseen) and drops unseen deferred
 *     records.
 * Graph statuses of the offers and notes in steps 4 and 5: Ok and None continue. The drop-class
 * statuses of the file-comment mapping (InvalidFacts, StaleFacts, Cycle, AtCapacity,
 * GenerationExhausted, LookupFailed, AppKeyInvalid, Stale) are counted in graph_errors and the refresh
 * continues; because refresh_offer marks the window seen for every outcome, a rejected offer never
 * causes that window's retirement at the commit. QueueFull aborts the open refresh and returns
 * GraphQueueFull. Any other (call-level) status, such as StaleEpoch, WrongThread, InvalidArgument or
 * NoRefresh, aborts the open refresh and returns GraphError; a graph Reentrant status aborts and
 * returns Reentrant. A Suspended offer disposition (defensive; the core always sets OnInputDesktop)
 * aborts the refresh and returns DesktopInactive. Any failure before the commit therefore aborts
 * the open refresh first, so nothing is retired. There is no desktop re-check between the
 * enumeration and the commit: the desktop is checked once, in step 1.
 *
 * A refresh containing failed queries still commits and returns Ok: every window whose query failed
 * was kept with refresh_keep, so a failed query never retires a window; destroy hints still retire.
 * A QueueFull on the commit itself returns GraphQueueFull and leaves the refresh open in the graph; the
 * next refresh call detects that (RefreshActive), aborts it and starts over. The attempt timestamp is
 * stamped when a refresh is attempted, before the desktop check, so a persistently failing refresh
 * retries at the normal 250 ms cadence, not in a loop; the refresh latch clears only on commit.
 * Pending hints are not consumed.
 *
 * @param[in,out] observer Non-NULL Started handle.
 * @param[in,out] graph Non-NULL graph (same thread rule as pump).
 * @param[in] session_epoch Captured epoch of the graph.
 * @param[out] stats Nullable; snapshot of the cumulative counters after the call.
 * @return Ok (committed, possibly with failed queries); DesktopInactive; EnumFailed; GraphQueueFull;
 *         GraphError; StampExhausted; Reentrant; BadState; InvalidArgument; WrongThread.
 * @note Owner thread only; calls api entries and the graph; uses the observer's enumeration buffer
 *       (outermost call only). Total queries per call are bounded by WindowObserverMaxEnumerated plus
 *       InteropMaxWindows (each with at most one owner sub-query).
 */
window_observer_status_t window_observer_refresh(window_observer_t *observer, interop_window_graph_t *graph,
                                                 uint64_t session_epoch, window_observer_stats_t *stats);

/**
 * @brief Report whether the 250 ms cadence (or a latched resync) calls for a refresh.
 *
 * @param[in] observer Handle; NULL returns 0.
 * @param[in] now_ms Current time from the same clock as api->monotonic_ms.
 * @return 1 when the observer is Started and any of: no refresh attempt has been made since start();
 *         at least InteropWindowGraphRefreshIntervalMs have elapsed since last_refresh_attempt_ms (a
 *         clock that went backwards counts as zero elapsed); the refresh latch is set and
 *         last_refresh_attempt_ms is earlier than latch_ms (see Refresh cadence and latch). Otherwise 0,
 *         including off the owner thread and while another entry point is active.
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
 *         InteropWindowGraphRefreshIntervalMs). The latch yields 0 at most once per latch event: once a
 *         refresh attempt was stamped at or after latch_ms, even a failed one, the latch no longer forces
 *         0. While the desktop is inactive a hint backlog does not force 0, so the loop does not spin;
 *         the cadence re-polls the desktop. Returns WindowObserverWaitForever when the observer is not
 *         Started, off the owner thread, or while another entry point is active.
 * @note Owner thread only; no side effects.
 */
uint32_t window_observer_next_timeout_ms(const window_observer_t *observer, uint64_t now_ms);

/**
 * @brief Copy the lifecycle state, gauges and cumulative counters.
 *
 * @param[in] observer Non-NULL handle.
 * @param[out] stats Non-NULL; written fully on Ok, unchanged otherwise.
 * @return Ok; InvalidArgument; WrongThread; Reentrant (another entry point is active).
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
 * outside all three hooked ranges [ObjectCreate, ObjectParentChange], [SystemMinimizeStart,
 * SystemMinimizeEnd] and [ObjectCloaked, ObjectUncloaked]. Otherwise
 * the hint draws a stamp from the logical clock and merges into the dirty table (coalescing per
 * HWND: OR of kind bits, latest stamp, and for the Destroy event the latest destroy stamp and
 * event time, first-seen order kept). A new HWND latches overflow instead when the table is full,
 * where full means dirty_pending plus one reserved slot for a key that pump has popped (if any)
 * has reached WindowObserverDirtyCapacity; the reservation guarantees that a put-back after a failed
 * query or a stop always fits.
 *
 * Process-wide in-flight counter. Every call increments an atomic counter before it loads the
 * active-observer slot and decrements it when it has finished with the observer; start's rollback,
 * stop and destroy wait for the counter to reach zero after clearing the slot, so the call can never
 * touch a freed observer. This is the only synchronization note_hint performs and it never blocks.
 *
 * It MAY: read the active slot, call api->current_thread_id, update the dirty table and counters on
 * the owner thread. It MUST NOT: touch the graph, call any other api entry (no queries, no
 * enumeration, no desktop check, no clock), allocate, take locks, block, log, retain pointers, read or
 * write the busy flag, or call any other observer function. Hook procedures can run while the owner
 * thread is inside another observer call that pumps messages, so the observer holds no table iterator
 * or half-written entry across any api call.
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
 *         window_observer_note_hint and does nothing else. The table is runnable only on a thread that
 *         passed window_observer_create's native checks (Unsupported otherwise); it sends no message to
 *         any window of the current process and never reads their titles.
 * @note Thread-safe (immutable).
 */
const window_observer_api_t *window_observer_native_api(void);
#endif

#endif
