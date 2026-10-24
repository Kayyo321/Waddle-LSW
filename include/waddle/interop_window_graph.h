#ifndef WaddleInteropWindowGraphH
#define WaddleInteropWindowGraphH
/**
 * @file interop_window_graph.h
 * @brief Public C ABI of the bounded, session-wide guest window graph (TODO #10).
 *
 * The graph is a pure state machine implemented in Zig (src/interop/window_graph.zig). The Zig
 * engine consumes the structs of this header directly through Zig's C import; no Zig mirror of any ABI
 * struct exists, so the layouts asserted below are the only layout. It never calls the operating
 * system. A native observer (window_observer_windows.h) turns Win32 state into value snapshots
 * (interop_window_facts_t); the graph classifies them (IMPL_DESC section 4.4), assigns
 * monotonic identities, orders parents before children, bounds storage, and emits
 * value events (Announce, Update, RetireIntent). It serializes no wire frame, owns no
 * shared memory, heap run, capture or transport, and never treats loss of connection
 * as proof that host buffers are gone.
 *
 * Ownership: one graph exists per nonzero session epoch. interop_window_graph_create()
 * performs the graph's only heap allocation; interop_window_graph_destroy() frees it.
 * Every struct crossing this ABI is a fixed-layout value copy; no pointer into graph
 * storage is ever returned, and callbacks must not retain any pointer they receive.
 *
 * Threading: a graph is bound to the thread that created it. Every call, including
 * read-only ones, must be made from that thread; a call from another thread returns
 * WrongThread without touching any graph memory (not even counters). A call made from
 * inside the app-key lookup callback or the drain sink returns Reentrant under the same
 * rule. Callbacks therefore must not call into the graph.
 *
 * Common call rules. Every function except create applies these checks in this order,
 * stopping at the first failure:
 *  1. graph is NULL: InvalidArgument; nothing is touched.
 *  2. wrong thread: WrongThread; nothing is touched.
 *  3. re-entrant call: Reentrant; nothing is touched.
 *  4. session_epoch (where a function takes one) differs from the graph epoch, including
 *     zero: StaleEpoch; only the rejected_stale_epoch counter changes. A late call from
 *     an old epoch can never affect a newer graph.
 *  5. any other argument invalid: InvalidArgument; nothing changes, outputs unchanged.
 *  6. function specific checks (documented per function).
 * Unless a function documents otherwise, a call that returns a status other than Ok or
 * None leaves all graph state and all output parameters unchanged, except diagnostic
 * counters. The documented exceptions are Cycle (observe, refresh_offer: drops a stale
 * deferred record), the seen mark of refresh_offer (written for every outcome once the guards
 * pass, see refresh_offer) and SinkFailed (drain: events accepted before the failure stay
 * consumed). Mutating calls are plan, pre-check, one external call, infallible apply;
 * the plan phase never writes (the refresh_offer seen mark is the only write that precedes it).
 *
 * Facts copy: observe and refresh_offer copy the caller's interop_window_facts_t into a
 * stack-local at entry (about 1.7 KiB of stack, no allocation), validate only that copy, and use
 * only that copy for the rest of the call. The app-key lookup callback receives the copy, never
 * the caller's pointer, so a callback or another thread cannot change the facts between
 * validation and use.
 *
 * Identity: (session_epoch, window, generation). generation is assigned at admission
 * from a graph-owned counter that starts at 1, increases by one per admission, is never
 * reset, never reused and never wraps. When the next value would not be representable,
 * admission fails with GenerationExhausted and the session must roll to a new epoch.
 * Deferred records carry no generation. Reuse of an HWND always receives a new generation.
 *
 * Logical clock: every fact snapshot and destroy hint carries a nonzero stamp that the
 * observer draws from one monotonic counter. The graph tracks stamp_high_water so a
 * replacement observer can start above it. Facts older than the newest facts accepted
 * for the same window are rejected with StaleFacts.
 *
 * Tick clock: facts.captured_ms (observe, refresh_offer) and event_time_ms (note_destroyed) stay
 * 32-bit wire values in the Win32 tick domain, which wraps every 2^32 ms (about 49.7 days). The
 * graph never compares two 32-bit ticks directly. It keeps an internal 64-bit base, tick_base,
 * and unwraps every tick t32 to
 *     t64 = tick_base + (int64_t)(int32_t)(t32 - (uint32_t)tick_base)
 * where the subtraction wraps modulo 2^32. Every call that returns Ok and carries a tick then
 * sets tick_base to the larger of the old base and the t64 it computed (the base never moves
 * backwards; a delayed older tick unwraps below the base without changing it); a call
 * that does not return Ok never changes tick_base. The base starts at 2^32 so that no unwrapped
 * value is negative; only differences between unwrapped values are observable. Records store the
 * unwrapped 64-bit values (admission tick, last offered tick) and the destroy rule compares them
 * with a plain 64-bit "<". Unwrapping is exact while an Ok call that carries a tick happens at
 * least once per 2^31 ms (about 24.8 days); the observer's refresh cadence of
 * InteropWindowGraphRefreshIntervalMs satisfies this by orders of magnitude. After a longer
 * silence a tick can alias by a multiple of 2^32 ms, so a destroy hint may be ignored or applied
 * wrongly; the next refresh corrects both (RefreshUnseen retires a vanished window, a requery
 * re-admits a live one).
 *
 * Classification (pure function of facts and uac_mode; first matching step wins; exported as
 * interop_window_graph_classify() so the observer can classify an owner window without a graph):
 *  Definition. A window is "owned" iff owner != 0 AND OwnerHidden is clear. The observer sets
 *  OwnerHidden when the window's owner (owner_root) can never be exported or no longer exists. Such
 *  a window is treated as NOT owned in steps 4 to 7 and for the ImePreedit parent: a Menu stays a
 *  Menu with parent 0, a popup or owned tool window is judged with owned = false, and an owned
 *  window that is not APPWINDOW becomes Toplevel with parent 0. This is a deliberate refinement of
 *  IMPL_DESC section 4.4, which is silent about owners that are never exportable; it needs Codex
 *  sign-off. The observer alone decides the flag, so rejecting the refinement means the observer
 *  never sets OwnerHidden (nothing else in the graph changes). A change of the flag is not itself
 *  a change: only the resulting role or parent change goes through the Reclassified path
 *  (the old generation retires, a new one is announced).
 *  0. OnInputDesktop clear: Suspended (nothing admitted, retired or updated; the observer
 *     resynchronizes when the desktop returns). Visible clear: not exported (NotVisible).
 *  1. Excluded classes, compared ASCII case-insensitively over the full class length:
 *     Shell_TrayWnd, Shell_SecondaryTrayWnd, Progman, WorkerW, NotifyIconOverflowWindow,
 *     TaskListThumbnailWnd, "MSCTFIME UI", IME. Windows.UI.Core.CoreWindow is excluded
 *     only when the image basename equals ShellExperienceHost.exe, StartMenuExperienceHost.exe
 *     or SearchHost.exe (ASCII case-insensitive); any other owner, or an unknown image,
 *     is not excluded. A class with ClassTruncated set matches no class rule. AgentOwned:
 *     AgentPreedit selects role ImePreedit (steps 2 to 7 skipped, step 8 still applies) with
 *     parent = owner_root when owner != 0 and OwnerHidden is clear, otherwise parent 0 (so the host
 *     can map the preedit as a popup of its owner); without AgentPreedit the window is excluded.
 *  2. Image consent.exe: class "$$$Secure UAP Dummy Window Class For Interim Dialog" is
 *     excluded in every mode; class "Credential Dialog Xaml Host" is role UacConsent in
 *     modes Window and Auto and not exported (UacNativeHidden) in mode Native; any other
 *     consent.exe window continues with step 3. An unknown image never matches a UAC rule.
 *  3. root != window: not exported (Child).
 *  4. Menu: class "#32768" or "ComboLBox" (unconditional), or style WS_POPUP and owned and
 *     the caption bits not both set (title bar absent) and ex-style WS_EX_NOACTIVATE
 *     clear. The NOACTIVATE exclusion binds to the style clause only.
 *  5. Tooltip: class "tooltips_class32", or WS_EX_NOACTIVATE, or (WS_EX_TOOLWINDOW and owned).
 *  6. Owned and WS_EX_APPWINDOW clear: Dialog with parent = owner_root.
 *  7. Otherwise Toplevel (no parent); this includes an owned APPWINDOW and every window whose
 *     owner is hidden.
 *  8. Gate for every role: width == 0 or height == 0 is not exported (ZeroArea);
 *     Layered and LayeredAlphaKnown and alpha == 0 is not exported (Transparent). A layered
 *     window without alpha information, or with a color key only, is treated as opaque.
 *  Menu and Tooltip parent = owner_root when owned, otherwise none. Owned and APPWINDOW
 *  falls through to step 7. A disabled visible window is exported like an enabled one.
 *
 * Retirement is quarantine. An entry that stops being exported becomes Retiring: it keeps
 * occupying one of the InteropMaxWindows slots, blocks check_action, and queues one
 * RetireIntent event. Only the session coordinator ends the quarantine, with
 * interop_window_graph_complete_retire() after it has proven that capture stopped and every
 * frame pin drained. The graph releases nothing external.
 */
#include <stddef.h>
#include <stdint.h>

#include "waddle/interop_protocol.h"

_Static_assert(sizeof(void *) == 8, "window graph ABI supports 64-bit targets only");

/** @brief Maximum deferred (parent-pending) records; separate from InteropMaxWindows. */
#define InteropWindowGraphMaxDeferred 64u
/** @brief Capacity of the bounded event ring, in events. */
#define InteropWindowGraphEventCapacity 256u
/** @brief Upper bound on events one successful call can queue (3 * InteropMaxWindows). */
#define InteropWindowGraphMaxCallEvents 192u
/** @brief Cadence in milliseconds at which the caller should run a full refresh. */
#define InteropWindowGraphRefreshIntervalMs 250u
/** @brief Minimum milliseconds between two capacity diagnostics. */
#define InteropWindowGraphDiagnosticIntervalMs 60000u
/** @brief Maximum class-name bytes in a facts snapshot (UTF-8, no NUL). */
#define InteropWindowGraphMaxClassBytes 256u
/** @brief Maximum image basename bytes in a facts snapshot (UTF-8, no NUL). */
#define InteropWindowGraphMaxImageBytes 260u
/** @brief Maximum title bytes in facts and events; equals InteropMaxTitleBytes. */
#define InteropWindowGraphMaxTitleBytes InteropMaxTitleBytes
/** @brief Maximum application-key bytes; equals InteropMaxAppKeyBytes. */
#define InteropWindowGraphMaxAppKeyBytes InteropMaxAppKeyBytes
/** @brief Minimum accepted dpi in facts (matches the wire dpi tag). */
#define InteropWindowGraphMinDpi 96u
/** @brief Maximum accepted dpi in facts (matches the wire dpi tag). */
#define InteropWindowGraphMaxDpi 960u
/** @brief Maximum rect width/height and min_width/min_height (matches the wire rect and min_size). */
#define InteropWindowGraphMaxExtent 16384u

/** @brief Win32 window style bit WS_POPUP, as found in raw GWL_STYLE. */
#define InteropWindowStylePopup 0x80000000u
/** @brief Win32 style mask WS_CAPTION (WS_BORDER | WS_DLGFRAME); a title bar needs both bits. */
#define InteropWindowStyleCaption 0x00C00000u
/** @brief Win32 window style bit WS_THICKFRAME (maps to InteropStateResizable). */
#define InteropWindowStyleThickFrame 0x00040000u
/** @brief Win32 window style bit WS_MAXIMIZEBOX (maps to InteropStateMaximizable). */
#define InteropWindowStyleMaximizeBox 0x00010000u
/** @brief Win32 window style bit WS_MINIMIZEBOX (maps to InteropStateMinimizable). */
#define InteropWindowStyleMinimizeBox 0x00020000u
/** @brief Win32 extended style bit WS_EX_TOOLWINDOW, as found in raw GWL_EXSTYLE. */
#define InteropWindowExStyleToolWindow 0x00000080u
/** @brief Win32 extended style bit WS_EX_APPWINDOW. */
#define InteropWindowExStyleAppWindow 0x00040000u
/** @brief Win32 extended style bit WS_EX_LAYERED. */
#define InteropWindowExStyleLayered 0x00080000u
/** @brief Win32 extended style bit WS_EX_NOACTIVATE. */
#define InteropWindowExStyleNoActivate 0x08000000u

/** @brief Bits of interop_window_facts_t.flags. Any other bit is InvalidFacts. */
typedef enum interop_window_fact_flag_t {
    InteropWindowFactVisible = 1u << 0, /**< Window is shown: IsWindowVisible true AND not DWM-cloaked (adapter must clear this bit for cloaked windows). */
    InteropWindowFactEnabled = 1u << 1, /**< Window accepts input; recorded only, classification ignores it. */
    InteropWindowFactIconic = 1u << 2, /**< Window is minimized; maps to InteropStateMinimized. */
    InteropWindowFactZoomed = 1u << 3, /**< Window is maximized; maps to InteropStateMaximized. */
    InteropWindowFactLayered = 1u << 4, /**< WS_EX_LAYERED is set. */
    InteropWindowFactLayeredAlphaKnown = 1u << 5, /**< alpha holds a real constant alpha (requires Layered). */
    InteropWindowFactFullscreen = 1u << 6, /**< Adapter-computed: covers its whole monitor; maps to InteropStateFullscreen. */
    InteropWindowFactAgentOwned = 1u << 7, /**< Window belongs to the agent's own process. */
    InteropWindowFactAgentPreedit = 1u << 8, /**< Window is the agent's IME preedit window (requires AgentOwned). */
    InteropWindowFactOnInputDesktop = 1u << 9, /**< The snapshot was taken while the interactive input desktop was the Default desktop. */
    InteropWindowFactClassTruncated = 1u << 10, /**< class_name was cut to fit InteropWindowGraphMaxClassBytes; it then matches no class rule. */
    InteropWindowFactImageUnknown = 1u << 11, /**< The process image could not be read (neither OpenProcess nor the process-list fallback); set if and only if image_length is 0. */
    InteropWindowFactOwnerHidden = 1u << 12 /**< Observer-computed: owner != 0 and the owner window (owner_root) is never exported or no longer exists; classification then treats the window as not owned (see the classification text). Requires owner != 0. */
} interop_window_fact_flag_t;

/** @brief Mask of all defined interop_window_fact_flag_t bits. */
#define InteropWindowFactMask 0x1FFFu

/** @brief Bits of the changed_mask in events: which payload groups differ from what the consumer last saw. */
typedef enum interop_window_change_t {
    InteropWindowChangeRect = 1u << 0, /**< x, y, width or height changed. */
    InteropWindowChangeState = 1u << 1, /**< The derived interop_window_state_t bits changed. */
    InteropWindowChangeDpi = 1u << 2, /**< dpi changed. */
    InteropWindowChangeTitle = 1u << 3, /**< title changed (an empty title is a valid value). */
    InteropWindowChangeMinSize = 1u << 4 /**< min_width or min_height changed. */
} interop_window_change_t;

/** @brief Mask of all defined interop_window_change_t bits; the changed_mask of every Announce. */
#define InteropWindowChangeMask 0x1Fu

/** @brief Kinds of events queued by the graph. No FrameReady and no wire message exists here. */
typedef enum interop_window_event_kind_t {
    InteropWindowEventAnnounce = 1, /**< A new generation was admitted; parents are announced strictly before children. */
    InteropWindowEventUpdate = 2, /**< Rect/state/dpi/title/min-size of an announced generation changed. */
    InteropWindowEventRetireIntent = 3 /**< The generation entered quarantine; new actions are blocked, the coordinator must stop capture. */
} interop_window_event_kind_t;

/** @brief How the guest handles UAC consent windows (IMPL_DESC section 3.11). */
typedef enum interop_window_uac_mode_t {
    InteropWindowUacWindow = 0, /**< Export the consent dialog as role UacConsent. */
    InteropWindowUacAuto = 1, /**< Same export rule as Window (a prompt should not appear; if it does, exporting is fail-visible). */
    InteropWindowUacNative = 2 /**< Hide the consent dialog from export; the coordinator reads it by other means. */
} interop_window_uac_mode_t;

/**
 * @brief Outcome kinds of the pure classification (interop_window_graph_classify).
 *
 * Stored in interop_window_verdict_t.kind. The values are stable and nonzero.
 */
typedef enum interop_window_verdict_kind_t {
    InteropWindowVerdictEligible = 1, /**< The window would be exported: role and parent_window are valid. */
    InteropWindowVerdictNotExported = 2, /**< The window is not exported: reason (1..9) says why. */
    InteropWindowVerdictSuspended = 3 /**< OnInputDesktop is clear (classification step 0); no judgment is made. */
} interop_window_verdict_kind_t;

/** @brief Outcome class of one observe, refresh_offer or note_destroyed call that returned Ok. */
typedef enum interop_window_disposition_t {
    InteropWindowDispositionUnchanged = 1, /**< Nothing changed: an identical offer, or a destroy hint with nothing live to retire. */
    InteropWindowDispositionAdmitted = 2, /**< A new generation was admitted and its Announce queued (also flushed deferred children, see entries_admitted). */
    InteropWindowDispositionUpdated = 3, /**< An Update was queued or coalesced into a pending Announce/Update. */
    InteropWindowDispositionDeferred = 4, /**< Waiting for its parent to be announced; a deferred record was stored. */
    InteropWindowDispositionRetired = 5, /**< At least one live entry moved to Retiring (see reason and entries_retired). */
    InteropWindowDispositionReclassified = 6, /**< Role or parent changed: old generation retired and a new one announced. */
    InteropWindowDispositionExcluded = 7, /**< Classified as not exported and nothing was live; reason says why. */
    InteropWindowDispositionSuspended = 8 /**< The input desktop was not Default; the call had no effect. */
} interop_window_disposition_t;

/**
 * @brief Why a window is not exported, was deferred, or was retired.
 *
 * Values 1 to 9 are classification outcomes and appear both in observations and in
 * RetireIntent events. Values 10 to 16 describe graph decisions.
 */
typedef enum interop_window_reason_t {
    InteropWindowReasonNone = 0, /**< No reason applies. */
    InteropWindowReasonNotVisible = 1, /**< Visible flag clear. */
    InteropWindowReasonExcludedClass = 2, /**< Shell, tray, IME or other excluded class. */
    InteropWindowReasonExcludedCoreWindow = 3, /**< CoreWindow of ShellExperienceHost, StartMenuExperienceHost or SearchHost. */
    InteropWindowReasonExcludedAgent = 4, /**< The agent's own window other than the preedit window. */
    InteropWindowReasonExcludedUacDummy = 5, /**< consent.exe interim dummy window. */
    InteropWindowReasonUacNativeHidden = 6, /**< consent.exe credential dialog hidden in UAC mode Native. */
    InteropWindowReasonChild = 7, /**< root != window: a child window is never exported. */
    InteropWindowReasonZeroArea = 8, /**< Width or height is zero. */
    InteropWindowReasonTransparent = 9, /**< Layered with known constant alpha 0. */
    InteropWindowReasonParentPending = 10, /**< Deferred: the parent is not an announced, still-exported generation. */
    InteropWindowReasonDestroyed = 11, /**< A destroy hint applied (see the destroy rule and its deviation note), or the window no longer exists. */
    InteropWindowReasonIncarnationChanged = 12, /**< Same HWND, different (process, thread, start time, class) identity. */
    InteropWindowReasonReclassified = 13, /**< Role or parent changed (also through an OwnerHidden change), which needs a new host surface. */
    InteropWindowReasonParentRetired = 14, /**< An ancestor was retired (children retire first); the reason of every descendant of a retired entry, whatever the cause. Descendants are re-deferred only when the ancestor's own cause is Reclassified of the same incarnation; for every other cause they are not re-deferred and return through their next hint or refresh offer. */
    InteropWindowReasonRefreshUnseen = 15, /**< Not marked seen (offered or kept) during a committed refresh. */
    InteropWindowReasonInvalidated = 16 /**< interop_window_graph_invalidate_all(). */
} interop_window_reason_t;

/**
 * @brief Result codes of every graph call. Values are stable. Only Ok and None are
 *        non-error results: compare against Ok explicitly, None is nonzero.
 *
 * Effects per status are specified by each function; the common rules are in the file
 * comment.
 */
typedef enum interop_window_graph_status_t {
    InteropWindowGraphStatusOk = 0, /**< The call completed and its outputs are valid. */
    InteropWindowGraphStatusNone = 1, /**< Benign: nothing to return (empty queue, nothing due, no live window, refresh_keep found neither an Active entry nor a deferred record). Outputs unchanged. */
    InteropWindowGraphStatusInvalidArgument = 2, /**< NULL graph/pointer, zero window/generation/budget, bad config, uac_mode above 2; nothing changed. */
    InteropWindowGraphStatusInvalidFacts = 3, /**< Facts violate the validation list on interop_window_facts_t; nothing changed. */
    InteropWindowGraphStatusWrongThread = 4, /**< Not the creating thread; nothing touched (a destroy does not free). */
    InteropWindowGraphStatusReentrant = 5, /**< Called from inside a graph callback; nothing touched. */
    InteropWindowGraphStatusNoMemory = 6, /**< create could not allocate; no graph exists. */
    InteropWindowGraphStatusStaleEpoch = 7, /**< session_epoch is not this graph's epoch; only a counter changed. */
    InteropWindowGraphStatusStale = 8, /**< No entry has that exact (window, generation), or it was already completed; nothing changed. */
    InteropWindowGraphStatusStaleFacts = 9, /**< Facts are older than the newest accepted for that window; nothing changed. */
    InteropWindowGraphStatusQueueFull = 10, /**< The event ring lacks room for the whole plan; nothing changed; drain and retry. */
    InteropWindowGraphStatusAtCapacity = 11, /**< 64 exported entries (Active plus Retiring) and no eviction is allowed; nothing changed. */
    InteropWindowGraphStatusCycle = 12, /**< Self-ownership or an owner cycle through deferred records; see Cycle exception. A cycle met while replacing a live entry whose identity changed, or a cascade cycle, is not returned: the retirement happens and the call returns Ok (Retired). */
    InteropWindowGraphStatusLookupFailed = 13, /**< app_key_lookup returned nonzero; nothing published or fabricated. */
    InteropWindowGraphStatusAppKeyInvalid = 14, /**< The looked-up key is not a valid protocol-v2 application key (3..200 bytes, "a-" or "p-" prefix, only ASCII letters, digits, '_' and '-'); nothing published. */
    InteropWindowGraphStatusGenerationExhausted = 15, /**< No generation is left; existing entries keep working; roll to a new epoch. */
    InteropWindowGraphStatusRefreshActive = 16, /**< refresh_begin while a refresh is already open; the open refresh continues. */
    InteropWindowGraphStatusNoRefresh = 17, /**< refresh_offer/keep/unseen/commit/abort with no open refresh. */
    InteropWindowGraphStatusProofIncomplete = 18, /**< complete_retire proof flags are not both exactly 1; the entry stays quarantined. */
    InteropWindowGraphStatusNotRetiring = 19, /**< complete_retire named an Active entry; nothing changed. */
    InteropWindowGraphStatusSinkFailed = 20 /**< The drain sink refused an event; that event stays queued. */
} interop_window_graph_status_t;

/** @brief Opaque graph handle. Allocated by create, freed by destroy; layout is private to Zig. */
typedef struct interop_window_graph_t interop_window_graph_t;

/**
 * @brief Portable snapshot of one native window, supplied by the observer. Caller-owned value.
 *
 * The graph is the trust boundary and validates every field; any violation returns
 * InvalidFacts with no effect. Zero the whole struct before filling it (padding and
 * reserved bytes must be zero). Rules:
 *  - window, root and stamp are nonzero; owner != 0 requires owner_root != 0 and
 *    owner == 0 requires owner_root == 0.
 *  - flags contains only InteropWindowFactMask bits; AgentPreedit requires AgentOwned;
 *    LayeredAlphaKnown requires Layered; ImageUnknown is set if and only if image_length is 0;
 *    OwnerHidden requires owner != 0 (otherwise InvalidFacts).
 *  - dpi is in InteropWindowGraphMinDpi..InteropWindowGraphMaxDpi.
 *  - width, height, min_width and min_height are at most InteropWindowGraphMaxExtent. The
 *    graph never clamps: a larger window is rejected, not silently resized.
 *  - class_length is 1..InteropWindowGraphMaxClassBytes, image_length at most
 *    InteropWindowGraphMaxImageBytes, title_length at most InteropWindowGraphMaxTitleBytes.
 *    class_name, image_name and title are valid UTF-8 (no overlong forms, no surrogates,
 *    nothing above U+10FFFF) without NUL. Bytes after the stated length are ignored and
 *    never copied. title_length 0 is valid.
 *  - reserved0 is zero.
 *
 * Ticks: captured_ms is a 32-bit wire value; the graph unwraps it against an internal 64-bit
 * base (see Tick clock in the file comment), so the caller always passes the raw 32-bit tick.
 * observe and refresh_offer validate and use a stack-local copy of this struct (see Facts copy).
 */
typedef struct interop_window_facts_t {
    uint64_t window; /**< HWND zero-extended; nonzero. */
    uint64_t owner; /**< GW_OWNER, or 0 when ownerless. */
    uint64_t owner_root; /**< GA_ROOT(owner), or 0 when ownerless. */
    uint64_t root; /**< GA_ROOT(window); equals window for a top-level window. */
    uint64_t process_start_time; /**< Creation time of the owning process in 100 ns units; 0 = unknown. */
    uint64_t stamp; /**< Observer logical clock; nonzero, non-decreasing across calls. */
    uint32_t style; /**< Raw GWL_STYLE bits. */
    uint32_t ex_style; /**< Raw GWL_EXSTYLE bits. */
    uint32_t flags; /**< interop_window_fact_flag_t bits (InteropWindowFactMask, 13 bits). */
    uint32_t process_id; /**< Owning process id (reported as is). */
    uint32_t thread_id; /**< Creating thread id. */
    uint32_t captured_ms; /**< Raw 32-bit tick-domain time of the query. Never compared as 32-bit values: the graph unwraps it to 64 bits against its internal tick_base, advances the base on every Ok call, and stores the unwrapped value as the record's admission or last-offered tick. */
    int32_t x; /**< Left edge in guest desktop pixels (restored geometry when iconic). */
    int32_t y; /**< Top edge in guest desktop pixels (restored geometry when iconic). */
    uint32_t width; /**< Physical pixel width, 0..InteropWindowGraphMaxExtent. */
    uint32_t height; /**< Physical pixel height, 0..InteropWindowGraphMaxExtent. */
    uint32_t dpi; /**< Effective window dpi, 96..960. */
    uint32_t min_width; /**< Minimum tracking width; 0 = none. The Win32 adapter currently always reports 0. */
    uint32_t min_height; /**< Minimum tracking height; 0 = none. The Win32 adapter currently always reports 0. */
    uint32_t class_length; /**< Valid bytes in class_name, 1..256. */
    uint32_t image_length; /**< Valid bytes in image_name, 0..260. */
    uint32_t title_length; /**< Valid bytes in title, 0..1024. */
    uint8_t alpha; /**< Constant layered alpha; meaningful only with Layered and LayeredAlphaKnown. */
    uint8_t reserved0[3]; /**< Must be zero. */
    uint8_t class_name[InteropWindowGraphMaxClassBytes]; /**< Window class, UTF-8, not NUL-terminated. */
    uint8_t image_name[InteropWindowGraphMaxImageBytes]; /**< Image basename such as consent.exe, UTF-8, not NUL-terminated. */
    uint8_t title[InteropWindowGraphMaxTitleBytes]; /**< Window title, UTF-8, cut at a code-point boundary by the observer, not NUL-terminated. */
} interop_window_facts_t;

/**
 * @brief Result of the pure classification interop_window_graph_classify. Caller-owned value.
 *
 * Which fields are meaningful depends on kind; every field that is not meaningful for the kind is
 * written as zero. The struct holds no pointer and is fully written on Ok.
 */
typedef struct interop_window_verdict_t {
    uint32_t kind; /**< interop_window_verdict_kind_t; always valid on Ok. */
    uint32_t role; /**< interop_window_role_t; meaningful only for Eligible, otherwise 0. */
    uint32_t reason; /**< interop_window_reason_t, 1..9 (a classification outcome); meaningful only for NotExported, otherwise 0. */
    uint32_t reserved0; /**< Zero. */
    uint64_t parent_window; /**< Parent HWND the role implies (0 = none); meaningful only for Eligible, otherwise 0. Whether that parent is announced is a graph matter and not judged here. */
} interop_window_verdict_t;

/**
 * @brief One queued event. Value copy; nothing in it points into graph storage.
 *
 * Announce and Update carry the full current payload; Update's changed_mask says which
 * optional wire groups (title, min_size) and which required groups changed since the
 * consumer last saw this generation. Announce always has changed_mask ==
 * InteropWindowChangeMask. RetireIntent carries the last known payload, reason, and a
 * changed_mask of 0. Events are delivered in queue order; a parent's Announce precedes
 * every child's Announce, and children's RetireIntent precede their parent's.
 * Coalescing: while queueing an Announce or Update for (window, generation), if the
 * newest queued event of that identity is an unconsumed Announce or Update, its payload
 * is overwritten in place (an Announce stays an Announce, changed_mask is OR-ed, the
 * events_coalesced counter is incremented). Events never coalesce across a RetireIntent.
 */
typedef struct interop_window_event_t {
    uint64_t session_epoch; /**< Epoch of the graph that produced the event. */
    uint64_t window; /**< HWND zero-extended. */
    uint64_t generation; /**< Generation of this incarnation, nonzero. */
    uint64_t parent_window; /**< Parent HWND, 0 = none. */
    uint64_t parent_generation; /**< Generation of the Active parent at admission, 0 when parent_window is 0. */
    int32_t x; /**< Left edge in guest desktop pixels. */
    int32_t y; /**< Top edge in guest desktop pixels. */
    uint32_t width; /**< Physical pixel width. */
    uint32_t height; /**< Physical pixel height. */
    uint32_t kind; /**< interop_window_event_kind_t. */
    uint32_t role; /**< interop_window_role_t. */
    uint32_t state; /**< interop_window_state_t bits derived from the facts; never Activated. */
    uint32_t dpi; /**< 96..960. */
    uint32_t min_width; /**< 0 = none. */
    uint32_t min_height; /**< 0 = none. */
    uint32_t process_id; /**< Owning process id. */
    uint32_t changed_mask; /**< interop_window_change_t bits. */
    uint32_t reason; /**< interop_window_reason_t; meaningful for RetireIntent, otherwise 0. */
    uint32_t title_length; /**< Valid bytes in title, 0..1024. */
    uint32_t app_key_length; /**< Valid bytes in app_key, 3..200. */
    uint32_t reserved0; /**< Zero. */
    uint8_t title[InteropWindowGraphMaxTitleBytes]; /**< Title copy, UTF-8, not NUL-terminated. */
    uint8_t app_key[InteropWindowGraphMaxAppKeyBytes]; /**< Application key copy; a validated protocol-v2 key (ASCII), not NUL-terminated. */
} interop_window_event_t;

/** @brief Result summary of observe, refresh_offer and note_destroyed. Fully written on Ok, unchanged otherwise. */
typedef struct interop_window_observation_t {
    uint64_t window; /**< Window the call was about. */
    uint64_t generation; /**< Live generation after the call (new one for Admitted/Reclassified); 0 for Deferred, Retired, Excluded, Suspended and an Unchanged non-live result. */
    uint64_t retired_generation; /**< Generation of this window moved to Retiring by this call; 0 if none. */
    uint32_t disposition; /**< interop_window_disposition_t. */
    uint32_t role; /**< interop_window_role_t of the live entry or deferred record, otherwise 0. */
    uint32_t reason; /**< interop_window_reason_t: when retired_generation is nonzero, why it retired; else the classification reason (Excluded), ParentPending (Deferred), or 0. */
    uint32_t events_queued; /**< New events appended by this call (a coalesced payload counts as 0). */
    uint32_t entries_admitted; /**< Entries announced by this call, including the primary window and flushed deferred children. */
    uint32_t entries_retired; /**< Entries moved to Retiring by this call, including descendants. */
    uint32_t deferred_dropped; /**< Deferred records dropped or evicted by this call. */
    uint32_t reserved0; /**< Zero. */
} interop_window_observation_t;

/** @brief Description of the live (Active) entry for a window. Fully written on Ok. */
typedef struct interop_window_live_t {
    uint64_t window; /**< HWND. */
    uint64_t generation; /**< Live generation. */
    uint64_t parent_window; /**< Parent HWND, 0 = none. */
    uint64_t parent_generation; /**< Parent generation, 0 = none. */
    uint32_t role; /**< interop_window_role_t. */
    uint32_t state; /**< interop_window_state_t bits as last announced or updated. */
    uint32_t process_id; /**< Owning process id. */
    uint32_t reserved0; /**< Zero. */
} interop_window_live_t;

/** @brief One quarantined entry as listed by list_retiring. */
typedef struct interop_window_retiring_entry_t {
    uint64_t window; /**< HWND. */
    uint64_t generation; /**< Generation in quarantine. */
    uint32_t reason; /**< interop_window_reason_t of the retirement. */
    uint32_t role; /**< interop_window_role_t it had. */
} interop_window_retiring_entry_t;

/**
 * @brief Coordinator proof for ending a quarantine. Both flags must be exactly 1.
 *
 * There is no flag for "connection lost": losing the transport is never proof.
 */
typedef struct interop_window_retire_proof_t {
    uint32_t capture_stopped; /**< Exactly 1 once capture for the generation has stopped. */
    uint32_t pins_drained; /**< Exactly 1 once every outstanding frame pin for the generation has drained. */
} interop_window_retire_proof_t;

/** @brief Counters since the last successful take_capacity_diagnostic; the output of that call. */
typedef struct interop_window_capacity_diagnostic_t {
    uint64_t at_capacity; /**< Admissions refused or cut short because 64 entries were exported. */
    uint64_t deferred_evicted; /**< Oldest deferred records evicted to admit a newer deferral. */
    uint64_t queue_full; /**< Calls rejected with QueueFull. */
    uint64_t generation_exhausted; /**< Admissions refused because no generation was left. */
    uint64_t elapsed_ms; /**< Milliseconds since the previous successful take; 0 for the first. */
    uint32_t active_count; /**< Active entries now. */
    uint32_t retiring_count; /**< Retiring entries now. */
    uint32_t deferred_count; /**< Deferred records now. */
    uint32_t events_pending; /**< Queued events now. */
} interop_window_capacity_diagnostic_t;

/** @brief Cumulative counters and gauges. Counters start at 0 at create, saturate at UINT64_MAX, never wrap. */
typedef struct interop_window_graph_stats_t {
    uint64_t session_epoch; /**< Epoch fixed at create. */
    uint64_t generations_issued; /**< Last generation handed out; 0 before the first admission. */
    uint64_t stamp_high_water; /**< Largest stamp accepted by a call that returned Ok. */
    uint32_t active_count; /**< Gauge: Active entries. */
    uint32_t retiring_count; /**< Gauge: Retiring entries. */
    uint32_t deferred_count; /**< Gauge: deferred records. */
    uint32_t events_pending; /**< Gauge: queued events. */
    uint32_t refresh_active; /**< Gauge: 1 while a refresh is open. */
    uint32_t reserved0; /**< Zero. */
    uint64_t admitted; /**< Generations admitted. */
    uint64_t updates_queued; /**< Update events appended (not counting coalesced ones). */
    uint64_t reclassified; /**< Role/parent reclassifications. */
    uint64_t retire_intents; /**< Entries moved to Retiring. */
    uint64_t retire_completed; /**< Quarantines ended by complete_retire. */
    uint64_t deferred_added; /**< Deferred records created. */
    uint64_t deferred_evicted; /**< Deferred records evicted by a newer deferral. */
    uint64_t deferred_dropped; /**< Deferred records dropped for any other reason. */
    uint64_t events_queued; /**< Events appended. */
    uint64_t events_coalesced; /**< Payloads merged into a pending event. */
    uint64_t events_drained; /**< Events popped by next_event or drain. */
    uint64_t refresh_commits; /**< Refreshes committed. */
    uint64_t refresh_aborts; /**< Refreshes aborted (explicitly or by invalidate_all). */
    uint64_t rejected_at_capacity; /**< Admissions blocked by the 64-entry limit. */
    uint64_t rejected_queue_full; /**< Calls rejected with QueueFull. */
    uint64_t rejected_stale_epoch; /**< Calls rejected with StaleEpoch. */
    uint64_t rejected_stale_facts; /**< Offers rejected with StaleFacts. */
    uint64_t rejected_invalid_facts; /**< Offers rejected with InvalidFacts. */
    uint64_t rejected_cycle; /**< Offers rejected with Cycle (also counted when the cycle was met during a forced retirement and the call returned Ok). */
    uint64_t rejected_generation_exhausted; /**< Admissions blocked by an exhausted counter. */
    uint64_t lookup_failures; /**< app_key_lookup returned nonzero. */
    uint64_t app_key_invalid; /**< Lookup produced an invalid key. */
    uint64_t sink_failures; /**< Drain sink refusals. */
    uint64_t rejected_stale; /**< check_action/complete_retire answered Stale. */
    uint64_t proof_incomplete; /**< complete_retire answered ProofIncomplete. */
    uint64_t suspended; /**< Offers answered Suspended. */
} interop_window_graph_stats_t;

/**
 * @brief Caller-supplied application-key lookup, called at most once per mutating call.
 *
 * Invoked synchronously on the owner thread while the graph is mid-call and not
 * re-entrant: the callback must not call any graph function (they return Reentrant) and
 * must not retain facts or key.
 *
 * @param[in,out] context Opaque pointer from the config; may be NULL; owned by the caller.
 * @param[in] facts Non-NULL borrowed snapshot of the window being admitted or deferred: the graph's
 *                  stack-local copy of the caller's facts (never the caller's own pointer); valid only
 *                  during the call.
 * @param[out] key Non-NULL writable buffer of capacity bytes (always InteropMaxAppKeyBytes); not NUL-terminated.
 * @param[in] capacity Bytes writable in key; the callback must never write more.
 * @param[out] length Non-NULL; preset to 0 before the call. The callback stores the full key length;
 *                    a length of 0 or above capacity, or bytes that are not a valid protocol-v2
 *                    application key (3..200 bytes, prefix "a-" or "p-", only ASCII letters,
 *                    digits, '_' and '-', exactly the frozen codec rule), is reported as AppKeyInvalid.
 * @return 0 on success; nonzero reports LookupFailed. No application identity may be invented on failure.
 * @note Thread rules: runs on the owner thread only.
 */
typedef int (*interop_window_app_key_lookup_t)(void *context,
                                               const interop_window_facts_t *facts,
                                               uint8_t *key, uint32_t capacity,
                                               uint32_t *length);

/**
 * @brief Monotonic millisecond clock used only for capacity-diagnostic rate limiting.
 *
 * @param[in,out] context Opaque pointer from the config; may be NULL; owned by the caller.
 * @return Milliseconds on a monotonic clock. A value that goes backwards counts as zero elapsed time.
 * @note Must not call into the graph; runs on the owner thread only.
 */
typedef uint64_t (*interop_window_now_ms_t)(void *context);

/**
 * @brief Event sink for drain.
 *
 * @param[in,out] context Opaque pointer given to drain; may be NULL; owned by the caller.
 * @param[in] event Non-NULL borrowed copy valid only during the call; the sink must copy what it needs.
 * @return 0 when the event was accepted (it is then popped); nonzero refuses it (it stays queued and drain stops).
 * @note Must not call into the graph (Reentrant); runs on the owner thread only.
 */
typedef int (*interop_window_event_sink_t)(void *context, const interop_window_event_t *event);

/** @brief Graph creation parameters. The struct is copied; the pointers inside are borrowed. */
typedef struct interop_window_graph_config_t {
    uint64_t session_epoch; /**< Nonzero epoch this graph serves for its whole life. */
    uint32_t uac_mode; /**< interop_window_uac_mode_t. */
    uint32_t reserved0; /**< Must be zero. */
    interop_window_app_key_lookup_t app_key_lookup; /**< Required, non-NULL. */
    void *app_key_context; /**< Passed to app_key_lookup; must outlive the graph. */
    interop_window_now_ms_t now_ms; /**< NULL selects the default monotonic clock. */
    void *now_ms_context; /**< Passed to now_ms; must outlive the graph. */
} interop_window_graph_config_t;

_Static_assert(sizeof(interop_window_fact_flag_t) == 4, "fact flag enum ABI");
_Static_assert(sizeof(interop_window_change_t) == 4, "change enum ABI");
_Static_assert(sizeof(interop_window_event_kind_t) == 4, "event kind enum ABI");
_Static_assert(sizeof(interop_window_uac_mode_t) == 4, "uac mode enum ABI");
_Static_assert(sizeof(interop_window_verdict_kind_t) == 4, "verdict kind enum ABI");
_Static_assert(sizeof(interop_window_disposition_t) == 4, "disposition enum ABI");
_Static_assert(sizeof(interop_window_reason_t) == 4, "reason enum ABI");
_Static_assert(sizeof(interop_window_graph_status_t) == 4, "status enum ABI");
_Static_assert(InteropWindowGraphMaxCallEvents == 3u * InteropMaxWindows, "per-call event bound is three per window");
_Static_assert(InteropWindowGraphMaxCallEvents <= InteropWindowGraphEventCapacity, "one call must fit an empty ring");
_Static_assert(InteropWindowGraphMaxDeferred == InteropMaxWindows, "deferred bound matches IMPL_DESC 3.2.8");
_Static_assert(InteropWindowFactMask == 0x1FFFu, "fact mask is 13 bits");
_Static_assert(InteropWindowFactMask == ((InteropWindowFactOwnerHidden << 1) - 1u), "fact mask covers all flags");
_Static_assert(InteropWindowChangeMask == ((InteropWindowChangeMinSize << 1) - 1u), "change mask covers all bits");

_Static_assert(sizeof(interop_window_facts_t) == 1656, "facts ABI");
_Static_assert(offsetof(interop_window_facts_t, window) == 0, "facts window ABI");
_Static_assert(offsetof(interop_window_facts_t, owner) == 8, "facts owner ABI");
_Static_assert(offsetof(interop_window_facts_t, owner_root) == 16, "facts owner_root ABI");
_Static_assert(offsetof(interop_window_facts_t, root) == 24, "facts root ABI");
_Static_assert(offsetof(interop_window_facts_t, process_start_time) == 32, "facts process_start_time ABI");
_Static_assert(offsetof(interop_window_facts_t, stamp) == 40, "facts stamp ABI");
_Static_assert(offsetof(interop_window_facts_t, style) == 48, "facts style ABI");
_Static_assert(offsetof(interop_window_facts_t, ex_style) == 52, "facts ex_style ABI");
_Static_assert(offsetof(interop_window_facts_t, flags) == 56, "facts flags ABI");
_Static_assert(offsetof(interop_window_facts_t, process_id) == 60, "facts process_id ABI");
_Static_assert(offsetof(interop_window_facts_t, thread_id) == 64, "facts thread_id ABI");
_Static_assert(offsetof(interop_window_facts_t, captured_ms) == 68, "facts captured_ms ABI");
_Static_assert(offsetof(interop_window_facts_t, x) == 72, "facts x ABI");
_Static_assert(offsetof(interop_window_facts_t, y) == 76, "facts y ABI");
_Static_assert(offsetof(interop_window_facts_t, width) == 80, "facts width ABI");
_Static_assert(offsetof(interop_window_facts_t, height) == 84, "facts height ABI");
_Static_assert(offsetof(interop_window_facts_t, dpi) == 88, "facts dpi ABI");
_Static_assert(offsetof(interop_window_facts_t, min_width) == 92, "facts min_width ABI");
_Static_assert(offsetof(interop_window_facts_t, min_height) == 96, "facts min_height ABI");
_Static_assert(offsetof(interop_window_facts_t, class_length) == 100, "facts class_length ABI");
_Static_assert(offsetof(interop_window_facts_t, image_length) == 104, "facts image_length ABI");
_Static_assert(offsetof(interop_window_facts_t, title_length) == 108, "facts title_length ABI");
_Static_assert(offsetof(interop_window_facts_t, alpha) == 112, "facts alpha ABI");
_Static_assert(offsetof(interop_window_facts_t, reserved0) == 113, "facts reserved ABI");
_Static_assert(offsetof(interop_window_facts_t, class_name) == 116, "facts class_name ABI");
_Static_assert(offsetof(interop_window_facts_t, image_name) == 372, "facts image_name ABI");
_Static_assert(offsetof(interop_window_facts_t, title) == 632, "facts title ABI");

_Static_assert(sizeof(interop_window_verdict_t) == 24, "verdict ABI");
_Static_assert(offsetof(interop_window_verdict_t, kind) == 0, "verdict kind ABI");
_Static_assert(offsetof(interop_window_verdict_t, role) == 4, "verdict role ABI");
_Static_assert(offsetof(interop_window_verdict_t, reason) == 8, "verdict reason ABI");
_Static_assert(offsetof(interop_window_verdict_t, reserved0) == 12, "verdict reserved ABI");
_Static_assert(offsetof(interop_window_verdict_t, parent_window) == 16, "verdict parent ABI");

_Static_assert(sizeof(interop_window_event_t) == 1328, "event ABI");
_Static_assert(offsetof(interop_window_event_t, session_epoch) == 0, "event epoch ABI");
_Static_assert(offsetof(interop_window_event_t, window) == 8, "event window ABI");
_Static_assert(offsetof(interop_window_event_t, generation) == 16, "event generation ABI");
_Static_assert(offsetof(interop_window_event_t, parent_window) == 24, "event parent ABI");
_Static_assert(offsetof(interop_window_event_t, parent_generation) == 32, "event parent generation ABI");
_Static_assert(offsetof(interop_window_event_t, x) == 40, "event x ABI");
_Static_assert(offsetof(interop_window_event_t, y) == 44, "event y ABI");
_Static_assert(offsetof(interop_window_event_t, width) == 48, "event width ABI");
_Static_assert(offsetof(interop_window_event_t, height) == 52, "event height ABI");
_Static_assert(offsetof(interop_window_event_t, kind) == 56, "event kind ABI");
_Static_assert(offsetof(interop_window_event_t, role) == 60, "event role ABI");
_Static_assert(offsetof(interop_window_event_t, state) == 64, "event state ABI");
_Static_assert(offsetof(interop_window_event_t, dpi) == 68, "event dpi ABI");
_Static_assert(offsetof(interop_window_event_t, min_width) == 72, "event min_width ABI");
_Static_assert(offsetof(interop_window_event_t, min_height) == 76, "event min_height ABI");
_Static_assert(offsetof(interop_window_event_t, process_id) == 80, "event process_id ABI");
_Static_assert(offsetof(interop_window_event_t, changed_mask) == 84, "event changed_mask ABI");
_Static_assert(offsetof(interop_window_event_t, reason) == 88, "event reason ABI");
_Static_assert(offsetof(interop_window_event_t, title_length) == 92, "event title_length ABI");
_Static_assert(offsetof(interop_window_event_t, app_key_length) == 96, "event app_key_length ABI");
_Static_assert(offsetof(interop_window_event_t, reserved0) == 100, "event reserved ABI");
_Static_assert(offsetof(interop_window_event_t, title) == 104, "event title ABI");
_Static_assert(offsetof(interop_window_event_t, app_key) == 1128, "event app_key ABI");

_Static_assert(sizeof(interop_window_observation_t) == 56, "observation ABI");
_Static_assert(offsetof(interop_window_observation_t, window) == 0, "observation window ABI");
_Static_assert(offsetof(interop_window_observation_t, generation) == 8, "observation generation ABI");
_Static_assert(offsetof(interop_window_observation_t, retired_generation) == 16, "observation retired generation ABI");
_Static_assert(offsetof(interop_window_observation_t, disposition) == 24, "observation disposition ABI");
_Static_assert(offsetof(interop_window_observation_t, role) == 28, "observation role ABI");
_Static_assert(offsetof(interop_window_observation_t, reason) == 32, "observation reason ABI");
_Static_assert(offsetof(interop_window_observation_t, events_queued) == 36, "observation events ABI");
_Static_assert(offsetof(interop_window_observation_t, entries_admitted) == 40, "observation admitted ABI");
_Static_assert(offsetof(interop_window_observation_t, entries_retired) == 44, "observation retired ABI");
_Static_assert(offsetof(interop_window_observation_t, deferred_dropped) == 48, "observation dropped ABI");
_Static_assert(offsetof(interop_window_observation_t, reserved0) == 52, "observation reserved ABI");

_Static_assert(sizeof(interop_window_live_t) == 48, "live ABI");
_Static_assert(offsetof(interop_window_live_t, window) == 0, "live window ABI");
_Static_assert(offsetof(interop_window_live_t, generation) == 8, "live generation ABI");
_Static_assert(offsetof(interop_window_live_t, parent_window) == 16, "live parent ABI");
_Static_assert(offsetof(interop_window_live_t, parent_generation) == 24, "live parent generation ABI");
_Static_assert(offsetof(interop_window_live_t, role) == 32, "live role ABI");
_Static_assert(offsetof(interop_window_live_t, state) == 36, "live state ABI");
_Static_assert(offsetof(interop_window_live_t, process_id) == 40, "live process ABI");
_Static_assert(offsetof(interop_window_live_t, reserved0) == 44, "live reserved ABI");

_Static_assert(sizeof(interop_window_retiring_entry_t) == 24, "retiring entry ABI");
_Static_assert(offsetof(interop_window_retiring_entry_t, window) == 0, "retiring window ABI");
_Static_assert(offsetof(interop_window_retiring_entry_t, generation) == 8, "retiring generation ABI");
_Static_assert(offsetof(interop_window_retiring_entry_t, reason) == 16, "retiring reason ABI");
_Static_assert(offsetof(interop_window_retiring_entry_t, role) == 20, "retiring role ABI");

_Static_assert(sizeof(interop_window_retire_proof_t) == 8, "proof ABI");
_Static_assert(offsetof(interop_window_retire_proof_t, capture_stopped) == 0, "proof capture ABI");
_Static_assert(offsetof(interop_window_retire_proof_t, pins_drained) == 4, "proof pins ABI");

_Static_assert(sizeof(interop_window_capacity_diagnostic_t) == 56, "diagnostic ABI");
_Static_assert(offsetof(interop_window_capacity_diagnostic_t, at_capacity) == 0, "diagnostic capacity ABI");
_Static_assert(offsetof(interop_window_capacity_diagnostic_t, deferred_evicted) == 8, "diagnostic evicted ABI");
_Static_assert(offsetof(interop_window_capacity_diagnostic_t, queue_full) == 16, "diagnostic queue ABI");
_Static_assert(offsetof(interop_window_capacity_diagnostic_t, generation_exhausted) == 24, "diagnostic generation ABI");
_Static_assert(offsetof(interop_window_capacity_diagnostic_t, elapsed_ms) == 32, "diagnostic elapsed ABI");
_Static_assert(offsetof(interop_window_capacity_diagnostic_t, active_count) == 40, "diagnostic active ABI");
_Static_assert(offsetof(interop_window_capacity_diagnostic_t, retiring_count) == 44, "diagnostic retiring ABI");
_Static_assert(offsetof(interop_window_capacity_diagnostic_t, deferred_count) == 48, "diagnostic deferred ABI");
_Static_assert(offsetof(interop_window_capacity_diagnostic_t, events_pending) == 52, "diagnostic pending ABI");

_Static_assert(sizeof(interop_window_graph_stats_t) == 256, "stats ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, session_epoch) == 0, "stats epoch ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, generations_issued) == 8, "stats generations ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, stamp_high_water) == 16, "stats stamp ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, active_count) == 24, "stats active ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, retiring_count) == 28, "stats retiring ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, deferred_count) == 32, "stats deferred ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, events_pending) == 36, "stats pending ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, refresh_active) == 40, "stats refresh ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, reserved0) == 44, "stats reserved ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, admitted) == 48, "stats admitted ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, updates_queued) == 56, "stats updates ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, reclassified) == 64, "stats reclassified ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, retire_intents) == 72, "stats retire intents ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, retire_completed) == 80, "stats retire completed ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, deferred_added) == 88, "stats deferred added ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, deferred_evicted) == 96, "stats deferred evicted ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, deferred_dropped) == 104, "stats deferred dropped ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, events_queued) == 112, "stats events queued ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, events_coalesced) == 120, "stats events coalesced ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, events_drained) == 128, "stats events drained ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, refresh_commits) == 136, "stats refresh commits ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, refresh_aborts) == 144, "stats refresh aborts ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, rejected_at_capacity) == 152, "stats at capacity ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, rejected_queue_full) == 160, "stats queue full ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, rejected_stale_epoch) == 168, "stats stale epoch ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, rejected_stale_facts) == 176, "stats stale facts ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, rejected_invalid_facts) == 184, "stats invalid facts ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, rejected_cycle) == 192, "stats cycle ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, rejected_generation_exhausted) == 200, "stats generation exhausted ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, lookup_failures) == 208, "stats lookup failures ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, app_key_invalid) == 216, "stats app key invalid ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, sink_failures) == 224, "stats sink failures ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, rejected_stale) == 232, "stats stale ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, proof_incomplete) == 240, "stats proof incomplete ABI");
_Static_assert(offsetof(interop_window_graph_stats_t, suspended) == 248, "stats suspended ABI");

_Static_assert(sizeof(interop_window_graph_config_t) == 48, "config ABI");
_Static_assert(offsetof(interop_window_graph_config_t, session_epoch) == 0, "config epoch ABI");
_Static_assert(offsetof(interop_window_graph_config_t, uac_mode) == 8, "config uac ABI");
_Static_assert(offsetof(interop_window_graph_config_t, reserved0) == 12, "config reserved ABI");
_Static_assert(offsetof(interop_window_graph_config_t, app_key_lookup) == 16, "config lookup ABI");
_Static_assert(offsetof(interop_window_graph_config_t, app_key_context) == 24, "config lookup context ABI");
_Static_assert(offsetof(interop_window_graph_config_t, now_ms) == 32, "config clock ABI");
_Static_assert(offsetof(interop_window_graph_config_t, now_ms_context) == 40, "config clock context ABI");

/**
 * @brief Create a graph bound to the calling thread.
 *
 * @param[in] config Non-NULL borrowed config (copied). session_epoch nonzero, uac_mode a defined
 *                   interop_window_uac_mode_t, reserved0 zero, app_key_lookup non-NULL.
 * @param[out] out_graph Non-NULL caller slot. On Ok it receives the new handle (owned by the
 *                       caller until destroy). On every failure it is set to NULL once the slot
 *                       itself was valid, so no stale handle can survive.
 * @return Ok; InvalidArgument for a NULL slot, NULL config, or invalid config field; NoMemory when
 *         the single graph allocation fails (nothing is leaked).
 * @note The owner thread is the calling thread. Allocates exactly once; never retains config.
 *       Thread-safe for distinct arguments; the callbacks' contexts must stay valid until destroy.
 */
interop_window_graph_status_t interop_window_graph_create(const interop_window_graph_config_t *config,
                                                           interop_window_graph_t **out_graph);

/**
 * @brief Free a graph and set the caller's handle to NULL.
 *
 * Pending events, deferred records and quarantined (Retiring) entries are discarded without any
 * proof; the graph releases no heap run, mapping or capture, so the coordinator must have
 * handed the pairs from interop_window_graph_list_retiring() to the slot runtime first.
 * Destroying a graph is never evidence that host buffers are gone.
 *
 * @param[in,out] graph_slot Non-NULL caller slot. If *graph_slot is NULL the call is a successful no-op.
 *                           On Ok the graph is freed and *graph_slot is NULL; on any other status
 *                           the graph is not freed and *graph_slot is unchanged.
 * @return Ok; InvalidArgument for a NULL slot; WrongThread (graph not freed); Reentrant (graph not freed).
 * @note Owner thread only. Not thread-safe against concurrent use of the same graph.
 */
interop_window_graph_status_t interop_window_graph_destroy(interop_window_graph_t **graph_slot);

/**
 * @brief Classify one fact snapshot with the exact rules of the graph, without a graph.
 *
 * Applies the same facts validation as observe (every rule on interop_window_facts_t, including a
 * nonzero stamp and OwnerHidden requiring owner != 0) and then the classification steps 0 to 8 of the
 * file comment, so the observer can ask "would this owner window be exported?" before it offers a
 * window that depends on it. The function is pure: it needs no graph, no thread binding and no epoch,
 * allocates nothing, keeps no state, touches no counter and calls no callback. It does not check
 * freshness (no StaleFacts) and does not look at parents, capacity or deferral: Eligible says only
 * "the role rules accept this snapshot", not "the graph would admit it now".
 *
 * @param[in] facts Non-NULL borrowed snapshot; read during the call only, never retained.
 * @param[in] uac_mode An interop_window_uac_mode_t value, 0..2 (InteropWindowUacWindow,
 *                     InteropWindowUacAuto or InteropWindowUacNative); anything above 2 is InvalidArgument.
 * @param[out] verdict Non-NULL caller struct; written fully on Ok, unchanged on every other status.
 *                     kind is Eligible (role and parent_window valid, reason 0), NotExported (reason
 *                     valid, role and parent_window 0) or Suspended (everything else 0, because
 *                     OnInputDesktop is clear).
 * @return Ok; InvalidArgument for a NULL facts or verdict pointer or a uac_mode above 2; InvalidFacts
 *         when the facts violate the validation list. Non-Ok statuses leave verdict unchanged.
 * @note Thread-safe and re-entrant: usable from any thread, including from inside a graph callback,
 *       because it never reads or writes graph memory. No allocation.
 */
interop_window_graph_status_t interop_window_graph_classify(const interop_window_facts_t *facts,
                                                            uint32_t uac_mode,
                                                            interop_window_verdict_t *verdict);

/**
 * @brief Offer one fact snapshot: classify, then admit, update, defer or retire.
 *
 * Order of work (each stage stops the call at its first failure; nothing is mutated before the last
 * pre-check, so every failure below leaves the graph unchanged except where stated):
 *  1. Common rules. The caller's facts are copied into a stack-local (see Facts copy in the file
 *     comment) and the copy is validated (InvalidFacts). All later stages use only the copy.
 *  2. StaleFacts: the stamp is below the window's newest accepted stamp.
 *  3. Classification of the copy (rules of interop_window_graph_classify).
 *  4. Identity check, BEFORE the cycle check. A live entry L whose identity tuple (process_id,
 *     thread_id, process_start_time, class hash) differs from the copy is scheduled to retire with
 *     reason IncarnationChanged. A deferred record D for the window whose identity tuple differs
 *     from the copy is dropped (deferred_dropped) and the window is treated as having no deferred
 *     record: the key lookup runs afresh and a new deferral takes a new age position. Both changes are
 *     applied together with the rest of the plan.
 *  5. Cycle, capacity, generation, event-queue room for the whole plan (QueueFull), then the single
 *     lookup (LookupFailed, AppKeyInvalid).
 *
 * Outcomes on Ok (observation->disposition):
 *  - Suspended: OnInputDesktop was clear; no entry, deferred record or event changed.
 *  - Excluded: classification says not exported and no live entry existed; a deferred record for the
 *    window, if any, is dropped (deferred_dropped).
 *  - Retired: classification says not exported (reason = the classification reason) or a
 *    replacement could not be admitted; the live entry and its descendants moved to Retiring
 *    (descendants first, descending generation), one RetireIntent each (reason ParentRetired for
 *    descendants, never re-deferred except as the Reclassified bullet states); a deferred record for
 *    the window is dropped.
 *  - Unchanged / Updated: a live entry exists with the same identity tuple (process_id, thread_id,
 *    process_start_time, class hash), role and parent. Rect, derived state, dpi, title and
 *    min size are compared; no difference is Unchanged, otherwise an Update is queued or
 *    coalesced. Parent and role changes are never sent as Update. A change of the OwnerHidden flag
 *    that leaves role and parent unchanged is Unchanged.
 *  - Reclassified: role or parent_window changed (including because OwnerHidden changed), or ownership
 *    changed. The entry and its descendants retire (reason Reclassified for the entry, ParentRetired
 *    for descendants), then a new generation is announced reusing the stored application key (no new
 *    lookup). Cascade re-deferral: ONLY in this case, where the incarnation is the same, the
 *    descendants are still alive and still owned by that HWND; they are re-deferred in descendant
 *    order and re-announced after the new parent Announce as far as capacity allows; leftovers stay
 *    deferred and, being seen, survive a refresh commit. Every re-deferred record inherits the seen
 *    mark of the Active entry it replaces.
 *  - Admitted: no live entry existed (or the identity tuple differed, in which case the old entry
 *    retired first with retired_generation set and reason IncarnationChanged) and the window is
 *    eligible with no parent or an Active parent. app_key_lookup runs once; the Announce is
 *    queued; then deferred children of the new entry are flushed in age order, transitively,
 *    parent Announce strictly before child Announce, cut off by remaining capacity and
 *    generations (cut-off children stay deferred and are counted as at_capacity).
 *  - Deferred: eligible but the parent is absent, itself deferred, or Retiring. The key lookup runs
 *    at the first deferral and its result is stored (a stored record with a different identity tuple
 *    is replaced as described in stage 4); a full deferred set evicts the oldest
 *    deferral (deferred_dropped). Deferral does not consume exported capacity.
 *  Replacement rule: whenever an identity change or reclassification retires the live
 *  entry, the disposition describes the replacement (Admitted for an identity change,
 *  Reclassified for a role/parent change, Deferred if the new parent is not yet announced,
 *  Retired if the new facts classify as not exported), retired_generation is nonzero, and
 *  reason names the cause (IncarnationChanged, Reclassified or the classification reason).
 *  Replacement shortfall (forced retirement): if the retirement is required but the replacement
 *  cannot be admitted for a persistent reason (AtCapacity, GenerationExhausted, LookupFailed,
 *  AppKeyInvalid; and Cycle when the retirement is an identity change or the cascade cycle below),
 *  the retirement still happens, the call returns Ok with disposition Retired and reason
 *  IncarnationChanged or Reclassified, the matching counter increments, and the next offer
 *  retries admission. A transient shortage (QueueFull) changes nothing instead. This prevents a
 *  stale surface from staying live under the wrong identity or role.
 *
 * Cycle: owner_root == window, or a parent chain through deferred records that leads back to the
 * window, returns Cycle and drops that window's own deferred record (a documented mutation on a
 * non-Ok status); a live entry is left unchanged, with two exceptions that are forced retirements.
 * (a) Identity change: the identity check (stage 4) precedes the cycle check, so a live entry with a
 * different identity tuple retires with reason IncarnationChanged even when the replacement is
 * rejected with Cycle, and the call returns Ok (Retired). (b) Cascade cycle: if the new parent of a
 * reclassified window is in that window's own descendant (cascade) set, the replacement is treated as
 * Cycle: the cascade retires (the window with reason Reclassified, its descendants with
 * ParentRetired), nothing is deferred or re-deferred, and the call returns Ok (Retired).
 *
 * Seen marking: while a refresh is open, a plain observe marks the window seen only when it
 * returns Ok. refresh_offer is the call that marks every outcome seen; use it for refresh offers.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @param[in] facts Non-NULL borrowed snapshot. Copied into a stack-local at entry; only the copy is
 *                  validated, used and passed to app_key_lookup. The caller's struct is never
 *                  retained or modified.
 * @param[out] observation Non-NULL caller struct; written fully on Ok and unchanged otherwise.
 * @return Ok; the common-rule statuses; InvalidFacts; StaleFacts; Cycle; AtCapacity;
 *         GenerationExhausted; QueueFull; LookupFailed; AppKeyInvalid. Every non-Ok status leaves
 *         graph state and observation unchanged except counters and the Cycle exception. A shortfall
 *         status met while a retirement is forced is not returned (see Replacement shortfall).
 * @note Owner thread only; the only callback inside the call is app_key_lookup (Reentrant if it
 *       calls back). No heap allocation. stamp_high_water and tick_base advance only on Ok.
 */
interop_window_graph_status_t interop_window_graph_observe(interop_window_graph_t *graph,
                                                           uint64_t session_epoch,
                                                           const interop_window_facts_t *facts,
                                                           interop_window_observation_t *observation);

/**
 * @brief Apply a window-destroyed hint without trusting it blindly.
 *
 * Destroy rule. Let e64 be event_time_ms unwrapped to 64 bits against the graph's tick_base (Tick
 * clock in the file comment). The live entry L for window retires (reason Destroyed, descendants
 * ParentRetired, never re-deferred) iff hint_stamp > L.admit_stamp AND NOT (e64 < L.admit_captured64),
 * where admit_captured64 is the unwrapped captured_ms of the facts that admitted L and "<" is a plain
 * 64-bit comparison. A hint older than the facts that admitted L describes an earlier incarnation of a
 * recycled HWND and is ignored. Equal ticks are ambiguous and count as destroyed (conservative
 * churn, never a stuck stale surface). A deferred record D for the window is dropped iff
 * hint_stamp > D.last_stamp AND NOT (e64 < D.last_captured64), where last_captured64 is the unwrapped
 * captured_ms of the newest facts accepted for D. Other hints (hide, show, name, location) carry no
 * authority; the observer requeries and calls observe instead. Retiring entries are never touched.
 *
 * Deviation note (honest scope of the rule). The rule narrows, but does not literally satisfy, the
 * handoff wording that a hint about an earlier incarnation must never retire a newer one. It can
 * retire and then readmit a newer incarnation when the ticks are equal, or when the destroy fell
 * between the observer's tick sample and its native read of the facts that admitted L (the event time
 * is then not older than captured_ms although the facts already describe the new window). The cost
 * is churn bounded by one lost generation: the observer's next query of the window finds it alive
 * and admits it under a new generation, so a live window is never permanently lost. Acceptance of
 * this deviation and the native clock check are tracked in IMPL_DESC section 8.6.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @param[in] window Nonzero HWND.
 * @param[in] hint_stamp Nonzero observer stamp assigned when the hint (or query result) arrived.
 * @param[in] event_time_ms Raw 32-bit tick-domain time the OS generated the event, or the query tick
 *                          for a "window is gone" result; unwrapped by the graph (never compared as
 *                          32-bit). An Ok call advances tick_base to the unwrapped value.
 * @param[out] observation Non-NULL; written fully on Ok. disposition is Retired when an entry moved
 *                         to Retiring, otherwise Unchanged (deferred_dropped may be 1).
 * @return Ok; common-rule statuses; InvalidArgument for window 0, hint_stamp 0 or NULL output;
 *         QueueFull when the ring lacks room for the RetireIntent events (nothing changed).
 * @note Owner thread only; no callback; no allocation. stamp_high_water and tick_base advance on Ok.
 */
interop_window_graph_status_t interop_window_graph_note_destroyed(interop_window_graph_t *graph,
                                                                  uint64_t session_epoch,
                                                                  uint64_t window,
                                                                  uint64_t hint_stamp,
                                                                  uint32_t event_time_ms,
                                                                  interop_window_observation_t *observation);

/**
 * @brief End the quarantine of one retired generation after the coordinator's proof.
 *
 * Releases only the graph entry (its capacity slot). It does not free heap runs or unmap memory:
 * the coordinator did that before calling. A stale, duplicate or older completion can never
 * release a newer generation of the same HWND.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @param[in] window Nonzero HWND.
 * @param[in] generation Nonzero generation; must match a Retiring entry exactly.
 * @param[in] proof Non-NULL borrowed proof; both flags must be exactly 1.
 * @return Ok when the entry was released; InvalidArgument for zero window/generation or NULL proof;
 *         Stale when no entry has that exact (window, generation) (unknown, already completed, other
 *         generation); NotRetiring when that exact pair is Active; ProofIncomplete when a flag is
 *         not 1 (checked after Stale and NotRetiring; the entry stays quarantined). Non-Ok statuses
 *         change only counters.
 * @note Owner thread only; no callback; no allocation.
 */
interop_window_graph_status_t interop_window_graph_complete_retire(interop_window_graph_t *graph,
                                                                   uint64_t session_epoch,
                                                                   uint64_t window,
                                                                   uint64_t generation,
                                                                   const interop_window_retire_proof_t *proof);

/**
 * @brief Check that an action may be performed on (window, generation) right now.
 *
 * @param[in,out] graph Non-NULL owner-thread handle (only counters can change).
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @param[in] window Nonzero HWND.
 * @param[in] generation Nonzero generation.
 * @return Ok only for an Active entry with exactly that pair; Stale for Retiring, unknown or
 *         any other generation (retirement intent blocks new actions); InvalidArgument for zero
 *         window/generation; common-rule statuses.
 * @note Owner thread only; read-only apart from counters; no callback; no allocation.
 */
interop_window_graph_status_t interop_window_graph_check_action(interop_window_graph_t *graph,
                                                                uint64_t session_epoch,
                                                                uint64_t window,
                                                                uint64_t generation);

/**
 * @brief Retire every Active entry (children first) and clear all deferred records.
 *
 * Used after the observer lost hints (hint-table overflow). An open refresh is aborted. Retiring
 * entries already in quarantine are unchanged.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @param[out] retired_out Nullable. On Ok receives the number of entries moved to Retiring.
 * @return Ok; common-rule statuses; QueueFull when the ring cannot hold every RetireIntent (nothing changed).
 * @note Owner thread only; no callback; no allocation.
 */
interop_window_graph_status_t interop_window_graph_invalidate_all(interop_window_graph_t *graph,
                                                                  uint64_t session_epoch,
                                                                  uint32_t *retired_out);

/**
 * @brief Open a refresh: clears every seen mark and starts collecting seen marks.
 *
 * The caller runs one refresh every InteropWindowGraphRefreshIntervalMs. Evicted or capacity-
 * blocked windows are re-offered by the next refresh. Within an open refresh a window is "seen" when
 * refresh_offer was called for it (any outcome), refresh_keep was called for it, or a plain observe
 * returned Ok for it. refresh_unseen lists what is still unseen; refresh_commit retires it.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @return Ok; RefreshActive when one is already open (it continues, nothing changed); common-rule statuses.
 * @note Owner thread only; no callback; no allocation.
 */
interop_window_graph_status_t interop_window_graph_refresh_begin(interop_window_graph_t *graph,
                                                                 uint64_t session_epoch);

/**
 * @brief Offer one snapshot inside an open refresh: observe plus mark seen for every outcome.
 *
 * Behaves exactly like interop_window_graph_observe (including the facts copy, the identity check
 * before the cycle check, the forced-retire rule and the cascade rules) and additionally applies the
 * seen rule. Seen rule: an offered window is alive for the purposes of this refresh. After the common
 * guards (graph, thread, reentrancy, epoch) have passed, a NULL facts or observation pointer returns
 * InvalidArgument and marks nothing (the arguments are unusable); then, if no refresh is open,
 * NoRefresh is returned and nothing is marked; otherwise the window of the copied facts (when
 * nonzero) is marked seen for EVERY outcome, including every error status and QueueFull: the Active
 * entry and the deferred record that exist for it, and any entry or record the call creates for
 * it (a new generation is born seen, a record created by a deferral is seen). Marking happens
 * before the plan; a failed call changes nothing else. Because the mark survives a failure,
 * a window whose offer failed (StaleFacts, InvalidFacts, Cycle, AtCapacity, QueueFull, ...) is not
 * retired by the commit; it is simply not updated, and the next refresh offers it again. Deferred
 * records, entries promoted from them by a flush, flush leftovers cut off by capacity (they stay
 * deferred) and records re-deferred by a Reclassified cascade keep their seen mark, so they
 * survive the commit. Offer order is the caller's order and is deterministic; a parent offered
 * after its child makes the child defer and flush.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @param[in] facts Non-NULL borrowed snapshot; copied into a stack-local at entry (the lookup
 *                  callback receives the copy), never retained or modified.
 * @param[out] observation Non-NULL; written fully on Ok.
 * @return As observe, plus NoRefresh when no refresh is open (checked after the argument checks
 *         and before facts validation). The seen mark is written for every status after NoRefresh.
 * @note Owner thread only; same callback rule as observe; no heap allocation.
 */
interop_window_graph_status_t interop_window_graph_refresh_offer(interop_window_graph_t *graph,
                                                                 uint64_t session_epoch,
                                                                 const interop_window_facts_t *facts,
                                                                 interop_window_observation_t *observation);

/**
 * @brief Mark a window seen without offering facts, so the commit will not retire it.
 *
 * For a window whose current state could not be determined (a failed native query). Marks the Active
 * entry of window and the deferred record of window seen, whichever exist. Retiring entries carry no
 * seen mark and are not touched. Queues no event, changes no entry, record, stamp or tick.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @param[in] window Nonzero HWND.
 * @return Ok when an Active entry or a deferred record was marked; None when neither exists (nothing
 *         marked); NoRefresh when no refresh is open; InvalidArgument for window 0; common-rule statuses.
 * @note Owner thread only; no callback; no allocation. Only the seen mark and counters can change.
 */
interop_window_graph_status_t interop_window_graph_refresh_keep(interop_window_graph_t *graph,
                                                                uint64_t session_epoch,
                                                                uint64_t window);

/**
 * @brief List the Active windows that are still unseen in the open refresh.
 *
 * Lists only Active entries (never Retiring ones, never deferred records), in ascending generation
 * order, at most capacity of them. These are exactly the windows interop_window_graph_refresh_commit
 * would retire with reason RefreshUnseen if it ran now; the caller can requery each one and call
 * refresh_offer, refresh_keep or note_destroyed before committing. A value copy: the list is not
 * updated by later calls, and no seen mark changes.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @param[out] windows Caller array of capacity HWND values (zero-extended); may be NULL only if
 *                     capacity is 0. Up to capacity entries are written on Ok; the rest of the array
 *                     is untouched. InteropMaxWindows always suffices.
 * @param[in] capacity Elements available in windows.
 * @param[out] count Non-NULL; on Ok receives the total number of unseen Active windows even when it
 *                   exceeds capacity (truncation is then visible to the caller). Unchanged otherwise.
 * @return Ok; NoRefresh when no refresh is open; InvalidArgument (NULL count, NULL windows with
 *         capacity > 0); common-rule statuses. Non-Ok statuses leave windows and count unchanged.
 * @note Owner thread only; no callback; no allocation; no state change except counters.
 */
interop_window_graph_status_t interop_window_graph_refresh_unseen(interop_window_graph_t *graph,
                                                                  uint64_t session_epoch,
                                                                  uint64_t *windows,
                                                                  uint32_t capacity,
                                                                  uint32_t *count);

/**
 * @brief Close the refresh: retire every Active entry not seen, drop every unseen deferred record.
 *
 * "Not seen" means no refresh_offer, refresh_keep or Ok observe named the window since refresh_begin.
 * Retirement is children first, reason RefreshUnseen (ParentRetired for descendants, never
 * re-deferred). The caller must not commit a refresh in which any offer returned Suspended or in
 * which its enumeration was partial: absence from the enumeration is treated as destruction. A window
 * whose query failed is kept with refresh_keep and a refresh containing failed queries may commit;
 * refresh_unseen lets the caller requery the would-be victims first.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @param[out] retired_out Nullable. On Ok receives the number of entries moved to Retiring.
 * @return Ok (refresh closed); NoRefresh; QueueFull when the ring cannot hold every RetireIntent
 *         (nothing changed and the refresh stays open for a retry after draining); common-rule statuses.
 * @note Owner thread only; no callback; no allocation.
 */
interop_window_graph_status_t interop_window_graph_refresh_commit(interop_window_graph_t *graph,
                                                                  uint64_t session_epoch,
                                                                  uint32_t *retired_out);

/**
 * @brief Close the refresh without retiring anything; seen marks are discarded.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @return Ok; NoRefresh when none is open; common-rule statuses.
 * @note Owner thread only; no callback; no allocation.
 */
interop_window_graph_status_t interop_window_graph_refresh_abort(interop_window_graph_t *graph,
                                                                 uint64_t session_epoch);

/**
 * @brief Pop the oldest queued event into a caller struct.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @param[out] event Non-NULL; receives a value copy on Ok, unchanged otherwise.
 * @return Ok when an event was popped; None when the queue is empty; common-rule statuses.
 * @note Owner thread only; no callback; no allocation.
 */
interop_window_graph_status_t interop_window_graph_next_event(interop_window_graph_t *graph,
                                                              uint64_t session_epoch,
                                                              interop_window_event_t *event);

/**
 * @brief Deliver up to max_events queued events, oldest first, to a sink.
 *
 * An event is popped only after the sink returns 0. A nonzero return stops the drain, leaves that
 * event queued, and yields SinkFailed. Calling any graph function from the sink returns Reentrant.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[in] session_epoch Captured epoch; must equal the graph's.
 * @param[in] sink Non-NULL callback; the event pointer it receives is valid only during the call.
 * @param[in,out] sink_context Passed to the sink unchanged; may be NULL; owned by the caller.
 * @param[in] max_events At least 1 (0 is InvalidArgument).
 * @param[out] delivered Non-NULL; receives the number of events accepted and popped, on Ok, None
 *                       and SinkFailed alike (0 for None). Unchanged for other statuses.
 * @return Ok when at least one event was delivered and the sink never refused; None when the queue
 *         was empty; SinkFailed when the sink refused one (events accepted before it stay consumed);
 *         common-rule statuses.
 * @note Owner thread only; the sink runs on the owner thread. No allocation.
 */
interop_window_graph_status_t interop_window_graph_drain(interop_window_graph_t *graph,
                                                         uint64_t session_epoch,
                                                         interop_window_event_sink_t sink,
                                                         void *sink_context,
                                                         uint32_t max_events,
                                                         uint32_t *delivered);

/**
 * @brief Take the rate-limited capacity diagnostic.
 *
 * Returns Ok at most once per InteropWindowGraphDiagnosticIntervalMs of the injected clock
 * (the first take is immediate), and only when at least one of the four counters is nonzero since
 * the previous take. A clock that goes backwards counts as zero elapsed time. On Ok the counters
 * reset to zero. This call has no epoch parameter because it only reads and resets diagnostic
 * counters.
 *
 * @param[in,out] graph Non-NULL owner-thread handle.
 * @param[out] diagnostic Non-NULL; written fully on Ok, unchanged otherwise.
 * @return Ok; None when nothing is due or all counters are zero; common-rule statuses (no epoch check).
 * @note Owner thread only; calls the configured now_ms clock; no allocation.
 */
interop_window_graph_status_t interop_window_graph_take_capacity_diagnostic(
    interop_window_graph_t *graph, interop_window_capacity_diagnostic_t *diagnostic);

/**
 * @brief Copy the cumulative counters and gauges.
 *
 * @param[in] graph Non-NULL owner-thread handle (no epoch parameter; read-only).
 * @param[out] stats Non-NULL; written fully on Ok, unchanged otherwise.
 * @return Ok; InvalidArgument; WrongThread; Reentrant.
 * @note Owner thread only; no state or counter changes.
 */
interop_window_graph_status_t interop_window_graph_get_stats(const interop_window_graph_t *graph,
                                                             interop_window_graph_stats_t *stats);

/**
 * @brief Read the largest stamp accepted so far, for starting a replacement observer above it.
 *
 * @param[in] graph Non-NULL owner-thread handle.
 * @param[out] stamp Non-NULL; receives the value on Ok (0 before any accepted call), unchanged otherwise.
 * @return Ok; InvalidArgument; WrongThread; Reentrant.
 * @note Owner thread only; no state or counter changes.
 */
interop_window_graph_status_t interop_window_graph_stamp_high_water(const interop_window_graph_t *graph,
                                                                    uint64_t *stamp);

/**
 * @brief Look up the Active entry for a window.
 *
 * Retiring entries are not live and are not found.
 *
 * @param[in] graph Non-NULL owner-thread handle.
 * @param[in] window Nonzero HWND.
 * @param[out] live Non-NULL; written fully on Ok, unchanged otherwise.
 * @return Ok when an Active entry exists; None when it does not; InvalidArgument for window 0 or
 *         a NULL pointer; WrongThread; Reentrant.
 * @note Owner thread only; no state or counter changes.
 */
interop_window_graph_status_t interop_window_graph_find_live(const interop_window_graph_t *graph,
                                                             uint64_t window,
                                                             interop_window_live_t *live);

/**
 * @brief List the quarantined entries so the coordinator can hand them to the slot runtime.
 *
 * Entries are ordered by ascending generation. The list is a value copy.
 *
 * @param[in] graph Non-NULL owner-thread handle.
 * @param[out] entries Caller array of capacity elements; may be NULL only if capacity is 0. Up to
 *                     capacity entries are written on Ok.
 * @param[in] capacity Elements available in entries. InteropMaxWindows always suffices.
 * @param[out] count Non-NULL; on Ok receives the total number of Retiring entries even when it
 *                   exceeds capacity (truncation is then visible to the caller). Unchanged otherwise.
 * @return Ok; InvalidArgument (NULL count, NULL entries with capacity > 0); WrongThread; Reentrant.
 * @note Owner thread only; no state or counter changes.
 */
interop_window_graph_status_t interop_window_graph_list_retiring(const interop_window_graph_t *graph,
                                                                 interop_window_retiring_entry_t *entries,
                                                                 uint32_t capacity,
                                                                 uint32_t *count);

#endif
