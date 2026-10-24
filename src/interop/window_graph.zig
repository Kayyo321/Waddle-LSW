//! Session-wide guest window graph (TODO #10): pure value rules, graph state and the planning engine.
//!
//! Layer 1 is the stateless part: the C ABI type aliases, the PascalCase constants asserted
//! against `include/waddle/interop_window_graph.h`, the persistent record layout, facts
//! validation, the window classifier (header steps 0 to 8, also exported as
//! `classify_public`), identity comparison and the small arithmetic helpers (FNV-1a class
//! hash, 64-bit tick unwrapping, saturating counters). Nothing there allocates, keeps state,
//! calls the operating system or touches a thread. Layer 2 holds the graph state, the event
//! ring and its queries, and the planning engine: `observe`, `note_destroyed`, the retire
//! cascade and deferral. Every mutating call is plan, pre-check, at most one `app_key_lookup`,
//! then an infallible apply. The refresh engine is appended by the next stage.
//!
//! The pure functions are safe to call from any thread concurrently with other pure calls on
//! distinct or shared read-only arguments; every stateful function is owner-thread only. Test
//! blocks live below the `Test-only` delimiter line at the end of the production area, which is
//! the split point of the coverage tool.

const std_t = @import("std");

/// The committed public C header, imported verbatim. The engine uses these C
/// structs directly: there are no hand-written mirrors that could drift.
pub const c_t = @cImport({
    @cInclude("waddle/interop_window_graph.h");
});

/// One native window snapshot supplied by the observer (`interop_window_facts_t`).
pub const facts_t = c_t.interop_window_facts_t;
/// One queued event (`interop_window_event_t`); a value copy with no pointers.
pub const event_t = c_t.interop_window_event_t;
/// Result summary of observe, refresh_offer and note_destroyed.
pub const observation_t = c_t.interop_window_observation_t;
/// Description of the Active entry for one window.
pub const live_t = c_t.interop_window_live_t;
/// One quarantined entry as listed by list_retiring.
pub const retiring_entry_t = c_t.interop_window_retiring_entry_t;
/// Coordinator proof that ends a quarantine.
pub const proof_t = c_t.interop_window_retire_proof_t;
/// Cumulative counters and gauges of one graph.
pub const stats_t = c_t.interop_window_graph_stats_t;
/// Rate-limited capacity diagnostic.
pub const diagnostic_t = c_t.interop_window_capacity_diagnostic_t;
/// Graph creation parameters (callbacks, epoch, UAC mode).
pub const config_t = c_t.interop_window_graph_config_t;

/// Exported-entry bound (Active plus Retiring); comptime_int, equals `InteropMaxWindows`.
pub const MaxEntries = 64;
/// Deferred-record bound, separate from `MaxEntries`; comptime_int.
pub const MaxDeferred = 64;
/// Capacity of the bounded event ring in events; comptime_int.
pub const EventCapacity = 256;
/// Upper bound on events one successful call can queue (three per window); comptime_int.
pub const MaxCallEvents = 192;
/// Cadence in milliseconds at which the caller should run a full refresh; comptime_int.
pub const RefreshIntervalMs = 250;
/// Minimum milliseconds between two capacity diagnostics; comptime_int.
pub const DiagnosticIntervalMs = 60000;
/// Maximum class-name bytes in facts; comptime_int.
pub const MaxClassBytes = 256;
/// Maximum image-basename bytes in facts; comptime_int.
pub const MaxImageBytes = 260;
/// Maximum title bytes in facts and events; comptime_int.
pub const MaxTitleBytes = 1024;
/// Maximum application-key bytes; comptime_int.
pub const MaxAppKeyBytes = 200;
/// Minimum application-key bytes (`a-` or `p-` plus one character); comptime_int.
pub const MinAppKeyBytes = 3;
/// Minimum accepted facts dpi; comptime_int.
pub const MinDpi = 96;
/// Maximum accepted facts dpi; comptime_int.
pub const MaxDpi = 960;
/// Maximum accepted width, height, min_width and min_height; comptime_int.
pub const MaxExtent = 16384;

/// FNV-1a 64-bit offset basis used by `class_hash_of`.
pub const FnvOffsetBasis: u64 = 0xcbf29ce484222325;
/// FNV-1a 64-bit prime used by `class_hash_of`.
pub const FnvPrime: u64 = 0x100000001b3;

/// Fact flag: window is shown (visible and not DWM-cloaked).
pub const FactVisible: u32 = 1 << 0;
/// Fact flag: window accepts input; recorded only, classification ignores it.
pub const FactEnabled: u32 = 1 << 1;
/// Fact flag: window is minimized.
pub const FactIconic: u32 = 1 << 2;
/// Fact flag: window is maximized.
pub const FactZoomed: u32 = 1 << 3;
/// Fact flag: WS_EX_LAYERED is set.
pub const FactLayered: u32 = 1 << 4;
/// Fact flag: `alpha` holds a real constant alpha (requires `FactLayered`).
pub const FactLayeredAlphaKnown: u32 = 1 << 5;
/// Fact flag: window covers its whole monitor (adapter computed).
pub const FactFullscreen: u32 = 1 << 6;
/// Fact flag: window belongs to the agent's own process.
pub const FactAgentOwned: u32 = 1 << 7;
/// Fact flag: window is the agent's IME preedit window (requires `FactAgentOwned`).
pub const FactAgentPreedit: u32 = 1 << 8;
/// Fact flag: snapshot was taken while the Default desktop was the input desktop.
pub const FactOnInputDesktop: u32 = 1 << 9;
/// Fact flag: class name was cut to fit `MaxClassBytes`; it then matches no class rule.
pub const FactClassTruncated: u32 = 1 << 10;
/// Fact flag: process image unreadable; set if and only if `image_length` is 0.
pub const FactImageUnknown: u32 = 1 << 11;
/// Fact flag: observer-computed; `owner` is never exported or no longer exists, so
/// classification treats the window as not owned (requires `owner != 0`).
pub const FactOwnerHidden: u32 = 1 << 12;
/// Mask of every defined fact flag; any other bit is invalid facts.
pub const FactMask: u32 = 0x1FFF;

/// Change bit: x, y, width or height changed.
pub const ChangeRect: u32 = 1 << 0;
/// Change bit: derived state bits changed.
pub const ChangeState: u32 = 1 << 1;
/// Change bit: dpi changed.
pub const ChangeDpi: u32 = 1 << 2;
/// Change bit: title changed.
pub const ChangeTitle: u32 = 1 << 3;
/// Change bit: min_width or min_height changed.
pub const ChangeMinSize: u32 = 1 << 4;
/// Mask of all change bits; the changed_mask of every Announce.
pub const ChangeMask: u32 = 0x1F;

/// Event kind: a new generation was admitted.
pub const EventAnnounce: u32 = 1;
/// Event kind: rect/state/dpi/title/min-size of an announced generation changed.
pub const EventUpdate: u32 = 2;
/// Event kind: the generation entered quarantine.
pub const EventRetireIntent: u32 = 3;

/// UAC mode: export the consent dialog as role `RoleUacConsent`.
pub const UacWindow: u32 = 0;
/// UAC mode: same export rule as `UacWindow`.
pub const UacAuto: u32 = 1;
/// UAC mode: hide the consent dialog from export.
pub const UacNative: u32 = 2;

/// Disposition: nothing changed.
pub const DispositionUnchanged: u32 = 1;
/// Disposition: a new generation was admitted.
pub const DispositionAdmitted: u32 = 2;
/// Disposition: an Update was queued or coalesced.
pub const DispositionUpdated: u32 = 3;
/// Disposition: waiting for the parent; a deferred record was stored.
pub const DispositionDeferred: u32 = 4;
/// Disposition: at least one live entry moved to Retiring.
pub const DispositionRetired: u32 = 5;
/// Disposition: role or parent changed; old generation retired, new one announced.
pub const DispositionReclassified: u32 = 6;
/// Disposition: classified as not exported and nothing was live.
pub const DispositionExcluded: u32 = 7;
/// Disposition: the input desktop was not Default; the call had no effect.
pub const DispositionSuspended: u32 = 8;

/// Reason: no reason applies.
pub const ReasonNone: u32 = 0;
/// Reason: Visible flag clear.
pub const ReasonNotVisible: u32 = 1;
/// Reason: shell, tray, IME or other excluded class.
pub const ReasonExcludedClass: u32 = 2;
/// Reason: CoreWindow of a shell host image.
pub const ReasonExcludedCoreWindow: u32 = 3;
/// Reason: the agent's own window other than the preedit window.
pub const ReasonExcludedAgent: u32 = 4;
/// Reason: consent.exe interim dummy window.
pub const ReasonExcludedUacDummy: u32 = 5;
/// Reason: consent.exe credential dialog hidden in UAC mode Native.
pub const ReasonUacNativeHidden: u32 = 6;
/// Reason: root != window.
pub const ReasonChild: u32 = 7;
/// Reason: width or height is zero.
pub const ReasonZeroArea: u32 = 8;
/// Reason: layered with known constant alpha 0.
pub const ReasonTransparent: u32 = 9;
/// Reason: deferred because the parent is not an announced, exported generation.
pub const ReasonParentPending: u32 = 10;
/// Reason: a destroy hint applied or the window no longer exists.
pub const ReasonDestroyed: u32 = 11;
/// Reason: same HWND, different (process, thread, start time, class) identity.
pub const ReasonIncarnationChanged: u32 = 12;
/// Reason: role or parent changed, which needs a new host surface.
pub const ReasonReclassified: u32 = 13;
/// Reason: an ancestor was retired.
pub const ReasonParentRetired: u32 = 14;
/// Reason: not offered during a committed refresh.
pub const ReasonRefreshUnseen: u32 = 15;
/// Reason: `interop_window_graph_invalidate_all`.
pub const ReasonInvalidated: u32 = 16;

/// Role: ownerless or WS_EX_APPWINDOW window.
pub const RoleToplevel: u32 = 1;
/// Role: owned activatable window; parent is the owner root.
pub const RoleDialog: u32 = 2;
/// Role: menu or dropdown.
pub const RoleMenu: u32 = 3;
/// Role: tooltip or no-activate popup.
pub const RoleTooltip: u32 = 4;
/// Role: consent.exe credential dialog.
pub const RoleUacConsent: u32 = 5;
/// Role: agent-owned IME preedit window.
pub const RoleImePreedit: u32 = 6;

/// Derived state bit: minimized.
pub const StateMinimized: u32 = 1 << 0;
/// Derived state bit: maximized.
pub const StateMaximized: u32 = 1 << 1;
/// Derived state bit: fullscreen.
pub const StateFullscreen: u32 = 1 << 2;
/// Host-to-guest state bit; the graph never derives it.
pub const StateActivated: u32 = 1 << 3;
/// Derived state bit: WS_THICKFRAME.
pub const StateResizable: u32 = 1 << 4;
/// Derived state bit: WS_MAXIMIZEBOX.
pub const StateMaximizable: u32 = 1 << 5;
/// Derived state bit: WS_MINIMIZEBOX.
pub const StateMinimizable: u32 = 1 << 6;
/// Mask of every protocol state bit.
pub const StateMask: u32 = 0x7F;

/// Win32 style bit WS_POPUP.
pub const StylePopup: u32 = 0x80000000;
/// Win32 style mask WS_CAPTION (WS_BORDER and WS_DLGFRAME); a title bar needs both bits.
pub const StyleCaption: u32 = 0x00C00000;
/// Win32 style bit WS_THICKFRAME.
pub const StyleThickFrame: u32 = 0x00040000;
/// Win32 style bit WS_MAXIMIZEBOX.
pub const StyleMaximizeBox: u32 = 0x00010000;
/// Win32 style bit WS_MINIMIZEBOX.
pub const StyleMinimizeBox: u32 = 0x00020000;
/// Win32 extended style bit WS_EX_TOOLWINDOW.
pub const ExStyleToolWindow: u32 = 0x00000080;
/// Win32 extended style bit WS_EX_APPWINDOW.
pub const ExStyleAppWindow: u32 = 0x00040000;
/// Win32 extended style bit WS_EX_LAYERED.
pub const ExStyleLayered: u32 = 0x00080000;
/// Win32 extended style bit WS_EX_NOACTIVATE.
pub const ExStyleNoActivate: u32 = 0x08000000;

/// Integer role code (`RoleToplevel` and siblings).
pub const role_t = u32;
/// Integer reason code (`ReasonNone` and siblings).
pub const reason_t = u32;
/// Integer disposition code (`DispositionUnchanged` and siblings).
pub const disposition_t = u32;
/// Integer event kind code (`EventAnnounce` and siblings).
pub const event_kind_t = u32;
/// Integer UAC mode code (`UacWindow` and siblings).
pub const uac_mode_t = u32;

/// Result codes of every graph call. Numeric values equal
/// `interop_window_graph_status_t`; only `Ok` and `None` are non-error results.
/// The ABI layer converts with `@intFromEnum`.
pub const status_t = enum(c_int) {
    /// The call completed and its outputs are valid.
    Ok = 0,
    /// Benign: nothing to return. Outputs unchanged.
    None = 1,
    /// NULL pointer, zero window/generation/budget or bad config; nothing changed.
    InvalidArgument = 2,
    /// Facts violate the validation list; nothing changed.
    InvalidFacts = 3,
    /// Not the creating thread; nothing touched.
    WrongThread = 4,
    /// Called from inside a graph callback; nothing touched.
    Reentrant = 5,
    /// create could not allocate; no graph exists.
    NoMemory = 6,
    /// session_epoch is not this graph's epoch; only a counter changed.
    StaleEpoch = 7,
    /// No entry has that exact (window, generation); nothing changed.
    Stale = 8,
    /// Facts are older than the newest accepted for that window.
    StaleFacts = 9,
    /// The event ring lacks room for the whole plan; nothing changed.
    QueueFull = 10,
    /// 64 exported entries and no eviction is allowed; nothing changed.
    AtCapacity = 11,
    /// Self-ownership or an owner cycle through deferred records.
    Cycle = 12,
    /// app_key_lookup returned nonzero; nothing published.
    LookupFailed = 13,
    /// The looked-up key is not a valid protocol-v2 application key.
    AppKeyInvalid = 14,
    /// No generation is left; existing entries keep working.
    GenerationExhausted = 15,
    /// refresh_begin while a refresh is already open.
    RefreshActive = 16,
    /// refresh_offer/commit/abort with no open refresh.
    NoRefresh = 17,
    /// complete_retire proof flags are not both exactly 1.
    ProofIncomplete = 18,
    /// complete_retire named an Active entry; nothing changed.
    NotRetiring = 19,
    /// The drain sink refused an event; that event stays queued.
    SinkFailed = 20,
};

comptime {
    // Hard guards: the Zig constants and the committed C header must never drift.
    const pairs = .{
        .{ MaxEntries, c_t.InteropMaxWindows },
        .{ MaxDeferred, c_t.InteropWindowGraphMaxDeferred },
        .{ EventCapacity, c_t.InteropWindowGraphEventCapacity },
        .{ MaxCallEvents, c_t.InteropWindowGraphMaxCallEvents },
        .{ RefreshIntervalMs, c_t.InteropWindowGraphRefreshIntervalMs },
        .{ DiagnosticIntervalMs, c_t.InteropWindowGraphDiagnosticIntervalMs },
        .{ MaxClassBytes, c_t.InteropWindowGraphMaxClassBytes },
        .{ MaxImageBytes, c_t.InteropWindowGraphMaxImageBytes },
        .{ MaxTitleBytes, c_t.InteropWindowGraphMaxTitleBytes },
        .{ MaxTitleBytes, c_t.InteropMaxTitleBytes },
        .{ MaxAppKeyBytes, c_t.InteropWindowGraphMaxAppKeyBytes },
        .{ MaxAppKeyBytes, c_t.InteropMaxAppKeyBytes },
        .{ MinDpi, c_t.InteropWindowGraphMinDpi },
        .{ MaxDpi, c_t.InteropWindowGraphMaxDpi },
        .{ MaxExtent, c_t.InteropWindowGraphMaxExtent },
        .{ FactVisible, c_t.InteropWindowFactVisible },
        .{ FactEnabled, c_t.InteropWindowFactEnabled },
        .{ FactIconic, c_t.InteropWindowFactIconic },
        .{ FactZoomed, c_t.InteropWindowFactZoomed },
        .{ FactLayered, c_t.InteropWindowFactLayered },
        .{ FactLayeredAlphaKnown, c_t.InteropWindowFactLayeredAlphaKnown },
        .{ FactFullscreen, c_t.InteropWindowFactFullscreen },
        .{ FactAgentOwned, c_t.InteropWindowFactAgentOwned },
        .{ FactAgentPreedit, c_t.InteropWindowFactAgentPreedit },
        .{ FactOnInputDesktop, c_t.InteropWindowFactOnInputDesktop },
        .{ FactClassTruncated, c_t.InteropWindowFactClassTruncated },
        .{ FactImageUnknown, c_t.InteropWindowFactImageUnknown },
        .{ FactOwnerHidden, c_t.InteropWindowFactOwnerHidden },
        .{ FactMask, c_t.InteropWindowFactMask },
        .{ ChangeRect, c_t.InteropWindowChangeRect },
        .{ ChangeState, c_t.InteropWindowChangeState },
        .{ ChangeDpi, c_t.InteropWindowChangeDpi },
        .{ ChangeTitle, c_t.InteropWindowChangeTitle },
        .{ ChangeMinSize, c_t.InteropWindowChangeMinSize },
        .{ ChangeMask, c_t.InteropWindowChangeMask },
        .{ EventAnnounce, c_t.InteropWindowEventAnnounce },
        .{ EventUpdate, c_t.InteropWindowEventUpdate },
        .{ EventRetireIntent, c_t.InteropWindowEventRetireIntent },
        .{ @intFromEnum(verdict_kind_t.Eligible), c_t.InteropWindowVerdictEligible },
        .{ @intFromEnum(verdict_kind_t.NotExported), c_t.InteropWindowVerdictNotExported },
        .{ @intFromEnum(verdict_kind_t.Suspended), c_t.InteropWindowVerdictSuspended },
        .{ UacWindow, c_t.InteropWindowUacWindow },
        .{ UacAuto, c_t.InteropWindowUacAuto },
        .{ UacNative, c_t.InteropWindowUacNative },
        .{ DispositionUnchanged, c_t.InteropWindowDispositionUnchanged },
        .{ DispositionAdmitted, c_t.InteropWindowDispositionAdmitted },
        .{ DispositionUpdated, c_t.InteropWindowDispositionUpdated },
        .{ DispositionDeferred, c_t.InteropWindowDispositionDeferred },
        .{ DispositionRetired, c_t.InteropWindowDispositionRetired },
        .{ DispositionReclassified, c_t.InteropWindowDispositionReclassified },
        .{ DispositionExcluded, c_t.InteropWindowDispositionExcluded },
        .{ DispositionSuspended, c_t.InteropWindowDispositionSuspended },
        .{ ReasonNone, c_t.InteropWindowReasonNone },
        .{ ReasonNotVisible, c_t.InteropWindowReasonNotVisible },
        .{ ReasonExcludedClass, c_t.InteropWindowReasonExcludedClass },
        .{ ReasonExcludedCoreWindow, c_t.InteropWindowReasonExcludedCoreWindow },
        .{ ReasonExcludedAgent, c_t.InteropWindowReasonExcludedAgent },
        .{ ReasonExcludedUacDummy, c_t.InteropWindowReasonExcludedUacDummy },
        .{ ReasonUacNativeHidden, c_t.InteropWindowReasonUacNativeHidden },
        .{ ReasonChild, c_t.InteropWindowReasonChild },
        .{ ReasonZeroArea, c_t.InteropWindowReasonZeroArea },
        .{ ReasonTransparent, c_t.InteropWindowReasonTransparent },
        .{ ReasonParentPending, c_t.InteropWindowReasonParentPending },
        .{ ReasonDestroyed, c_t.InteropWindowReasonDestroyed },
        .{ ReasonIncarnationChanged, c_t.InteropWindowReasonIncarnationChanged },
        .{ ReasonReclassified, c_t.InteropWindowReasonReclassified },
        .{ ReasonParentRetired, c_t.InteropWindowReasonParentRetired },
        .{ ReasonRefreshUnseen, c_t.InteropWindowReasonRefreshUnseen },
        .{ ReasonInvalidated, c_t.InteropWindowReasonInvalidated },
        .{ RoleToplevel, c_t.InteropRoleToplevel },
        .{ RoleDialog, c_t.InteropRoleDialog },
        .{ RoleMenu, c_t.InteropRoleMenu },
        .{ RoleTooltip, c_t.InteropRoleTooltip },
        .{ RoleUacConsent, c_t.InteropRoleUacConsent },
        .{ RoleImePreedit, c_t.InteropRoleImePreedit },
        .{ StateMinimized, c_t.InteropStateMinimized },
        .{ StateMaximized, c_t.InteropStateMaximized },
        .{ StateFullscreen, c_t.InteropStateFullscreen },
        .{ StateActivated, c_t.InteropStateActivated },
        .{ StateResizable, c_t.InteropStateResizable },
        .{ StateMaximizable, c_t.InteropStateMaximizable },
        .{ StateMinimizable, c_t.InteropStateMinimizable },
        .{ StateMask, c_t.InteropStateMask },
        .{ StylePopup, c_t.InteropWindowStylePopup },
        .{ StyleCaption, c_t.InteropWindowStyleCaption },
        .{ StyleThickFrame, c_t.InteropWindowStyleThickFrame },
        .{ StyleMaximizeBox, c_t.InteropWindowStyleMaximizeBox },
        .{ StyleMinimizeBox, c_t.InteropWindowStyleMinimizeBox },
        .{ ExStyleToolWindow, c_t.InteropWindowExStyleToolWindow },
        .{ ExStyleAppWindow, c_t.InteropWindowExStyleAppWindow },
        .{ ExStyleLayered, c_t.InteropWindowExStyleLayered },
        .{ ExStyleNoActivate, c_t.InteropWindowExStyleNoActivate },
        .{ @intFromEnum(status_t.Ok), c_t.InteropWindowGraphStatusOk },
        .{ @intFromEnum(status_t.None), c_t.InteropWindowGraphStatusNone },
        .{ @intFromEnum(status_t.InvalidArgument), c_t.InteropWindowGraphStatusInvalidArgument },
        .{ @intFromEnum(status_t.InvalidFacts), c_t.InteropWindowGraphStatusInvalidFacts },
        .{ @intFromEnum(status_t.WrongThread), c_t.InteropWindowGraphStatusWrongThread },
        .{ @intFromEnum(status_t.Reentrant), c_t.InteropWindowGraphStatusReentrant },
        .{ @intFromEnum(status_t.NoMemory), c_t.InteropWindowGraphStatusNoMemory },
        .{ @intFromEnum(status_t.StaleEpoch), c_t.InteropWindowGraphStatusStaleEpoch },
        .{ @intFromEnum(status_t.Stale), c_t.InteropWindowGraphStatusStale },
        .{ @intFromEnum(status_t.StaleFacts), c_t.InteropWindowGraphStatusStaleFacts },
        .{ @intFromEnum(status_t.QueueFull), c_t.InteropWindowGraphStatusQueueFull },
        .{ @intFromEnum(status_t.AtCapacity), c_t.InteropWindowGraphStatusAtCapacity },
        .{ @intFromEnum(status_t.Cycle), c_t.InteropWindowGraphStatusCycle },
        .{ @intFromEnum(status_t.LookupFailed), c_t.InteropWindowGraphStatusLookupFailed },
        .{ @intFromEnum(status_t.AppKeyInvalid), c_t.InteropWindowGraphStatusAppKeyInvalid },
        .{ @intFromEnum(status_t.GenerationExhausted), c_t.InteropWindowGraphStatusGenerationExhausted },
        .{ @intFromEnum(status_t.RefreshActive), c_t.InteropWindowGraphStatusRefreshActive },
        .{ @intFromEnum(status_t.NoRefresh), c_t.InteropWindowGraphStatusNoRefresh },
        .{ @intFromEnum(status_t.ProofIncomplete), c_t.InteropWindowGraphStatusProofIncomplete },
        .{ @intFromEnum(status_t.NotRetiring), c_t.InteropWindowGraphStatusNotRetiring },
        .{ @intFromEnum(status_t.SinkFailed), c_t.InteropWindowGraphStatusSinkFailed },
    };
    @setEvalBranchQuota(100_000);
    for (pairs) |pair| {
        if (pair[0] != pair[1]) @compileError("window graph constant drifted from interop_window_graph.h");
    }
    if (@typeInfo(status_t).Enum.fields.len != 21) @compileError("status_t must list exactly the 21 header statuses");
    if (@sizeOf(facts_t) != 1656 or @sizeOf(event_t) != 1328) @compileError("window graph C struct layout drifted");
    if (MaxCallEvents != 3 * MaxEntries or MaxCallEvents > EventCapacity) @compileError("per-call event bound invariant");
    if (MaxDeferred != MaxEntries) @compileError("deferred bound matches the entry bound");
}

/// One persistent window record: an entry body or a deferred record body.
///
/// Plain Zig struct (not an ABI type); the stateful engine embeds it by value in
/// its fixed arrays. All counters are zero after `record_init`. A deferred record
/// keeps `generation == 0` and `seq` set to its age; an entry keeps `seq == 0`.
pub const record_t = struct {
    /// HWND zero-extended; nonzero for a live record.
    window: u64,
    /// Assigned generation; 0 for a deferred record.
    generation: u64,
    /// Parent HWND, 0 = none.
    parent_window: u64,
    /// Generation of the Active parent at admission, 0 when `parent_window` is 0.
    parent_generation: u64,
    /// Process creation time in 100 ns units; 0 = unknown.
    process_start_time: u64,
    /// FNV-1a 64 of the class bytes (see `class_hash_of`).
    class_hash: u64,
    /// Observer stamp of the facts that admitted the record.
    admit_stamp: u64,
    /// Newest observer stamp accepted for the window.
    last_stamp: u64,
    /// Deferral age counter; 0 for entries.
    seq: u64,
    /// Left edge in guest desktop pixels.
    x: i32,
    /// Top edge in guest desktop pixels.
    y: i32,
    /// Physical pixel width.
    width: u32,
    /// Physical pixel height.
    height: u32,
    /// Role code (`RoleToplevel` and siblings).
    role: u32,
    /// Derived protocol state bits (`derive_state`).
    state: u32,
    /// Effective dpi, 96..960.
    dpi: u32,
    /// Minimum tracking width, 0 = none.
    min_width: u32,
    /// Minimum tracking height, 0 = none.
    min_height: u32,
    /// Owning process id.
    process_id: u32,
    /// Creating thread id.
    thread_id: u32,
    /// Unwrapped 64-bit tick (see `unwrap_tick`) of the facts that admitted the record.
    admit_captured_ms: u64,
    /// Unwrapped 64-bit tick of the newest accepted facts.
    last_captured_ms: u64,
    /// Reason code recorded when the record retired.
    retire_reason: u32,
    /// Valid bytes in `title`, 0..1024.
    title_length: u32,
    /// Valid bytes in `app_key`, 3..200 for an announced record.
    app_key_length: u32,
    /// Title copy, UTF-8, not NUL-terminated.
    title: [MaxTitleBytes]u8,
    /// Application key copy, ASCII, not NUL-terminated.
    app_key: [MaxAppKeyBytes]u8,
};

/// Build an all-zero record.
///
/// @return A record whose every field, including both byte arrays, is zero.
/// @note Pure; thread-safe. The record is a value owned by the caller.
pub fn record_init() record_t {
    return std_t.mem.zeroes(record_t);
}

/// Borrow the stated class-name bytes of a facts snapshot.
///
/// @param[in] facts Non-null borrowed snapshot; need not be validated.
/// @return A slice of `class_name` limited to `class_length`, clamped to `MaxClassBytes`
///         so an unvalidated length can never run past the array. Valid while `facts` lives.
/// @note Pure; thread-safe. The slice is not copied and not owned.
pub fn class_bytes(facts: *const facts_t) []const u8 {
    return facts.class_name[0..@min(facts.class_length, MaxClassBytes)];
}

/// Borrow the stated image-basename bytes of a facts snapshot.
///
/// @param[in] facts Non-null borrowed snapshot; need not be validated.
/// @return A slice of `image_name` limited to `image_length`, clamped to `MaxImageBytes`.
///         Valid while `facts` lives; empty when the image is unknown.
/// @note Pure; thread-safe. The slice is not copied and not owned.
pub fn image_bytes(facts: *const facts_t) []const u8 {
    return facts.image_name[0..@min(facts.image_length, MaxImageBytes)];
}

/// Borrow the stated title bytes of a facts snapshot.
///
/// @param[in] facts Non-null borrowed snapshot; need not be validated.
/// @return A slice of `title` limited to `title_length`, clamped to `MaxTitleBytes`.
///         Valid while `facts` lives; empty for an empty title.
/// @note Pure; thread-safe. The slice is not copied and not owned.
pub fn title_bytes(facts: *const facts_t) []const u8 {
    return facts.title[0..@min(facts.title_length, MaxTitleBytes)];
}

/// Check that bytes are well-formed UTF-8 and contain no NUL.
///
/// Rejects overlong forms, surrogates (U+D800..U+DFFF), code points above U+10FFFF,
/// truncated sequences, stray continuation bytes and every 0x00 byte. The empty
/// slice is valid.
///
/// @param[in] bytes Borrowed bytes; may be empty. Not retained.
/// @return true when the whole slice is valid UTF-8 without NUL.
/// @note Pure; thread-safe. `std.unicode` alone accepts NUL, so it is checked here.
pub fn utf8_valid_no_nul(bytes: []const u8) bool {
    if (std_t.mem.indexOfScalar(u8, bytes, 0) != null) return false;
    return std_t.unicode.utf8ValidateSlice(bytes);
}

/// Check a protocol-v2 application key.
///
/// The accepted set is exactly the frozen wire rule of `app_key_valid` in
/// `interop_codec.zig` (first byte `a` or `p`, second byte `-`, every byte in
/// `[A-Za-z0-9_-]`) combined with the length rule the codec enforces on the
/// application-key tag (3..200 bytes, `MinAppKeyBytes`..`MaxAppKeyBytes`).
///
/// @param[in] bytes Borrowed key bytes; not retained, not NUL-terminated.
/// @return true when `bytes` is a valid application key.
/// @note Pure; thread-safe.
pub fn app_key_valid(bytes: []const u8) bool {
    if (bytes.len < MinAppKeyBytes or bytes.len > MaxAppKeyBytes) return false;
    if (bytes[1] != '-' or (bytes[0] != 'a' and bytes[0] != 'p')) return false;
    for (bytes) |byte| {
        const ok = (byte >= 'A' and byte <= 'Z') or (byte >= 'a' and byte <= 'z') or
            (byte >= '0' and byte <= '9') or byte == '_' or byte == '-';
        if (!ok) return false;
    }
    return true;
}

/// Hash class-name bytes with FNV-1a 64 (exact bytes, case-sensitive).
///
/// @param[in] bytes Borrowed class bytes; may be empty. Not retained.
/// @return The FNV-1a 64-bit hash; `FnvOffsetBasis` for the empty slice.
/// @note Pure; thread-safe. Used only for the incarnation identity tuple, never as a key.
pub fn class_hash_of(bytes: []const u8) u64 {
    var hash: u64 = FnvOffsetBasis;
    for (bytes) |byte| {
        hash ^= byte;
        hash *%= FnvPrime;
    }
    return hash;
}

/// Compare two byte strings for equality ignoring ASCII case.
///
/// Only `A`..`Z` fold to `a`..`z`; every other byte (including non-ASCII) must match exactly.
///
/// @param[in] a Borrowed bytes; not retained.
/// @param[in] b Borrowed bytes; not retained.
/// @return true when both lengths are equal and all bytes match after ASCII folding.
/// @note Pure; thread-safe.
pub fn ascii_eql_ignore_case(a: []const u8, b: []const u8) bool {
    if (a.len != b.len) return false;
    for (a, b) |left, right| {
        if (std_t.ascii.toLower(left) != std_t.ascii.toLower(right)) return false;
    }
    return true;
}

/// Initial value of `graph_t.tick_base`: 2^32, so no unwrapped tick is ever negative.
pub const TickBaseStart: u64 = 4294967296;

/// Unwrap a raw 32-bit tick against a 64-bit base.
///
/// Computes `base + (i64)(i32)(t32 -% (u32)base)` with the subtraction wrapping modulo 2^32, so the
/// result is the 64-bit value congruent to `t32` that lies within +-2^31 of `base`. The graph passes
/// the clock-advanced estimate `tick_estimate`, not the raw `graph_t.tick_base`, so the result is
/// exact while the graph clock does not trail the tick domain by 2^31 ms between two Ok calls that
/// carry a tick. The caller (never this function) advances `graph_t.tick_base` (and
/// `tick_clock_at_base`, see `commit_ok`) to the larger of the old value and the result, and only
/// when the call returns Ok.
///
/// @param[in] base 64-bit base or estimate, at least `TickBaseStart` so the result cannot underflow.
/// @param[in] t32 Raw 32-bit tick-domain value from the wire.
/// @return The unwrapped 64-bit tick; below `base` for a delayed older tick.
/// @note Pure; thread-safe. Wrapping arithmetic only, no overflow trap.
pub fn unwrap_tick(base: u64, t32: u32) u64 {
    const delta: i32 = @bitCast(t32 -% @as(u32, @truncate(base)));
    return base +% @as(u64, @bitCast(@as(i64, delta)));
}

/// Derive protocol state bits from facts. Never sets `StateActivated`.
///
/// Iconic maps to `StateMinimized`, Zoomed to `StateMaximized`, Fullscreen to
/// `StateFullscreen`, WS_THICKFRAME to `StateResizable`, WS_MAXIMIZEBOX to
/// `StateMaximizable` and WS_MINIMIZEBOX to `StateMinimizable`.
///
/// @param[in] facts Non-null borrowed snapshot; need not be validated.
/// @return The union of state bits, a subset of `StateMask`.
/// @note Pure; thread-safe.
pub fn derive_state(facts: *const facts_t) u32 {
    var state: u32 = 0;
    if (facts.flags & FactIconic != 0) state |= StateMinimized;
    if (facts.flags & FactZoomed != 0) state |= StateMaximized;
    if (facts.flags & FactFullscreen != 0) state |= StateFullscreen;
    if (facts.style & StyleThickFrame != 0) state |= StateResizable;
    if (facts.style & StyleMaximizeBox != 0) state |= StateMaximizable;
    if (facts.style & StyleMinimizeBox != 0) state |= StateMinimizable;
    return state;
}

/// Validate a facts snapshot against every rule of `interop_window_facts_t`.
///
/// Rules: window, root and stamp nonzero; owner and owner_root both zero or both
/// nonzero; flags within `FactMask`; AgentPreedit requires AgentOwned;
/// OwnerHidden requires `owner != 0`; LayeredAlphaKnown requires Layered; ImageUnknown set if and only if
/// `image_length` is 0; dpi in 96..960; width, height, min_width and min_height at
/// most 16384; class_length 1..256, image_length 0..260, title_length 0..1024;
/// class, image and title valid UTF-8 without NUL over their stated lengths only
/// (bytes beyond a stated length are ignored); `reserved0` all zero.
///
/// @param[in] facts Non-null borrowed snapshot; read only, not retained.
/// @return true when every rule holds.
/// @note Pure; thread-safe. Lengths larger than the arrays are rejected before any byte is read.
pub fn validate_facts(facts: *const facts_t) bool {
    if (facts.window == 0 or facts.root == 0 or facts.stamp == 0) return false;
    if ((facts.owner != 0) != (facts.owner_root != 0)) return false;
    if (facts.flags & ~FactMask != 0) return false;
    if (facts.flags & FactAgentPreedit != 0 and facts.flags & FactAgentOwned == 0) return false;
    if (facts.flags & FactOwnerHidden != 0 and facts.owner == 0) return false;
    if (facts.flags & FactLayeredAlphaKnown != 0 and facts.flags & FactLayered == 0) return false;
    if ((facts.flags & FactImageUnknown != 0) != (facts.image_length == 0)) return false;
    if (facts.dpi < MinDpi or facts.dpi > MaxDpi) return false;
    if (facts.width > MaxExtent or facts.height > MaxExtent) return false;
    if (facts.min_width > MaxExtent or facts.min_height > MaxExtent) return false;
    if (facts.class_length < 1 or facts.class_length > MaxClassBytes) return false;
    if (facts.image_length > MaxImageBytes or facts.title_length > MaxTitleBytes) return false;
    if (!utf8_valid_no_nul(class_bytes(facts))) return false;
    if (!utf8_valid_no_nul(image_bytes(facts))) return false;
    if (!utf8_valid_no_nul(title_bytes(facts))) return false;
    for (facts.reserved0) |byte| {
        if (byte != 0) return false;
    }
    return true;
}

/// Role and parent of an exportable window.
pub const eligible_t = struct {
    /// Role code (`RoleToplevel` and siblings).
    role: u32,
    /// Parent HWND, 0 = none. `owner_root` for Dialog, Menu and Tooltip when owned.
    parent_window: u64,
};

/// Kind tag of a classification outcome; numeric values equal `interop_window_verdict_kind_t`.
pub const verdict_kind_t = enum(u32) {
    /// The window is exportable with a role and a parent.
    Eligible = 1,
    /// The window is not exported; the payload is a classification `Reason*` code (1..9).
    NotExported = 2,
    /// The input desktop is not Default: nothing may be admitted, retired or updated.
    Suspended = 3,
};

/// Outcome of classification.
pub const verdict_t = union(verdict_kind_t) {
    /// The window is exportable with this role and parent.
    Eligible: eligible_t,
    /// The window is not exported; the payload is a classification `Reason*` code (1..9).
    NotExported: u32,
    /// The input desktop is not Default: nothing may be admitted, retired or updated.
    Suspended: void,
};

const ExcludedClasses = [_][]const u8{
    "Shell_TrayWnd",
    "Shell_SecondaryTrayWnd",
    "Progman",
    "WorkerW",
    "NotifyIconOverflowWindow",
    "TaskListThumbnailWnd",
    "MSCTFIME UI",
    "IME",
};
const ShellHostImages = [_][]const u8{
    "ShellExperienceHost.exe",
    "StartMenuExperienceHost.exe",
    "SearchHost.exe",
};
const CoreWindowClass = "Windows.UI.Core.CoreWindow";
const ConsentImage = "consent.exe";
const UacDummyClass = "$$$Secure UAP Dummy Window Class For Interim Dialog";
const UacCredentialClass = "Credential Dialog Xaml Host";
const MenuClassPopup = "#32768";
const MenuClassCombo = "ComboLBox";
const TooltipClass = "tooltips_class32";

fn flag_set(facts: *const facts_t, flag: u32) bool {
    return facts.flags & flag != 0;
}

/// A class rule never matches a truncated class name.
fn class_is(facts: *const facts_t, name: []const u8) bool {
    return !flag_set(facts, FactClassTruncated) and ascii_eql_ignore_case(class_bytes(facts), name);
}

/// An unknown image (length 0) never equals a non-empty image name.
fn image_is(facts: *const facts_t, name: []const u8) bool {
    return ascii_eql_ignore_case(image_bytes(facts), name);
}

fn image_is_shell_host(facts: *const facts_t) bool {
    for (ShellHostImages) |name| {
        if (image_is(facts, name)) return true;
    }
    return false;
}

fn class_is_excluded(facts: *const facts_t) bool {
    for (ExcludedClasses) |name| {
        if (class_is(facts, name)) return true;
    }
    return false;
}

/// A window is owned iff `owner != 0` and OwnerHidden is clear.
fn is_owned(facts: *const facts_t) bool {
    return facts.owner != 0 and !flag_set(facts, FactOwnerHidden);
}

/// Step 8 gate shared by every role.
fn gate(facts: *const facts_t, role: u32, parent_window: u64) verdict_t {
    if (facts.width == 0 or facts.height == 0) return .{ .NotExported = ReasonZeroArea };
    if (flag_set(facts, FactLayered) and flag_set(facts, FactLayeredAlphaKnown) and facts.alpha == 0) {
        return .{ .NotExported = ReasonTransparent };
    }
    return .{ .Eligible = .{ .role = role, .parent_window = parent_window } };
}

/// Classify a facts snapshot (header steps 0 to 8; the first matching step wins).
///
/// "Owned" means `owner != 0` and OwnerHidden clear; an owner-hidden window is judged as not owned
/// in steps 4 to 7 and for the preedit parent.
/// Step 0: OnInputDesktop clear is `Suspended`; Visible clear is NotVisible.
/// Step 1: excluded classes, CoreWindow of the three shell-host images, then the
/// agent's own windows (preedit selects ImePreedit, parent `owner_root` when owned else 0, and
/// skips to step 8).
/// Step 2: consent.exe dummy class and credential dialog per `uac_mode`.
/// Step 3: root != window is Child. Step 4: Menu. Step 5: Tooltip. Step 6: owned
/// and not APPWINDOW is Dialog. Step 7: Toplevel. Step 8: ZeroArea and Transparent
/// gate every role. Class and image names compare ASCII case-insensitively over
/// the full length; a class with ClassTruncated set matches no class rule.
///
/// @param[in] facts Non-null borrowed snapshot; should already satisfy `validate_facts`
///                  (the function itself never reads out of bounds regardless).
/// @param[in] uac_mode `UacWindow` or `UacAuto` exports the credential dialog as UacConsent;
///                     any other value, including `UacNative`, hides it (fail closed).
/// @return A `verdict_t` value; nothing in it borrows from `facts`.
/// @note Pure; thread-safe. Self-ownership and cycles are the graph's concern, not this function's.
pub fn classify(facts: *const facts_t, uac_mode: u32) verdict_t {
    if (!flag_set(facts, FactOnInputDesktop)) return .Suspended;
    if (!flag_set(facts, FactVisible)) return .{ .NotExported = ReasonNotVisible };
    if (class_is_excluded(facts)) return .{ .NotExported = ReasonExcludedClass };
    if (class_is(facts, CoreWindowClass) and image_is_shell_host(facts)) {
        return .{ .NotExported = ReasonExcludedCoreWindow };
    }
    if (flag_set(facts, FactAgentOwned)) {
        if (!flag_set(facts, FactAgentPreedit)) return .{ .NotExported = ReasonExcludedAgent };
        return gate(facts, RoleImePreedit, if (is_owned(facts)) facts.owner_root else 0);
    }
    if (image_is(facts, ConsentImage)) {
        if (class_is(facts, UacDummyClass)) return .{ .NotExported = ReasonExcludedUacDummy };
        if (class_is(facts, UacCredentialClass)) {
            if (uac_mode != UacWindow and uac_mode != UacAuto) return .{ .NotExported = ReasonUacNativeHidden };
            return gate(facts, RoleUacConsent, 0);
        }
    }
    if (facts.root != facts.window) return .{ .NotExported = ReasonChild };
    const owned = is_owned(facts);
    const popup_parent: u64 = if (owned) facts.owner_root else 0;
    const caption_bar = facts.style & StyleCaption == StyleCaption;
    const no_activate = facts.ex_style & ExStyleNoActivate != 0;
    const popup_menu = facts.style & StylePopup != 0 and owned and !caption_bar and !no_activate;
    if (class_is(facts, MenuClassPopup) or class_is(facts, MenuClassCombo) or popup_menu) {
        return gate(facts, RoleMenu, popup_parent);
    }
    const tool_owned = facts.ex_style & ExStyleToolWindow != 0 and owned;
    if (class_is(facts, TooltipClass) or no_activate or tool_owned) {
        return gate(facts, RoleTooltip, popup_parent);
    }
    if (owned and facts.ex_style & ExStyleAppWindow == 0) return gate(facts, RoleDialog, facts.owner_root);
    return gate(facts, RoleToplevel, 0);
}

/// Pure `interop_window_graph_classify`: validate, then classify, writing the C verdict.
///
/// @param[in] facts Non-null borrowed snapshot; read only, never retained.
/// @param[in] uac_mode `UacWindow`, `UacAuto` or `UacNative`; anything above `UacNative` is InvalidArgument.
/// @param[out] out Non-null; fully written on Ok (fields not meaningful for the kind are zero,
///             `reserved0` zero), unchanged on every other status.
/// @return Ok; InvalidArgument for `uac_mode` above 2; InvalidFacts when `validate_facts` fails.
/// @note Pure and thread-safe: touches no graph, counter or callback; no allocation.
pub fn classify_public(facts: *const facts_t, uac_mode: u32, out: *c_t.interop_window_verdict_t) status_t {
    if (uac_mode > UacNative) return .InvalidArgument;
    if (!validate_facts(facts)) return .InvalidFacts;
    var verdict = std_t.mem.zeroes(c_t.interop_window_verdict_t);
    switch (classify(facts, uac_mode)) {
        .Eligible => |eligible_verdict| {
            verdict.kind = @intFromEnum(verdict_kind_t.Eligible);
            verdict.role = eligible_verdict.role;
            verdict.parent_window = eligible_verdict.parent_window;
        },
        .NotExported => |reason| {
            verdict.kind = @intFromEnum(verdict_kind_t.NotExported);
            verdict.reason = reason;
        },
        .Suspended => verdict.kind = @intFromEnum(verdict_kind_t.Suspended),
    }
    out.* = verdict;
    return .Ok;
}

/// Compare a stored record with a new snapshot of the same HWND for incarnation identity.
///
/// Equal means the same incarnation: `process_id`, `thread_id`, `process_start_time`
/// and the FNV-1a hash of the class bytes all match.
///
/// @param[in] rec Non-null borrowed record (entry body); read only.
/// @param[in] facts Non-null borrowed snapshot; need not be validated.
/// @return true when the identity tuples are equal.
/// @note Pure; thread-safe.
pub fn identity_equal(rec: *const record_t, facts: *const facts_t) bool {
    return rec.process_id == facts.process_id and rec.thread_id == facts.thread_id and
        rec.process_start_time == facts.process_start_time and
        rec.class_hash == class_hash_of(class_bytes(facts));
}

/// Increment a counter, saturating at the maximum u64 value.
///
/// @param[in,out] counter Non-null caller-owned counter; never wraps.
/// @note Not atomic; the graph is single-threaded by contract.
pub fn bump(counter: *u64) void {
    counter.* +|= 1;
}

/// Add to a counter, saturating at the maximum u64 value.
///
/// @param[in,out] counter Non-null caller-owned counter; never wraps.
/// @param[in] amount Value to add.
/// @note Not atomic; the graph is single-threaded by contract.
pub fn bump_by(counter: *u64, amount: u64) void {
    counter.* +|= amount;
}

/// Application-key lookup callback type (`interop_window_app_key_lookup_t`); optional C function pointer.
pub const app_key_lookup_t = c_t.interop_window_app_key_lookup_t;
/// Millisecond clock callback type (`interop_window_now_ms_t`); optional C function pointer.
pub const now_ms_t = c_t.interop_window_now_ms_t;
/// Drain sink callback type (`interop_window_event_sink_t`); optional C function pointer.
pub const event_sink_t = c_t.interop_window_event_sink_t;

/// Upper bound in bytes for the single heap allocation of one graph; asserted at compile time.
pub const MaxGraphBytes = 524288;

/// Lifecycle state of one exported-entry slot.
pub const slot_state_t = enum(u8) {
    /// The slot is unused; its record is all zero.
    Free,
    /// The entry is the live, exported generation of its window.
    Active,
    /// The entry is quarantined: it keeps its slot until `complete_retire` succeeds.
    Retiring,
};

/// One exported-entry slot (Active or Retiring generation of a window).
///
/// Invariants: a `Free` slot holds a zero record; `seen` is meaningful only while a refresh is open.
pub const entry_t = struct {
    /// Slot lifecycle state.
    state: slot_state_t,
    /// Refresh mark: set when the open refresh offered this window.
    seen: bool,
    /// Persistent body of the generation.
    record: record_t,
};

/// One deferred (parent-pending) record; never exported and never holds a generation.
pub const deferred_t = struct {
    /// True while the slot holds a record.
    used: bool,
    /// Refresh mark: set when the open refresh offered this window.
    seen: bool,
    /// Persistent body; `generation` is 0 and `seq` is the deferral age.
    record: record_t,
};

/// The complete private state of one window graph.
///
/// One heap allocation made by `create` and released by `destroy`; nothing inside points
/// to other heap memory. Every field is confined to the owner thread (`owner_thread`).
/// `busy` is true for the whole of a mutating call so that a callback re-entering the
/// graph is rejected. Gauges (active, retiring, deferred, pending events) are never
/// stored: they are recomputed from the arrays when read.
pub const graph_t = struct {
    /// Allocator that produced this graph; used by `destroy`.
    allocator: std_t.mem.Allocator,
    /// Copy of the creation parameters (the pointers inside are borrowed from the caller).
    config: config_t,
    /// Thread that created the graph; the only thread allowed to call it.
    owner_thread: std_t.Thread.Id,
    /// True between a successful `enter` and `leave`; rejects re-entry.
    busy: bool,
    /// True while a refresh is open.
    refresh_open: bool,
    /// True after the first successful capacity diagnostic.
    have_diag: bool,
    /// Monotonic timer of the default clock; null when the platform has none (clock reads 0).
    timer: ?std_t.time.Timer,
    /// Last generation handed out; 0 before the first admission.
    generations_issued: u64,
    /// Largest stamp accepted by an Ok call.
    stamp_high_water: u64,
    /// Monotonic 64-bit tick base (`unwrap_tick`); advanced to the larger of itself and the
    /// unwrapped tick on every Ok call that carries a tick; starts at `TickBaseStart`.
    tick_base: u64 = TickBaseStart,
    /// Reading of the graph clock (`clock_now_ms`) at which `tick_base` is exact. Together with
    /// `tick_base` it gives the unwrap estimate `tick_estimate`; 0 at creation.
    tick_clock_at_base: u64 = 0,
    /// Next deferral age to assign; starts at 1, saturating.
    next_seq: u64,
    /// Clock reading of the last successful capacity diagnostic.
    last_diag_ms: u64,
    /// Diagnostic counter: admissions refused because 64 entries were exported.
    diag_at_capacity: u64,
    /// Diagnostic counter: deferred records evicted by a newer deferral.
    diag_deferred_evicted: u64,
    /// Diagnostic counter: calls rejected with QueueFull.
    diag_queue_full: u64,
    /// Diagnostic counter: admissions refused for lack of a generation.
    diag_generation_exhausted: u64,
    /// Index (0..255) of the oldest queued event.
    ring_head: u32,
    /// Number of queued events (0..256).
    ring_len: u32,
    /// Cumulative counters; gauge fields are filled on read only.
    stats: stats_t,
    /// Exported entries, Active or Retiring, at most `MaxEntries`.
    entries: [MaxEntries]entry_t,
    /// Deferred records, at most `MaxDeferred`.
    deferred: [MaxDeferred]deferred_t,
    /// Circular event queue; the i-th oldest event is at `(ring_head + i) & (EventCapacity - 1)`.
    ring: [EventCapacity]event_t,
};

comptime {
    if (@sizeOf(graph_t) > MaxGraphBytes) @compileError("graph_t must fit its 524288 byte single allocation");
    if (EventCapacity & (EventCapacity - 1) != 0) @compileError("EventCapacity must be a power of two");
}

/// Create a graph: the only allocation of the graph's life.
///
/// The configuration is validated before anything is allocated: `session_epoch` nonzero,
/// `reserved0` zero, `uac_mode` at most `UacNative`, `app_key_lookup` non-null. The whole
/// graph is then zeroed (all slots Free, ring empty, counters 0), the configuration is copied
/// and the calling thread becomes the owner thread.
///
/// @param[in] allocator Allocator for the single `graph_t` allocation; copied into the graph.
///                      It must outlive the graph and stay usable for `destroy`.
/// @param[in] config Non-null borrowed configuration; copied by value, the callback and
///                   context pointers inside stay owned by the caller and must outlive the graph.
/// @return A pointer owned by the caller, to be released exactly once with `destroy`.
/// @retval error.InvalidArgument A configuration field is invalid; nothing was allocated.
/// @retval error.OutOfMemory The allocation failed; no graph exists and nothing leaked.
/// @note Thread-safe by itself (touches no shared state); the result is bound to the calling thread.
pub fn create(allocator: std_t.mem.Allocator, config: *const config_t) error{ OutOfMemory, InvalidArgument }!*graph_t {
    if (config.session_epoch == 0 or config.reserved0 != 0) return error.InvalidArgument;
    if (config.uac_mode > UacNative or config.app_key_lookup == null) return error.InvalidArgument;
    const graph = try allocator.create(graph_t);
    @memset(std_t.mem.asBytes(graph), 0);
    graph.allocator = allocator;
    graph.config = config.*;
    graph.owner_thread = std_t.Thread.getCurrentId();
    graph.timer = std_t.time.Timer.start() catch null;
    graph.next_seq = 1;
    graph.tick_base = TickBaseStart;
    graph.tick_clock_at_base = 0;
    return graph;
}

/// Free a graph created by `create`.
///
/// @param[in] graph Non-null graph previously returned by `create`; invalid after the call.
/// @note Owner thread only and never from a callback: the caller must first have seen
///       `enter_read` return null, because this function checks nothing. Contents are not scrubbed.
pub fn destroy(graph: *graph_t) void {
    const allocator = graph.allocator;
    allocator.destroy(graph);
}

fn guard_status(graph: *const graph_t) ?status_t {
    if (graph.owner_thread != std_t.Thread.getCurrentId()) return .WrongThread;
    if (graph.busy) return .Reentrant;
    return null;
}

/// Begin a mutating call: apply the header's common call rules 2 to 4 in order.
///
/// Order: wrong thread (WrongThread, nothing touched), busy (Reentrant, nothing touched),
/// epoch given and different from the graph epoch, zero included (StaleEpoch, only
/// `rejected_stale_epoch` changes). On success `busy` is set and the caller must pair this
/// with `leave` on every exit path.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch, or null for calls without an epoch parameter.
/// @return null when the call may proceed (`busy` is now true); otherwise the status to return
///         (`busy` is unchanged).
/// @note Owner thread semantics: this function itself reads only the immutable `owner_thread`
///       from any thread and writes nothing unless the thread matches.
pub fn enter(graph: *graph_t, epoch: ?u64) ?status_t {
    if (guard_status(graph)) |status| return status;
    if (epoch) |captured| {
        if (captured != graph.config.session_epoch) {
            bump(&graph.stats.rejected_stale_epoch);
            return .StaleEpoch;
        }
    }
    graph.busy = true;
    return null;
}

/// Read-only variant of `enter` for getters: rules 2 and 3 only, and `busy` is not set.
///
/// @param[in] graph Non-null graph; never written.
/// @return null when the call may proceed; WrongThread or Reentrant otherwise.
/// @note Reads only `owner_thread` and `busy`; safe from any thread (a foreign thread gets WrongThread).
pub fn enter_read(graph: *const graph_t) ?status_t {
    return guard_status(graph);
}

/// End a call begun with a successful `enter`.
///
/// @param[in,out] graph Non-null graph whose `busy` flag is set by the same call.
/// @note Owner thread only; clears `busy`.
pub fn leave(graph: *graph_t) void {
    graph.busy = false;
}

/// Count the slots currently in a state.
///
/// @param[in] graph Non-null graph.
/// @param[in] state State to count (`Free` is allowed).
/// @return Number of entry slots in that state, 0..64.
/// @note Owner thread only; pure read.
pub fn count_state(graph: *const graph_t, state: slot_state_t) u32 {
    var total: u32 = 0;
    for (&graph.entries) |*entry| {
        if (entry.state == state) total += 1;
    }
    return total;
}

/// Count the deferred records currently stored.
///
/// @param[in] graph Non-null graph.
/// @return Number of used deferred slots, 0..64.
/// @note Owner thread only; pure read.
pub fn count_deferred(graph: *const graph_t) u32 {
    var total: u32 = 0;
    for (&graph.deferred) |*slot| {
        if (slot.used) total += 1;
    }
    return total;
}

/// Find the Active entry of a window.
///
/// @param[in] graph Non-null graph.
/// @param[in] window HWND to find (zero never matches a live entry).
/// @return Slot index of the Active entry, or null when the window has none.
/// @note Owner thread only; pure read. Invariant I-2 allows at most one match.
pub fn find_active(graph: *const graph_t, window: u64) ?usize {
    for (&graph.entries, 0..) |*entry, index| {
        if (entry.state == .Active and entry.record.window == window) return index;
    }
    return null;
}

/// Find the entry with an exact (window, generation) pair, Active or Retiring.
///
/// @param[in] graph Non-null graph.
/// @param[in] window HWND to find.
/// @param[in] generation Generation to find; a different generation of the same window never matches.
/// @return Slot index, or null when no non-Free entry has that pair.
/// @note Owner thread only; pure read.
pub fn find_exact(graph: *const graph_t, window: u64, generation: u64) ?usize {
    for (&graph.entries, 0..) |*entry, index| {
        if (entry.state == .Free) continue;
        if (entry.record.window == window and entry.record.generation == generation) return index;
    }
    return null;
}

/// Find the deferred record of a window.
///
/// @param[in] graph Non-null graph.
/// @param[in] window HWND to find.
/// @return Slot index of the used deferred record, or null when the window has none.
/// @note Owner thread only; pure read. Invariant I-1 allows at most one match.
pub fn find_deferred(graph: *const graph_t, window: u64) ?usize {
    for (&graph.deferred, 0..) |*slot, index| {
        if (slot.used and slot.record.window == window) return index;
    }
    return null;
}

fn ring_index(graph: *const graph_t, offset: u32) usize {
    return (graph.ring_head + offset) & (EventCapacity - 1);
}

fn ring_pop_head(graph: *graph_t) void {
    graph.ring_head = (graph.ring_head + 1) & (EventCapacity - 1);
    graph.ring_len -= 1;
}

/// Free event slots in the ring.
///
/// @param[in] graph Non-null graph.
/// @return `EventCapacity` minus the queued event count, 0..256.
/// @note Owner thread only; pure read.
pub fn ring_room(graph: *const graph_t) u32 {
    return EventCapacity - graph.ring_len;
}

/// Find the pending event an Announce or Update of (window, generation) would merge into.
///
/// The ring is scanned newest first and the first event with the same (window, generation)
/// decides: an Announce or Update is the merge target, a RetireIntent ends the scan with no
/// target (events never merge across a retirement).
///
/// @param[in] graph Non-null graph; the returned pointer aliases ring storage.
/// @param[in] window HWND of the generation.
/// @param[in] generation Generation to match.
/// @return Pointer to the newest matching Announce/Update event, or null when a new slot is needed.
/// @note Owner thread only; no allocation; at most 256 iterations. The pointer is valid until
///       the next ring mutation and is never retained.
pub fn ring_merge_target(graph: *graph_t, window: u64, generation: u64) ?*event_t {
    var offset = graph.ring_len;
    while (offset > 0) {
        offset -= 1;
        const queued = &graph.ring[ring_index(graph, offset)];
        if (queued.window != window or queued.generation != generation) continue;
        return if (queued.kind == EventRetireIntent) null else queued;
    }
    return null;
}

/// Queue an event, merging an Announce or Update into a pending one when the header allows.
///
/// For kind Announce or Update the ring is searched with `ring_merge_target`. If a target exists
/// its payload (x, y, width, height, state, dpi, min_width, min_height, title and title_length) is
/// overwritten in place, its kind is kept (an Announce stays an Announce), its changed_mask is
/// OR-ed with the new one and `events_coalesced` is bumped. Otherwise the event is appended and
/// `events_queued` is bumped (`updates_queued` as well for an Update). RetireIntent is always
/// appended.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] ev Non-null borrowed event; copied, never retained.
/// @return true when a new ring slot was consumed; false when the payload was merged into a
///         pending event, or when the ring was full and no merge target existed (then
///         nothing changed; the planning stage must prove `ring_room` first, so a full
///         ring is a caller defect and is not counted anywhere).
/// @note Owner thread only; no allocation; at most 256 iterations.
pub fn ring_push_or_coalesce(graph: *graph_t, ev: *const event_t) bool {
    if (ev.kind == EventAnnounce or ev.kind == EventUpdate) {
        if (ring_merge_target(graph, ev.window, ev.generation)) |queued| {
            queued.x = ev.x;
            queued.y = ev.y;
            queued.width = ev.width;
            queued.height = ev.height;
            queued.state = ev.state;
            queued.dpi = ev.dpi;
            queued.min_width = ev.min_width;
            queued.min_height = ev.min_height;
            queued.title_length = ev.title_length;
            queued.title = ev.title;
            queued.changed_mask |= ev.changed_mask;
            bump(&graph.stats.events_coalesced);
            return false;
        }
    }
    if (graph.ring_len >= EventCapacity) return false;
    graph.ring[ring_index(graph, graph.ring_len)] = ev.*;
    graph.ring_len += 1;
    bump(&graph.stats.events_queued);
    if (ev.kind == EventUpdate) bump(&graph.stats.updates_queued);
    return true;
}

/// Pop the oldest queued event.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @param[out] out Non-null; receives a value copy on Ok, unchanged otherwise.
/// @return Ok when an event was popped (`events_drained` bumped); None when the queue is empty;
///         WrongThread, Reentrant or StaleEpoch per `enter`.
/// @note Owner thread only; no callback; no allocation.
pub fn next_event(graph: *graph_t, epoch: u64, out: *event_t) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    if (graph.ring_len == 0) return .None;
    out.* = graph.ring[graph.ring_head];
    ring_pop_head(graph);
    bump(&graph.stats.events_drained);
    return .Ok;
}

/// Deliver up to `max_events` queued events, oldest first, to a sink.
///
/// Each event is copied to a local and the sink receives a pointer to that copy while `busy`
/// is set, so any graph call from the sink returns Reentrant. The event is popped (and
/// `events_drained` bumped) only after the sink returns 0. A nonzero return leaves the event
/// queued, bumps `sink_failures` and ends the drain with SinkFailed; events accepted before it
/// stay consumed.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @param[in] sink Callback; null is InvalidArgument. Borrowed for the call.
/// @param[in,out] context Passed to the sink unchanged; may be null; owned by the caller.
/// @param[in] max_events At least 1; 0 is InvalidArgument.
/// @param[out] out_delivered Non-null; receives the number of accepted events on Ok, None and
///                           SinkFailed (0 for None); unchanged for every other status.
/// @return Ok when at least one event was delivered and the sink never refused; None for an
///         empty queue; SinkFailed; InvalidArgument; WrongThread, Reentrant or StaleEpoch per `enter`.
/// @note Owner thread only; the sink runs on the owner thread; no allocation.
pub fn drain(graph: *graph_t, epoch: u64, sink: event_sink_t, context: ?*anyopaque, max_events: u32, out_delivered: *u32) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    const callback = sink orelse return .InvalidArgument;
    if (max_events == 0) return .InvalidArgument;
    var delivered: u32 = 0;
    while (delivered < max_events and graph.ring_len != 0) {
        const head = graph.ring[graph.ring_head];
        if (callback(context, &head) != 0) {
            bump(&graph.stats.sink_failures);
            out_delivered.* = delivered;
            return .SinkFailed;
        }
        ring_pop_head(graph);
        bump(&graph.stats.events_drained);
        delivered += 1;
    }
    out_delivered.* = delivered;
    return if (delivered == 0) .None else .Ok;
}

/// Check that an action may be performed on (window, generation) right now.
///
/// @param[in,out] graph Non-null graph (only counters can change).
/// @param[in] epoch Captured session epoch.
/// @param[in] window Nonzero HWND.
/// @param[in] generation Nonzero generation.
/// @return Ok only for an Active entry with exactly that pair; Stale (and `rejected_stale`
///         bumped) for a Retiring, unknown or other-generation pair; InvalidArgument for a zero
///         window or generation; WrongThread, Reentrant or StaleEpoch per `enter`.
/// @note Owner thread only; no callback; no allocation.
pub fn check_action(graph: *graph_t, epoch: u64, window: u64, generation: u64) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    if (window == 0 or generation == 0) return .InvalidArgument;
    if (find_exact(graph, window, generation)) |index| {
        if (graph.entries[index].state == .Active) return .Ok;
    }
    bump(&graph.stats.rejected_stale);
    return .Stale;
}

/// Describe the Active entry of a window.
///
/// @param[in] graph Non-null graph; never written.
/// @param[in] window Nonzero HWND.
/// @param[out] out Non-null; fully written (reserved0 zero) on Ok, unchanged otherwise.
/// @return Ok when an Active entry exists; None when it does not (Retiring entries are not live);
///         InvalidArgument for window 0; WrongThread or Reentrant per `enter_read`.
/// @note Owner thread only; no counter changes; no allocation.
pub fn find_live(graph: *const graph_t, window: u64, out: *live_t) status_t {
    if (enter_read(graph)) |status| return status;
    if (window == 0) return .InvalidArgument;
    const index = find_active(graph, window) orelse return .None;
    const record = &graph.entries[index].record;
    out.* = .{
        .window = record.window,
        .generation = record.generation,
        .parent_window = record.parent_window,
        .parent_generation = record.parent_generation,
        .role = record.role,
        .state = record.state,
        .process_id = record.process_id,
        .reserved0 = 0,
    };
    return .Ok;
}

/// List the quarantined entries in ascending generation order.
///
/// @param[in] graph Non-null graph; never written.
/// @param[out] entries Caller array of `capacity` elements; may be null only when `capacity` is 0.
///                     Up to `capacity` entries are written on Ok; the rest of the array is untouched.
/// @param[in] capacity Elements available in `entries`.
/// @param[out] out_count Non-null; on Ok receives the total number of Retiring entries even when
///                       it exceeds `capacity`; unchanged otherwise.
/// @return Ok; InvalidArgument for null `entries` with `capacity` above 0; WrongThread or Reentrant
///         per `enter_read`.
/// @note Owner thread only; no counter changes; no allocation.
pub fn list_retiring(graph: *const graph_t, entries: ?[*]retiring_entry_t, capacity: u32, out_count: *u32) status_t {
    if (enter_read(graph)) |status| return status;
    if (entries == null and capacity > 0) return .InvalidArgument;
    var order: [MaxEntries]usize = undefined;
    var total: u32 = 0;
    for (&graph.entries, 0..) |*entry, index| {
        if (entry.state != .Retiring) continue;
        var slot = total;
        while (slot > 0 and graph.entries[order[slot - 1]].record.generation > entry.record.generation) : (slot -= 1) {
            order[slot] = order[slot - 1];
        }
        order[slot] = index;
        total += 1;
    }
    const written = @min(total, capacity);
    for (order[0..written], 0..) |index, position| {
        const record = &graph.entries[index].record;
        entries.?[position] = .{
            .window = record.window,
            .generation = record.generation,
            .reason = record.retire_reason,
            .role = record.role,
        };
    }
    out_count.* = total;
    return .Ok;
}

/// Snapshot every unique nonzero HWND represented by an Active or Retiring entry.
///
/// The result is sorted by ascending numeric HWND. Equal HWNDs across generations appear once.
/// Deferred records are intentionally excluded. `out_free_entry_count` counts Free entry slots,
/// not `MaxEntries - unique HWND count`, because an Active and Retiring generation may share an
/// HWND while occupying distinct slots. This read-only query has the common thread/reentrancy
/// guards followed by an epoch comparison (without changing rejection counters), then validates
/// nullable output availability and minimum capacity. The C wrapper additionally checks complete
/// address spans/alignment/aliases. No busy bit, graph field, counter, or output is
/// changed unless every argument is valid.
///
/// @param[in] graph Non-null graph; never written.
/// @param[in] epoch Exact session epoch; stale epochs are rejected without counter changes.
/// @param[out] windows Non-null array of at least `MaxEntries` u64 values; only `count` elements
///                     are written on Ok and the remainder is untouched.
/// @param[in] capacity Number of u64 elements available; must be at least `MaxEntries`.
/// @param[out] out_count Non-null; receives the unique tracked HWND count on Ok only.
/// @param[out] out_free_entry_count Non-null; receives the exact Free entry-slot count on Ok only.
/// @return Ok; WrongThread; Reentrant; StaleEpoch; InvalidArgument.
/// @note Owner thread only; no callback, clock, allocation, lock, wait, or graph mutation.
pub fn list_tracked_windows(
    graph: *const graph_t,
    epoch: u64,
    windows: ?[*]u64,
    capacity: u32,
    out_count: ?*u32,
    out_free_entry_count: ?*u32,
) status_t {
    if (guard_status(graph)) |status| return status;
    if (epoch != graph.config.session_epoch) return .StaleEpoch;
    if (windows == null or capacity < MaxEntries or out_count == null or out_free_entry_count == null)
        return .InvalidArgument;

    var staged: [MaxEntries]u64 = undefined;
    var staged_count: usize = 0;
    for (&graph.entries) |*entry| {
        if (entry.state == .Free) continue;
        const window = entry.record.window;
        if (window == 0) continue;

        var insert_at: usize = 0;
        while (insert_at < staged_count and staged[insert_at] < window) : (insert_at += 1) {}
        if (insert_at < staged_count and staged[insert_at] == window) continue;
        var move_at: usize = staged_count;
        while (move_at > insert_at) : (move_at -= 1) staged[move_at] = staged[move_at - 1];
        staged[insert_at] = window;
        staged_count += 1;
    }

    const free_count = count_state(graph, .Free);
    @memcpy(windows.?[0..staged_count], staged[0..staged_count]);
    out_count.?.* = @intCast(staged_count);
    out_free_entry_count.?.* = free_count;
    return .Ok;
}

/// End the quarantine of one retired generation after the coordinator's proof.
///
/// Check order after `enter`: zero window or generation is InvalidArgument; no entry with the
/// exact pair is Stale (`rejected_stale`); an Active exact match is NotRetiring; either proof
/// flag different from exactly 1 is ProofIncomplete (`proof_incomplete`, the entry stays
/// quarantined). Success scrubs the slot, frees it and bumps `retire_completed`. Only the exact
/// generation is ever released, so a late completion cannot touch a newer generation.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @param[in] window Nonzero HWND.
/// @param[in] generation Nonzero generation of a Retiring entry.
/// @param[in] proof Non-null borrowed proof; both flags must be exactly 1.
/// @return Ok when the slot was released; InvalidArgument; Stale; NotRetiring; ProofIncomplete;
///         WrongThread, Reentrant or StaleEpoch per `enter`. Non-Ok statuses change only counters.
/// @note Owner thread only; no callback; no allocation.
pub fn complete_retire(graph: *graph_t, epoch: u64, window: u64, generation: u64, proof: *const proof_t) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    if (window == 0 or generation == 0) return .InvalidArgument;
    const index = find_exact(graph, window, generation) orelse {
        bump(&graph.stats.rejected_stale);
        return .Stale;
    };
    if (graph.entries[index].state == .Active) return .NotRetiring;
    if (proof.capture_stopped != 1 or proof.pins_drained != 1) {
        bump(&graph.stats.proof_incomplete);
        return .ProofIncomplete;
    }
    graph.entries[index] = .{ .state = .Free, .seen = false, .record = record_init() };
    bump(&graph.stats.retire_completed);
    return .Ok;
}

/// Copy the cumulative counters and the current gauges.
///
/// @param[in] graph Non-null graph; never written.
/// @param[out] out Non-null; fully written on Ok (gauges recomputed, reserved0 zero), unchanged otherwise.
/// @return Ok; WrongThread or Reentrant per `enter_read`.
/// @note Owner thread only; no counter changes; no allocation.
pub fn get_stats(graph: *const graph_t, out: *stats_t) status_t {
    if (enter_read(graph)) |status| return status;
    var snapshot = graph.stats;
    snapshot.session_epoch = graph.config.session_epoch;
    snapshot.generations_issued = graph.generations_issued;
    snapshot.stamp_high_water = graph.stamp_high_water;
    snapshot.active_count = count_state(graph, .Active);
    snapshot.retiring_count = count_state(graph, .Retiring);
    snapshot.deferred_count = count_deferred(graph);
    snapshot.events_pending = graph.ring_len;
    snapshot.refresh_active = if (graph.refresh_open) 1 else 0;
    snapshot.reserved0 = 0;
    out.* = snapshot;
    return .Ok;
}

/// Largest stamp accepted by an Ok call so far.
///
/// @param[in] graph Non-null graph.
/// @return The stamp high-water mark, 0 before any accepted call.
/// @note Pure read with no guard; callers apply `enter_read` first (see `stamp_high_water`).
pub fn stamp_high_water_of(graph: *const graph_t) u64 {
    return graph.stamp_high_water;
}

/// Guarded read of the stamp high-water mark.
///
/// @param[in] graph Non-null graph; never written.
/// @param[out] out Non-null; receives the mark on Ok, unchanged otherwise.
/// @return Ok; WrongThread or Reentrant per `enter_read`.
/// @note Owner thread only; no counter changes.
pub fn stamp_high_water(graph: *const graph_t, out: *u64) status_t {
    if (enter_read(graph)) |status| return status;
    out.* = stamp_high_water_of(graph);
    return .Ok;
}

fn clock_now_ms(graph: *graph_t) u64 {
    if (graph.config.now_ms) |callback| return callback(graph.config.now_ms_context);
    if (graph.timer) |*timer| return timer.read() / std_t.time.ns_per_ms;
    return 0;
}

/// Estimate of the current 64-bit tick: `tick_base` plus the graph-clock time elapsed since
/// `tick_clock_at_base`.
///
/// The elapsed time is 0 when `now` is not after `tick_clock_at_base` (a clock that does not move,
/// has no timer, or went backwards), so the estimate is never below `tick_base` and the result
/// saturates instead of wrapping. Unwrapping an incoming tick against this estimate keeps the unwrap
/// exact across a silence of any length (no Ok call that carries a tick for more than 2^31 ms) as
/// long as the graph clock kept running.
///
/// @param[in] graph Non-null graph; never written.
/// @param[in] now Clock reading taken for this call (`clock_now_ms`).
/// @return The estimate, at least `graph.tick_base`.
/// @note Owner thread only; pure.
pub fn tick_estimate(graph: *const graph_t, now: u64) u64 {
    const elapsed: u64 = if (now > graph.tick_clock_at_base) now - graph.tick_clock_at_base else 0;
    return graph.tick_base +| elapsed;
}

/// Take the rate-limited capacity diagnostic.
///
/// None without reading the clock when all four diagnostic counters are zero. Otherwise the
/// clock (injected `now_ms` or the default monotonic timer) is read: the first take is immediate
/// with `elapsed_ms` 0; later takes need `DiagnosticIntervalMs` since the previous one (exactly
/// that interval is enough); a clock that went backwards counts as 0 elapsed. A None answer keeps
/// the counters and the baseline. On Ok the four counters reset, the baseline moves to the clock
/// reading and the gauges are filled.
///
/// @param[in,out] graph Non-null graph.
/// @param[out] out Non-null; fully written on Ok, unchanged otherwise.
/// @return Ok; None; WrongThread or Reentrant per `enter` (no epoch parameter).
/// @note Owner thread only; the clock callback runs while `busy` is set; no allocation.
pub fn take_capacity_diagnostic(graph: *graph_t, out: *diagnostic_t) status_t {
    if (enter(graph, null)) |status| return status;
    defer leave(graph);
    const any = graph.diag_at_capacity | graph.diag_deferred_evicted | graph.diag_queue_full | graph.diag_generation_exhausted;
    if (any == 0) return .None;
    const now = clock_now_ms(graph);
    var elapsed: u64 = 0;
    if (graph.have_diag) {
        elapsed = if (now >= graph.last_diag_ms) now - graph.last_diag_ms else 0;
        if (elapsed < DiagnosticIntervalMs) return .None;
    }
    out.* = .{
        .at_capacity = graph.diag_at_capacity,
        .deferred_evicted = graph.diag_deferred_evicted,
        .queue_full = graph.diag_queue_full,
        .generation_exhausted = graph.diag_generation_exhausted,
        .elapsed_ms = elapsed,
        .active_count = count_state(graph, .Active),
        .retiring_count = count_state(graph, .Retiring),
        .deferred_count = count_deferred(graph),
        .events_pending = graph.ring_len,
    };
    graph.last_diag_ms = now;
    graph.have_diag = true;
    graph.diag_at_capacity = 0;
    graph.diag_deferred_evicted = 0;
    graph.diag_queue_full = 0;
    graph.diag_generation_exhausted = 0;
    return .Ok;
}

// ---------------------------------------------------------------------------------------------
// Planning engine (TODO #10, layer 2b1): observe, note_destroyed, retire cascade, deferral.
//
// Every mutating call follows the same shape: guards (`enter`), read-only plan, pre-checks that
// can still fail without touching anything (cycle, capacity, generations, event-queue room), the
// single `app_key_lookup` call, and then an infallible apply phase. Counters may change on a
// failing status; graph state may not (except the documented Cycle drop of a deferred record).
// ---------------------------------------------------------------------------------------------

/// Per-call effect counts that end up in `observation_t`.
pub const tally_t = struct {
    /// New ring slots consumed by this call (a coalesced payload counts 0).
    events: u32 = 0,
    /// Entries announced, including flushed children.
    admitted: u32 = 0,
    /// Entries moved to Retiring, including descendants.
    retired: u32 = 0,
    /// Deferred records dropped or evicted.
    dropped: u32 = 0,
};

/// A set of Active entries that retire together: the root and every transitive descendant.
///
/// `slots` holds entry-slot indices in descending generation order, so descendants come before
/// their ancestors and the root (the lowest generation, by invariant I-4) is the last element.
pub const cascade_t = struct {
    /// Number of valid elements in `slots`, 0..64.
    count: u32 = 0,
    /// Entry-slot indices, children first, root last.
    slots: [MaxEntries]u8 = [_]u8{0} ** MaxEntries,
};

/// Collect the retirement cascade of one Active entry.
///
/// Descendants are the Active entries whose (parent_window, parent_generation) pair names a
/// collected member, found transitively. The result is sorted by descending generation.
///
/// @param[in] graph Non-null graph; never written.
/// @param[in] root Slot index of an Active entry.
/// @param[out] out Non-null; fully rewritten.
/// @note Owner thread only; pure read; at most 64 x 64 comparisons; no allocation.
pub fn collect_cascade(graph: *const graph_t, root: usize, out: *cascade_t) void {
    var in_set = [_]bool{false} ** MaxEntries;
    out.* = .{};
    out.slots[0] = @intCast(root);
    out.count = 1;
    in_set[root] = true;
    var head: u32 = 0;
    while (head < out.count) : (head += 1) {
        const parent = &graph.entries[out.slots[head]].record;
        for (&graph.entries, 0..) |*entry, index| {
            if (entry.state != .Active or in_set[index]) continue;
            if (entry.record.parent_window != parent.window) continue;
            if (entry.record.parent_generation != parent.generation) continue;
            in_set[index] = true;
            out.slots[out.count] = @intCast(index);
            out.count += 1;
        }
    }
    var next: u32 = 1;
    while (next < out.count) : (next += 1) {
        const moving = out.slots[next];
        const generation = graph.entries[moving].record.generation;
        var hole = next;
        while (hole > 0 and graph.entries[out.slots[hole - 1]].record.generation < generation) : (hole -= 1) {
            out.slots[hole] = out.slots[hole - 1];
        }
        out.slots[hole] = moving;
    }
}

/// Test whether a window is the HWND of any member of a cascade.
///
/// @param[in] graph Non-null graph; never written.
/// @param[in] cascade Non-null cascade collected from the same graph.
/// @param[in] window HWND to look for.
/// @return true when some member entry has that window.
/// @note Owner thread only; pure read.
pub fn cascade_has_window(graph: *const graph_t, cascade: *const cascade_t, window: u64) bool {
    for (cascade.slots[0..cascade.count]) |slot| {
        if (graph.entries[slot].record.window == window) return true;
    }
    return false;
}

/// Build an event carrying the full current payload of a record.
///
/// @param[in] graph Non-null graph (supplies the session epoch).
/// @param[in] rec Non-null record; title and application key are copied by value.
/// @param[in] kind `EventAnnounce`, `EventUpdate` or `EventRetireIntent`.
/// @param[in] mask changed_mask (`ChangeMask` for Announce, the diff for Update, 0 for RetireIntent).
/// @param[in] reason Reason code, meaningful for RetireIntent only (0 otherwise).
/// @return A fully initialised event value (reserved0 zero).
/// @note Pure; thread-safe for distinct graphs; no allocation.
pub fn event_from_record(graph: *const graph_t, rec: *const record_t, kind: u32, mask: u32, reason: u32) event_t {
    var ev = std_t.mem.zeroes(event_t);
    ev.session_epoch = graph.config.session_epoch;
    ev.window = rec.window;
    ev.generation = rec.generation;
    ev.parent_window = rec.parent_window;
    ev.parent_generation = rec.parent_generation;
    ev.x = rec.x;
    ev.y = rec.y;
    ev.width = rec.width;
    ev.height = rec.height;
    ev.kind = kind;
    ev.role = rec.role;
    ev.state = rec.state;
    ev.dpi = rec.dpi;
    ev.min_width = rec.min_width;
    ev.min_height = rec.min_height;
    ev.process_id = rec.process_id;
    ev.changed_mask = mask;
    ev.reason = reason;
    ev.title_length = rec.title_length;
    ev.app_key_length = rec.app_key_length;
    ev.title = rec.title;
    ev.app_key = rec.app_key;
    return ev;
}

/// Retire a cascade: children first, root last.
///
/// Every member moves to Retiring (keeping slot, record and generation) and one RetireIntent
/// event with the last known payload is appended per member: the root with `root_reason`, every
/// descendant with `ReasonParentRetired`. `retire_intents` is bumped per member. The caller
/// must have proven `ring_room >= cascade.count`. Re-deferral of descendants is a separate step
/// (`redefer_descendants`) because it must follow the flush of the replacement.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] cascade Non-null cascade collected from this graph; all members Active.
/// @param[in] root_reason Reason recorded for the root (the last element).
/// @param[in,out] tally Non-null; `events` and `retired` are increased.
/// @note Owner thread only; no callback; no allocation.
pub fn retire_cascade(graph: *graph_t, cascade: *const cascade_t, root_reason: u32, tally: *tally_t) void {
    for (cascade.slots[0..cascade.count], 0..) |slot, position| {
        const entry = &graph.entries[slot];
        entry.state = .Retiring;
        entry.record.retire_reason = if (position + 1 == cascade.count) root_reason else ReasonParentRetired;
        const ev = event_from_record(graph, &entry.record, EventRetireIntent, 0, entry.record.retire_reason);
        if (ring_push_or_coalesce(graph, &ev)) tally.events += 1;
        bump(&graph.stats.retire_intents);
        tally.retired += 1;
    }
}

/// Remove a deferred record and count the drop.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] slot Index of a used deferred slot.
/// @param[in,out] tally Non-null; `dropped` is increased.
/// @note Owner thread only. Bumps `stats.deferred_dropped`; evictions use `defer_insert` instead.
pub fn drop_deferred(graph: *graph_t, slot: usize, tally: *tally_t) void {
    graph.deferred[slot] = .{ .used = false, .seen = false, .record = record_init() };
    bump(&graph.stats.deferred_dropped);
    tally.dropped += 1;
}

/// Store a new deferred record, evicting the oldest one when the set is full.
///
/// The record gets the next age `seq` (saturating counter, ties broken by lowest index when
/// choosing a victim). When all 64 slots are used the record with the lowest `seq` is evicted:
/// `stats.deferred_evicted` and the diagnostic counter are bumped and `tally.dropped` grows.
/// `stats.deferred_added` is bumped for the new record. The record is stored as a deferred
/// record: `generation`, `parent_generation`, `admit_stamp`, `admit_captured_ms` and
/// `retire_reason` are forced to zero.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] rec Non-null record to copy; its `seq` is ignored.
/// @param[in] seen Initial refresh seen mark.
/// @param[in,out] tally Non-null; `dropped` is increased on eviction.
/// @note Owner thread only; no callback; no allocation; infallible.
pub fn defer_insert(graph: *graph_t, rec: *const record_t, seen: bool, tally: *tally_t) void {
    var slot: usize = 0;
    var found = false;
    for (&graph.deferred, 0..) |*candidate, index| {
        if (!candidate.used) {
            slot = index;
            found = true;
            break;
        }
    }
    if (!found) {
        for (&graph.deferred, 0..) |*candidate, index| {
            if (candidate.record.seq < graph.deferred[slot].record.seq) slot = index;
        }
        graph.deferred[slot] = .{ .used = false, .seen = false, .record = record_init() };
        bump(&graph.stats.deferred_evicted);
        bump(&graph.diag_deferred_evicted);
        tally.dropped += 1;
    }
    var stored = rec.*;
    stored.generation = 0;
    stored.parent_generation = 0;
    stored.admit_stamp = 0;
    stored.admit_captured_ms = 0;
    stored.retire_reason = 0;
    stored.seq = graph.next_seq;
    graph.next_seq +|= 1;
    graph.deferred[slot] = .{ .used = true, .seen = seen, .record = stored };
    bump(&graph.stats.deferred_added);
}

fn exported_count(graph: *const graph_t) u32 {
    return MaxEntries - count_state(graph, .Free);
}

fn find_free_entry(graph: *const graph_t) ?usize {
    for (&graph.entries, 0..) |*entry, index| {
        if (entry.state == .Free) return index;
    }
    return null;
}

fn reject_queue_full(graph: *graph_t) status_t {
    bump(&graph.stats.rejected_queue_full);
    bump(&graph.diag_queue_full);
    return .QueueFull;
}

fn note_at_capacity(graph: *graph_t, amount: u64) void {
    bump_by(&graph.stats.rejected_at_capacity, amount);
    bump_by(&graph.diag_at_capacity, amount);
}

fn note_generations_exhausted(graph: *graph_t, amount: u64) void {
    bump_by(&graph.stats.rejected_generation_exhausted, amount);
    bump_by(&graph.diag_generation_exhausted, amount);
}

/// Mark the Active entry and the deferred record of a window as seen in the open refresh.
///
/// The mark is written unconditionally; `refresh_begin` clears every mark, so it is observable
/// only while a refresh is open.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] window HWND whose entry and record (whichever exist) are marked.
/// @note Owner thread only; Retiring entries are never marked.
pub fn mark_seen(graph: *graph_t, window: u64) void {
    if (find_active(graph, window)) |slot| graph.entries[slot].seen = true;
    if (find_deferred(graph, window)) |slot| graph.deferred[slot].seen = true;
}

/// Result of following an owner chain through deferred records (`deferred_chain_end`).
pub const chain_end_t = struct {
    /// True when the chain hit the offered window itself or did not end within 65 steps.
    cycle: bool,
    /// HWND of the Active entry that ended the chain; 0 when the chain ended at a window with
    /// neither an Active entry nor a deferred record, or when `cycle` is true.
    terminal_window: u64,
};

/// Follow the owner chain of a candidate parent through deferred records and report where it ends.
///
/// Starting at `parent`, the chain follows each deferred record's `parent_window` (always nonzero)
/// for at most 65 steps and stops at an Active entry or at a window with no deferred record.
///
/// @param[in] graph Non-null graph; never written.
/// @param[in] window The window being offered.
/// @param[in] parent Its candidate parent HWND (nonzero).
/// @return `cycle` true when `parent == window`, when a deferred hop names `window`, or when the
///         chain does not end within 65 steps (a pre-existing cycle). Otherwise `terminal_window`
///         is the Active entry that ended the chain (0 when it ended without one). The caller uses
///         it to detect a chain that re-enters the cascade of a retiring entry, which a purely
///         deferred walk cannot see.
/// @note Owner thread only; pure read.
pub fn deferred_chain_end(graph: *const graph_t, window: u64, parent: u64) chain_end_t {
    return deferred_chain_end_masked(graph, window, parent, null);
}

/// Follow a deferred parent chain while ignoring records scheduled for invalidation.
///
/// @param[in] graph Non-null graph; never written.
/// @param[in] window The window being offered.
/// @param[in] parent Its candidate parent HWND (nonzero).
/// @param[in] drop_mask Deferred slots that are being transactionally removed, or NULL.
/// @return The same result as `deferred_chain_end`, treating masked records as absent.
/// @note Owner thread only; pure read.
fn deferred_chain_end_masked(graph: *const graph_t, window: u64, parent: u64, drop_mask: ?*const [MaxDeferred]bool) chain_end_t {
    if (parent == window) return .{ .cycle = true, .terminal_window = 0 };
    var cursor = parent;
    var steps: u32 = 0;
    while (steps <= 65) : (steps += 1) {
        if (find_active(graph, cursor) != null) return .{ .cycle = false, .terminal_window = cursor };
        const slot = find_deferred(graph, cursor) orelse return .{ .cycle = false, .terminal_window = 0 };
        if (drop_mask) |mask| {
            if (mask[slot]) return .{ .cycle = false, .terminal_window = 0 };
        }
        const next = graph.deferred[slot].record.parent_window;
        if (next == window) return .{ .cycle = true, .terminal_window = 0 };
        cursor = next;
    }
    return .{ .cycle = true, .terminal_window = 0 };
}

/// Plan all cached deferred descendants of a set of parent HWNDs for deletion.
///
/// The direct children are seeded from `roots`; descendants are found by following each selected
/// deferred record's `window`. Every slot is selected at most once, so work is bounded by
/// `MaxDeferred * MaxDeferred`. Roots themselves are not selected by this helper: callers that
/// replace or remove a deferred root drop that record explicitly.
///
/// @param[in] graph Non-null graph; never written.
/// @param[in] roots Non-null array of root HWNDs; zero values are ignored.
/// @param[out] out_mask Non-null fixed-size mask; fully written.
/// @note Owner thread only; no allocation, callbacks or graph mutation.
fn plan_deferred_descendants(graph: *const graph_t, roots: []const u64, out_mask: *[MaxDeferred]bool) void {
    out_mask.* = [_]bool{false} ** MaxDeferred;
    var queue: [MaxDeferred]u8 = undefined;
    var queue_len: usize = 0;

    for (&graph.deferred, 0..) |*slot, index| {
        if (!slot.used) continue;
        for (roots) |root| {
            if (root != 0 and slot.record.parent_window == root) {
                out_mask[index] = true;
                queue[queue_len] = @intCast(index);
                queue_len += 1;
                break;
            }
        }
    }

    var head: usize = 0;
    while (head < queue_len) : (head += 1) {
        const parent_window = graph.deferred[queue[head]].record.window;
        for (&graph.deferred, 0..) |*slot, index| {
            if (!slot.used or out_mask[index] or slot.record.parent_window != parent_window) continue;
            out_mask[index] = true;
            queue[queue_len] = @intCast(index);
            queue_len += 1;
        }
    }
}

/// Apply a previously planned deferred-descendant mask using ordinary drop accounting.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] drop_mask Non-null fixed-size mask produced before mutation.
/// @param[in,out] tally Non-null; dropped count increases once per selected used slot.
/// @note Owner thread only; no allocation or callback.
fn apply_deferred_descendants(graph: *graph_t, drop_mask: *const [MaxDeferred]bool, tally: *tally_t) void {
    for (drop_mask, 0..) |drop, slot| {
        if (drop and graph.deferred[slot].used) drop_deferred(graph, slot, tally);
    }
}

/// Follow the owner chain of a candidate parent through deferred records.
///
/// Thin wrapper over `deferred_chain_end` that keeps only the `cycle` verdict.
///
/// @param[in] graph Non-null graph; never written.
/// @param[in] window The window being offered.
/// @param[in] parent Its candidate parent HWND (nonzero).
/// @return true when `parent == window`, when the chain reaches `window`, or when it does not end
///         within 65 steps (a pre-existing cycle).
/// @note Owner thread only; pure read.
pub fn deferred_chain_cycle(graph: *const graph_t, window: u64, parent: u64) bool {
    return deferred_chain_end(graph, window, parent).cycle;
}

/// Build the persistent record for a window from validated facts.
///
/// `last_stamp` and `last_captured_ms` are set; `admit_*` stay zero (the admission step sets them);
/// `generation`, `parent_generation`, `seq` and the application key are left zero.
///
/// @param[in] facts Non-null validated snapshot.
/// @param[in] t64 Unwrapped 64-bit tick of `facts.captured_ms`.
/// @param[in] role Role code of the verdict.
/// @param[in] parent_window Parent HWND of the verdict, 0 = none.
/// @return The record value; title bytes are copied over the stated length only.
/// @note Pure; thread-safe.
pub fn record_from_facts(facts: *const facts_t, t64: u64, role: u32, parent_window: u64) record_t {
    var rec = record_init();
    rec.window = facts.window;
    rec.parent_window = parent_window;
    rec.process_start_time = facts.process_start_time;
    rec.class_hash = class_hash_of(class_bytes(facts));
    rec.last_stamp = facts.stamp;
    rec.last_captured_ms = t64;
    rec.x = facts.x;
    rec.y = facts.y;
    rec.width = facts.width;
    rec.height = facts.height;
    rec.role = role;
    rec.state = derive_state(facts);
    rec.dpi = facts.dpi;
    rec.min_width = facts.min_width;
    rec.min_height = facts.min_height;
    rec.process_id = facts.process_id;
    rec.thread_id = facts.thread_id;
    const title = title_bytes(facts);
    rec.title_length = @intCast(title.len);
    @memcpy(rec.title[0..title.len], title);
    return rec;
}

const source_kind_t = enum(u8) {
    /// The candidate is a deferred record.
    Deferred,
    /// The candidate is a retired cascade descendant that is being re-announced.
    Cascade,
};

const PrimaryParent: u8 = 0xFF;
const QueueCapacity = MaxDeferred + MaxEntries;

const candidate_t = struct {
    source: source_kind_t = .Deferred,
    index: u8 = 0,
    parent_item: u8 = PrimaryParent,
};

/// The children that one admission flushes, in admission order.
const flush_plan_t = struct {
    /// Number of admitted candidates in `items`.
    count: u32 = 0,
    /// Candidates cut off by the remaining capacity or generations.
    cut: u32 = 0,
    /// True when the slot budget (not the generation budget) bound the flush.
    cut_by_slots: bool = true,
    /// Admission order; `parent_item` indexes this array or is `PrimaryParent`.
    items: [MaxEntries]candidate_t = [_]candidate_t{.{}} ** MaxEntries,
    /// Cascade-member slots that are re-announced by this flush.
    cascade_admitted: [MaxEntries]bool = [_]bool{false} ** MaxEntries,
};

const flush_scan_t = struct {
    queue: [QueueCapacity]candidate_t = [_]candidate_t{.{}} ** QueueCapacity,
    length: u32 = 0,
    order: [MaxDeferred]u8 = [_]u8{0} ** MaxDeferred,
    order_length: u32 = 0,
    deferred_taken: [MaxDeferred]bool = [_]bool{false} ** MaxDeferred,
    cascade_taken: [MaxEntries]bool = [_]bool{false} ** MaxEntries,

    fn push_children(self: *flush_scan_t, graph: *const graph_t, cascade: ?*const cascade_t, drop_mask: *const [MaxDeferred]bool, parent_window: u64, parent_item: u8) void {
        for (self.order[0..self.order_length]) |index| {
            if (self.deferred_taken[index] or drop_mask[index]) continue;
            if (graph.deferred[index].record.parent_window != parent_window) continue;
            self.deferred_taken[index] = true;
            self.queue[self.length] = .{ .source = .Deferred, .index = index, .parent_item = parent_item };
            self.length += 1;
        }
        const members = cascade orelse return;
        var position = members.count - 1;
        while (position > 0) {
            position -= 1;
            const slot = members.slots[position];
            if (self.cascade_taken[slot]) continue;
            if (graph.entries[slot].record.parent_window != parent_window) continue;
            self.cascade_taken[slot] = true;
            self.queue[self.length] = .{ .source = .Cascade, .index = slot, .parent_item = parent_item };
            self.length += 1;
        }
    }
};

/// Plan the flush of deferred children (and re-announced cascade descendants) of a new entry.
///
/// A FIFO worklist is seeded with the deferred records whose `parent_window` equals the
/// primary window in ascending `seq`, followed by the cascade descendants (when given) whose stored
/// parent is that window in ascending original generation. Each admitted candidate appends its
/// own children to the tail. At most `budget` candidates are admitted; the rest of the worklist
/// is the cut. Records in `drop_mask` are omitted as if absent, and deferred records of the primary
/// window itself are never candidates.
fn plan_flush(graph: *const graph_t, primary_window: u64, cascade: ?*const cascade_t, drop_mask: *const [MaxDeferred]bool, budget: u32, out: *flush_plan_t) void {
    var scan = flush_scan_t{};
    for (&graph.deferred, 0..) |*slot, index| {
        if (!slot.used or drop_mask[index] or slot.record.window == primary_window) {
            scan.deferred_taken[index] = true;
            continue;
        }
        var hole = scan.order_length;
        while (hole > 0 and graph.deferred[scan.order[hole - 1]].record.seq > slot.record.seq) : (hole -= 1) {
            scan.order[hole] = scan.order[hole - 1];
        }
        scan.order[hole] = @intCast(index);
        scan.order_length += 1;
    }
    scan.push_children(graph, cascade, drop_mask, primary_window, PrimaryParent);
    var head: u32 = 0;
    while (head < scan.length) : (head += 1) {
        const candidate = scan.queue[head];
        if (out.count >= budget) {
            out.cut += 1;
            continue;
        }
        out.items[out.count] = candidate;
        out.count += 1;
        const record = switch (candidate.source) {
            .Deferred => &graph.deferred[candidate.index].record,
            .Cascade => &graph.entries[candidate.index].record,
        };
        if (candidate.source == .Cascade) out.cascade_admitted[candidate.index] = true;
        scan.push_children(graph, cascade, drop_mask, record.window, @intCast(out.count - 1));
    }
}

/// Write the planned children as Active entries, each followed by its Announce event.
fn apply_flush(graph: *graph_t, flush: *const flush_plan_t, primary_generation: u64, tally: *tally_t) void {
    var generations = [_]u64{0} ** MaxEntries;
    for (flush.items[0..flush.count], 0..) |item, position| {
        const slot = find_free_entry(graph) orelse unreachable;
        var rec: record_t = undefined;
        var seen = false;
        switch (item.source) {
            .Deferred => {
                rec = graph.deferred[item.index].record;
                seen = graph.deferred[item.index].seen;
                graph.deferred[item.index] = .{ .used = false, .seen = false, .record = record_init() };
            },
            .Cascade => {
                rec = graph.entries[item.index].record;
                seen = graph.entries[item.index].seen;
            },
        }
        graph.generations_issued += 1;
        rec.generation = graph.generations_issued;
        rec.parent_generation = if (item.parent_item == PrimaryParent) primary_generation else generations[item.parent_item];
        rec.admit_stamp = rec.last_stamp;
        rec.admit_captured_ms = rec.last_captured_ms;
        rec.seq = 0;
        rec.retire_reason = 0;
        generations[position] = rec.generation;
        graph.entries[slot] = .{ .state = .Active, .seen = seen, .record = rec };
        const ev = event_from_record(graph, &rec, EventAnnounce, ChangeMask, 0);
        if (ring_push_or_coalesce(graph, &ev)) tally.events += 1;
        bump(&graph.stats.admitted);
        tally.admitted += 1;
    }
    if (flush.cut > 0) {
        if (flush.cut_by_slots) note_at_capacity(graph, flush.cut) else note_generations_exhausted(graph, flush.cut);
    }
}

/// Re-defer the cascade descendants that were not re-announced, in ascending original generation.
///
/// Used only for a Reclassified retirement of the same incarnation (header rule): the descendants
/// are still alive and still owned by that HWND, so their stored records return to the deferred
/// set with new ages, inheriting the seen mark of the entry they replace. Each insert may evict the
/// oldest deferred record.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] cascade Non-null cascade whose members are now Retiring.
/// @param[in] admitted Per entry slot: true when the member was re-announced by the flush.
/// @param[in,out] tally Non-null; `dropped` grows on eviction.
/// @note Owner thread only; no callback; no allocation; infallible.
pub fn redefer_descendants(graph: *graph_t, cascade: *const cascade_t, admitted: *const [MaxEntries]bool, tally: *tally_t) void {
    var position = cascade.count - 1;
    while (position > 0) {
        position -= 1;
        const slot = cascade.slots[position];
        if (admitted[slot]) continue;
        const record = graph.entries[slot].record;
        defer_insert(graph, &record, graph.entries[slot].seen, tally);
    }
}

fn run_lookup(graph: *graph_t, facts: *const facts_t, key: *[MaxAppKeyBytes]u8, key_length: *u32) ?status_t {
    const callback = graph.config.app_key_lookup orelse unreachable;
    var length: u32 = 0;
    key.* = [_]u8{0} ** MaxAppKeyBytes;
    if (callback(graph.config.app_key_context, facts, key, MaxAppKeyBytes, &length) != 0) {
        bump(&graph.stats.lookup_failures);
        return .LookupFailed;
    }
    if (length == 0 or length > MaxAppKeyBytes or !app_key_valid(key[0..length])) {
        bump(&graph.stats.app_key_invalid);
        return .AppKeyInvalid;
    }
    key_length.* = length;
    return null;
}

const call_t = struct {
    graph: *graph_t,
    facts: *const facts_t,
    t64: u64,
    now: u64,
    l_slot: ?usize,
    d_slot: ?usize,
    tally: tally_t = .{},
};

/// Commit an Ok call: raise `stamp_high_water` and the tick base.
///
/// `tick_base` becomes the larger of itself and `t64`. `tick_clock_at_base` is chosen so that the
/// estimate right now, `max(tick_estimate(now), t64)`, is preserved: a tick at or beyond the old
/// estimate re-anchors the pair at `now`; a delayed older tick leaves the estimate unchanged, so
/// the estimate never moves backwards (except when the clock itself went backwards, which
/// re-anchors at `now`).
fn commit_ok(graph: *graph_t, stamp: u64, t64: u64, now: u64) void {
    const estimate = @max(tick_estimate(graph, now), t64);
    graph.stamp_high_water = @max(graph.stamp_high_water, stamp);
    graph.tick_base = @max(graph.tick_base, t64);
    graph.tick_clock_at_base = now -| (estimate - graph.tick_base);
}

fn write_observation(out: *observation_t, window: u64, tally: *const tally_t, disposition: u32, role: u32, generation: u64, retired_generation: u64, reason: u32) void {
    out.* = .{
        .window = window,
        .generation = generation,
        .retired_generation = retired_generation,
        .disposition = disposition,
        .role = role,
        .reason = reason,
        .events_queued = tally.events,
        .entries_admitted = tally.admitted,
        .entries_retired = tally.retired,
        .deferred_dropped = tally.dropped,
        .reserved0 = 0,
    };
}

fn observe_suspended(call: *call_t, out: *observation_t) status_t {
    bump(&call.graph.stats.suspended);
    commit_ok(call.graph, call.facts.stamp, call.t64, call.now);
    write_observation(out, call.facts.window, &call.tally, DispositionSuspended, 0, 0, 0, ReasonNone);
    return .Ok;
}

fn observe_not_exported(call: *call_t, reason: u32, out: *observation_t) status_t {
    const graph = call.graph;
    var retired_generation: u64 = 0;
    var disposition: u32 = DispositionExcluded;
    var cascade = cascade_t{};
    var roots: [MaxEntries + 1]u64 = undefined;
    var root_count: usize = 1;
    roots[0] = call.facts.window;
    if (call.l_slot) |slot| {
        collect_cascade(graph, slot, &cascade);
        for (cascade.slots[0..cascade.count]) |member| {
            roots[root_count] = graph.entries[member].record.window;
            root_count += 1;
        }
    }
    var drop_mask: [MaxDeferred]bool = undefined;
    plan_deferred_descendants(graph, roots[0..root_count], &drop_mask);
    if (call.l_slot) |slot| {
        if (cascade.count > ring_room(graph)) return reject_queue_full(graph);
        retired_generation = graph.entries[slot].record.generation;
        retire_cascade(graph, &cascade, reason, &call.tally);
        disposition = DispositionRetired;
    } else if (call.d_slot) |slot| {
        drop_deferred(graph, slot, &call.tally);
    }
    apply_deferred_descendants(graph, &drop_mask, &call.tally);
    commit_ok(graph, call.facts.stamp, call.t64, call.now);
    write_observation(out, call.facts.window, &call.tally, disposition, 0, 0, retired_generation, reason);
    return .Ok;
}

fn diff_mask(rec: *const record_t, facts: *const facts_t, state: u32) u32 {
    var mask: u32 = 0;
    if (rec.x != facts.x or rec.y != facts.y or rec.width != facts.width or rec.height != facts.height) mask |= ChangeRect;
    if (rec.state != state) mask |= ChangeState;
    if (rec.dpi != facts.dpi) mask |= ChangeDpi;
    if (!std_t.mem.eql(u8, rec.title[0..rec.title_length], title_bytes(facts))) mask |= ChangeTitle;
    if (rec.min_width != facts.min_width or rec.min_height != facts.min_height) mask |= ChangeMinSize;
    return mask;
}

fn observe_update(call: *call_t, slot: usize, out: *observation_t) status_t {
    const graph = call.graph;
    const facts = call.facts;
    const rec = &graph.entries[slot].record;
    const state = derive_state(facts);
    const mask = diff_mask(rec, facts, state);
    if (mask != 0 and ring_merge_target(graph, rec.window, rec.generation) == null and ring_room(graph) == 0) {
        return reject_queue_full(graph);
    }
    rec.last_stamp = facts.stamp;
    rec.last_captured_ms = call.t64;
    var disposition: u32 = DispositionUnchanged;
    if (mask != 0) {
        rec.x = facts.x;
        rec.y = facts.y;
        rec.width = facts.width;
        rec.height = facts.height;
        rec.state = state;
        rec.dpi = facts.dpi;
        rec.min_width = facts.min_width;
        rec.min_height = facts.min_height;
        const title = title_bytes(facts);
        @memset(&rec.title, 0);
        @memcpy(rec.title[0..title.len], title);
        rec.title_length = @intCast(title.len);
        const ev = event_from_record(graph, rec, EventUpdate, mask, 0);
        if (ring_push_or_coalesce(graph, &ev)) call.tally.events += 1;
        disposition = DispositionUpdated;
    }
    commit_ok(graph, facts.stamp, call.t64, call.now);
    write_observation(out, facts.window, &call.tally, disposition, rec.role, rec.generation, 0, ReasonNone);
    return .Ok;
}

const replacement_t = enum {
    /// No replacement entry or record (retire-only).
    None,
    /// Announce a new generation.
    Admit,
    /// Store a deferred record.
    Defer,
};

const key_source_t = enum {
    /// Ask `app_key_lookup`.
    Lookup,
    /// Reuse the key of the matching deferred record.
    Deferred,
    /// Reuse the key of the live entry being reclassified.
    Entry,
};

const shortfall_t = enum {
    /// No shortfall.
    None,
    /// 64 exported entries.
    AtCapacity,
    /// No generation left.
    Generations,
    /// The key lookup failed or returned an invalid key (already counted).
    Lookup,
};

const plan_t = struct {
    retire: bool = false,
    cause: u32 = ReasonNone,
    redefer: bool = false,
    cycle_forced: bool = false,
    shortfall: shortfall_t = .None,
    want: replacement_t = .None,
    key_source: key_source_t = .Lookup,
    drop_d: bool = false,
    cascade: cascade_t = .{},
    deferred_drop_mask: [MaxDeferred]bool = [_]bool{false} ** MaxDeferred,
    flush: flush_plan_t = .{},
};

fn observe_eligible(call: *call_t, verdict: eligible_t, out: *observation_t) status_t {
    const graph = call.graph;
    const facts = call.facts;
    var plan = plan_t{};

    // Stage 4a: identity of the live entry (before the cycle check), then role/parent change.
    if (call.l_slot) |slot| {
        const rec = &graph.entries[slot].record;
        if (!identity_equal(rec, facts)) {
            plan.cause = ReasonIncarnationChanged;
        } else if (verdict.role != rec.role or verdict.parent_window != rec.parent_window) {
            plan.cause = ReasonReclassified;
        } else {
            return observe_update(call, slot, out);
        }
        plan.retire = true;
        plan.redefer = plan.cause == ReasonReclassified;
        collect_cascade(graph, slot, &plan.cascade);
    }
    // Stage 4b: a deferred record of another incarnation is dropped; its key is never reused.
    if (call.d_slot) |slot| plan.drop_d = !identity_equal(&graph.deferred[slot].record, facts);
    if ((plan.retire and plan.cause == ReasonIncarnationChanged) or plan.drop_d) {
        var roots: [MaxEntries]u64 = undefined;
        var root_count: usize = 0;
        if (plan.retire and plan.cause == ReasonIncarnationChanged) {
            for (plan.cascade.slots[0..plan.cascade.count]) |member| {
                roots[root_count] = graph.entries[member].record.window;
                root_count += 1;
            }
        }
        if (plan.drop_d) {
            roots[root_count] = facts.window;
            root_count += 1;
        }
        plan_deferred_descendants(graph, roots[0..root_count], &plan.deferred_drop_mask);
    }

    // Stage 5a: cycle check, including the cascade-set rule.
    // The cascade rule follows the new parent chain through deferred records to the Active entry
    // that ends it; a terminal inside the retiring cascade (a capacity-cut deferred leftover
    // chained to a descendant) would let the replacement, or the re-deferred descendants, close a
    // deferred cycle, so it is forced to the same retire-only outcome as a direct cascade parent.
    var chain_cycle = false;
    var cascade_cycle = false;
    if (verdict.parent_window != 0) {
        const end = deferred_chain_end_masked(graph, facts.window, verdict.parent_window, &plan.deferred_drop_mask);
        chain_cycle = end.cycle;
        cascade_cycle = !chain_cycle and plan.retire and
            (cascade_has_window(graph, &plan.cascade, verdict.parent_window) or
            (end.terminal_window != 0 and cascade_has_window(graph, &plan.cascade, end.terminal_window)));
    }
    if (chain_cycle or cascade_cycle) {
        const forced = plan.retire and (plan.cause == ReasonIncarnationChanged or cascade_cycle);
        if (!forced) {
            bump(&graph.stats.rejected_cycle);
            if (call.d_slot) |slot| drop_deferred(graph, slot, &call.tally);
            apply_deferred_descendants(graph, &plan.deferred_drop_mask, &call.tally);
            return .Cycle;
        }
        plan.cycle_forced = true;
        plan.redefer = false;
    } else if (verdict.parent_window == 0 or find_active(graph, verdict.parent_window) != null) {
        plan.want = .Admit;
    } else {
        plan.want = .Defer;
    }

    // Stage 5b: capacity and generations for the primary admission.
    const free: u32 = MaxEntries - exported_count(graph);
    const generations_left: u64 = std_t.math.maxInt(u64) - graph.generations_issued;
    if (plan.want == .Admit and (free == 0 or generations_left == 0)) {
        const by_slots = free == 0;
        if (!plan.retire and !plan.drop_d) {
            if (by_slots) {
                note_at_capacity(graph, 1);
                return .AtCapacity;
            }
            note_generations_exhausted(graph, 1);
            return .GenerationExhausted;
        }
        plan.shortfall = if (by_slots) .AtCapacity else .Generations;
        if (!plan.retire and plan.drop_d) plan.cause = ReasonIncarnationChanged;
        plan.want = .None;
    }
    if (plan.want == .Admit) {
        const slots_after: u32 = free - 1;
        const generations_after: u64 = generations_left - 1;
        plan.flush.cut_by_slots = slots_after <= generations_after;
        const budget: u32 = @intCast(@min(@as(u64, slots_after), generations_after));
        plan_flush(graph, facts.window, if (plan.redefer) &plan.cascade else null, &plan.deferred_drop_mask, budget, &plan.flush);
    }

    // Stage 5c: room for the whole plan in the event ring (QueueFull changes nothing).
    var needed: u32 = if (plan.retire) plan.cascade.count else 0;
    if (plan.want == .Admit) needed += 1 + plan.flush.count;
    if (needed > ring_room(graph)) return reject_queue_full(graph);

    // Stage 5d: the single key lookup (or a stored key).
    var key = [_]u8{0} ** MaxAppKeyBytes;
    var key_length: u32 = 0;
    if (plan.want != .None) {
        if (plan.cause == ReasonReclassified) {
            plan.key_source = .Entry;
        } else if (call.d_slot != null and !plan.drop_d) {
            plan.key_source = .Deferred;
        }
        switch (plan.key_source) {
            .Entry => {
                const rec = &graph.entries[call.l_slot orelse unreachable].record;
                key_length = rec.app_key_length;
                key = rec.app_key;
            },
            .Deferred => {
                const rec = &graph.deferred[call.d_slot orelse unreachable].record;
                key_length = rec.app_key_length;
                key = rec.app_key;
            },
            .Lookup => if (run_lookup(graph, facts, &key, &key_length)) |failure| {
                if (!plan.retire and !plan.drop_d) return failure;
                plan.shortfall = .Lookup;
                if (!plan.retire and plan.drop_d) plan.cause = ReasonIncarnationChanged;
                plan.want = .None;
                plan.flush = .{};
            },
        }
    }
    return apply_eligible(call, &plan, verdict, &key, key_length, out);
}

fn apply_eligible(call: *call_t, plan: *plan_t, verdict: eligible_t, key: *const [MaxAppKeyBytes]u8, key_length: u32, out: *observation_t) status_t {
    const graph = call.graph;
    const facts = call.facts;
    const tally = &call.tally;
    if (plan.cycle_forced) bump(&graph.stats.rejected_cycle);
    switch (plan.shortfall) {
        .AtCapacity => note_at_capacity(graph, 1),
        .Generations => note_generations_exhausted(graph, 1),
        .Lookup, .None => {},
    }
    var retired_generation: u64 = 0;
    if (plan.retire) {
        if (plan.cause == ReasonReclassified) bump(&graph.stats.reclassified);
        retired_generation = graph.entries[call.l_slot orelse unreachable].record.generation;
        retire_cascade(graph, &plan.cascade, plan.cause, tally);
    }
    if (plan.drop_d) drop_deferred(graph, call.d_slot orelse unreachable, tally);
    apply_deferred_descendants(graph, &plan.deferred_drop_mask, tally);

    var generation: u64 = 0;
    switch (plan.want) {
        .Admit => {
            var rec = record_from_facts(facts, call.t64, verdict.role, verdict.parent_window);
            rec.app_key = key.*;
            rec.app_key_length = key_length;
            rec.admit_stamp = facts.stamp;
            rec.admit_captured_ms = call.t64;
            if (verdict.parent_window != 0) {
                const parent = find_active(graph, verdict.parent_window) orelse unreachable;
                rec.parent_generation = graph.entries[parent].record.generation;
            }
            graph.generations_issued += 1;
            generation = graph.generations_issued;
            rec.generation = generation;
            const slot = find_free_entry(graph) orelse unreachable;
            graph.entries[slot] = .{ .state = .Active, .seen = true, .record = rec };
            if (call.d_slot) |d_slot| {
                if (!plan.drop_d) graph.deferred[d_slot] = .{ .used = false, .seen = false, .record = record_init() };
            }
            const ev = event_from_record(graph, &rec, EventAnnounce, ChangeMask, 0);
            if (ring_push_or_coalesce(graph, &ev)) tally.events += 1;
            bump(&graph.stats.admitted);
            tally.admitted += 1;
            apply_flush(graph, &plan.flush, generation, tally);
        },
        .Defer => {
            var rec = record_from_facts(facts, call.t64, verdict.role, verdict.parent_window);
            rec.app_key = key.*;
            rec.app_key_length = key_length;
            if (call.d_slot != null and !plan.drop_d) {
                const slot = call.d_slot orelse unreachable;
                rec.seq = graph.deferred[slot].record.seq;
                graph.deferred[slot] = .{ .used = true, .seen = true, .record = rec };
            } else {
                defer_insert(graph, &rec, true, tally);
            }
        },
        .None => {},
    }
    if (plan.redefer) redefer_descendants(graph, &plan.cascade, &plan.flush.cascade_admitted, tally);

    commit_ok(graph, facts.stamp, call.t64, call.now);
    var disposition: u32 = if (!plan.retire and plan.drop_d and plan.cause == ReasonIncarnationChanged)
        DispositionExcluded
    else
        DispositionRetired;
    var reason: u32 = plan.cause;
    var role: u32 = 0;
    switch (plan.want) {
        .Admit => {
            disposition = if (plan.cause == ReasonReclassified) DispositionReclassified else DispositionAdmitted;
            role = verdict.role;
        },
        .Defer => {
            disposition = DispositionDeferred;
            role = verdict.role;
            if (!plan.retire) reason = ReasonParentPending;
        },
        .None => {},
    }
    write_observation(out, facts.window, tally, disposition, role, generation, retired_generation, reason);
    return .Ok;
}

/// Offer one fact snapshot after the common guards have passed and `busy` is set.
///
/// This is the body shared by `observe` and the refresh engine's offer call. Stage order: facts
/// validation (InvalidFacts); StaleFacts against the stamp of the Active entry or else the
/// deferred record; classification; Suspended; NotExported (retire the cascade or drop the
/// deferred record); for Eligible facts: identity check of the live entry BEFORE the cycle check,
/// role/parent change (Reclassified), deferred-record identity check, cycle check (including the
/// cascade-set rule), capacity and generations, ring room for the whole plan (QueueFull), the single
/// lookup, then the infallible apply. Every Ok marks the window's entry and record seen and advances
/// `stamp_high_water` and `tick_base` (with `tick_clock_at_base`). Valid facts read the graph clock
/// once, before the stale check, to unwrap `captured_ms` against `tick_estimate`.
///
/// @param[in,out] graph Non-null graph with `busy` set by `enter`.
/// @param[in] facts Non-null stack-local copy of the caller's facts; it is the only copy used and
///                  the one passed to the lookup callback.
/// @param[out] out Non-null; fully written on Ok, unchanged otherwise.
/// @return Ok; InvalidFacts; StaleFacts; Cycle; AtCapacity; GenerationExhausted; QueueFull;
///         LookupFailed; AppKeyInvalid. A shortfall met while an Active retirement is forced is not
///         returned; a Deferred-only identity mismatch instead drops its old subtree and returns Ok
///         Excluded with reason IncarnationChanged when replacement capacity, generation or lookup fails.
/// @note Owner thread only; the callbacks are `app_key_lookup` and the clock (`now_ms`, read once per
///       call with valid facts); no allocation.
pub fn observe_locked(graph: *graph_t, facts: *const facts_t, out: *observation_t) status_t {
    if (!validate_facts(facts)) {
        bump(&graph.stats.rejected_invalid_facts);
        return .InvalidFacts;
    }
    const now = clock_now_ms(graph);
    var call = call_t{
        .graph = graph,
        .facts = facts,
        .now = now,
        .t64 = unwrap_tick(tick_estimate(graph, now), facts.captured_ms),
        .l_slot = find_active(graph, facts.window),
        .d_slot = find_deferred(graph, facts.window),
    };
    var reference: u64 = 0;
    if (call.l_slot) |slot| {
        reference = graph.entries[slot].record.last_stamp;
    } else if (call.d_slot) |slot| {
        reference = graph.deferred[slot].record.last_stamp;
    }
    if (facts.stamp < reference) {
        bump(&graph.stats.rejected_stale_facts);
        return .StaleFacts;
    }
    const status = switch (classify(facts, graph.config.uac_mode)) {
        .Suspended => observe_suspended(&call, out),
        .NotExported => |reason| observe_not_exported(&call, reason, out),
        .Eligible => |verdict| observe_eligible(&call, verdict, out),
    };
    if (status == .Ok) mark_seen(graph, facts.window);
    return status;
}

/// Offer one fact snapshot: classify, then admit, update, defer or retire.
///
/// Applies the common call rules (`enter`), copies the caller's facts to a stack local and runs
/// `observe_locked` on the copy; see that function for the stage order and the header for the
/// outcome table.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @param[in] facts Non-null borrowed snapshot; copied at entry, never retained or modified.
/// @param[out] out Non-null; fully written on Ok, unchanged otherwise.
/// @return Ok; WrongThread, Reentrant or StaleEpoch per `enter`; the statuses of `observe_locked`.
/// @note Owner thread only; the lookup callback receives the stack copy; the clock callback is read
///       once for valid facts; no allocation.
pub fn observe(graph: *graph_t, epoch: u64, facts: *const facts_t, out: *observation_t) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    const local = facts.*;
    return observe_locked(graph, &local, out);
}

/// Apply a window-destroyed hint without trusting it blindly.
///
/// With e64 the event time unwrapped against `tick_estimate` (`tick_base` advanced by the graph-clock
/// time elapsed since it was set; the clock is read once per call): the Active entry of the window retires
/// (reason Destroyed, descendants ParentRetired, never re-deferred) iff
/// `hint_stamp > admit_stamp` and not `e64 < admit_captured_ms` (plain 64-bit compare; equal
/// ticks count as destroyed). The deferred record of the window is dropped iff
/// `hint_stamp > last_stamp` and not `e64 < last_captured_ms`. On an accepted match, deferred
/// descendants waiting on every HWND in the retiring Active cascade, or on the removed Deferred
/// record, are dropped transitively. Retiring entries are never touched. Ring room for all RetireIntent
/// events is proven before mutation (QueueFull changes nothing).
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @param[in] window Nonzero HWND.
/// @param[in] hint_stamp Nonzero observer stamp of the hint.
/// @param[in] event_time_ms Raw 32-bit tick of the destroy event.
/// @param[out] out Non-null; written fully on Ok: Retired when an entry moved to Retiring, else
///             Unchanged (generation and role describe a still-live entry, deferred_dropped may be 1).
/// @return Ok; InvalidArgument for window 0 or stamp 0; QueueFull; WrongThread, Reentrant or
///         StaleEpoch per `enter`.
/// @note Owner thread only; the only callback is the clock (`now_ms`, read once after the argument
///       check); no allocation. `stamp_high_water` and `tick_base` advance on Ok.
pub fn note_destroyed(graph: *graph_t, epoch: u64, window: u64, hint_stamp: u64, event_time_ms: u32, out: *observation_t) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    if (window == 0 or hint_stamp == 0) return .InvalidArgument;
    const now = clock_now_ms(graph);
    const e64 = unwrap_tick(tick_estimate(graph, now), event_time_ms);
    var tally = tally_t{};
    var cascade = cascade_t{};
    var retire_slot: ?usize = null;
    var deferred_drop_slot: ?usize = null;
    const live = find_active(graph, window);
    if (live) |slot| {
        const rec = &graph.entries[slot].record;
        if (hint_stamp > rec.admit_stamp and !(e64 < rec.admit_captured_ms)) {
            retire_slot = slot;
            collect_cascade(graph, slot, &cascade);
        }
    }
    if (find_deferred(graph, window)) |slot| {
        const rec = &graph.deferred[slot].record;
        if (hint_stamp > rec.last_stamp and !(e64 < rec.last_captured_ms)) deferred_drop_slot = slot;
    }
    var roots: [MaxEntries]u64 = undefined;
    var root_count: usize = 0;
    if (retire_slot != null) {
        for (cascade.slots[0..cascade.count]) |member| {
            roots[root_count] = graph.entries[member].record.window;
            root_count += 1;
        }
    }
    if (deferred_drop_slot != null) {
        roots[root_count] = window;
        root_count += 1;
    }
    var drop_mask = [_]bool{false} ** MaxDeferred;
    if (root_count > 0) plan_deferred_descendants(graph, roots[0..root_count], &drop_mask);
    if (cascade.count > ring_room(graph)) return reject_queue_full(graph);
    var retired_generation: u64 = 0;
    if (retire_slot) |slot| {
        retired_generation = graph.entries[slot].record.generation;
        retire_cascade(graph, &cascade, ReasonDestroyed, &tally);
    }
    if (deferred_drop_slot) |slot| drop_deferred(graph, slot, &tally);
    apply_deferred_descendants(graph, &drop_mask, &tally);
    commit_ok(graph, hint_stamp, e64, now);
    if (retire_slot != null) {
        write_observation(out, window, &tally, DispositionRetired, 0, 0, retired_generation, ReasonDestroyed);
    } else if (live) |slot| {
        const rec = &graph.entries[slot].record;
        write_observation(out, window, &tally, DispositionUnchanged, rec.role, rec.generation, 0, ReasonNone);
    } else {
        write_observation(out, window, &tally, DispositionUnchanged, 0, 0, 0, ReasonNone);
    }
    return .Ok;
}

// ---------------------------------------------------------------------------------------------
// Refresh engine (TODO #10, layer 2b2): refresh_begin/offer/keep/unseen/commit/abort, invalidate_all.
//
// A refresh is a graph-wide mark-and-sweep. `refresh_begin` clears every seen mark; offers, keeps and
// successful observes set marks; `refresh_commit` retires the Active entries and drops the deferred
// records that are still unmarked. The same shape as the planning engine applies: guards, read-only
// plan, ring-room pre-check (QueueFull changes nothing), then an infallible apply phase.
// ---------------------------------------------------------------------------------------------

/// List the Active entry slots in ascending generation order (parents before their children).
///
/// An insertion sort over at most 64 slots; generations are unique (invariant I-3), so the order is total.
///
/// @param[in] graph Non-null graph; never written.
/// @param[out] order Non-null; the first return-value elements are slot indices of Active entries,
///             ascending by generation; the remaining elements are unchanged.
/// @return Number of Active entries, 0..64.
/// @note Owner thread only; pure read; no allocation.
pub fn active_by_generation(graph: *const graph_t, order: *[MaxEntries]u8) u32 {
    var total: u32 = 0;
    for (&graph.entries, 0..) |*entry, index| {
        if (entry.state != .Active) continue;
        const generation = entry.record.generation;
        var position = total;
        while (position > 0 and graph.entries[order[position - 1]].record.generation > generation) : (position -= 1) {
            order[position] = order[position - 1];
        }
        order[position] = @intCast(index);
        total += 1;
    }
    return total;
}

/// Open a refresh: clear every seen mark and start collecting marks.
///
/// All 64 entry slots and all 64 deferred slots get `seen = false`, then `refresh_open` is set. When a
/// refresh is already open nothing changes: its marks and its open state are kept and it continues.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @return Ok; RefreshActive when a refresh is already open; WrongThread, Reentrant or StaleEpoch per `enter`.
/// @note Owner thread only; no callback; no allocation; queues no event.
pub fn refresh_begin(graph: *graph_t, epoch: u64) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    if (graph.refresh_open) return .RefreshActive;
    for (&graph.entries) |*entry| entry.seen = false;
    for (&graph.deferred) |*slot| slot.seen = false;
    graph.refresh_open = true;
    return .Ok;
}

/// Offer one snapshot inside the open refresh: `observe` plus a seen mark for every outcome.
///
/// After the guards and the NoRefresh check the facts are copied to a stack local, the window of the
/// copy (when nonzero) is marked seen on its Active entry and its deferred record, and `observe_locked`
/// runs on the copy. The mark is written BEFORE the plan and survives every failure status (InvalidFacts,
/// StaleFacts, Cycle, AtCapacity, QueueFull, LookupFailed, ...): an offered window is alive for this
/// refresh, it is merely not updated. Entries and records the call creates for the window are born seen.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @param[in] facts Non-null borrowed snapshot; copied at entry, never retained or modified.
/// @param[out] out Non-null; fully written on Ok, unchanged otherwise.
/// @return As `observe`, plus NoRefresh when no refresh is open (checked before the facts are validated).
/// @note Owner thread only; the only callback is `app_key_lookup`, which receives the stack copy; no allocation.
pub fn refresh_offer(graph: *graph_t, epoch: u64, facts: *const facts_t, out: *observation_t) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    if (!graph.refresh_open) return .NoRefresh;
    const local = facts.*;
    if (local.window != 0) mark_seen(graph, local.window);
    return observe_locked(graph, &local, out);
}

/// Mark a window seen without offering facts (a window whose native query failed).
///
/// Marks the Active entry and the deferred record of `window`, whichever exist. Retiring entries carry
/// no mark. Queues no event and changes no entry, record, stamp or tick.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @param[in] window Nonzero HWND.
/// @return Ok when an Active entry or a deferred record exists (and is now marked); None when neither
///         exists (nothing marked); InvalidArgument for window 0; NoRefresh when no refresh is open;
///         WrongThread, Reentrant or StaleEpoch per `enter`. The argument check precedes NoRefresh.
/// @note Owner thread only; no callback; no allocation; only a seen mark changes.
pub fn refresh_keep(graph: *graph_t, epoch: u64, window: u64) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    if (window == 0) return .InvalidArgument;
    if (!graph.refresh_open) return .NoRefresh;
    const live = find_active(graph, window);
    const deferred = find_deferred(graph, window);
    if (live == null and deferred == null) return .None;
    mark_seen(graph, window);
    return .Ok;
}

/// List the Active windows that are still unseen in the open refresh.
///
/// Exactly the entries `refresh_commit` would retire with reason RefreshUnseen if it ran now: every
/// Active entry whose mark is clear, in ascending generation order. Seen descendants of those entries
/// retire with them as ParentRetired and are not listed; Retiring entries and deferred records never
/// are. A value copy; no mark changes.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @param[out] windows Caller array of `capacity` HWNDs; may be null only when `capacity` is 0. Up to
///             `capacity` elements are written on Ok; the rest is untouched.
/// @param[in] capacity Elements available in `windows`.
/// @param[out] out_count Non-null; on Ok receives the total number of unseen Active windows even when it
///             exceeds `capacity`. Unchanged on every other status.
/// @return Ok; InvalidArgument for null `windows` with `capacity` above 0; NoRefresh when no refresh is
///         open; WrongThread, Reentrant or StaleEpoch per `enter`. The argument check precedes NoRefresh.
/// @note Owner thread only; no callback; no allocation; no state change.
pub fn refresh_unseen(graph: *graph_t, epoch: u64, windows: ?[*]u64, capacity: u32, out_count: *u32) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    if (windows == null and capacity > 0) return .InvalidArgument;
    if (!graph.refresh_open) return .NoRefresh;
    var order = [_]u8{0} ** MaxEntries;
    const active = active_by_generation(graph, &order);
    var total: u32 = 0;
    for (order[0..active]) |slot| {
        if (graph.entries[slot].seen) continue;
        if (total < capacity) windows.?[total] = graph.entries[slot].record.window;
        total += 1;
    }
    out_count.* = total;
    return .Ok;
}

/// Close the refresh: retire every unseen Active entry, drop every unseen deferred record and
/// deferred descendants of doomed Active entries.
///
/// Plan (read-only): the victims are every Active entry whose mark is clear plus all descendants of
/// those (a seen child cannot outlive its retiring parent). The ring must hold one RetireIntent per
/// victim, otherwise QueueFull is returned with nothing changed and the refresh stays open. Apply: the
/// unseen entries are visited in descending generation; each one still Active retires through
/// `retire_cascade` with reason RefreshUnseen, taking its still-Active descendants along with reason
/// ParentRetired (those descendants are seen ones, since every unseen descendant has a higher
/// generation and was visited first). The event order is therefore children first. Nothing is
/// re-deferred. Then every unseen deferred record and each selected deferred descendant is dropped
/// once (`deferred_dropped`), `refresh_open` is
/// cleared and `refresh_commits` is bumped.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @param[out] retired_out Nullable; on Ok receives the number of entries moved to Retiring.
/// @return Ok; NoRefresh when none is open; QueueFull (nothing changed, refresh still open);
///         WrongThread, Reentrant or StaleEpoch per `enter`.
/// @note Owner thread only; no callback; no allocation.
pub fn refresh_commit(graph: *graph_t, epoch: u64, retired_out: ?*u32) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    if (!graph.refresh_open) return .NoRefresh;
    var order = [_]u8{0} ** MaxEntries;
    const active = active_by_generation(graph, &order);
    var doomed = [_]bool{false} ** MaxEntries;
    var cascade = cascade_t{};
    for (order[0..active]) |slot| {
        if (graph.entries[slot].seen) continue;
        collect_cascade(graph, slot, &cascade);
        for (cascade.slots[0..cascade.count]) |member| doomed[member] = true;
    }
    var roots: [MaxEntries]u64 = undefined;
    var root_count: usize = 0;
    for (order[0..active]) |slot| {
        if (!doomed[slot]) continue;
        roots[root_count] = graph.entries[slot].record.window;
        root_count += 1;
    }
    var drop_mask = [_]bool{false} ** MaxDeferred;
    if (root_count > 0) plan_deferred_descendants(graph, roots[0..root_count], &drop_mask);
    var needed: u32 = 0;
    for (doomed) |flag| {
        if (flag) needed += 1;
    }
    if (needed > ring_room(graph)) return reject_queue_full(graph);
    var tally = tally_t{};
    var position = active;
    while (position > 0) {
        position -= 1;
        const slot = order[position];
        if (graph.entries[slot].state != .Active or graph.entries[slot].seen) continue;
        collect_cascade(graph, slot, &cascade);
        retire_cascade(graph, &cascade, ReasonRefreshUnseen, &tally);
    }
    for (&graph.deferred, 0..) |*record, index| {
        if (record.used and (!record.seen or drop_mask[index])) drop_deferred(graph, index, &tally);
    }
    graph.refresh_open = false;
    bump(&graph.stats.refresh_commits);
    if (retired_out) |out| out.* = tally.retired;
    return .Ok;
}

/// Close the refresh without retiring anything; the seen marks are meaningless afterwards.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @return Ok (`refresh_open` cleared, `refresh_aborts` bumped); NoRefresh when none is open;
///         WrongThread, Reentrant or StaleEpoch per `enter`.
/// @note Owner thread only; no callback; no allocation; queues no event.
pub fn refresh_abort(graph: *graph_t, epoch: u64) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    if (!graph.refresh_open) return .NoRefresh;
    graph.refresh_open = false;
    bump(&graph.stats.refresh_aborts);
    return .Ok;
}

/// Retire every Active entry and clear all deferred records (recovery after lost hints).
///
/// Plan: one RetireIntent per Active entry must fit in the ring, otherwise QueueFull with nothing
/// changed (an open refresh stays open). Apply: entries retire in descending generation (children
/// first), each with reason Invalidated (not ParentRetired: every entry is a victim of the same
/// cause); nothing is re-deferred; every deferred record is dropped (`deferred_dropped`); an open
/// refresh is aborted (`refresh_aborts`). Retiring entries already in quarantine are unchanged.
///
/// @param[in,out] graph Non-null graph.
/// @param[in] epoch Captured session epoch.
/// @param[out] retired_out Nullable; on Ok receives the number of entries moved to Retiring.
/// @return Ok; QueueFull (nothing changed); WrongThread, Reentrant or StaleEpoch per `enter`.
/// @note Owner thread only; no callback; no allocation.
pub fn invalidate_all(graph: *graph_t, epoch: u64, retired_out: ?*u32) status_t {
    if (enter(graph, epoch)) |status| return status;
    defer leave(graph);
    var order = [_]u8{0} ** MaxEntries;
    const active = active_by_generation(graph, &order);
    if (active > ring_room(graph)) return reject_queue_full(graph);
    var tally = tally_t{};
    var cascade = cascade_t{};
    var position = active;
    while (position > 0) {
        position -= 1;
        cascade.slots[0] = order[position];
        cascade.count = 1;
        retire_cascade(graph, &cascade, ReasonInvalidated, &tally);
    }
    for (&graph.deferred, 0..) |*record, index| {
        if (record.used) drop_deferred(graph, index, &tally);
    }
    if (graph.refresh_open) {
        graph.refresh_open = false;
        bump(&graph.stats.refresh_aborts);
    }
    if (retired_out) |out| out.* = tally.retired;
    return .Ok;
}

// Exported surface: see window_graph_abi.zig.
// Test-only fixtures.

const testing_t = std_t.testing;

const OwnerWindow: u64 = 0x20002;
const OwnerRootWindow: u64 = 0x20001;
const ChildRootWindow: u64 = 0x99999;
// WS_OVERLAPPEDWINDOW: caption, sysmenu, thickframe, minimize and maximize boxes.
const DefaultStyle: u32 = 0x00CF0000;
const BorderOnly: u32 = 0x00800000;
const DialogFrameOnly: u32 = 0x00400000;

fn call_validate(facts: *const facts_t) bool {
    return @call(.never_inline, validate_facts, .{facts});
}

fn call_classify(facts: *const facts_t, uac_mode: u32) verdict_t {
    return @call(.never_inline, classify, .{ facts, uac_mode });
}

fn call_utf8(bytes: []const u8) bool {
    return @call(.never_inline, utf8_valid_no_nul, .{bytes});
}

fn call_app_key(bytes: []const u8) bool {
    return @call(.never_inline, app_key_valid, .{bytes});
}

fn call_hash(bytes: []const u8) u64 {
    return @call(.never_inline, class_hash_of, .{bytes});
}

fn call_eql_fold(a: []const u8, b: []const u8) bool {
    return @call(.never_inline, ascii_eql_ignore_case, .{ a, b });
}

fn call_unwrap(base: u64, t32: u32) u64 {
    return @call(.never_inline, unwrap_tick, .{ base, t32 });
}

fn call_state(facts: *const facts_t) u32 {
    return @call(.never_inline, derive_state, .{facts});
}

fn call_identity(rec: *const record_t, facts: *const facts_t) bool {
    return @call(.never_inline, identity_equal, .{ rec, facts });
}

fn call_bump(counter: *u64) void {
    @call(.never_inline, bump, .{counter});
}

fn call_record_init() record_t {
    return @call(.never_inline, record_init, .{});
}

fn call_class_bytes(facts: *const facts_t) []const u8 {
    return @call(.never_inline, class_bytes, .{facts});
}

fn call_image_bytes(facts: *const facts_t) []const u8 {
    return @call(.never_inline, image_bytes, .{facts});
}

fn call_title_bytes(facts: *const facts_t) []const u8 {
    return @call(.never_inline, title_bytes, .{facts});
}

fn set_text(dest: []u8, length: *u32, text: []const u8) void {
    @memset(dest, 0);
    @memcpy(dest[0..text.len], text);
    length.* = @intCast(text.len);
}

fn set_class(facts: *facts_t, text: []const u8) void {
    set_text(&facts.class_name, &facts.class_length, text);
}

fn set_image(facts: *facts_t, text: []const u8) void {
    set_text(&facts.image_name, &facts.image_length, text);
    if (text.len == 0) facts.flags |= FactImageUnknown else facts.flags &= ~FactImageUnknown;
}

fn set_title(facts: *facts_t, text: []const u8) void {
    set_text(&facts.title, &facts.title_length, text);
}

/// Zeroed, valid top-level notepad window on the input desktop.
fn fixture_facts() facts_t {
    var facts = std_t.mem.zeroes(facts_t);
    facts.window = 0x10001;
    facts.root = 0x10001;
    facts.stamp = 1;
    facts.style = DefaultStyle;
    facts.flags = FactVisible | FactEnabled | FactOnInputDesktop;
    facts.process_id = 4242;
    facts.thread_id = 4243;
    facts.process_start_time = 133_000_000_000_000_000;
    facts.captured_ms = 1000;
    facts.x = 10;
    facts.y = -20;
    facts.width = 640;
    facts.height = 480;
    facts.dpi = 96;
    set_class(&facts, "Notepad");
    set_image(&facts, "notepad.exe");
    set_title(&facts, "Untitled - Notepad");
    return facts;
}

fn eligible(role: u32, parent_window: u64) verdict_t {
    return .{ .Eligible = .{ .role = role, .parent_window = parent_window } };
}

fn excluded(reason: u32) verdict_t {
    return .{ .NotExported = reason };
}

const row_t = struct {
    name: []const u8,
    expect: verdict_t,
    class: []const u8 = "Notepad",
    image: []const u8 = "notepad.exe",
    style: u32 = DefaultStyle,
    ex_style: u32 = 0,
    flags_set: u32 = 0,
    flags_clear: u32 = 0,
    owned: bool = false,
    child: bool = false,
    width: u32 = 640,
    height: u32 = 480,
    alpha: u8 = 255,
    uac: u32 = UacWindow,
};

fn row_facts(row: row_t) facts_t {
    var facts = fixture_facts();
    set_class(&facts, row.class);
    set_image(&facts, row.image);
    facts.style = row.style;
    facts.ex_style = row.ex_style;
    facts.flags |= row.flags_set;
    facts.flags &= ~row.flags_clear;
    if (row.owned) {
        facts.owner = OwnerWindow;
        facts.owner_root = OwnerRootWindow;
    }
    if (row.child) facts.root = ChildRootWindow;
    facts.width = row.width;
    facts.height = row.height;
    facts.alpha = row.alpha;
    return facts;
}

const PopupNoCaption: u32 = StylePopup;
const AgentPreeditFlags: u32 = FactAgentOwned | FactAgentPreedit;
const LayeredKnown: u32 = FactLayered | FactLayeredAlphaKnown;
const CoreWindowName = "Windows.UI.Core.CoreWindow";
const UacDummyName = "$$$Secure UAP Dummy Window Class For Interim Dialog";
const UacCredentialName = "Credential Dialog Xaml Host";

const Matrix = [_]row_t{
    // Step 0.
    .{ .name = "plain top-level", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "off input desktop is suspended", .flags_clear = FactOnInputDesktop, .expect = .Suspended },
    .{ .name = "off input desktop beats invisible", .flags_clear = FactOnInputDesktop | FactVisible, .expect = .Suspended },
    .{ .name = "off input desktop beats excluded class", .class = "Progman", .flags_clear = FactOnInputDesktop, .expect = .Suspended },
    .{ .name = "invisible", .flags_clear = FactVisible, .expect = excluded(ReasonNotVisible) },
    .{ .name = "invisible beats excluded class", .class = "Progman", .flags_clear = FactVisible, .expect = excluded(ReasonNotVisible) },
    .{ .name = "invisible beats agent", .flags_set = FactAgentOwned, .flags_clear = FactVisible, .expect = excluded(ReasonNotVisible) },
    .{ .name = "disabled visible window is exported", .flags_clear = FactEnabled, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "iconic window is exported", .flags_set = FactIconic, .expect = eligible(RoleToplevel, 0) },
    // Step 1: excluded classes (the full per-class matrix is in its own test).
    .{ .name = "tray", .class = "Shell_TrayWnd", .expect = excluded(ReasonExcludedClass) },
    .{ .name = "tray upper case", .class = "SHELL_TRAYWND", .expect = excluded(ReasonExcludedClass) },
    .{ .name = "tray lower case", .class = "shell_traywnd", .expect = excluded(ReasonExcludedClass) },
    .{ .name = "tray one byte longer", .class = "Shell_TrayWnd2", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "tray one byte shorter", .class = "Shell_TrayWn", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "truncated tray class matches nothing", .class = "Shell_TrayWnd", .flags_set = FactClassTruncated, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "excluded class beats child", .class = "Progman", .child = true, .expect = excluded(ReasonExcludedClass) },
    .{ .name = "excluded class beats agent", .class = "IME", .flags_set = FactAgentOwned, .expect = excluded(ReasonExcludedClass) },
    .{ .name = "excluded class beats preedit", .class = "IME", .flags_set = AgentPreeditFlags, .expect = excluded(ReasonExcludedClass) },
    .{ .name = "excluded class beats consent rules", .class = "WorkerW", .image = "consent.exe", .expect = excluded(ReasonExcludedClass) },
    // Step 1: CoreWindow.
    .{ .name = "core window shell experience host", .class = CoreWindowName, .image = "ShellExperienceHost.exe", .expect = excluded(ReasonExcludedCoreWindow) },
    .{ .name = "core window start menu host", .class = CoreWindowName, .image = "StartMenuExperienceHost.exe", .expect = excluded(ReasonExcludedCoreWindow) },
    .{ .name = "core window search host", .class = CoreWindowName, .image = "SearchHost.exe", .expect = excluded(ReasonExcludedCoreWindow) },
    .{ .name = "core window search host upper case", .class = CoreWindowName, .image = "SEARCHHOST.EXE", .expect = excluded(ReasonExcludedCoreWindow) },
    .{ .name = "core window class lower case", .class = "windows.ui.core.corewindow", .image = "searchhost.exe", .expect = excluded(ReasonExcludedCoreWindow) },
    .{ .name = "core window foreign owner", .class = CoreWindowName, .image = "calc.exe", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "core window unknown image", .class = CoreWindowName, .image = "", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "core window image with suffix", .class = CoreWindowName, .image = "SearchHost.exe.bak", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "core window image prefix only", .class = CoreWindowName, .image = "SearchHost", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "core window truncated class", .class = CoreWindowName, .image = "SearchHost.exe", .flags_set = FactClassTruncated, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "shell host image with other class", .image = "ShellExperienceHost.exe", .expect = eligible(RoleToplevel, 0) },
    // Step 1: agent.
    .{ .name = "agent window excluded", .flags_set = FactAgentOwned, .expect = excluded(ReasonExcludedAgent) },
    .{ .name = "agent window excluded even owned", .flags_set = FactAgentOwned, .owned = true, .expect = excluded(ReasonExcludedAgent) },
    .{ .name = "preedit role", .flags_set = AgentPreeditFlags, .expect = eligible(RoleImePreedit, 0) },
    .{ .name = "preedit skips child step", .flags_set = AgentPreeditFlags, .child = true, .expect = eligible(RoleImePreedit, 0) },
    .{ .name = "preedit skips owner and menu steps", .flags_set = AgentPreeditFlags, .style = PopupNoCaption, .owned = true, .expect = eligible(RoleImePreedit, OwnerRootWindow) },
    .{ .name = "preedit ownerless has no parent", .flags_set = AgentPreeditFlags, .expect = eligible(RoleImePreedit, 0) },
    .{ .name = "preedit with hidden owner has no parent", .flags_set = AgentPreeditFlags | FactOwnerHidden, .owned = true, .expect = eligible(RoleImePreedit, 0) },
    .{ .name = "preedit skips tooltip step", .flags_set = AgentPreeditFlags, .ex_style = ExStyleNoActivate, .expect = eligible(RoleImePreedit, 0) },
    .{ .name = "preedit still hits zero width", .flags_set = AgentPreeditFlags, .width = 0, .expect = excluded(ReasonZeroArea) },
    .{ .name = "preedit still hits zero height", .flags_set = AgentPreeditFlags, .height = 0, .expect = excluded(ReasonZeroArea) },
    .{ .name = "preedit still hits transparency", .flags_set = AgentPreeditFlags | LayeredKnown, .alpha = 0, .expect = excluded(ReasonTransparent) },
    .{ .name = "preedit with opaque alpha", .flags_set = AgentPreeditFlags | LayeredKnown, .alpha = 1, .expect = eligible(RoleImePreedit, 0) },
    .{ .name = "agent consent lookalike is agent-excluded", .class = UacCredentialName, .image = "consent.exe", .flags_set = FactAgentOwned, .expect = excluded(ReasonExcludedAgent) },
    // Step 2: consent.exe.
    .{ .name = "dummy in window mode", .class = UacDummyName, .image = "consent.exe", .uac = UacWindow, .expect = excluded(ReasonExcludedUacDummy) },
    .{ .name = "dummy in auto mode", .class = UacDummyName, .image = "consent.exe", .uac = UacAuto, .expect = excluded(ReasonExcludedUacDummy) },
    .{ .name = "dummy in native mode", .class = UacDummyName, .image = "consent.exe", .uac = UacNative, .expect = excluded(ReasonExcludedUacDummy) },
    .{ .name = "dummy beats child", .class = UacDummyName, .image = "consent.exe", .child = true, .expect = excluded(ReasonExcludedUacDummy) },
    .{ .name = "credential in window mode", .class = UacCredentialName, .image = "consent.exe", .uac = UacWindow, .expect = eligible(RoleUacConsent, 0) },
    .{ .name = "credential in auto mode", .class = UacCredentialName, .image = "consent.exe", .uac = UacAuto, .expect = eligible(RoleUacConsent, 0) },
    .{ .name = "credential in native mode", .class = UacCredentialName, .image = "consent.exe", .uac = UacNative, .expect = excluded(ReasonUacNativeHidden) },
    .{ .name = "credential in unknown mode fails closed", .class = UacCredentialName, .image = "consent.exe", .uac = 7, .expect = excluded(ReasonUacNativeHidden) },
    .{ .name = "credential image upper case", .class = UacCredentialName, .image = "CONSENT.EXE", .expect = eligible(RoleUacConsent, 0) },
    .{ .name = "credential class lower case", .class = "credential dialog xaml host", .image = "consent.exe", .expect = eligible(RoleUacConsent, 0) },
    .{ .name = "credential precedes child step", .class = UacCredentialName, .image = "consent.exe", .child = true, .expect = eligible(RoleUacConsent, 0) },
    .{ .name = "credential ignores owner", .class = UacCredentialName, .image = "consent.exe", .owned = true, .expect = eligible(RoleUacConsent, 0) },
    .{ .name = "credential zero area", .class = UacCredentialName, .image = "consent.exe", .width = 0, .expect = excluded(ReasonZeroArea) },
    .{ .name = "credential transparent", .class = UacCredentialName, .image = "consent.exe", .flags_set = LayeredKnown, .alpha = 0, .expect = excluded(ReasonTransparent) },
    .{ .name = "credential unknown image", .class = UacCredentialName, .image = "", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "credential from other image", .class = UacCredentialName, .image = "notepad.exe", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "credential truncated class", .class = UacCredentialName, .image = "consent.exe", .flags_set = FactClassTruncated, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "dummy unknown image", .class = UacDummyName, .image = "", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "dummy from other image", .class = UacDummyName, .image = "notepad.exe", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "other consent window in window mode", .image = "consent.exe", .uac = UacWindow, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "other consent window in auto mode", .image = "consent.exe", .uac = UacAuto, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "other consent window in native mode", .image = "consent.exe", .uac = UacNative, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "other consent window child", .image = "consent.exe", .child = true, .expect = excluded(ReasonChild) },
    .{ .name = "other consent window owned", .image = "consent.exe", .owned = true, .expect = eligible(RoleDialog, OwnerRootWindow) },
    .{ .name = "consent image with suffix", .class = UacCredentialName, .image = "consent.exe.x", .expect = eligible(RoleToplevel, 0) },
    // Step 3.
    .{ .name = "child window", .child = true, .expect = excluded(ReasonChild) },
    .{ .name = "child popup menu class", .class = "#32768", .child = true, .expect = excluded(ReasonChild) },
    .{ .name = "child tooltip", .ex_style = ExStyleNoActivate, .child = true, .expect = excluded(ReasonChild) },
    .{ .name = "child owned", .owned = true, .child = true, .expect = excluded(ReasonChild) },
    // Step 4: Menu.
    .{ .name = "menu class", .class = "#32768", .expect = eligible(RoleMenu, 0) },
    .{ .name = "menu class owned", .class = "#32768", .owned = true, .expect = eligible(RoleMenu, OwnerRootWindow) },
    .{ .name = "menu class with no-activate stays menu", .class = "#32768", .ex_style = ExStyleNoActivate, .expect = eligible(RoleMenu, 0) },
    .{ .name = "menu class owned with no-activate stays menu", .class = "#32768", .ex_style = ExStyleNoActivate, .owned = true, .expect = eligible(RoleMenu, OwnerRootWindow) },
    .{ .name = "combo list box", .class = "ComboLBox", .expect = eligible(RoleMenu, 0) },
    .{ .name = "combo list box owned lower case", .class = "combolbox", .owned = true, .expect = eligible(RoleMenu, OwnerRootWindow) },
    .{ .name = "combo list box with no-activate stays menu", .class = "ComboLBox", .ex_style = ExStyleNoActivate, .owned = true, .expect = eligible(RoleMenu, OwnerRootWindow) },
    .{ .name = "menu class truncated is not a class match", .class = "#32768", .flags_set = FactClassTruncated, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "menu class one byte longer", .class = "#327680", .expect = eligible(RoleToplevel, 0) },
    .{ .name = "popup owned without caption", .style = PopupNoCaption, .owned = true, .expect = eligible(RoleMenu, OwnerRootWindow) },
    .{ .name = "popup owned with border bit only", .style = StylePopup | BorderOnly, .owned = true, .expect = eligible(RoleMenu, OwnerRootWindow) },
    .{ .name = "popup owned with dialog-frame bit only", .style = StylePopup | DialogFrameOnly, .owned = true, .expect = eligible(RoleMenu, OwnerRootWindow) },
    .{ .name = "popup owned with caption is a dialog", .style = StylePopup | StyleCaption, .owned = true, .expect = eligible(RoleDialog, OwnerRootWindow) },
    .{ .name = "popup owned no caption no-activate is a tooltip", .style = PopupNoCaption, .ex_style = ExStyleNoActivate, .owned = true, .expect = eligible(RoleTooltip, OwnerRootWindow) },
    .{ .name = "popup ownerless no caption is top-level", .style = PopupNoCaption, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "owned no caption without popup is a dialog", .style = 0, .owned = true, .expect = eligible(RoleDialog, OwnerRootWindow) },
    .{ .name = "menu zero height", .class = "#32768", .height = 0, .expect = excluded(ReasonZeroArea) },
    .{ .name = "menu transparent", .class = "#32768", .flags_set = LayeredKnown, .alpha = 0, .expect = excluded(ReasonTransparent) },
    // Step 5: Tooltip.
    .{ .name = "tooltip class", .class = "tooltips_class32", .expect = eligible(RoleTooltip, 0) },
    .{ .name = "tooltip class owned", .class = "tooltips_class32", .owned = true, .expect = eligible(RoleTooltip, OwnerRootWindow) },
    .{ .name = "tooltip class upper case", .class = "TOOLTIPS_CLASS32", .expect = eligible(RoleTooltip, 0) },
    .{ .name = "no-activate ownerless", .ex_style = ExStyleNoActivate, .expect = eligible(RoleTooltip, 0) },
    .{ .name = "no-activate owned", .ex_style = ExStyleNoActivate, .owned = true, .expect = eligible(RoleTooltip, OwnerRootWindow) },
    .{ .name = "no-activate beats app-window", .ex_style = ExStyleNoActivate | ExStyleAppWindow, .owned = true, .expect = eligible(RoleTooltip, OwnerRootWindow) },
    .{ .name = "tool window owned", .ex_style = ExStyleToolWindow, .owned = true, .expect = eligible(RoleTooltip, OwnerRootWindow) },
    .{ .name = "tool window ownerless is top-level", .ex_style = ExStyleToolWindow, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "tool window owned with app-window is still tooltip", .ex_style = ExStyleToolWindow | ExStyleAppWindow, .owned = true, .expect = eligible(RoleTooltip, OwnerRootWindow) },
    .{ .name = "tooltip zero width", .class = "tooltips_class32", .width = 0, .expect = excluded(ReasonZeroArea) },
    // Step 6 and 7.
    .{ .name = "owned dialog", .owned = true, .expect = eligible(RoleDialog, OwnerRootWindow) },
    .{ .name = "owned with app-window falls to top-level", .owned = true, .ex_style = ExStyleAppWindow, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "ownerless with app-window", .ex_style = ExStyleAppWindow, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "dialog zero area", .owned = true, .height = 0, .expect = excluded(ReasonZeroArea) },
    // Hidden owner: the window is judged as not owned in steps 4 to 7.
    .{ .name = "hidden owner keeps a menu class a menu without a parent", .class = "#32768", .owned = true, .flags_set = FactOwnerHidden, .expect = eligible(RoleMenu, 0) },
    .{ .name = "hidden owner combo list box is a menu without a parent", .class = "ComboLBox", .owned = true, .flags_set = FactOwnerHidden, .expect = eligible(RoleMenu, 0) },
    .{ .name = "hidden owner popup style is no menu", .style = PopupNoCaption, .owned = true, .flags_set = FactOwnerHidden, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "hidden owner popup with no-activate is a tooltip without a parent", .style = PopupNoCaption, .ex_style = ExStyleNoActivate, .owned = true, .flags_set = FactOwnerHidden, .expect = eligible(RoleTooltip, 0) },
    .{ .name = "hidden owner tooltip class has no parent", .class = "tooltips_class32", .owned = true, .flags_set = FactOwnerHidden, .expect = eligible(RoleTooltip, 0) },
    .{ .name = "hidden owner tool window is no tooltip", .ex_style = ExStyleToolWindow, .owned = true, .flags_set = FactOwnerHidden, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "hidden owner dialog is a top-level", .owned = true, .flags_set = FactOwnerHidden, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "hidden owner app-window stays a top-level", .owned = true, .ex_style = ExStyleAppWindow, .flags_set = FactOwnerHidden, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "hidden owner credential dialog is unaffected", .class = UacCredentialName, .image = "consent.exe", .owned = true, .flags_set = FactOwnerHidden, .expect = eligible(RoleUacConsent, 0) },
    .{ .name = "hidden owner child window is still a child", .owned = true, .child = true, .flags_set = FactOwnerHidden, .expect = excluded(ReasonChild) },
    .{ .name = "hidden owner window still hits zero area", .owned = true, .height = 0, .flags_set = FactOwnerHidden, .expect = excluded(ReasonZeroArea) },
    .{ .name = "hidden owner preedit keeps ImePreedit without a parent", .owned = true, .flags_set = AgentPreeditFlags | FactOwnerHidden, .expect = eligible(RoleImePreedit, 0) },
    // Step 8.
    .{ .name = "zero width", .width = 0, .expect = excluded(ReasonZeroArea) },
    .{ .name = "zero height", .height = 0, .expect = excluded(ReasonZeroArea) },
    .{ .name = "zero both", .width = 0, .height = 0, .expect = excluded(ReasonZeroArea) },
    .{ .name = "one pixel", .width = 1, .height = 1, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "layered alpha zero known", .flags_set = LayeredKnown, .alpha = 0, .expect = excluded(ReasonTransparent) },
    .{ .name = "layered alpha one known", .flags_set = LayeredKnown, .alpha = 1, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "layered alpha max known", .flags_set = LayeredKnown, .alpha = 255, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "layered alpha zero unknown is opaque", .flags_set = FactLayered, .alpha = 0, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "layered ex-style bit without flag is opaque", .ex_style = ExStyleLayered, .alpha = 0, .expect = eligible(RoleToplevel, 0) },
    .{ .name = "zero area beats transparent", .width = 0, .flags_set = LayeredKnown, .alpha = 0, .expect = excluded(ReasonZeroArea) },
    .{ .name = "dialog transparent", .owned = true, .flags_set = LayeredKnown, .alpha = 0, .expect = excluded(ReasonTransparent) },
};

test "classification matrix covers every step of the header table" {
    for (Matrix) |row| {
        var facts = row_facts(row);
        testing_t.expect(call_validate(&facts)) catch |err| {
            std_t.debug.print("matrix row has invalid facts: {s}\n", .{row.name});
            return err;
        };
        const got = call_classify(&facts, row.uac);
        testing_t.expectEqual(row.expect, got) catch |err| {
            std_t.debug.print("matrix row failed: {s}\n", .{row.name});
            return err;
        };
    }
}

test "every excluded class matches exactly, case-insensitively and nothing else" {
    const names = [_][]const u8{
        "Shell_TrayWnd",
        "Shell_SecondaryTrayWnd",
        "Progman",
        "WorkerW",
        "NotifyIconOverflowWindow",
        "TaskListThumbnailWnd",
        "MSCTFIME UI",
        "IME",
    };
    for (names) |name| {
        var facts = fixture_facts();
        set_class(&facts, name);
        try testing_t.expectEqual(excluded(ReasonExcludedClass), call_classify(&facts, UacWindow));
        var mangled: [64]u8 = undefined;
        for (name, 0..) |byte, index| {
            mangled[index] = if (index % 2 == 0) std_t.ascii.toUpper(byte) else std_t.ascii.toLower(byte);
        }
        set_class(&facts, mangled[0..name.len]);
        try testing_t.expectEqual(excluded(ReasonExcludedClass), call_classify(&facts, UacNative));
        set_class(&facts, name[0 .. name.len - 1]);
        try testing_t.expectEqual(eligible(RoleToplevel, 0), call_classify(&facts, UacWindow));
        @memcpy(mangled[0..name.len], name);
        mangled[name.len] = 'x';
        set_class(&facts, mangled[0 .. name.len + 1]);
        try testing_t.expectEqual(eligible(RoleToplevel, 0), call_classify(&facts, UacWindow));
        set_class(&facts, name);
        facts.flags |= FactClassTruncated;
        try testing_t.expectEqual(eligible(RoleToplevel, 0), call_classify(&facts, UacWindow));
    }
}

test "classification parents follow owner_root, never the raw owner" {
    var facts = fixture_facts();
    facts.owner = OwnerWindow;
    facts.owner_root = OwnerRootWindow;
    try testing_t.expectEqual(eligible(RoleDialog, OwnerRootWindow), call_classify(&facts, UacWindow));
    facts.ex_style = ExStyleNoActivate;
    try testing_t.expectEqual(eligible(RoleTooltip, OwnerRootWindow), call_classify(&facts, UacWindow));
    set_class(&facts, "#32768");
    try testing_t.expectEqual(eligible(RoleMenu, OwnerRootWindow), call_classify(&facts, UacWindow));
    // Self ownership is the graph's Cycle case: the classifier still reports the parent as given.
    facts.owner_root = facts.window;
    try testing_t.expectEqual(eligible(RoleMenu, facts.window), call_classify(&facts, UacWindow));
}

test "classification never reads past an unvalidated class or image length" {
    var facts = fixture_facts();
    facts.class_length = 0xFFFF_FFFF;
    facts.image_length = 0xFFFF_FFFF;
    facts.title_length = 0xFFFF_FFFF;
    try testing_t.expectEqual(@as(usize, MaxClassBytes), call_class_bytes(&facts).len);
    try testing_t.expectEqual(@as(usize, MaxImageBytes), call_image_bytes(&facts).len);
    try testing_t.expectEqual(@as(usize, MaxTitleBytes), call_title_bytes(&facts).len);
    try testing_t.expectEqual(eligible(RoleToplevel, 0), call_classify(&facts, UacWindow));
    try testing_t.expect(!call_validate(&facts));
    facts.class_length = 0;
    facts.image_length = 0;
    facts.title_length = 0;
    try testing_t.expectEqual(@as(usize, 0), call_class_bytes(&facts).len);
    try testing_t.expectEqual(@as(usize, 0), call_image_bytes(&facts).len);
    try testing_t.expectEqual(@as(usize, 0), call_title_bytes(&facts).len);
    set_class(&facts, "Notepad");
    set_image(&facts, "notepad.exe");
    set_title(&facts, "Untitled");
    try testing_t.expectEqualStrings("Notepad", call_class_bytes(&facts));
    try testing_t.expectEqualStrings("notepad.exe", call_image_bytes(&facts));
    try testing_t.expectEqualStrings("Untitled", call_title_bytes(&facts));
}

test "valid facts: defaults, maxima and boundaries" {
    var facts = fixture_facts();
    try testing_t.expect(call_validate(&facts));
    facts.window = std_t.math.maxInt(u64);
    facts.root = std_t.math.maxInt(u64);
    facts.stamp = std_t.math.maxInt(u64);
    facts.owner = 1;
    facts.owner_root = std_t.math.maxInt(u64);
    facts.dpi = MinDpi;
    try testing_t.expect(call_validate(&facts));
    facts.dpi = MaxDpi;
    facts.width = MaxExtent;
    facts.height = MaxExtent;
    facts.min_width = MaxExtent;
    facts.min_height = MaxExtent;
    facts.alpha = 0;
    facts.flags = FactMask & ~(FactAgentPreedit | FactLayeredAlphaKnown | FactImageUnknown);
    facts.style = std_t.math.maxInt(u32);
    facts.ex_style = std_t.math.maxInt(u32);
    facts.x = std_t.math.minInt(i32);
    facts.y = std_t.math.maxInt(i32);
    try testing_t.expect(call_validate(&facts));
    // Maximal strings of non-ASCII text: 128 two-byte characters fill the class array.
    const two_byte = "\xC3\xA9" ** 128;
    @memcpy(facts.class_name[0..two_byte.len], two_byte);
    facts.class_length = MaxClassBytes;
    @memset(&facts.image_name, 'i');
    facts.image_length = MaxImageBytes;
    facts.flags &= ~FactImageUnknown;
    @memset(&facts.title, 't');
    facts.title_length = MaxTitleBytes;
    try testing_t.expect(call_validate(&facts));
    // Preedit with agent ownership and layered with known alpha are valid combinations.
    facts.flags |= FactAgentOwned | FactAgentPreedit | FactLayered | FactLayeredAlphaKnown;
    try testing_t.expect(call_validate(&facts));
    // Empty title and unknown image are valid.
    facts = fixture_facts();
    set_title(&facts, "");
    set_image(&facts, "");
    try testing_t.expect(call_validate(&facts));
    try testing_t.expect(facts.flags & FactImageUnknown != 0);
}

test "validate_facts rejects each violated rule and nothing else" {
    const base = fixture_facts();
    try testing_t.expect(call_validate(&base));

    var facts = base;
    facts.window = 0;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.root = 0;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.stamp = 0;
    try testing_t.expect(!call_validate(&facts));

    facts = base;
    facts.owner = 5;
    facts.owner_root = 0;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.owner = 0;
    facts.owner_root = 5;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.owner = 5;
    facts.owner_root = 6;
    try testing_t.expect(call_validate(&facts));

    var bit: u5 = 13;
    while (true) : (bit += 1) {
        facts = base;
        facts.flags |= @as(u32, 1) << bit;
        try testing_t.expect(!call_validate(&facts));
        if (bit == 31) break;
    }

    facts = base;
    facts.flags |= FactAgentPreedit;
    try testing_t.expect(!call_validate(&facts));
    facts.flags |= FactAgentOwned;
    try testing_t.expect(call_validate(&facts));
    facts = base;
    facts.flags |= FactLayeredAlphaKnown;
    try testing_t.expect(!call_validate(&facts));
    facts.flags |= FactLayered;
    try testing_t.expect(call_validate(&facts));

    facts = base;
    facts.flags |= FactImageUnknown;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.image_length = 0;
    try testing_t.expect(!call_validate(&facts));
    facts.flags |= FactImageUnknown;
    try testing_t.expect(call_validate(&facts));

    for ([_]u32{ 0, 1, MinDpi - 1, MaxDpi + 1, std_t.math.maxInt(u32) }) |dpi| {
        facts = base;
        facts.dpi = dpi;
        try testing_t.expect(!call_validate(&facts));
    }
    facts = base;
    facts.width = MaxExtent + 1;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.height = MaxExtent + 1;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.min_width = MaxExtent + 1;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.min_height = std_t.math.maxInt(u32);
    try testing_t.expect(!call_validate(&facts));

    facts = base;
    facts.class_length = 0;
    try testing_t.expect(!call_validate(&facts));
    facts.class_length = MaxClassBytes + 1;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.image_length = MaxImageBytes + 1;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.title_length = MaxTitleBytes + 1;
    try testing_t.expect(!call_validate(&facts));

    for (0..3) |index| {
        facts = base;
        facts.reserved0[index] = 1;
        try testing_t.expect(!call_validate(&facts));
    }
}

test "validate_facts checks text only over the stated length" {
    const base = fixture_facts();
    var facts = base;
    facts.class_name[3] = 0;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.image_name[0] = 0;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.title[2] = 0;
    try testing_t.expect(!call_validate(&facts));

    facts = base;
    facts.class_name[1] = 0xFF;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.image_name[4] = 0xC0;
    try testing_t.expect(!call_validate(&facts));
    facts = base;
    facts.title[5] = 0xED;
    facts.title[6] = 0xA0;
    facts.title[7] = 0x80;
    try testing_t.expect(!call_validate(&facts));
    // A multi-byte sequence cut by the stated length is invalid.
    facts = base;
    set_title(&facts, "ab\xC3\xA9");
    facts.title_length = 3;
    try testing_t.expect(!call_validate(&facts));
    facts.title_length = 4;
    try testing_t.expect(call_validate(&facts));

    // Garbage after every stated length is ignored.
    facts = base;
    facts.class_name[facts.class_length] = 0;
    facts.class_name[facts.class_length + 1] = 0xFF;
    facts.image_name[facts.image_length] = 0xC0;
    facts.title[facts.title_length] = 0;
    facts.title[MaxTitleBytes - 1] = 0xFF;
    try testing_t.expect(call_validate(&facts));
    set_title(&facts, "");
    facts.title[0] = 0xFF;
    try testing_t.expect(call_validate(&facts));
}

test "utf8_valid_no_nul accepts well-formed text and rejects every malformed class" {
    const valid = [_][]const u8{
        "",
        "plain ascii",
        "\x7F",
        "\xC2\x80",
        "\xC3\xA9",
        "\xDF\xBF",
        "\xE0\xA0\x80",
        "\xE2\x82\xAC",
        "\xED\x9F\xBF",
        "\xEE\x80\x80",
        "\xEF\xBF\xBD",
        "\xEF\xBF\xBF",
        "\xF0\x90\x80\x80",
        "\xF0\x9F\x98\x80",
        "\xF4\x8F\xBF\xBF",
        "mixed \xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 text",
    };
    for (valid) |bytes| try testing_t.expect(call_utf8(bytes));
    const invalid = [_][]const u8{
        "\x00",
        "a\x00b",
        "abc\x00",
        "\xC2\x80\x00",
        "\xC0\x80",
        "\xC0\xAF",
        "\xC1\xBF",
        "\xE0\x80\x80",
        "\xE0\x9F\xBF",
        "\xF0\x80\x80\x80",
        "\xF0\x8F\xBF\xBF",
        "\xED\xA0\x80",
        "\xED\xBF\xBF",
        "\xF4\x90\x80\x80",
        "\xF5\x80\x80\x80",
        "\xF8\x88\x80\x80\x80",
        "\xFF",
        "\xFE",
        "\x80",
        "\xBF",
        "\xC3",
        "\xE2\x82",
        "\xF0\x9F\x98",
        "\xC3\x28",
        "\xE2\x28\xA1",
        "\xE2\x82\x28",
        "\xF0\x28\x8C\xBC",
        "\xF0\x9F\x28\x80",
        "ok then \xFF",
    };
    for (invalid) |bytes| try testing_t.expect(!call_utf8(bytes));
}

test "app_key_valid matches the frozen codec rule and the 3..200 length rule" {
    try testing_t.expect(call_app_key("a-x"));
    try testing_t.expect(call_app_key("p-x"));
    try testing_t.expect(call_app_key("a-Notepad_Plus-Plus-1234abcd"));
    try testing_t.expect(call_app_key("p-0123456789_-ABCxyz"));
    try testing_t.expect(call_app_key("a--"));
    try testing_t.expect(call_app_key("a-_"));
    const invalid = [_][]const u8{
        "",
        "a",
        "a-",
        "p-",
        "ax",
        "ab-",
        "ax-y",
        "b-xyz",
        "A-xyz",
        "P-xyz",
        "-a-x",
        "a_xyz",
        "a:xyz",
        "a-x y",
        "a-x.y",
        "a-x\x00",
        "a-\xC3\xA9",
        "a-x~",
        "a-x/y",
        "a-x\n",
        "a-x@",
        "a-[",
        "a-`",
    };
    for (invalid) |bytes| try testing_t.expect(!call_app_key(bytes));

    var key: [MaxAppKeyBytes + 1]u8 = undefined;
    @memset(&key, 'k');
    key[0] = 'a';
    key[1] = '-';
    try testing_t.expect(call_app_key(key[0 .. 2 + 1]));
    try testing_t.expect(call_app_key(key[0 .. MaxAppKeyBytes - 1]));
    try testing_t.expect(call_app_key(key[0..MaxAppKeyBytes]));
    try testing_t.expect(!call_app_key(key[0 .. MaxAppKeyBytes + 1]));
    try testing_t.expect(!call_app_key(key[0..2]));

    // Every single byte value at every position of a short key.
    var byte: usize = 0;
    while (byte < 256) : (byte += 1) {
        var probe = [_]u8{ 'a', '-', 'k', 'k' };
        const expected = (byte >= 'A' and byte <= 'Z') or (byte >= 'a' and byte <= 'z') or
            (byte >= '0' and byte <= '9') or byte == '_' or byte == '-';
        for (2..4) |position| {
            probe = [_]u8{ 'a', '-', 'k', 'k' };
            probe[position] = @intCast(byte);
            try testing_t.expectEqual(expected, call_app_key(&probe));
        }
        probe = .{ 'a', '-', 'k', 'k' };
        probe[0] = @intCast(byte);
        try testing_t.expectEqual(byte == 'a' or byte == 'p', call_app_key(&probe));
        probe = .{ 'a', '-', 'k', 'k' };
        probe[1] = @intCast(byte);
        try testing_t.expectEqual(byte == '-', call_app_key(&probe));
    }
}

test "class_hash_of follows FNV-1a 64 and distinguishes case" {
    try testing_t.expectEqual(FnvOffsetBasis, call_hash(""));
    try testing_t.expectEqual(@as(u64, 0xcbf29ce484222325), call_hash(""));
    try testing_t.expectEqual(@as(u64, 0xaf63dc4c8601ec8c), call_hash("a"));
    try testing_t.expectEqual(@as(u64, 0x85944171f73967e8), call_hash("foobar"));
    try testing_t.expectEqual(@as(u64, 0x8842b206a7bfc712), call_hash("Notepad"));
    try testing_t.expectEqual(@as(u64, 0x62bde74ad4d13eb2), call_hash("notepad"));
    try testing_t.expectEqual(@as(u64, 0xdebead0ba0919302), call_hash("#32768"));
    try testing_t.expect(call_hash("Notepad") != call_hash("notepad"));
    // The hash covers the exact stated bytes: a trailing byte changes it.
    try testing_t.expect(call_hash("Notepad") != call_hash("Notepad\x00"));
}

test "ascii_eql_ignore_case folds only ASCII letters over the full length" {
    try testing_t.expect(call_eql_fold("", ""));
    try testing_t.expect(call_eql_fold("Shell_TrayWnd", "SHELL_TRAYWND"));
    try testing_t.expect(call_eql_fold("abcXYZ019_-", "ABCxyz019_-"));
    try testing_t.expect(!call_eql_fold("abc", "abcd"));
    try testing_t.expect(!call_eql_fold("abcd", "abc"));
    try testing_t.expect(!call_eql_fold("", "a"));
    try testing_t.expect(!call_eql_fold("a", "b"));
    // '@' (0x40) and '`' (0x60) differ by 0x20 but are not letters.
    try testing_t.expect(!call_eql_fold("@", "`"));
    try testing_t.expect(!call_eql_fold("[", "{"));
    // Non-ASCII bytes compare exactly.
    try testing_t.expect(call_eql_fold("\xC3\xA9", "\xC3\xA9"));
    try testing_t.expect(!call_eql_fold("\xC3\x89", "\xC3\xA9"));
    var byte: usize = 0;
    while (byte < 256) : (byte += 1) {
        const left = [_]u8{@intCast(byte)};
        const right = [_]u8{@intCast(byte ^ 0x20)};
        const letter = (byte >= 'A' and byte <= 'Z') or (byte >= 'a' and byte <= 'z');
        try testing_t.expectEqual(letter, call_eql_fold(&left, &right));
    }
}

test "unwrap_tick resolves a raw tick to the 64-bit value nearest the base" {
    const base: u64 = TickBaseStart + 1000;
    try testing_t.expectEqual(TickBaseStart + 1000, call_unwrap(base, 1000));
    try testing_t.expectEqual(TickBaseStart + 1005, call_unwrap(base, 1005));
    try testing_t.expectEqual(TickBaseStart + 990, call_unwrap(base, 990));
    // Across the 32-bit wrap: base just below 2^33, raw tick just past the wrap.
    const high: u64 = 2 * TickBaseStart - 16;
    try testing_t.expectEqual(2 * TickBaseStart + 16, call_unwrap(high, 16));
    try testing_t.expectEqual(2 * TickBaseStart - 32, call_unwrap(high, 0xFFFF_FFE0));
    // Plus and minus 2^31 - 1 are exact; exactly 2^31 aliases to the earlier value.
    try testing_t.expectEqual(base + 0x7FFF_FFFF, call_unwrap(base, @truncate(base +% 0x7FFF_FFFF)));
    try testing_t.expectEqual(base - 0x7FFF_FFFF, call_unwrap(base, @truncate(base -% 0x7FFF_FFFF)));
    try testing_t.expectEqual(base - 0x8000_0000, call_unwrap(base, @truncate(base +% 0x8000_0000)));
    // The smallest legal base never underflows.
    try testing_t.expectEqual(TickBaseStart - 0x8000_0000 + 0, call_unwrap(TickBaseStart, 0x8000_0000));
}

test "derive_state maps facts to protocol bits and never sets Activated" {
    var facts = fixture_facts();
    facts.style = 0;
    try testing_t.expectEqual(@as(u32, 0), call_state(&facts));
    facts.flags |= FactIconic;
    try testing_t.expectEqual(StateMinimized, call_state(&facts));
    facts.flags = FactVisible | FactOnInputDesktop | FactZoomed;
    try testing_t.expectEqual(StateMaximized, call_state(&facts));
    facts.flags = FactVisible | FactOnInputDesktop | FactFullscreen;
    try testing_t.expectEqual(StateFullscreen, call_state(&facts));
    facts.flags = FactVisible | FactOnInputDesktop;
    facts.style = StyleThickFrame;
    try testing_t.expectEqual(StateResizable, call_state(&facts));
    facts.style = StyleMaximizeBox;
    try testing_t.expectEqual(StateMaximizable, call_state(&facts));
    facts.style = StyleMinimizeBox;
    try testing_t.expectEqual(StateMinimizable, call_state(&facts));
    facts.style = DefaultStyle;
    try testing_t.expectEqual(StateResizable | StateMaximizable | StateMinimizable, call_state(&facts));
    facts.flags = FactMask;
    facts.style = std_t.math.maxInt(u32);
    facts.ex_style = std_t.math.maxInt(u32);
    try testing_t.expectEqual(StateMask & ~StateActivated, call_state(&facts));
    try testing_t.expectEqual(@as(u32, 0), call_state(&facts) & StateActivated);
}

test "identity_equal compares process, thread, start time and class hash" {
    var facts = fixture_facts();
    var rec = call_record_init();
    rec.process_id = facts.process_id;
    rec.thread_id = facts.thread_id;
    rec.process_start_time = facts.process_start_time;
    rec.class_hash = call_hash(call_class_bytes(&facts));
    try testing_t.expect(call_identity(&rec, &facts));
    // Title, rect, style and stamps are not part of the identity.
    facts.x += 100;
    facts.width += 1;
    facts.stamp += 9;
    facts.style = 0;
    set_title(&facts, "another title");
    try testing_t.expect(call_identity(&rec, &facts));
    // Class bytes past the stated length are not part of it either.
    facts.class_name[facts.class_length] = 'Z';
    try testing_t.expect(call_identity(&rec, &facts));

    var other = facts;
    other.process_id += 1;
    try testing_t.expect(!call_identity(&rec, &other));
    other = facts;
    other.thread_id += 1;
    try testing_t.expect(!call_identity(&rec, &other));
    other = facts;
    other.process_start_time += 1;
    try testing_t.expect(!call_identity(&rec, &other));
    other = facts;
    other.process_start_time = 0;
    try testing_t.expect(!call_identity(&rec, &other));
    other = facts;
    set_class(&other, "notepad");
    try testing_t.expect(!call_identity(&rec, &other));
    set_class(&other, "Notepad2");
    try testing_t.expect(!call_identity(&rec, &other));
    set_class(&other, "Notepad");
    try testing_t.expect(call_identity(&rec, &other));
}

test "record_init returns an all-zero record" {
    const rec = call_record_init();
    try testing_t.expectEqual(@as(u64, 0), rec.window);
    try testing_t.expectEqual(@as(u64, 0), rec.generation);
    try testing_t.expectEqual(@as(u64, 0), rec.parent_window);
    try testing_t.expectEqual(@as(u64, 0), rec.parent_generation);
    try testing_t.expectEqual(@as(u64, 0), rec.process_start_time);
    try testing_t.expectEqual(@as(u64, 0), rec.class_hash);
    try testing_t.expectEqual(@as(u64, 0), rec.admit_stamp);
    try testing_t.expectEqual(@as(u64, 0), rec.last_stamp);
    try testing_t.expectEqual(@as(u64, 0), rec.seq);
    try testing_t.expectEqual(@as(i32, 0), rec.x);
    try testing_t.expectEqual(@as(i32, 0), rec.y);
    try testing_t.expectEqual(@as(u32, 0), rec.width);
    try testing_t.expectEqual(@as(u32, 0), rec.height);
    try testing_t.expectEqual(@as(u32, 0), rec.role);
    try testing_t.expectEqual(@as(u32, 0), rec.state);
    try testing_t.expectEqual(@as(u32, 0), rec.dpi);
    try testing_t.expectEqual(@as(u32, 0), rec.min_width);
    try testing_t.expectEqual(@as(u32, 0), rec.min_height);
    try testing_t.expectEqual(@as(u32, 0), rec.process_id);
    try testing_t.expectEqual(@as(u32, 0), rec.thread_id);
    try testing_t.expectEqual(@as(u64, 0), rec.admit_captured_ms);
    try testing_t.expectEqual(@as(u64, 0), rec.last_captured_ms);
    try testing_t.expectEqual(@as(u32, 0), rec.retire_reason);
    try testing_t.expectEqual(@as(u32, 0), rec.title_length);
    try testing_t.expectEqual(@as(u32, 0), rec.app_key_length);
    try testing_t.expect(std_t.mem.allEqual(u8, &rec.title, 0));
    try testing_t.expect(std_t.mem.allEqual(u8, &rec.app_key, 0));
    try testing_t.expectEqual(@as(usize, MaxTitleBytes), rec.title.len);
    try testing_t.expectEqual(@as(usize, MaxAppKeyBytes), rec.app_key.len);
}

test "bump increments and saturates without wrapping" {
    var counter: u64 = 0;
    call_bump(&counter);
    try testing_t.expectEqual(@as(u64, 1), counter);
    call_bump(&counter);
    try testing_t.expectEqual(@as(u64, 2), counter);
    counter = std_t.math.maxInt(u64) - 1;
    call_bump(&counter);
    try testing_t.expectEqual(std_t.math.maxInt(u64), counter);
    call_bump(&counter);
    try testing_t.expectEqual(std_t.math.maxInt(u64), counter);
}

test "status values and constants equal the committed header" {
    try testing_t.expectEqual(@as(c_int, 0), @intFromEnum(status_t.Ok));
    try testing_t.expectEqual(@as(c_int, 1), @intFromEnum(status_t.None));
    try testing_t.expectEqual(@as(c_int, 20), @intFromEnum(status_t.SinkFailed));
    try testing_t.expectEqual(@as(usize, 21), @typeInfo(status_t).Enum.fields.len);
    try testing_t.expectEqual(@as(usize, 64), MaxEntries);
    try testing_t.expectEqual(@as(usize, 192), MaxCallEvents);
    try testing_t.expectEqual(@sizeOf(c_t.interop_window_facts_t), @sizeOf(facts_t));
    try testing_t.expectEqual(@as(usize, 1656), @sizeOf(facts_t));
    try testing_t.expectEqual(@as(usize, 1328), @sizeOf(event_t));
    try testing_t.expectEqual(@as(usize, 56), @sizeOf(observation_t));
    try testing_t.expectEqual(@as(usize, 48), @sizeOf(live_t));
    try testing_t.expectEqual(@as(usize, 24), @sizeOf(retiring_entry_t));
    try testing_t.expectEqual(@as(usize, 8), @sizeOf(proof_t));
    try testing_t.expectEqual(@as(usize, 256), @sizeOf(stats_t));
    try testing_t.expectEqual(@as(usize, 56), @sizeOf(diagnostic_t));
    try testing_t.expectEqual(@as(usize, 48), @sizeOf(config_t));
}

/// Verbatim copy of the private `app_key_valid` in interop_codec.zig (the frozen wire rule).
fn codec_app_key_oracle(value: []const u8) bool {
    if (value.len < 3 or value[1] != '-' or (value[0] != 'a' and value[0] != 'p')) return false;
    for (value) |byte| {
        const ok = (byte >= 'A' and byte <= 'Z') or (byte >= 'a' and byte <= 'z') or
            (byte >= '0' and byte <= '9') or byte == '_' or byte == '-';
        if (!ok) return false;
    }
    return true;
}

test "app_key_valid agrees with the codec rule on random keys up to 200 bytes" {
    var prng = std_t.Random.DefaultPrng.init(0x57AD_D1E0);
    const random = prng.random();
    const alphabet = "ap-_09AZaz!. ~\x00\xC3\xA9";
    var buffer: [MaxAppKeyBytes]u8 = undefined;
    var round: usize = 0;
    while (round < 4000) : (round += 1) {
        const length = random.uintLessThan(usize, MaxAppKeyBytes + 1);
        for (buffer[0..length]) |*byte| byte.* = alphabet[random.uintLessThan(usize, alphabet.len)];
        if (length >= 2 and random.boolean()) {
            buffer[0] = if (random.boolean()) 'a' else 'p';
            buffer[1] = '-';
        }
        const key = buffer[0..length];
        try testing_t.expectEqual(codec_app_key_oracle(key), call_app_key(key));
    }
}

// Stateful graph fixtures and tests (layer 2a).

const TestEpoch: u64 = 0x5E55_1011;
const Unset: u64 = 0xDEAD_BEEF_DEAD_BEEF;

/// Settable fake clock; `reads` counts callback invocations.
const fake_clock_t = struct {
    now: u64 = 0,
    reads: u32 = 0,
    probe: ?*graph_t = null,
    probe_status: ?status_t = null,
};

var test_clock: fake_clock_t = .{};

fn fake_now(context: ?*anyopaque) callconv(.C) u64 {
    const clock: *fake_clock_t = @ptrCast(@alignCast(context.?));
    clock.reads += 1;
    if (clock.probe) |graph| clock.probe_status = enter_read(graph) orelse .Ok;
    return clock.now;
}

fn stub_lookup(context: ?*anyopaque, facts: [*c]const facts_t, key: [*c]u8, capacity: u32, length: [*c]u32) callconv(.C) c_int {
    _ = context;
    _ = facts;
    const text = "a-stub";
    if (capacity < text.len) return 1;
    @memcpy(key[0..text.len], text);
    length.* = text.len;
    return 0;
}

fn test_config(epoch: u64) config_t {
    var config = std_t.mem.zeroes(config_t);
    config.session_epoch = epoch;
    config.uac_mode = UacWindow;
    config.app_key_lookup = stub_lookup;
    config.now_ms = fake_now;
    config.now_ms_context = &test_clock;
    return config;
}

/// Create a graph with the stub lookup and the settable fake clock (reset to 0).
fn test_graph(allocator: std_t.mem.Allocator, epoch: u64) !*graph_t {
    test_clock = .{};
    const config = test_config(epoch);
    return call_create(allocator, &config);
}

fn call_create(allocator: std_t.mem.Allocator, config: *const config_t) error{ OutOfMemory, InvalidArgument }!*graph_t {
    return @call(.never_inline, create, .{ allocator, config });
}

fn call_destroy(graph: *graph_t) void {
    @call(.never_inline, destroy, .{graph});
}

fn call_enter(graph: *graph_t, epoch: ?u64) ?status_t {
    return @call(.never_inline, enter, .{ graph, epoch });
}

fn call_enter_read(graph: *const graph_t) ?status_t {
    return @call(.never_inline, enter_read, .{graph});
}

fn call_leave(graph: *graph_t) void {
    @call(.never_inline, leave, .{graph});
}

fn call_count_state(graph: *const graph_t, state: slot_state_t) u32 {
    return @call(.never_inline, count_state, .{ graph, state });
}

fn call_count_deferred(graph: *const graph_t) u32 {
    return @call(.never_inline, count_deferred, .{graph});
}

fn call_find_active(graph: *const graph_t, window: u64) ?usize {
    return @call(.never_inline, find_active, .{ graph, window });
}

fn call_find_exact(graph: *const graph_t, window: u64, generation: u64) ?usize {
    return @call(.never_inline, find_exact, .{ graph, window, generation });
}

fn call_find_deferred(graph: *const graph_t, window: u64) ?usize {
    return @call(.never_inline, find_deferred, .{ graph, window });
}

fn call_ring_room(graph: *const graph_t) u32 {
    return @call(.never_inline, ring_room, .{graph});
}

fn call_push(graph: *graph_t, ev: *const event_t) bool {
    return @call(.never_inline, ring_push_or_coalesce, .{ graph, ev });
}

fn call_next_event(graph: *graph_t, epoch: u64, out: *event_t) status_t {
    return @call(.never_inline, next_event, .{ graph, epoch, out });
}

fn call_drain(graph: *graph_t, epoch: u64, sink: event_sink_t, context: ?*anyopaque, max_events: u32, out: *u32) status_t {
    return @call(.never_inline, drain, .{ graph, epoch, sink, context, max_events, out });
}

fn call_check_action(graph: *graph_t, epoch: u64, window: u64, generation: u64) status_t {
    return @call(.never_inline, check_action, .{ graph, epoch, window, generation });
}

fn call_find_live(graph: *const graph_t, window: u64, out: *live_t) status_t {
    return @call(.never_inline, find_live, .{ graph, window, out });
}

fn call_list_retiring(graph: *const graph_t, entries: ?[*]retiring_entry_t, capacity: u32, out_count: *u32) status_t {
    return @call(.never_inline, list_retiring, .{ graph, entries, capacity, out_count });
}

fn call_list_tracked_windows(
    graph: *const graph_t,
    epoch: u64,
    windows: ?[*]u64,
    capacity: u32,
    out_count: ?*u32,
    out_free_entry_count: ?*u32,
) status_t {
    return @call(.never_inline, list_tracked_windows, .{ graph, epoch, windows, capacity, out_count, out_free_entry_count });
}

fn call_complete_retire(graph: *graph_t, epoch: u64, window: u64, generation: u64, proof: *const proof_t) status_t {
    return @call(.never_inline, complete_retire, .{ graph, epoch, window, generation, proof });
}

fn call_get_stats(graph: *const graph_t, out: *stats_t) status_t {
    return @call(.never_inline, get_stats, .{ graph, out });
}

fn call_stamp_high_water_of(graph: *const graph_t) u64 {
    return @call(.never_inline, stamp_high_water_of, .{graph});
}

fn call_stamp_high_water(graph: *const graph_t, out: *u64) status_t {
    return @call(.never_inline, stamp_high_water, .{ graph, out });
}

fn call_take_diag(graph: *graph_t, out: *diagnostic_t) status_t {
    return @call(.never_inline, take_capacity_diagnostic, .{ graph, out });
}

/// Place an entry directly into slot `slot` (bypasses admission).
fn inject_entry(graph: *graph_t, slot: usize, state: slot_state_t, window: u64, generation: u64) *record_t {
    graph.entries[slot] = .{ .state = state, .seen = false, .record = record_init() };
    const record = &graph.entries[slot].record;
    record.window = window;
    record.generation = generation;
    record.role = RoleToplevel;
    record.state = StateResizable;
    record.process_id = 4242;
    if (state == .Retiring) record.retire_reason = ReasonDestroyed;
    return record;
}

fn inject_deferred(graph: *graph_t, slot: usize, window: u64) void {
    graph.deferred[slot] = .{ .used = true, .seen = false, .record = record_init() };
    graph.deferred[slot].record.window = window;
    graph.deferred[slot].record.seq = slot + 1;
}

fn make_event(kind: u32, window: u64, generation: u64) event_t {
    var ev = std_t.mem.zeroes(event_t);
    ev.session_epoch = TestEpoch;
    ev.window = window;
    ev.generation = generation;
    ev.kind = kind;
    ev.role = RoleToplevel;
    ev.width = 100;
    ev.height = 50;
    ev.dpi = 96;
    ev.process_id = 77;
    ev.changed_mask = if (kind == EventAnnounce) ChangeMask else 0;
    const key = "a-test";
    @memcpy(ev.app_key[0..key.len], key);
    ev.app_key_length = key.len;
    return ev;
}

/// Append an event directly to the ring (bypasses coalescing).
fn queue_raw(graph: *graph_t, ev: event_t) void {
    graph.ring[(graph.ring_head + graph.ring_len) & (EventCapacity - 1)] = ev;
    graph.ring_len += 1;
}

fn stats_bytes(graph: *const graph_t) []const u8 {
    return std_t.mem.asBytes(&graph.stats);
}

test "create validates every configuration field and builds a zeroed graph" {
    var config = test_config(TestEpoch);
    const bad_epoch = blk: {
        var bad = config;
        bad.session_epoch = 0;
        break :blk bad;
    };
    try testing_t.expectError(error.InvalidArgument, call_create(testing_t.allocator, &bad_epoch));
    var bad = config;
    bad.reserved0 = 1;
    try testing_t.expectError(error.InvalidArgument, call_create(testing_t.allocator, &bad));
    bad = config;
    bad.uac_mode = UacNative + 1;
    try testing_t.expectError(error.InvalidArgument, call_create(testing_t.allocator, &bad));
    bad.uac_mode = 0xFFFF_FFFF;
    try testing_t.expectError(error.InvalidArgument, call_create(testing_t.allocator, &bad));
    bad = config;
    bad.app_key_lookup = null;
    try testing_t.expectError(error.InvalidArgument, call_create(testing_t.allocator, &bad));

    for ([_]u32{ UacWindow, UacAuto, UacNative }) |mode| {
        config.uac_mode = mode;
        const accepted = try call_create(testing_t.allocator, &config);
        try testing_t.expectEqual(mode, accepted.config.uac_mode);
        call_destroy(accepted);
    }

    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    try testing_t.expectEqual(TestEpoch, graph.config.session_epoch);
    try testing_t.expectEqual(std_t.Thread.getCurrentId(), graph.owner_thread);
    try testing_t.expect(!graph.busy and !graph.refresh_open and !graph.have_diag);
    try testing_t.expectEqual(@as(u64, 1), graph.next_seq);
    try testing_t.expectEqual(TickBaseStart, graph.tick_base);
    try testing_t.expectEqual(@as(u64, 0), graph.generations_issued);
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
    try testing_t.expectEqual(@as(u32, MaxEntries), call_count_state(graph, .Free));
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try testing_t.expect(std_t.mem.allEqual(u8, stats_bytes(graph), 0));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&graph.ring[EventCapacity - 1]), 0));
    try testing_t.expect(@sizeOf(graph_t) <= MaxGraphBytes);
}

test "create copies the configuration by value" {
    var config = test_config(TestEpoch);
    const graph = try call_create(testing_t.allocator, &config);
    defer call_destroy(graph);
    config.session_epoch = 99;
    config.uac_mode = UacNative;
    try testing_t.expectEqual(TestEpoch, graph.config.session_epoch);
    try testing_t.expectEqual(UacWindow, graph.config.uac_mode);
}

test "create under allocator failure leaves no graph and leaks nothing" {
    const config = test_config(TestEpoch);
    var fail_index: usize = 0;
    while (true) : (fail_index += 1) {
        try testing_t.expect(fail_index < 4);
        var failing = std_t.testing.FailingAllocator.init(testing_t.allocator, .{ .fail_index = fail_index });
        if (call_create(failing.allocator(), &config)) |graph| {
            try testing_t.expectEqual(@as(usize, 1), failing.allocations);
            call_destroy(graph);
            try testing_t.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            break;
        } else |err| {
            try testing_t.expectEqual(error.OutOfMemory, err);
            try testing_t.expectEqual(@as(usize, 0), failing.allocated_bytes);
            try testing_t.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        }
    }
    // The graph needs exactly one allocation: index 0 fails, index 1 succeeds.
    try testing_t.expectEqual(@as(usize, 1), fail_index);
}

test "create takes the default monotonic clock when no now_ms is given" {
    var config = test_config(TestEpoch);
    config.now_ms = null;
    config.now_ms_context = null;
    const graph = try call_create(testing_t.allocator, &config);
    defer call_destroy(graph);
    graph.diag_queue_full = 2;
    var diag = std_t.mem.zeroes(diagnostic_t);
    try testing_t.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
    try testing_t.expectEqual(@as(u64, 2), diag.queue_full);
    graph.diag_queue_full = 1;
    try testing_t.expectEqual(status_t.None, call_take_diag(graph, &diag));
    graph.timer = null;
    graph.have_diag = false;
    try testing_t.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
    try testing_t.expectEqual(@as(u64, 0), graph.last_diag_ms);
}

test "enter applies thread, busy and epoch rules in order" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);

    try testing_t.expectEqual(@as(?status_t, null), call_enter(graph, null));
    try testing_t.expect(graph.busy);
    try testing_t.expectEqual(@as(?status_t, status_t.Reentrant), call_enter(graph, null));
    // Reentrant outranks a stale epoch and bumps nothing.
    try testing_t.expectEqual(@as(?status_t, status_t.Reentrant), call_enter(graph, 0));
    try testing_t.expectEqual(@as(u64, 0), graph.stats.rejected_stale_epoch);
    try testing_t.expectEqual(@as(?status_t, status_t.Reentrant), call_enter_read(graph));
    try testing_t.expect(graph.busy);
    call_leave(graph);
    try testing_t.expect(!graph.busy);

    try testing_t.expectEqual(@as(?status_t, null), call_enter(graph, TestEpoch));
    try testing_t.expect(graph.busy);
    call_leave(graph);

    try testing_t.expectEqual(@as(?status_t, status_t.StaleEpoch), call_enter(graph, TestEpoch + 1));
    try testing_t.expectEqual(@as(?status_t, status_t.StaleEpoch), call_enter(graph, 0));
    try testing_t.expect(!graph.busy);
    try testing_t.expectEqual(@as(u64, 2), graph.stats.rejected_stale_epoch);
    var expected = std_t.mem.zeroes(stats_t);
    expected.rejected_stale_epoch = 2;
    try testing_t.expectEqualSlices(u8, std_t.mem.asBytes(&expected), stats_bytes(graph));

    try testing_t.expectEqual(@as(?status_t, null), call_enter_read(graph));
    try testing_t.expect(!graph.busy);
}

const thread_probe_t = struct {
    graph: *graph_t,
    enter_status: ?status_t = null,
    enter_null_status: ?status_t = null,
    read_status: ?status_t = null,
    next_status: status_t = .Ok,
    stats_status: status_t = .Ok,
    diag_status: status_t = .Ok,
};

fn thread_body(probe: *thread_probe_t) void {
    var ev = std_t.mem.zeroes(event_t);
    var stats = std_t.mem.zeroes(stats_t);
    var diag = std_t.mem.zeroes(diagnostic_t);
    probe.enter_status = call_enter(probe.graph, 0);
    probe.enter_null_status = call_enter(probe.graph, null);
    probe.read_status = call_enter_read(probe.graph);
    probe.next_status = call_next_event(probe.graph, 0, &ev);
    probe.stats_status = call_get_stats(probe.graph, &stats);
    probe.diag_status = call_take_diag(probe.graph, &diag);
}

test "a foreign thread gets WrongThread and touches nothing" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    queue_raw(graph, make_event(EventAnnounce, 1, 1));
    graph.diag_at_capacity = 5;
    // Busy is set too: WrongThread must still outrank Reentrant.
    graph.busy = true;
    const before = graph.stats;
    var probe = thread_probe_t{ .graph = graph };
    const thread = try std_t.Thread.spawn(.{}, thread_body, .{&probe});
    thread.join();
    try testing_t.expectEqual(@as(?status_t, status_t.WrongThread), probe.enter_status);
    try testing_t.expectEqual(@as(?status_t, status_t.WrongThread), probe.enter_null_status);
    try testing_t.expectEqual(@as(?status_t, status_t.WrongThread), probe.read_status);
    try testing_t.expectEqual(status_t.WrongThread, probe.next_status);
    try testing_t.expectEqual(status_t.WrongThread, probe.stats_status);
    try testing_t.expectEqual(status_t.WrongThread, probe.diag_status);
    try testing_t.expectEqualSlices(u8, std_t.mem.asBytes(&before), stats_bytes(graph));
    try testing_t.expectEqual(@as(u32, 1), graph.ring_len);
    try testing_t.expectEqual(@as(u64, 5), graph.diag_at_capacity);
    try testing_t.expect(graph.busy);
    graph.busy = false;
}

test "ring wraps around over many more pushes than its capacity" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    const total: u64 = 3 * EventCapacity + 11;
    const backlog: u64 = 100;
    var out = std_t.mem.zeroes(event_t);
    var sequence: u64 = 0;
    while (sequence < total) : (sequence += 1) {
        const ev = make_event(EventAnnounce, sequence + 1, 1);
        try testing_t.expect(call_push(graph, &ev));
        if (sequence >= backlog) {
            try testing_t.expectEqual(status_t.Ok, call_next_event(graph, TestEpoch, &out));
            try testing_t.expectEqual(sequence - backlog + 1, out.window);
        }
    }
    try testing_t.expectEqual(@as(u32, backlog), graph.ring_len);
    try testing_t.expect(graph.ring_head < EventCapacity);
    var expected_window = total - backlog + 1;
    while (graph.ring_len > 0) : (expected_window += 1) {
        try testing_t.expectEqual(status_t.Ok, call_next_event(graph, TestEpoch, &out));
        try testing_t.expectEqual(expected_window, out.window);
    }
    try testing_t.expectEqual(total + 1, expected_window);
    try testing_t.expectEqual(total, graph.stats.events_queued);
    try testing_t.expectEqual(total, graph.stats.events_drained);
}

test "a full ring refuses a new event and changes nothing" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    var index: u64 = 0;
    while (index < EventCapacity) : (index += 1) {
        const ev = make_event(EventAnnounce, index + 1, 1);
        try testing_t.expect(call_push(graph, &ev));
    }
    try testing_t.expectEqual(@as(u32, 0), call_ring_room(graph));
    const before = graph.stats;
    const extra = make_event(EventAnnounce, 9999, 1);
    try testing_t.expect(!call_push(graph, &extra));
    try testing_t.expectEqual(@as(u32, EventCapacity), graph.ring_len);
    try testing_t.expectEqualSlices(u8, std_t.mem.asBytes(&before), stats_bytes(graph));
    // A merge still works on a full ring.
    var update = make_event(EventUpdate, EventCapacity, 1);
    update.x = 77;
    update.changed_mask = ChangeRect;
    try testing_t.expect(!call_push(graph, &update));
    try testing_t.expectEqual(@as(u32, EventCapacity), graph.ring_len);
    try testing_t.expectEqual(@as(u64, 1), graph.stats.events_coalesced);
    var out = std_t.mem.zeroes(event_t);
    try testing_t.expectEqual(status_t.Ok, call_next_event(graph, TestEpoch, &out));
    try testing_t.expectEqual(@as(u32, 1), call_ring_room(graph));
    try testing_t.expect(call_push(graph, &extra));
    try testing_t.expectEqual(@as(u32, 0), call_ring_room(graph));
}

fn pop_all(graph: *graph_t, events: []event_t) !usize {
    var count: usize = 0;
    var out = std_t.mem.zeroes(event_t);
    while (call_next_event(graph, TestEpoch, &out) == .Ok) : (count += 1) events[count] = out;
    return count;
}

test "announce and update merge keep the announce kind and OR the change mask" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    const announce = make_event(EventAnnounce, 10, 3);
    try testing_t.expect(call_push(graph, &announce));
    var update = make_event(EventUpdate, 10, 3);
    update.x = -5;
    update.y = 6;
    update.width = 321;
    update.height = 123;
    update.state = StateMaximized;
    update.dpi = 144;
    update.min_width = 50;
    update.min_height = 40;
    update.changed_mask = ChangeRect | ChangeTitle;
    update.process_id = 999;
    update.role = RoleDialog;
    update.parent_window = 77;
    const title = "merged title";
    @memcpy(update.title[0..title.len], title);
    update.title_length = title.len;
    try testing_t.expect(!call_push(graph, &update));
    try testing_t.expectEqual(@as(u32, 1), graph.ring_len);
    try testing_t.expectEqual(@as(u64, 1), graph.stats.events_queued);
    try testing_t.expectEqual(@as(u64, 1), graph.stats.events_coalesced);
    try testing_t.expectEqual(@as(u64, 0), graph.stats.updates_queued);

    var events: [4]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 1), try pop_all(graph, &events));
    const merged = events[0];
    try testing_t.expectEqual(EventAnnounce, merged.kind);
    try testing_t.expectEqual(ChangeMask, merged.changed_mask);
    try testing_t.expectEqual(@as(i32, -5), merged.x);
    try testing_t.expectEqual(@as(i32, 6), merged.y);
    try testing_t.expectEqual(@as(u32, 321), merged.width);
    try testing_t.expectEqual(@as(u32, 123), merged.height);
    try testing_t.expectEqual(StateMaximized, merged.state);
    try testing_t.expectEqual(@as(u32, 144), merged.dpi);
    try testing_t.expectEqual(@as(u32, 50), merged.min_width);
    try testing_t.expectEqual(@as(u32, 40), merged.min_height);
    try testing_t.expectEqual(@as(u32, title.len), merged.title_length);
    try testing_t.expectEqualSlices(u8, title, merged.title[0..title.len]);
    // Identity-bound fields are not rewritten.
    try testing_t.expectEqual(@as(u32, 77), merged.process_id);
    try testing_t.expectEqual(RoleToplevel, merged.role);
    try testing_t.expectEqual(@as(u64, 0), merged.parent_window);
    try testing_t.expectEqual(@as(u32, 6), merged.app_key_length);
}

test "update and update merge OR their masks and count only appended updates" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    var first = make_event(EventUpdate, 10, 3);
    first.changed_mask = ChangeRect;
    first.x = 1;
    var second = make_event(EventUpdate, 10, 3);
    second.changed_mask = ChangeTitle | ChangeDpi;
    second.x = 2;
    try testing_t.expect(call_push(graph, &first));
    try testing_t.expect(!call_push(graph, &second));
    var events: [4]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 1), try pop_all(graph, &events));
    try testing_t.expectEqual(EventUpdate, events[0].kind);
    try testing_t.expectEqual(ChangeRect | ChangeTitle | ChangeDpi, events[0].changed_mask);
    try testing_t.expectEqual(@as(i32, 2), events[0].x);
    try testing_t.expectEqual(@as(u64, 1), graph.stats.updates_queued);
    try testing_t.expectEqual(@as(u64, 1), graph.stats.events_queued);
    try testing_t.expectEqual(@as(u64, 1), graph.stats.events_coalesced);
}

test "events never merge across a retire intent" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    const announce = make_event(EventAnnounce, 10, 3);
    var retire = make_event(EventRetireIntent, 10, 3);
    retire.reason = ReasonDestroyed;
    var update = make_event(EventUpdate, 10, 3);
    update.changed_mask = ChangeRect;
    try testing_t.expect(call_push(graph, &announce));
    try testing_t.expect(call_push(graph, &retire));
    try testing_t.expect(call_push(graph, &update));
    // A second update merges into the newest one (after the retire), never into the announce.
    var again = make_event(EventUpdate, 10, 3);
    again.changed_mask = ChangeState;
    try testing_t.expect(!call_push(graph, &again));
    var events: [8]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 3), try pop_all(graph, &events));
    try testing_t.expectEqual(EventAnnounce, events[0].kind);
    try testing_t.expectEqual(ChangeMask, events[0].changed_mask);
    try testing_t.expectEqual(EventRetireIntent, events[1].kind);
    try testing_t.expectEqual(@as(u32, 0), events[1].changed_mask);
    try testing_t.expectEqual(ReasonDestroyed, events[1].reason);
    try testing_t.expectEqual(EventUpdate, events[2].kind);
    try testing_t.expectEqual(ChangeRect | ChangeState, events[2].changed_mask);
    try testing_t.expectEqual(@as(u64, 1), graph.stats.events_coalesced);
}

test "retire intents are always appended and an older update stays unmerged" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    var update = make_event(EventUpdate, 10, 3);
    update.changed_mask = ChangeRect;
    const retire = make_event(EventRetireIntent, 10, 3);
    try testing_t.expect(call_push(graph, &update));
    try testing_t.expect(call_push(graph, &retire));
    // After the retire, an Announce for the same identity (defensive) starts a new event.
    const later = make_event(EventAnnounce, 10, 3);
    try testing_t.expect(call_push(graph, &later));
    try testing_t.expectEqual(@as(u32, 3), graph.ring_len);
    try testing_t.expectEqual(@as(u64, 0), graph.stats.events_coalesced);
    try testing_t.expectEqual(@as(u64, 3), graph.stats.events_queued);
}

test "events of another generation or window do not merge" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    const announce = make_event(EventAnnounce, 10, 3);
    const other_generation = make_event(EventUpdate, 10, 4);
    const other_window = make_event(EventUpdate, 11, 3);
    try testing_t.expect(call_push(graph, &announce));
    try testing_t.expect(call_push(graph, &other_generation));
    try testing_t.expect(call_push(graph, &other_window));
    try testing_t.expectEqual(@as(u32, 3), graph.ring_len);
    try testing_t.expectEqual(@as(u64, 0), graph.stats.events_coalesced);
    // The newest event of the same identity wins even when other identities were queued after it.
    var update = make_event(EventUpdate, 10, 3);
    update.changed_mask = ChangeMinSize;
    update.min_width = 12;
    try testing_t.expect(!call_push(graph, &update));
    try testing_t.expectEqual(@as(u32, 3), graph.ring_len);
    var events: [4]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 3), try pop_all(graph, &events));
    try testing_t.expectEqual(@as(u32, 12), events[0].min_width);
    try testing_t.expectEqual(EventAnnounce, events[0].kind);
    try testing_t.expectEqual(@as(u32, 0), events[1].min_width);
}

test "next_event reports None on an empty queue and leaves the output alone" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    var out = std_t.mem.zeroes(event_t);
    out.window = Unset;
    try testing_t.expectEqual(status_t.None, call_next_event(graph, TestEpoch, &out));
    try testing_t.expectEqual(Unset, out.window);
    try testing_t.expectEqual(@as(u64, 0), graph.stats.events_drained);
    try testing_t.expect(!graph.busy);
    // Wrong epoch (including zero) is rejected before the queue is looked at.
    queue_raw(graph, make_event(EventAnnounce, 5, 1));
    try testing_t.expectEqual(status_t.StaleEpoch, call_next_event(graph, 0, &out));
    try testing_t.expectEqual(status_t.StaleEpoch, call_next_event(graph, TestEpoch + 1, &out));
    try testing_t.expectEqual(@as(u32, 1), graph.ring_len);
    try testing_t.expectEqual(@as(u64, 2), graph.stats.rejected_stale_epoch);
    try testing_t.expectEqual(Unset, out.window);
    try testing_t.expectEqual(status_t.Ok, call_next_event(graph, TestEpoch, &out));
    try testing_t.expectEqual(@as(u64, 5), out.window);
}

const sink_probe_t = struct {
    graph: *graph_t,
    calls: u32 = 0,
    fail_on_call: u32 = 0,
    seen: [8]u64 = [_]u64{0} ** 8,
    reentry_read: ?status_t = null,
    reentry_next: ?status_t = null,
    reentry_drain: ?status_t = null,
    busy_inside: bool = false,
};

fn probe_sink(context: ?*anyopaque, ev: [*c]const event_t) callconv(.C) c_int {
    const probe: *sink_probe_t = @ptrCast(@alignCast(context.?));
    probe.seen[probe.calls] = ev.*.window;
    probe.calls += 1;
    probe.busy_inside = probe.graph.busy;
    if (probe.fail_on_call != 0 and probe.calls == probe.fail_on_call) return 1;
    return 0;
}

fn reentrant_sink(context: ?*anyopaque, ev: [*c]const event_t) callconv(.C) c_int {
    const probe: *sink_probe_t = @ptrCast(@alignCast(context.?));
    var scratch = std_t.mem.zeroes(event_t);
    var delivered: u32 = Unset & 0xFFFF;
    probe.reentry_read = call_enter_read(probe.graph) orelse .Ok;
    probe.reentry_next = call_next_event(probe.graph, TestEpoch, &scratch);
    probe.reentry_drain = call_drain(probe.graph, TestEpoch, probe_sink, null, 1, &delivered);
    probe.seen[probe.calls] = ev.*.window;
    probe.calls += 1;
    return 0;
}

fn fill_ring(graph: *graph_t, count: u64) void {
    var window: u64 = 1;
    while (window <= count) : (window += 1) queue_raw(graph, make_event(EventAnnounce, window, 1));
}

test "drain delivers events oldest first up to max_events" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    fill_ring(graph, 5);
    var probe = sink_probe_t{ .graph = graph };
    var delivered: u32 = 99;
    try testing_t.expectEqual(status_t.Ok, call_drain(graph, TestEpoch, probe_sink, &probe, 3, &delivered));
    try testing_t.expectEqual(@as(u32, 3), delivered);
    try testing_t.expectEqualSlices(u64, &[_]u64{ 1, 2, 3 }, probe.seen[0..3]);
    try testing_t.expect(probe.busy_inside);
    try testing_t.expect(!graph.busy);
    try testing_t.expectEqual(@as(u32, 2), graph.ring_len);
    try testing_t.expectEqual(status_t.Ok, call_drain(graph, TestEpoch, probe_sink, &probe, 10, &delivered));
    try testing_t.expectEqual(@as(u32, 2), delivered);
    try testing_t.expectEqualSlices(u64, &[_]u64{ 4, 5 }, probe.seen[3..5]);
    try testing_t.expectEqual(@as(u64, 5), graph.stats.events_drained);
    try testing_t.expectEqual(@as(u64, 0), graph.stats.sink_failures);
}

test "drain on an empty queue reports None and delivered 0" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    var probe = sink_probe_t{ .graph = graph };
    var delivered: u32 = 99;
    try testing_t.expectEqual(status_t.None, call_drain(graph, TestEpoch, probe_sink, &probe, 4, &delivered));
    try testing_t.expectEqual(@as(u32, 0), delivered);
    try testing_t.expectEqual(@as(u32, 0), probe.calls);
    try testing_t.expect(!graph.busy);
}

test "drain stops at a refused event and redelivers it first next time" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    fill_ring(graph, 5);
    var probe = sink_probe_t{ .graph = graph, .fail_on_call = 3 };
    var delivered: u32 = 99;
    try testing_t.expectEqual(status_t.SinkFailed, call_drain(graph, TestEpoch, probe_sink, &probe, 10, &delivered));
    try testing_t.expectEqual(@as(u32, 2), delivered);
    try testing_t.expectEqual(@as(u32, 3), probe.calls);
    try testing_t.expectEqual(@as(u32, 3), graph.ring_len);
    try testing_t.expectEqual(@as(u64, 2), graph.stats.events_drained);
    try testing_t.expectEqual(@as(u64, 1), graph.stats.sink_failures);
    try testing_t.expect(!graph.busy);
    // The refused event is the first one of the next drain (here refused again at once).
    probe.fail_on_call = probe.calls + 1;
    try testing_t.expectEqual(status_t.SinkFailed, call_drain(graph, TestEpoch, probe_sink, &probe, 10, &delivered));
    try testing_t.expectEqual(@as(u32, 0), delivered);
    try testing_t.expectEqual(@as(u64, 3), probe.seen[probe.calls - 1]);
    try testing_t.expectEqual(@as(u32, 3), graph.ring_len);
    try testing_t.expectEqual(@as(u64, 2), graph.stats.sink_failures);
    probe.fail_on_call = 0;
    try testing_t.expectEqual(status_t.Ok, call_drain(graph, TestEpoch, probe_sink, &probe, 10, &delivered));
    try testing_t.expectEqual(@as(u32, 3), delivered);
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
}

test "drain rejects bad arguments after the common rules and changes nothing" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    fill_ring(graph, 2);
    var probe = sink_probe_t{ .graph = graph };
    var delivered: u32 = 99;
    try testing_t.expectEqual(status_t.InvalidArgument, call_drain(graph, TestEpoch, probe_sink, &probe, 0, &delivered));
    try testing_t.expectEqual(status_t.InvalidArgument, call_drain(graph, TestEpoch, null, &probe, 1, &delivered));
    try testing_t.expectEqual(status_t.StaleEpoch, call_drain(graph, 0, null, &probe, 0, &delivered));
    try testing_t.expectEqual(@as(u32, 99), delivered);
    try testing_t.expectEqual(@as(u32, 2), graph.ring_len);
    try testing_t.expectEqual(@as(u32, 0), probe.calls);
    try testing_t.expectEqual(@as(u64, 1), graph.stats.rejected_stale_epoch);
    try testing_t.expect(!graph.busy);
}

test "graph calls made from inside the sink are Reentrant and nothing is delivered twice" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    fill_ring(graph, 3);
    var probe = sink_probe_t{ .graph = graph };
    var delivered: u32 = 99;
    try testing_t.expectEqual(status_t.Ok, call_drain(graph, TestEpoch, reentrant_sink, &probe, 10, &delivered));
    try testing_t.expectEqual(@as(u32, 3), delivered);
    try testing_t.expectEqual(@as(?status_t, status_t.Reentrant), probe.reentry_read);
    try testing_t.expectEqual(@as(?status_t, status_t.Reentrant), probe.reentry_next);
    try testing_t.expectEqual(@as(?status_t, status_t.Reentrant), probe.reentry_drain);
    try testing_t.expectEqualSlices(u64, &[_]u64{ 1, 2, 3 }, probe.seen[0..3]);
    try testing_t.expectEqual(@as(u64, 3), graph.stats.events_drained);
    try testing_t.expect(!graph.busy);
}

test "check_action is Ok only for the exact Active pair" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 0, .Active, 500, 10);
    _ = inject_entry(graph, 1, .Retiring, 500, 9);
    _ = inject_entry(graph, 2, .Retiring, 600, 4);
    try testing_t.expectEqual(status_t.Ok, call_check_action(graph, TestEpoch, 500, 10));
    try testing_t.expectEqual(@as(u64, 0), graph.stats.rejected_stale);
    // Retiring, older generation, newer generation, unknown window: all Stale.
    try testing_t.expectEqual(status_t.Stale, call_check_action(graph, TestEpoch, 500, 9));
    try testing_t.expectEqual(status_t.Stale, call_check_action(graph, TestEpoch, 600, 4));
    try testing_t.expectEqual(status_t.Stale, call_check_action(graph, TestEpoch, 500, 11));
    try testing_t.expectEqual(status_t.Stale, call_check_action(graph, TestEpoch, 700, 1));
    try testing_t.expectEqual(@as(u64, 4), graph.stats.rejected_stale);
    // Zero arguments are InvalidArgument and bump nothing; a stale epoch outranks them.
    try testing_t.expectEqual(status_t.InvalidArgument, call_check_action(graph, TestEpoch, 0, 10));
    try testing_t.expectEqual(status_t.InvalidArgument, call_check_action(graph, TestEpoch, 500, 0));
    try testing_t.expectEqual(@as(u64, 4), graph.stats.rejected_stale);
    try testing_t.expectEqual(status_t.StaleEpoch, call_check_action(graph, 0, 0, 0));
    try testing_t.expectEqual(status_t.StaleEpoch, call_check_action(graph, TestEpoch + 1, 500, 10));
    try testing_t.expectEqual(@as(u64, 2), graph.stats.rejected_stale_epoch);
    try testing_t.expect(!graph.busy);
    try testing_t.expectEqual(@as(u32, 1), call_count_state(graph, .Active));
}

test "lookup helpers distinguish Active, Retiring and deferred records" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 3, .Active, 500, 10);
    _ = inject_entry(graph, 4, .Retiring, 500, 9);
    _ = inject_entry(graph, 5, .Retiring, 600, 3);
    inject_deferred(graph, 7, 800);
    try testing_t.expectEqual(@as(?usize, 3), call_find_active(graph, 500));
    try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 600));
    try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 800));
    try testing_t.expectEqual(@as(?usize, 3), call_find_exact(graph, 500, 10));
    try testing_t.expectEqual(@as(?usize, 4), call_find_exact(graph, 500, 9));
    try testing_t.expectEqual(@as(?usize, 5), call_find_exact(graph, 600, 3));
    try testing_t.expectEqual(@as(?usize, null), call_find_exact(graph, 500, 8));
    try testing_t.expectEqual(@as(?usize, null), call_find_exact(graph, 0, 0));
    try testing_t.expectEqual(@as(?usize, 7), call_find_deferred(graph, 800));
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 500));
    graph.deferred[7].used = false;
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 800));
    try testing_t.expectEqual(@as(u32, 1), call_count_state(graph, .Active));
    try testing_t.expectEqual(@as(u32, 2), call_count_state(graph, .Retiring));
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
}

test "find_live describes only Active entries" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    const record = inject_entry(graph, 0, .Active, 500, 10);
    record.parent_window = 400;
    record.parent_generation = 7;
    record.role = RoleDialog;
    record.state = StateMinimized | StateResizable;
    record.process_id = 31337;
    _ = inject_entry(graph, 1, .Retiring, 600, 3);

    var live = std_t.mem.zeroes(live_t);
    live.reserved0 = 0xFFFF;
    try testing_t.expectEqual(status_t.Ok, call_find_live(graph, 500, &live));
    try testing_t.expectEqual(@as(u64, 500), live.window);
    try testing_t.expectEqual(@as(u64, 10), live.generation);
    try testing_t.expectEqual(@as(u64, 400), live.parent_window);
    try testing_t.expectEqual(@as(u64, 7), live.parent_generation);
    try testing_t.expectEqual(RoleDialog, live.role);
    try testing_t.expectEqual(StateMinimized | StateResizable, live.state);
    try testing_t.expectEqual(@as(u32, 31337), live.process_id);
    try testing_t.expectEqual(@as(u32, 0), live.reserved0);

    live = std_t.mem.zeroes(live_t);
    live.window = Unset;
    try testing_t.expectEqual(status_t.None, call_find_live(graph, 600, &live));
    try testing_t.expectEqual(status_t.None, call_find_live(graph, 12345, &live));
    try testing_t.expectEqual(status_t.InvalidArgument, call_find_live(graph, 0, &live));
    try testing_t.expectEqual(Unset, live.window);
    graph.busy = true;
    try testing_t.expectEqual(status_t.Reentrant, call_find_live(graph, 500, &live));
    graph.busy = false;
    try testing_t.expect(std_t.mem.allEqual(u8, stats_bytes(graph), 0));
}

test "list_retiring orders by generation, reports the total and truncates" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    inject_entry(graph, 5, .Retiring, 300, 30).retire_reason = ReasonReclassified;
    _ = inject_entry(graph, 2, .Retiring, 100, 10);
    inject_entry(graph, 9, .Retiring, 200, 20).role = RoleMenu;
    _ = inject_entry(graph, 1, .Active, 400, 40);

    var entries: [MaxEntries]retiring_entry_t = undefined;
    var count: u32 = Unset & 0xFFFF;
    try testing_t.expectEqual(status_t.Ok, call_list_retiring(graph, &entries, MaxEntries, &count));
    try testing_t.expectEqual(@as(u32, 3), count);
    try testing_t.expectEqual(@as(u64, 100), entries[0].window);
    try testing_t.expectEqual(@as(u64, 10), entries[0].generation);
    try testing_t.expectEqual(ReasonDestroyed, entries[0].reason);
    try testing_t.expectEqual(@as(u64, 20), entries[1].generation);
    try testing_t.expectEqual(RoleMenu, entries[1].role);
    try testing_t.expectEqual(@as(u64, 30), entries[2].generation);
    try testing_t.expectEqual(ReasonReclassified, entries[2].reason);

    var small: [2]retiring_entry_t = undefined;
    small[1] = .{ .window = Unset, .generation = Unset, .reason = 0, .role = 0 };
    count = 0;
    try testing_t.expectEqual(status_t.Ok, call_list_retiring(graph, &small, 1, &count));
    try testing_t.expectEqual(@as(u32, 3), count);
    try testing_t.expectEqual(@as(u64, 10), small[0].generation);
    try testing_t.expectEqual(Unset, small[1].window);

    count = 0;
    try testing_t.expectEqual(status_t.Ok, call_list_retiring(graph, null, 0, &count));
    try testing_t.expectEqual(@as(u32, 3), count);

    count = 77;
    try testing_t.expectEqual(status_t.InvalidArgument, call_list_retiring(graph, null, 4, &count));
    try testing_t.expectEqual(@as(u32, 77), count);
    graph.busy = true;
    try testing_t.expectEqual(status_t.Reentrant, call_list_retiring(graph, &entries, MaxEntries, &count));
    graph.busy = false;
    try testing_t.expectEqual(@as(u32, 77), count);

    for (&graph.entries) |*entry| entry.state = .Free;
    try testing_t.expectEqual(status_t.Ok, call_list_retiring(graph, &entries, MaxEntries, &count));
    try testing_t.expectEqual(@as(u32, 0), count);
}

test "list_tracked_windows sorts unique Active and Retiring handles and counts exact Free slots" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 9, .Active, 0x500, 30);
    _ = inject_entry(graph, 2, .Retiring, 0x300, 10);
    _ = inject_entry(graph, 5, .Retiring, 0x500, 20);
    _ = inject_entry(graph, 7, .Active, 0x100, 40);
    inject_deferred(graph, 4, 0x050); // Deferred is not part of the tracked export set.

    var windows = [_]u64{0xAAAA} ** (MaxEntries + 2);
    var count: u32 = 0xDEAD;
    var free_count: u32 = 0xBEEF;
    try testing_t.expectEqual(status_t.Ok, call_list_tracked_windows(graph, TestEpoch, &windows, MaxEntries, &count, &free_count));
    try testing_t.expectEqual(@as(u32, 3), count);
    try testing_t.expectEqual(@as(u32, MaxEntries - 4), free_count);
    try testing_t.expectEqualSlices(u64, &[_]u64{ 0x100, 0x300, 0x500 }, windows[0..count]);
    try testing_t.expectEqual(@as(u64, 0xAAAA), windows[count]);
    try testing_t.expectEqual(@as(u64, 0xAAAA), windows[MaxEntries + 1]);

    // A completed exact retirement frees one slot even though the unique HWND count does not
    // use the number of slots as its denominator.
    const proof = proof_t{ .capture_stopped = 1, .pins_drained = 1 };
    try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, 0x300, 10, &proof));
    try testing_t.expectEqual(status_t.Ok, call_list_tracked_windows(graph, TestEpoch, &windows, MaxEntries, &count, &free_count));
    try testing_t.expectEqual(@as(u32, 2), count);
    try testing_t.expectEqual(@as(u32, MaxEntries - 3), free_count);
    try testing_t.expectEqualSlices(u64, &[_]u64{ 0x100, 0x500 }, windows[0..count]);
}

test "list_tracked_windows handles empty and full entry arrays without touching output tails" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    var windows = [_]u64{0xCAFE} ** (MaxEntries + 1);
    var count: u32 = 11;
    var free_count: u32 = 12;
    try testing_t.expectEqual(status_t.Ok, call_list_tracked_windows(graph, TestEpoch, &windows, MaxEntries, &count, &free_count));
    try testing_t.expectEqual(@as(u32, 0), count);
    try testing_t.expectEqual(@as(u32, MaxEntries), free_count);
    for (windows) |window| try testing_t.expectEqual(@as(u64, 0xCAFE), window);
    try testing_t.expectEqual(@as(u64, 0xCAFE), windows[0]);
    try testing_t.expectEqual(@as(u64, 0xCAFE), windows[MaxEntries]);

    for (0..MaxEntries) |index| {
        _ = inject_entry(graph, index, if (index % 2 == 0) .Active else .Retiring, 0x1000 + index, index + 1);
    }
    try testing_t.expectEqual(status_t.Ok, call_list_tracked_windows(graph, TestEpoch, &windows, MaxEntries, &count, &free_count));
    try testing_t.expectEqual(@as(u32, MaxEntries), count);
    try testing_t.expectEqual(@as(u32, 0), free_count);
    for (windows[0..MaxEntries], 0..) |window, index| try testing_t.expectEqual(@as(u64, 0x1000 + index), window);
    try testing_t.expectEqual(@as(u64, 0xCAFE), windows[MaxEntries]);
}

test "list_tracked_windows guards epoch before arguments and changes no counters or outputs" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 0, .Active, 0x500, 1);
    var windows = [_]u64{0xABCD} ** MaxEntries;
    var count: u32 = 0x1234;
    var free_count: u32 = 0x5678;
    const entries_before = graph.entries;
    const ring_before = graph.ring;
    const stats_before = graph.stats;

    try testing_t.expectEqual(status_t.StaleEpoch, call_list_tracked_windows(graph, TestEpoch + 1, null, 0, null, null));
    try testing_t.expectEqual(status_t.StaleEpoch, call_list_tracked_windows(graph, 0, &windows, 0, &count, &free_count));
    try testing_t.expectEqualSlices(u64, &([_]u64{0xABCD} ** MaxEntries), &windows);
    try testing_t.expectEqual(@as(u32, 0x1234), count);
    try testing_t.expectEqual(@as(u32, 0x5678), free_count);
    try testing_t.expectEqualSlices(u8, std_t.mem.asBytes(&entries_before), std_t.mem.asBytes(&graph.entries));
    try testing_t.expectEqualSlices(u8, std_t.mem.asBytes(&ring_before), std_t.mem.asBytes(&graph.ring));
    try testing_t.expectEqualSlices(u8, std_t.mem.asBytes(&stats_before), std_t.mem.asBytes(&graph.stats));
    try testing_t.expectEqual(@as(u64, 0), graph.stats.rejected_stale_epoch);

    graph.busy = true;
    try testing_t.expectEqual(status_t.Reentrant, call_list_tracked_windows(graph, 0, null, 0, null, null));
    graph.busy = false;
    const saved_thread = graph.owner_thread;
    graph.owner_thread = saved_thread + 1;
    try testing_t.expectEqual(status_t.WrongThread, call_list_tracked_windows(graph, 0, null, 0, null, null));
    graph.owner_thread = saved_thread;
    try testing_t.expectEqual(status_t.InvalidArgument, call_list_tracked_windows(graph, TestEpoch, null, MaxEntries, &count, &free_count));
    try testing_t.expectEqual(status_t.InvalidArgument, call_list_tracked_windows(graph, TestEpoch, &windows, MaxEntries - 1, &count, &free_count));
    try testing_t.expectEqual(status_t.InvalidArgument, call_list_tracked_windows(graph, TestEpoch, &windows, MaxEntries, null, &free_count));
    try testing_t.expectEqual(status_t.InvalidArgument, call_list_tracked_windows(graph, TestEpoch, &windows, MaxEntries, &count, null));
    try testing_t.expectEqual(@as(u32, 0x1234), count);
    try testing_t.expectEqual(@as(u32, 0x5678), free_count);
    try testing_t.expectEqualSlices(u64, &([_]u64{0xABCD} ** MaxEntries), &windows);
    try testing_t.expectEqualSlices(u8, std_t.mem.asBytes(&stats_before), std_t.mem.asBytes(&graph.stats));
}

const ProofOk = proof_t{ .capture_stopped = 1, .pins_drained = 1 };

test "complete_retire releases only the exact Retiring generation" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 0, .Retiring, 500, 9);
    _ = inject_entry(graph, 1, .Active, 500, 10);
    _ = inject_entry(graph, 2, .Retiring, 600, 4);

    // Wrong epoch (even with zero ids) first; counters other than the epoch stay put.
    try testing_t.expectEqual(status_t.StaleEpoch, call_complete_retire(graph, TestEpoch + 1, 500, 9, &ProofOk));
    try testing_t.expectEqual(status_t.StaleEpoch, call_complete_retire(graph, 0, 0, 0, &ProofOk));
    try testing_t.expectEqual(@as(u64, 2), graph.stats.rejected_stale_epoch);
    try testing_t.expectEqual(@as(u32, 2), call_count_state(graph, .Retiring));

    // Zero window or generation.
    try testing_t.expectEqual(status_t.InvalidArgument, call_complete_retire(graph, TestEpoch, 0, 9, &ProofOk));
    try testing_t.expectEqual(status_t.InvalidArgument, call_complete_retire(graph, TestEpoch, 500, 0, &ProofOk));

    // Proof flags: only exactly 1 and 1 counts; the entry stays quarantined.
    const incomplete = [_]proof_t{
        .{ .capture_stopped = 1, .pins_drained = 0 },
        .{ .capture_stopped = 0, .pins_drained = 1 },
        .{ .capture_stopped = 0, .pins_drained = 0 },
        .{ .capture_stopped = 2, .pins_drained = 1 },
        .{ .capture_stopped = 1, .pins_drained = 2 },
        .{ .capture_stopped = 0xFFFF_FFFF, .pins_drained = 0xFFFF_FFFF },
    };
    for (incomplete, 1..) |proof, round| {
        try testing_t.expectEqual(status_t.ProofIncomplete, call_complete_retire(graph, TestEpoch, 500, 9, &proof));
        try testing_t.expectEqual(@as(u64, round), graph.stats.proof_incomplete);
        try testing_t.expectEqual(slot_state_t.Retiring, graph.entries[0].state);
        try testing_t.expectEqual(@as(u64, 9), graph.entries[0].record.generation);
    }
    try testing_t.expectEqual(@as(u64, 0), graph.stats.retire_completed);

    // Stale outranks ProofIncomplete; NotRetiring outranks it too and bumps nothing.
    const bad_proof = proof_t{ .capture_stopped = 0, .pins_drained = 0 };
    try testing_t.expectEqual(status_t.Stale, call_complete_retire(graph, TestEpoch, 500, 8, &bad_proof));
    try testing_t.expectEqual(@as(u64, 1), graph.stats.rejected_stale);
    try testing_t.expectEqual(status_t.NotRetiring, call_complete_retire(graph, TestEpoch, 500, 10, &bad_proof));
    try testing_t.expectEqual(status_t.NotRetiring, call_complete_retire(graph, TestEpoch, 500, 10, &ProofOk));
    try testing_t.expectEqual(@as(u64, 1), graph.stats.rejected_stale);
    try testing_t.expectEqual(@as(u64, 6), graph.stats.proof_incomplete);
    try testing_t.expectEqual(slot_state_t.Active, graph.entries[1].state);

    // Success scrubs and frees exactly that slot; the newer Active generation is untouched.
    try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, 500, 9, &ProofOk));
    try testing_t.expectEqual(slot_state_t.Free, graph.entries[0].state);
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&graph.entries[0].record), 0));
    try testing_t.expectEqual(@as(u64, 1), graph.stats.retire_completed);
    try testing_t.expectEqual(@as(?usize, 1), call_find_active(graph, 500));
    try testing_t.expectEqual(@as(u64, 10), graph.entries[1].record.generation);
    try testing_t.expectEqual(slot_state_t.Retiring, graph.entries[2].state);

    // Duplicate completion and a late stale one while a newer Active entry exists are Stale.
    try testing_t.expectEqual(status_t.Stale, call_complete_retire(graph, TestEpoch, 500, 9, &ProofOk));
    try testing_t.expectEqual(@as(u64, 2), graph.stats.rejected_stale);
    try testing_t.expectEqual(@as(u64, 1), graph.stats.retire_completed);
    try testing_t.expectEqual(slot_state_t.Active, graph.entries[1].state);
    try testing_t.expect(!graph.busy);

    // A freed slot no longer counts as exported.
    try testing_t.expectEqual(@as(u32, 1), call_count_state(graph, .Retiring));
    try testing_t.expectEqual(@as(u32, 1), call_count_state(graph, .Active));
}

test "get_stats fills counters and recomputed gauges" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 0, .Active, 1, 1);
    _ = inject_entry(graph, 7, .Active, 2, 2);
    _ = inject_entry(graph, 8, .Retiring, 3, 3);
    inject_deferred(graph, 0, 50);
    inject_deferred(graph, 9, 51);
    inject_deferred(graph, 10, 52);
    fill_ring(graph, 4);
    graph.refresh_open = true;
    graph.generations_issued = 17;
    graph.stamp_high_water = 4242;
    graph.stats.admitted = 6;
    graph.stats.suspended = 2;
    graph.stats.rejected_stale_epoch = 3;

    var stats = std_t.mem.zeroes(stats_t);
    stats.reserved0 = 0xFFFF;
    try testing_t.expectEqual(status_t.Ok, call_get_stats(graph, &stats));
    try testing_t.expectEqual(TestEpoch, stats.session_epoch);
    try testing_t.expectEqual(@as(u64, 17), stats.generations_issued);
    try testing_t.expectEqual(@as(u64, 4242), stats.stamp_high_water);
    try testing_t.expectEqual(@as(u32, 2), stats.active_count);
    try testing_t.expectEqual(@as(u32, 1), stats.retiring_count);
    try testing_t.expectEqual(@as(u32, 3), stats.deferred_count);
    try testing_t.expectEqual(@as(u32, 4), stats.events_pending);
    try testing_t.expectEqual(@as(u32, 1), stats.refresh_active);
    try testing_t.expectEqual(@as(u32, 0), stats.reserved0);
    try testing_t.expectEqual(@as(u64, 6), stats.admitted);
    try testing_t.expectEqual(@as(u64, 2), stats.suspended);
    try testing_t.expectEqual(@as(u64, 3), stats.rejected_stale_epoch);
    // The stored counters never receive the gauges.
    try testing_t.expectEqual(@as(u32, 0), graph.stats.active_count);
    try testing_t.expectEqual(@as(u64, 0), graph.stats.session_epoch);

    graph.refresh_open = false;
    try testing_t.expectEqual(status_t.Ok, call_get_stats(graph, &stats));
    try testing_t.expectEqual(@as(u32, 0), stats.refresh_active);

    var untouched = std_t.mem.zeroes(stats_t);
    untouched.admitted = Unset;
    graph.busy = true;
    try testing_t.expectEqual(status_t.Reentrant, call_get_stats(graph, &untouched));
    graph.busy = false;
    try testing_t.expectEqual(Unset, untouched.admitted);
}

test "stamp high water is read through the guard and the plain getter" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    var value: u64 = Unset;
    try testing_t.expectEqual(status_t.Ok, call_stamp_high_water(graph, &value));
    try testing_t.expectEqual(@as(u64, 0), value);
    graph.stamp_high_water = 777;
    try testing_t.expectEqual(@as(u64, 777), call_stamp_high_water_of(graph));
    try testing_t.expectEqual(status_t.Ok, call_stamp_high_water(graph, &value));
    try testing_t.expectEqual(@as(u64, 777), value);
    value = Unset;
    graph.busy = true;
    try testing_t.expectEqual(status_t.Reentrant, call_stamp_high_water(graph, &value));
    graph.busy = false;
    try testing_t.expectEqual(Unset, value);
}

test "capacity diagnostic is None without reading the clock when every counter is zero" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    test_clock.now = 5_000_000;
    var diag = std_t.mem.zeroes(diagnostic_t);
    diag.at_capacity = Unset;
    try testing_t.expectEqual(status_t.None, call_take_diag(graph, &diag));
    try testing_t.expectEqual(@as(u32, 0), test_clock.reads);
    try testing_t.expectEqual(Unset, diag.at_capacity);
    try testing_t.expect(!graph.have_diag);
    try testing_t.expect(!graph.busy);
}

test "capacity diagnostic follows the sixty second rate limit" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 0, .Active, 1, 1);
    _ = inject_entry(graph, 1, .Retiring, 2, 2);
    inject_deferred(graph, 0, 9);
    fill_ring(graph, 2);
    var diag = std_t.mem.zeroes(diagnostic_t);

    // First take is immediate, elapsed 0, counters reset, gauges filled.
    test_clock.now = 1000;
    graph.diag_at_capacity = 3;
    graph.diag_deferred_evicted = 4;
    graph.diag_queue_full = 5;
    graph.diag_generation_exhausted = 6;
    try testing_t.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
    try testing_t.expectEqual(@as(u64, 3), diag.at_capacity);
    try testing_t.expectEqual(@as(u64, 4), diag.deferred_evicted);
    try testing_t.expectEqual(@as(u64, 5), diag.queue_full);
    try testing_t.expectEqual(@as(u64, 6), diag.generation_exhausted);
    try testing_t.expectEqual(@as(u64, 0), diag.elapsed_ms);
    try testing_t.expectEqual(@as(u32, 1), diag.active_count);
    try testing_t.expectEqual(@as(u32, 1), diag.retiring_count);
    try testing_t.expectEqual(@as(u32, 1), diag.deferred_count);
    try testing_t.expectEqual(@as(u32, 2), diag.events_pending);
    try testing_t.expect(graph.have_diag);
    try testing_t.expectEqual(@as(u64, 1000), graph.last_diag_ms);
    try testing_t.expectEqual(@as(u64, 0), graph.diag_at_capacity | graph.diag_deferred_evicted | graph.diag_queue_full | graph.diag_generation_exhausted);

    // Counters all zero again: None without a clock read.
    const reads = test_clock.reads;
    try testing_t.expectEqual(status_t.None, call_take_diag(graph, &diag));
    try testing_t.expectEqual(reads, test_clock.reads);

    // Within the interval: None, counters and baseline kept.
    graph.diag_at_capacity = 2;
    test_clock.now = 30000;
    try testing_t.expectEqual(status_t.None, call_take_diag(graph, &diag));
    test_clock.now = 1000 + DiagnosticIntervalMs - 1;
    try testing_t.expectEqual(status_t.None, call_take_diag(graph, &diag));
    try testing_t.expectEqual(@as(u64, 2), graph.diag_at_capacity);
    try testing_t.expectEqual(@as(u64, 1000), graph.last_diag_ms);

    // Backwards clock counts as zero elapsed time and moves nothing.
    test_clock.now = 10;
    try testing_t.expectEqual(status_t.None, call_take_diag(graph, &diag));
    try testing_t.expectEqual(@as(u64, 1000), graph.last_diag_ms);
    try testing_t.expectEqual(@as(u64, 2), graph.diag_at_capacity);

    // Exactly the interval is enough; gauges follow the current state.
    graph.entries[1].state = .Free;
    test_clock.now = 1000 + DiagnosticIntervalMs;
    try testing_t.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
    try testing_t.expectEqual(@as(u64, DiagnosticIntervalMs), diag.elapsed_ms);
    try testing_t.expectEqual(@as(u64, 2), diag.at_capacity);
    try testing_t.expectEqual(@as(u64, 0), diag.queue_full);
    try testing_t.expectEqual(@as(u32, 0), diag.retiring_count);
    try testing_t.expectEqual(@as(u64, 1000 + DiagnosticIntervalMs), graph.last_diag_ms);
    try testing_t.expectEqual(@as(u64, 0), graph.diag_at_capacity);

    // Each counter alone makes the diagnostic due once the interval passes again.
    const fields = [_][]const u8{ "diag_at_capacity", "diag_deferred_evicted", "diag_queue_full", "diag_generation_exhausted" };
    inline for (fields) |name| {
        test_clock.now += DiagnosticIntervalMs;
        @field(graph, name) = 1;
        try testing_t.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
        try testing_t.expectEqual(@as(u64, 0), @field(graph, name));
    }
}

test "capacity diagnostic takes its first sample even at clock zero and guards the clock callback" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    graph.diag_generation_exhausted = 1;
    test_clock.probe = graph;
    var diag = std_t.mem.zeroes(diagnostic_t);
    try testing_t.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
    try testing_t.expectEqual(@as(u32, 1), test_clock.reads);
    try testing_t.expectEqual(@as(?status_t, status_t.Reentrant), test_clock.probe_status);
    try testing_t.expectEqual(@as(u64, 1), diag.generation_exhausted);
    try testing_t.expectEqual(@as(u64, 0), diag.elapsed_ms);
    try testing_t.expect(graph.have_diag);
    try testing_t.expect(!graph.busy);
    test_clock.probe = null;
    graph.busy = true;
    graph.diag_queue_full = 1;
    try testing_t.expectEqual(status_t.Reentrant, call_take_diag(graph, &diag));
    graph.busy = false;
    try testing_t.expectEqual(@as(u64, 1), graph.diag_queue_full);
}

// Planning engine fixtures and tests (layer 2b1).

const lookup_mode_t = enum { Ok, Fail, Invalid, Empty, TooLong, Reenter, MutateCaller };

const lookup_probe_t = struct {
    mode: lookup_mode_t = .Ok,
    calls: u32 = 0,
    last_window: u64 = 0,
    last_pointer: usize = 0,
    last_class_length: u32 = 0,
    graph: ?*graph_t = null,
    caller: ?*facts_t = null,
    reenter_observe: status_t = .Ok,
    reenter_check: status_t = .Ok,
    reenter_note: status_t = .Ok,
    reenter_next: status_t = .Ok,
};

var lookup_probe: lookup_probe_t = .{};

/// Programmable lookup: key is "a-<window hex>-<call number>" unless the mode says otherwise.
fn probe_lookup(context: ?*anyopaque, facts: [*c]const facts_t, key: [*c]u8, capacity: u32, length: [*c]u32) callconv(.C) c_int {
    _ = context;
    lookup_probe.calls += 1;
    lookup_probe.last_window = facts.*.window;
    lookup_probe.last_pointer = @intFromPtr(facts);
    lookup_probe.last_class_length = facts.*.class_length;
    switch (lookup_probe.mode) {
        .Fail => return 7,
        .Invalid => {
            const text = "x-bad";
            @memcpy(key[0..text.len], text);
            length.* = text.len;
            return 0;
        },
        .Empty => {
            length.* = 0;
            return 0;
        },
        .TooLong => {
            length.* = capacity + 1;
            return 0;
        },
        .Reenter => {
            const graph = lookup_probe.graph.?;
            var other = fixture_facts();
            var obs = std_t.mem.zeroes(observation_t);
            var ev = std_t.mem.zeroes(event_t);
            lookup_probe.reenter_observe = call_observe(graph, TestEpoch, &other, &obs);
            lookup_probe.reenter_check = call_check_action(graph, TestEpoch, 1, 1);
            lookup_probe.reenter_note = call_note_destroyed(graph, TestEpoch, 1, 1, 1, &obs);
            lookup_probe.reenter_next = call_next_event(graph, TestEpoch, &ev);
        },
        .MutateCaller => {
            const caller = lookup_probe.caller.?;
            caller.title_length = 0xFFFF_FFFF;
            caller.class_length = 0;
            caller.width = 0;
        },
        .Ok => {},
    }
    const written = std_t.fmt.bufPrint(key[0..capacity], "a-{x}-{d}", .{ facts.*.window, lookup_probe.calls }) catch return 1;
    length.* = @intCast(written.len);
    return 0;
}

fn probe_graph(allocator: std_t.mem.Allocator) !*graph_t {
    lookup_probe = .{};
    test_clock = .{};
    var config = test_config(TestEpoch);
    config.app_key_lookup = probe_lookup;
    const graph = try call_create(allocator, &config);
    lookup_probe.graph = graph;
    return graph;
}

fn expected_key(buffer: []u8, window: u64, call_number: u32) []const u8 {
    return std_t.fmt.bufPrint(buffer, "a-{x}-{d}", .{ window, call_number }) catch unreachable;
}

fn call_observe(graph: *graph_t, epoch: u64, facts: *const facts_t, out: *observation_t) status_t {
    return @call(.never_inline, observe, .{ graph, epoch, facts, out });
}

fn call_observe_locked(graph: *graph_t, facts: *const facts_t, out: *observation_t) status_t {
    return @call(.never_inline, observe_locked, .{ graph, facts, out });
}

fn call_note_destroyed(graph: *graph_t, epoch: u64, window: u64, hint_stamp: u64, event_time_ms: u32, out: *observation_t) status_t {
    return @call(.never_inline, note_destroyed, .{ graph, epoch, window, hint_stamp, event_time_ms, out });
}

fn call_collect_cascade(graph: *const graph_t, root: usize, out: *cascade_t) void {
    @call(.never_inline, collect_cascade, .{ graph, root, out });
}

fn call_cascade_has_window(graph: *const graph_t, cascade: *const cascade_t, window: u64) bool {
    return @call(.never_inline, cascade_has_window, .{ graph, cascade, window });
}

fn call_event_from_record(graph: *const graph_t, rec: *const record_t, kind: u32, mask: u32, reason: u32) event_t {
    return @call(.never_inline, event_from_record, .{ graph, rec, kind, mask, reason });
}

fn call_retire_cascade(graph: *graph_t, cascade: *const cascade_t, root_reason: u32, tally: *tally_t) void {
    @call(.never_inline, retire_cascade, .{ graph, cascade, root_reason, tally });
}

fn call_drop_deferred(graph: *graph_t, slot: usize, tally: *tally_t) void {
    @call(.never_inline, drop_deferred, .{ graph, slot, tally });
}

fn call_defer_insert(graph: *graph_t, rec: *const record_t, seen: bool, tally: *tally_t) void {
    @call(.never_inline, defer_insert, .{ graph, rec, seen, tally });
}

fn call_redefer(graph: *graph_t, cascade: *const cascade_t, admitted: *const [MaxEntries]bool, tally: *tally_t) void {
    @call(.never_inline, redefer_descendants, .{ graph, cascade, admitted, tally });
}

fn call_mark_seen(graph: *graph_t, window: u64) void {
    @call(.never_inline, mark_seen, .{ graph, window });
}

fn call_chain_cycle(graph: *const graph_t, window: u64, parent: u64) bool {
    return @call(.never_inline, deferred_chain_cycle, .{ graph, window, parent });
}

fn call_record_from_facts(facts: *const facts_t, t64: u64, role: u32, parent_window: u64) record_t {
    return @call(.never_inline, record_from_facts, .{ facts, t64, role, parent_window });
}

fn call_classify_public(facts: *const facts_t, uac_mode: u32, out: *c_t.interop_window_verdict_t) status_t {
    return @call(.never_inline, classify_public, .{ facts, uac_mode, out });
}

fn call_bump_by(counter: *u64, amount: u64) void {
    @call(.never_inline, bump_by, .{ counter, amount });
}

fn call_merge_target(graph: *graph_t, window: u64, generation: u64) ?*event_t {
    return @call(.never_inline, ring_merge_target, .{ graph, window, generation });
}

fn window_facts(window: u64, stamp: u64) facts_t {
    var facts = fixture_facts();
    facts.window = window;
    facts.root = window;
    facts.stamp = stamp;
    return facts;
}

/// Owned dialog (role Dialog, parent owner) of `owner`.
fn owned_facts(window: u64, owner: u64, stamp: u64) facts_t {
    var facts = window_facts(window, stamp);
    facts.owner = owner;
    facts.owner_root = owner;
    return facts;
}

fn sentinel_observation() observation_t {
    var obs: observation_t = undefined;
    @memset(std_t.mem.asBytes(&obs), 0xA5);
    return obs;
}

fn state_digest(graph: *const graph_t) u64 {
    var hasher = std_t.hash.Wyhash.init(0);
    hasher.update(std_t.mem.asBytes(&graph.entries));
    hasher.update(std_t.mem.asBytes(&graph.deferred));
    hasher.update(std_t.mem.asBytes(&graph.ring));
    hasher.update(std_t.mem.asBytes(&graph.ring_head));
    hasher.update(std_t.mem.asBytes(&graph.ring_len));
    hasher.update(std_t.mem.asBytes(&graph.generations_issued));
    hasher.update(std_t.mem.asBytes(&graph.next_seq));
    hasher.update(std_t.mem.asBytes(&graph.stamp_high_water));
    hasher.update(std_t.mem.asBytes(&graph.tick_base));
    hasher.update(std_t.mem.asBytes(&graph.tick_clock_at_base));
    return hasher.final();
}

/// Structural invariants of the graph; called after every operation of the scenario tests.
fn check_invariants(graph: *const graph_t) !void {
    var exported: u32 = 0;
    for (&graph.entries, 0..) |*entry, index| {
        if (entry.state == .Free) continue;
        exported += 1;
        const rec = &entry.record;
        try testing_t.expect(rec.window != 0);
        try testing_t.expect(rec.generation != 0 and rec.generation <= graph.generations_issued);
        try testing_t.expectEqual(@as(u64, 0), rec.seq);
        for (graph.entries[index + 1 ..]) |*other| {
            if (other.state == .Free) continue;
            try testing_t.expect(other.record.generation != rec.generation);
            if (entry.state == .Active and other.state == .Active) try testing_t.expect(other.record.window != rec.window);
        }
        if (entry.state == .Retiring) {
            try testing_t.expect(rec.retire_reason != 0);
            continue;
        }
        try testing_t.expectEqual(@as(u32, 0), rec.retire_reason);
        try testing_t.expect(rec.app_key_length >= MinAppKeyBytes and rec.app_key_length <= MaxAppKeyBytes);
        if (rec.parent_window == 0) {
            try testing_t.expectEqual(@as(u64, 0), rec.parent_generation);
            continue;
        }
        var found = false;
        for (&graph.entries) |*parent| {
            if (parent.state != .Active or parent.record.window != rec.parent_window) continue;
            found = true;
            try testing_t.expectEqual(parent.record.generation, rec.parent_generation);
            try testing_t.expect(parent.record.generation < rec.generation);
        }
        try testing_t.expect(found);
    }
    try testing_t.expect(exported <= MaxEntries);
    for (&graph.deferred, 0..) |*slot, index| {
        if (!slot.used) continue;
        const rec = &slot.record;
        try testing_t.expect(rec.window != 0 and rec.seq != 0);
        try testing_t.expectEqual(@as(u64, 0), rec.generation);
        try testing_t.expect(rec.parent_window != 0 and rec.parent_window != rec.window);
        try testing_t.expect(rec.app_key_length >= MinAppKeyBytes);
        for (&graph.entries) |*entry| {
            if (entry.state == .Active) try testing_t.expect(entry.record.window != rec.window);
        }
        for (graph.deferred[index + 1 ..]) |*other| {
            if (!other.used) continue;
            try testing_t.expect(other.record.seq != rec.seq);
            try testing_t.expect(other.record.window != rec.window);
        }
        var cursor = rec.parent_window;
        var steps: u32 = 0;
        while (true) : (steps += 1) {
            try testing_t.expect(steps <= MaxDeferred);
            var next: ?u64 = null;
            for (&graph.deferred) |*hop| {
                if (hop.used and hop.record.window == cursor) next = hop.record.parent_window;
            }
            cursor = next orelse break;
            try testing_t.expect(cursor != rec.window);
        }
    }
    try testing_t.expect(graph.ring_len <= EventCapacity and graph.ring_head < EventCapacity);
}

/// Offer facts, expect Ok and valid invariants.
fn offer(graph: *graph_t, facts: *const facts_t) !observation_t {
    var obs = sentinel_observation();
    try testing_t.expectEqual(status_t.Ok, call_observe(graph, TestEpoch, facts, &obs));
    try check_invariants(graph);
    return obs;
}

/// Offer facts, expect a non-Ok status, an untouched observation and (unless `mutates`) an untouched graph.
fn offer_fails(graph: *graph_t, facts: *const facts_t, want: status_t) !void {
    var obs = sentinel_observation();
    const before = state_digest(graph);
    try testing_t.expectEqual(want, call_observe(graph, TestEpoch, facts, &obs));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&obs), 0xA5));
    try testing_t.expectEqual(before, state_digest(graph));
    try check_invariants(graph);
}

fn destroy_hint(graph: *graph_t, window: u64, stamp: u64, tick: u32) !observation_t {
    var obs = sentinel_observation();
    try testing_t.expectEqual(status_t.Ok, call_note_destroyed(graph, TestEpoch, window, stamp, tick, &obs));
    try check_invariants(graph);
    return obs;
}

fn next_ev(graph: *graph_t) !event_t {
    var ev = std_t.mem.zeroes(event_t);
    try testing_t.expectEqual(status_t.Ok, call_next_event(graph, TestEpoch, &ev));
    return ev;
}

fn drain_events(graph: *graph_t, events: []event_t) !usize {
    return pop_all(graph, events);
}

fn expect_ev(ev: event_t, kind: u32, window: u64, generation: u64, parent_window: u64, parent_generation: u64, reason: u32) !void {
    try testing_t.expectEqual(kind, ev.kind);
    try testing_t.expectEqual(window, ev.window);
    try testing_t.expectEqual(generation, ev.generation);
    try testing_t.expectEqual(parent_window, ev.parent_window);
    try testing_t.expectEqual(parent_generation, ev.parent_generation);
    try testing_t.expectEqual(reason, ev.reason);
    try testing_t.expectEqual(TestEpoch, ev.session_epoch);
    try testing_t.expectEqual(@as(u32, 0), ev.reserved0);
}

fn expect_obs(obs: observation_t, disposition: u32, role: u32, generation: u64, retired_generation: u64, reason: u32) !void {
    try testing_t.expectEqual(disposition, obs.disposition);
    try testing_t.expectEqual(role, obs.role);
    try testing_t.expectEqual(generation, obs.generation);
    try testing_t.expectEqual(retired_generation, obs.retired_generation);
    try testing_t.expectEqual(reason, obs.reason);
    try testing_t.expectEqual(@as(u32, 0), obs.reserved0);
}

fn expect_counts(obs: observation_t, events: u32, admitted: u32, retired: u32, dropped: u32) !void {
    try testing_t.expectEqual(events, obs.events_queued);
    try testing_t.expectEqual(admitted, obs.entries_admitted);
    try testing_t.expectEqual(retired, obs.entries_retired);
    try testing_t.expectEqual(dropped, obs.deferred_dropped);
}

fn live_generation(graph: *const graph_t, window: u64) u64 {
    var live = std_t.mem.zeroes(live_t);
    if (call_find_live(graph, window, &live) != .Ok) return 0;
    return live.generation;
}

fn current_stats(graph: *const graph_t) stats_t {
    var stats = std_t.mem.zeroes(stats_t);
    std_t.debug.assert(call_get_stats(graph, &stats) == .Ok);
    return stats;
}

test "first admission announces the full payload with the looked-up key" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var facts = window_facts(0x1000, 10);
    facts.captured_ms = 5000;
    facts.x = 5;
    facts.y = -7;
    facts.min_width = 100;
    facts.min_height = 50;
    facts.dpi = 144;
    set_title(&facts, "Doc");
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 1, 0, ReasonNone);
    try expect_counts(obs, 1, 1, 0, 0);
    try testing_t.expectEqual(@as(u64, 0x1000), obs.window);
    try testing_t.expectEqual(@as(u32, 1), lookup_probe.calls);
    try testing_t.expect(lookup_probe.last_pointer != @intFromPtr(&facts));
    try testing_t.expectEqual(@as(u64, 0x1000), lookup_probe.last_window);

    const ev = try next_ev(graph);
    try expect_ev(ev, EventAnnounce, 0x1000, 1, 0, 0, 0);
    try testing_t.expectEqual(@as(i32, 5), ev.x);
    try testing_t.expectEqual(@as(i32, -7), ev.y);
    try testing_t.expectEqual(@as(u32, 640), ev.width);
    try testing_t.expectEqual(@as(u32, 480), ev.height);
    try testing_t.expectEqual(RoleToplevel, ev.role);
    try testing_t.expectEqual(StateResizable | StateMaximizable | StateMinimizable, ev.state);
    try testing_t.expectEqual(@as(u32, 144), ev.dpi);
    try testing_t.expectEqual(@as(u32, 100), ev.min_width);
    try testing_t.expectEqual(@as(u32, 50), ev.min_height);
    try testing_t.expectEqual(@as(u32, 4242), ev.process_id);
    try testing_t.expectEqual(ChangeMask, ev.changed_mask);
    try testing_t.expectEqual(@as(u32, 3), ev.title_length);
    try testing_t.expectEqualStrings("Doc", ev.title[0..3]);
    try testing_t.expect(std_t.mem.allEqual(u8, ev.title[3..], 0));
    var key_buffer: [64]u8 = undefined;
    const key = expected_key(&key_buffer, 0x1000, 1);
    try testing_t.expectEqual(@as(u32, @intCast(key.len)), ev.app_key_length);
    try testing_t.expectEqualStrings(key, ev.app_key[0..key.len]);
    try testing_t.expect(std_t.mem.allEqual(u8, ev.app_key[key.len..], 0));

    const slot = call_find_active(graph, 0x1000).?;
    const rec = &graph.entries[slot].record;
    try testing_t.expectEqual(@as(u64, 10), rec.admit_stamp);
    try testing_t.expectEqual(@as(u64, 10), rec.last_stamp);
    try testing_t.expectEqual(TickBaseStart + 5000, rec.admit_captured_ms);
    try testing_t.expectEqual(TickBaseStart + 5000, rec.last_captured_ms);
    try testing_t.expectEqual(TickBaseStart + 5000, graph.tick_base);
    try testing_t.expectEqual(@as(u64, 10), graph.stamp_high_water);
    try testing_t.expectEqual(@as(u64, 1), graph.generations_issued);
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.admitted);
    try testing_t.expectEqual(@as(u64, 1), stats.events_queued);
    try testing_t.expectEqual(@as(u64, 0), stats.events_coalesced);
    try testing_t.expectEqual(@as(u64, 1), live_generation(graph, 0x1000));
}

test "the lookup receives a stack copy of the facts, never the caller's struct" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var facts = window_facts(0x1000, 10);
    lookup_probe.mode = .MutateCaller;
    lookup_probe.caller = &facts;
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 1, 0, ReasonNone);
    // The callback corrupted the caller's struct; the copy that was validated and stored is intact.
    try testing_t.expectEqual(@as(u32, 0xFFFF_FFFF), facts.title_length);
    try testing_t.expectEqual(@as(u32, 7), lookup_probe.last_class_length);
    const ev = try next_ev(graph);
    try testing_t.expectEqual(@as(u32, 18), ev.title_length);
    try testing_t.expectEqualStrings("Untitled - Notepad", ev.title[0..18]);
    try testing_t.expectEqual(@as(u32, 640), ev.width);
    try testing_t.expectEqual(@as(u32, 640), graph.entries[call_find_active(graph, 0x1000).?].record.width);
}

fn admit_top(graph: *graph_t, window: u64, stamp: u64) !observation_t {
    var facts = window_facts(window, stamp);
    const obs = try offer(graph, &facts);
    try testing_t.expectEqual(DispositionAdmitted, obs.disposition);
    return obs;
}

fn admit_dialog(graph: *graph_t, window: u64, owner: u64, stamp: u64) !observation_t {
    var facts = owned_facts(window, owner, stamp);
    return offer(graph, &facts);
}

const mutation_t = struct {
    name: []const u8,
    mask: u32,
    apply: *const fn (*facts_t) void,
};

fn mutate_x(facts: *facts_t) void {
    facts.x += 1;
}
fn mutate_y(facts: *facts_t) void {
    facts.y -= 1;
}
fn mutate_width(facts: *facts_t) void {
    facts.width += 1;
}
fn mutate_height(facts: *facts_t) void {
    facts.height += 1;
}
fn mutate_iconic(facts: *facts_t) void {
    facts.flags |= FactIconic;
}
fn mutate_zoomed(facts: *facts_t) void {
    facts.flags |= FactZoomed;
}
fn mutate_style(facts: *facts_t) void {
    facts.style &= ~StyleThickFrame;
}
fn mutate_dpi(facts: *facts_t) void {
    facts.dpi = 192;
}
fn mutate_title(facts: *facts_t) void {
    set_title(facts, "Renamed");
}
fn mutate_title_empty(facts: *facts_t) void {
    set_title(facts, "");
}
fn mutate_title_same_length(facts: *facts_t) void {
    set_title(facts, "Untitled - Notepaf");
}
fn mutate_min_width(facts: *facts_t) void {
    facts.min_width = 10;
}
fn mutate_min_height(facts: *facts_t) void {
    facts.min_height = 10;
}
fn mutate_everything(facts: *facts_t) void {
    mutate_x(facts);
    mutate_iconic(facts);
    mutate_dpi(facts);
    mutate_title(facts);
    mutate_min_width(facts);
}

const Mutations = [_]mutation_t{
    .{ .name = "x", .mask = ChangeRect, .apply = mutate_x },
    .{ .name = "y", .mask = ChangeRect, .apply = mutate_y },
    .{ .name = "width", .mask = ChangeRect, .apply = mutate_width },
    .{ .name = "height", .mask = ChangeRect, .apply = mutate_height },
    .{ .name = "iconic", .mask = ChangeState, .apply = mutate_iconic },
    .{ .name = "zoomed", .mask = ChangeState, .apply = mutate_zoomed },
    .{ .name = "thick frame", .mask = ChangeState, .apply = mutate_style },
    .{ .name = "dpi", .mask = ChangeDpi, .apply = mutate_dpi },
    .{ .name = "title", .mask = ChangeTitle, .apply = mutate_title },
    .{ .name = "title emptied", .mask = ChangeTitle, .apply = mutate_title_empty },
    .{ .name = "title same length", .mask = ChangeTitle, .apply = mutate_title_same_length },
    .{ .name = "min width", .mask = ChangeMinSize, .apply = mutate_min_width },
    .{ .name = "min height", .mask = ChangeMinSize, .apply = mutate_min_height },
    .{ .name = "everything", .mask = ChangeRect | ChangeState | ChangeDpi | ChangeTitle | ChangeMinSize, .apply = mutate_everything },
};

test "each fact group maps to its change bit in a queued Update" {
    for (Mutations) |mutation| {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x1000, 10);
        _ = try next_ev(graph);
        var facts = window_facts(0x1000, 11);
        facts.captured_ms = 1100;
        mutation.apply(&facts);
        const obs = try offer(graph, &facts);
        testing_t.expectEqual(DispositionUpdated, obs.disposition) catch |err| {
            std_t.debug.print("mutation failed: {s}\n", .{mutation.name});
            return err;
        };
        try expect_obs(obs, DispositionUpdated, RoleToplevel, 1, 0, ReasonNone);
        try expect_counts(obs, 1, 0, 0, 0);
        const ev = try next_ev(graph);
        try expect_ev(ev, EventUpdate, 0x1000, 1, 0, 0, 0);
        testing_t.expectEqual(mutation.mask, ev.changed_mask) catch |err| {
            std_t.debug.print("mutation mask failed: {s}\n", .{mutation.name});
            return err;
        };
        // The update carries the full new payload, not just the changed group.
        try testing_t.expectEqual(facts.x, ev.x);
        try testing_t.expectEqual(facts.y, ev.y);
        try testing_t.expectEqual(facts.width, ev.width);
        try testing_t.expectEqual(facts.height, ev.height);
        try testing_t.expectEqual(derive_state(&facts), ev.state);
        try testing_t.expectEqual(facts.dpi, ev.dpi);
        try testing_t.expectEqual(facts.min_width, ev.min_width);
        try testing_t.expectEqual(facts.min_height, ev.min_height);
        try testing_t.expectEqual(facts.title_length, ev.title_length);
        try testing_t.expectEqualSlices(u8, title_bytes(&facts), ev.title[0..ev.title_length]);
        try testing_t.expect(ev.app_key_length >= MinAppKeyBytes);
        const stats = current_stats(graph);
        try testing_t.expectEqual(@as(u64, 1), stats.updates_queued);
        try testing_t.expectEqual(@as(u64, 11), stats.stamp_high_water);
        const record = &graph.entries[call_find_active(graph, 0x1000).?].record;
        try testing_t.expectEqual(@as(u64, 10), record.admit_stamp);
        try testing_t.expectEqual(@as(u64, 11), record.last_stamp);
        try testing_t.expectEqual(TickBaseStart + 1100, record.last_captured_ms);
        try testing_t.expectEqual(derive_state(&facts), record.state);
    }
}

test "updates coalesce into a pending Announce or Update and OR their masks" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x1000, 10);
    // First change merges into the unconsumed Announce.
    var facts = window_facts(0x1000, 11);
    facts.x = 99;
    var obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionUpdated, RoleToplevel, 1, 0, ReasonNone);
    try expect_counts(obs, 0, 0, 0, 0);
    try testing_t.expectEqual(@as(u32, 1), graph.ring_len);
    try testing_t.expectEqual(@as(i32, 99), graph.ring[graph.ring_head].x);
    try testing_t.expectEqual(EventAnnounce, graph.ring[graph.ring_head].kind);
    try testing_t.expectEqual(ChangeMask, graph.ring[graph.ring_head].changed_mask);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).events_coalesced);
    var ev = try next_ev(graph);
    try testing_t.expectEqual(@as(i32, 99), ev.x);
    // After the drain a new change appends an Update; the next one merges into it.
    facts = window_facts(0x1000, 12);
    facts.x = 99;
    facts.dpi = 120;
    obs = try offer(graph, &facts);
    try expect_counts(obs, 1, 0, 0, 0);
    facts = window_facts(0x1000, 13);
    facts.x = 99;
    facts.dpi = 120;
    set_title(&facts, "T");
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionUpdated, RoleToplevel, 1, 0, ReasonNone);
    try expect_counts(obs, 0, 0, 0, 0);
    try testing_t.expectEqual(@as(u32, 1), graph.ring_len);
    ev = try next_ev(graph);
    try expect_ev(ev, EventUpdate, 0x1000, 1, 0, 0, 0);
    try testing_t.expectEqual(ChangeDpi | ChangeTitle, ev.changed_mask);
    try testing_t.expectEqual(@as(u32, 1), ev.title_length);
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 2), stats.events_coalesced);
    try testing_t.expectEqual(@as(u64, 2), stats.events_queued);
    try testing_t.expectEqual(@as(u64, 1), stats.updates_queued);
}

test "an identical offer is Unchanged, queues nothing and still advances stamp and tick" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x1000, 10);
    _ = try next_ev(graph);
    var facts = window_facts(0x1000, 10);
    var obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 1, 0, ReasonNone);
    try expect_counts(obs, 0, 0, 0, 0);
    // Equal stamps are accepted; a later stamp and tick move the record forward.
    facts = window_facts(0x1000, 20);
    facts.captured_ms = 7000;
    // Bytes past the stated lengths and the enabled flag are not part of the comparison.
    facts.title[facts.title_length] = 'Z';
    facts.flags &= ~FactEnabled;
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 1, 0, ReasonNone);
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
    const record = &graph.entries[call_find_active(graph, 0x1000).?].record;
    try testing_t.expectEqual(@as(u64, 20), record.last_stamp);
    try testing_t.expectEqual(TickBaseStart + 7000, record.last_captured_ms);
    try testing_t.expectEqual(@as(u64, 10), record.admit_stamp);
    try testing_t.expectEqual(@as(u64, 20), graph.stamp_high_water);
    try testing_t.expectEqual(TickBaseStart + 7000, graph.tick_base);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).admitted);
    try testing_t.expectEqual(@as(u32, 1), lookup_probe.calls);
}

test "an older tick advances the record but never moves the tick base backwards" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var facts = window_facts(0x1000, 10);
    facts.captured_ms = 9000;
    _ = try offer(graph, &facts);
    try testing_t.expectEqual(TickBaseStart + 9000, graph.tick_base);
    facts.stamp = 11;
    facts.captured_ms = 8500;
    _ = try offer(graph, &facts);
    try testing_t.expectEqual(TickBaseStart + 9000, graph.tick_base);
    const record = &graph.entries[call_find_active(graph, 0x1000).?].record;
    try testing_t.expectEqual(TickBaseStart + 8500, record.last_captured_ms);
    try testing_t.expectEqual(TickBaseStart + 9000, record.admit_captured_ms);
    // A failing call (StaleFacts) never moves the base either.
    facts.stamp = 5;
    facts.captured_ms = 60_000;
    try offer_fails(graph, &facts, .StaleFacts);
    try testing_t.expectEqual(TickBaseStart + 9000, graph.tick_base);
    try testing_t.expectEqual(@as(u64, 11), graph.stamp_high_water);
}

test "an Update that cannot be queued returns QueueFull unless a pending event absorbs it" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x1000, 10);
    _ = try next_ev(graph);
    fill_ring(graph, EventCapacity);
    var facts = window_facts(0x1000, 11);
    facts.x = 3;
    try offer_fails(graph, &facts, .QueueFull);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_queue_full);
    try testing_t.expectEqual(@as(u64, 1), graph.diag_queue_full);
    try testing_t.expectEqual(@as(i32, 10), graph.entries[call_find_active(graph, 0x1000).?].record.x);
    // Pending Update for this window absorbs the next change even though the ring is full.
    _ = try next_ev(graph);
    var obs = try offer(graph, &facts);
    try expect_counts(obs, 1, 0, 0, 0);
    try testing_t.expectEqual(@as(u32, EventCapacity), graph.ring_len);
    facts.stamp = 12;
    facts.y = 8;
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionUpdated, RoleToplevel, 1, 0, ReasonNone);
    try expect_counts(obs, 0, 0, 0, 0);
    const ev = graph.ring[(graph.ring_head + graph.ring_len - 1) & (EventCapacity - 1)];
    try testing_t.expectEqual(ChangeRect, ev.changed_mask);
    try testing_t.expectEqual(@as(i32, 8), ev.y);
    // An unchanged offer needs no room at all.
    facts.stamp = 13;
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 1, 0, ReasonNone);
}

test "a child whose parent is absent is deferred, then flushed in age order behind its parent" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var obs = try admit_dialog(graph, 0x200, 0x100, 10);
    try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
    try expect_counts(obs, 0, 0, 0, 0);
    obs = try admit_dialog(graph, 0x300, 0x100, 11);
    try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
    try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u32, 2), lookup_probe.calls);
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
    const first = &graph.deferred[call_find_deferred(graph, 0x200).?];
    try testing_t.expectEqual(@as(u64, 1), first.record.seq);
    try testing_t.expectEqual(@as(u64, 0), first.record.generation);
    try testing_t.expectEqual(@as(u64, 0x100), first.record.parent_window);
    try testing_t.expect(first.seen);
    try testing_t.expectEqual(@as(u64, 2), graph.deferred[call_find_deferred(graph, 0x300).?].record.seq);
    var stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 2), stats.deferred_added);
    try testing_t.expectEqual(@as(u64, 0), stats.admitted);
    try testing_t.expectEqual(@as(u32, 2), stats.deferred_count);

    obs = try admit_top(graph, 0x100, 12);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 1, 0, ReasonNone);
    try expect_counts(obs, 3, 3, 0, 0);
    try testing_t.expectEqual(@as(u32, 3), lookup_probe.calls);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    var events: [8]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 3), try drain_events(graph, &events));
    try expect_ev(events[0], EventAnnounce, 0x100, 1, 0, 0, 0);
    try expect_ev(events[1], EventAnnounce, 0x200, 2, 0x100, 1, 0);
    try expect_ev(events[2], EventAnnounce, 0x300, 3, 0x100, 1, 0);
    try testing_t.expectEqual(RoleDialog, events[1].role);
    var key_buffer: [64]u8 = undefined;
    const kept = expected_key(&key_buffer, 0x200, 1);
    try testing_t.expectEqualStrings(kept, events[1].app_key[0..events[1].app_key_length]);
    try testing_t.expectEqual(ChangeMask, events[1].changed_mask);
    stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 3), stats.admitted);
    try testing_t.expectEqual(@as(u64, 3), stats.events_queued);
    // Flushed children keep the stamp of their last accepted facts as the admission stamp.
    try testing_t.expectEqual(@as(u64, 10), graph.entries[call_find_active(graph, 0x200).?].record.admit_stamp);
    try testing_t.expectEqual(@as(u64, 12), graph.stamp_high_water);
}

test "re-offering a deferred window updates its record in place without a new lookup" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var facts = owned_facts(0x200, 0x100, 10);
    _ = try offer(graph, &facts);
    _ = try admit_dialog(graph, 0x300, 0x100, 11);
    facts = owned_facts(0x200, 0x100, 12);
    facts.captured_ms = 4000;
    set_title(&facts, "second");
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
    try testing_t.expectEqual(@as(u32, 2), lookup_probe.calls);
    const record = &graph.deferred[call_find_deferred(graph, 0x200).?].record;
    try testing_t.expectEqual(@as(u64, 1), record.seq);
    try testing_t.expectEqual(@as(u64, 12), record.last_stamp);
    try testing_t.expectEqual(TickBaseStart + 4000, record.last_captured_ms);
    try testing_t.expectEqual(@as(u64, 0), record.admit_stamp);
    try testing_t.expectEqual(@as(u32, 6), record.title_length);
    try testing_t.expectEqual(@as(u64, 2), current_stats(graph).deferred_added);
    // Older stamps are stale for a deferred record too.
    facts.stamp = 11;
    try offer_fails(graph, &facts, .StaleFacts);
    // When the parent becomes Active the deferred window is admitted from its record's key.
    _ = try admit_top(graph, 0x100, 13);
    var events: [8]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 3), try drain_events(graph, &events));
    try expect_ev(events[1], EventAnnounce, 0x200, 2, 0x100, 1, 0);
    try testing_t.expectEqualStrings("second", events[1].title[0..events[1].title_length]);
}

test "a deferred window offered with an Active parent is admitted from the stored key" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var facts = owned_facts(0x200, 0x100, 10);
    _ = try offer(graph, &facts);
    // The window's owner changed to an already announced window before the parent ever arrived.
    _ = try admit_top(graph, 0x50, 11);
    _ = try next_ev(graph);
    facts = owned_facts(0x200, 0x50, 12);
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleDialog, 2, 0, ReasonNone);
    try expect_counts(obs, 1, 1, 0, 0);
    try testing_t.expectEqual(@as(u32, 2), lookup_probe.calls);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    const ev = try next_ev(graph);
    try expect_ev(ev, EventAnnounce, 0x200, 2, 0x50, 1, 0);
    var key_buffer: [64]u8 = undefined;
    try testing_t.expectEqualStrings(expected_key(&key_buffer, 0x200, 1), ev.app_key[0..ev.app_key_length]);
}

fn discard_events(graph: *graph_t) u32 {
    var ev = std_t.mem.zeroes(event_t);
    var count: u32 = 0;
    while (call_next_event(graph, TestEpoch, &ev) == .Ok) count += 1;
    return count;
}

/// Admit `count` ownerless top-level windows 0x8000, 0x8001, ... and drain their Announce events.
fn fill_graph(graph: *graph_t, count: u32) !void {
    var index: u32 = 0;
    while (index < count) : (index += 1) {
        _ = try admit_top(graph, 0x8000 + index, 100 + index);
        _ = discard_events(graph);
    }
}

fn set_generations_issued(graph: *graph_t, value: u64) void {
    graph.generations_issued = value;
}

/// Like `offer_fails` for statuses that may legitimately change the graph (the Cycle drop).
fn offer_status(graph: *graph_t, facts: *const facts_t, want: status_t) !void {
    var obs = sentinel_observation();
    try testing_t.expectEqual(want, call_observe(graph, TestEpoch, facts, &obs));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&obs), 0xA5));
    try check_invariants(graph);
}

const Permutations = [_][3]usize{
    .{ 0, 1, 2 }, .{ 0, 2, 1 }, .{ 1, 0, 2 }, .{ 1, 2, 0 }, .{ 2, 0, 1 }, .{ 2, 1, 0 },
};

test "a three-deep chain is announced root first whatever the arrival order" {
    for (Permutations) |order| {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        const chain = [3]facts_t{
            window_facts(0x100, 0),
            owned_facts(0x200, 0x100, 0),
            owned_facts(0x300, 0x200, 0),
        };
        var stamp: u64 = 10;
        for (order) |which| {
            var facts = chain[which];
            facts.stamp = stamp;
            stamp += 1;
            _ = try offer(graph, &facts);
        }
        try testing_t.expectEqual(@as(u32, 3), lookup_probe.calls);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
        var events: [8]event_t = undefined;
        try testing_t.expectEqual(@as(usize, 3), try drain_events(graph, &events));
        try expect_ev(events[0], EventAnnounce, 0x100, 1, 0, 0, 0);
        try expect_ev(events[1], EventAnnounce, 0x200, 2, 0x100, 1, 0);
        try expect_ev(events[2], EventAnnounce, 0x300, 3, 0x200, 2, 0);
        try testing_t.expectEqual(@as(u64, 3), graph.generations_issued);
    }
}

test "flushed children are admitted breadth first in deferral age order" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    const arrivals = [_]facts_t{
        owned_facts(0x302, 0x202, 10),
        owned_facts(0x301, 0x201, 11),
        owned_facts(0x202, 0x100, 12),
        owned_facts(0x201, 0x100, 13),
    };
    for (arrivals) |facts| {
        const obs = try offer(graph, &facts);
        try testing_t.expectEqual(DispositionDeferred, obs.disposition);
    }
    const obs = try admit_top(graph, 0x100, 14);
    try expect_counts(obs, 5, 5, 0, 0);
    var events: [8]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 5), try drain_events(graph, &events));
    try expect_ev(events[0], EventAnnounce, 0x100, 1, 0, 0, 0);
    try expect_ev(events[1], EventAnnounce, 0x202, 2, 0x100, 1, 0);
    try expect_ev(events[2], EventAnnounce, 0x201, 3, 0x100, 1, 0);
    try expect_ev(events[3], EventAnnounce, 0x302, 4, 0x202, 2, 0);
    try expect_ev(events[4], EventAnnounce, 0x301, 5, 0x201, 3, 0);
}

test "owner cycles through deferred records return Cycle and drop only the offered window's record" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    // Self ownership.
    var facts = owned_facts(0x100, 0x100, 10);
    try offer_fails(graph, &facts, .Cycle);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_cycle);
    try testing_t.expectEqual(@as(u32, 0), lookup_probe.calls);
    // Two-window cycle A -> B -> A.
    facts = owned_facts(0x100, 0x200, 11);
    var obs = try offer(graph, &facts);
    try testing_t.expectEqual(DispositionDeferred, obs.disposition);
    facts = owned_facts(0x200, 0x100, 12);
    try offer_fails(graph, &facts, .Cycle);
    try testing_t.expectEqual(@as(u32, 1), call_count_deferred(graph));
    // Three-window cycle A -> B -> C -> A.
    facts = owned_facts(0x200, 0x300, 13);
    obs = try offer(graph, &facts);
    try testing_t.expectEqual(DispositionDeferred, obs.disposition);
    facts = owned_facts(0x300, 0x100, 14);
    try offer_fails(graph, &facts, .Cycle);
    try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u64, 3), current_stats(graph).rejected_cycle);
    // A window with its own deferred record: Cycle drops that record (the documented mutation).
    facts = owned_facts(0x400, 0x900, 15);
    _ = try offer(graph, &facts);
    try testing_t.expectEqual(@as(u32, 3), call_count_deferred(graph));
    facts = owned_facts(0x400, 0x400, 16);
    try offer_status(graph, &facts, .Cycle);
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x400));
    try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
    var stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 4), stats.rejected_cycle);
    try testing_t.expectEqual(@as(u64, 1), stats.deferred_dropped);
    try testing_t.expectEqual(@as(u64, 0), stats.deferred_evicted);
    // A self-owned window that classification excludes is reported as excluded, not as a cycle.
    facts = owned_facts(0x500, 0x500, 17);
    facts.root = 0x501;
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionExcluded, 0, 0, 0, ReasonChild);
    stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 4), stats.rejected_cycle);
}

test "a live window offered with an owner cycle stays unchanged" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = discard_events(graph);
    var facts = owned_facts(0x100, 0x100, 11);
    try offer_fails(graph, &facts, .Cycle);
    // Via a deferred chain: 0x300 waits for 0x100; 0x100 is made to depend on 0x300.
    facts = owned_facts(0x300, 0x100, 12);
    const obs = try offer(graph, &facts);
    try testing_t.expectEqual(DispositionAdmitted, obs.disposition);
    _ = discard_events(graph);
    // 0x500 waits for 0x600 which waits for 0x700 (absent): 0x700 owned by 0x500 would close the loop.
    facts = owned_facts(0x500, 0x600, 13);
    _ = try offer(graph, &facts);
    facts = owned_facts(0x600, 0x700, 14);
    _ = try offer(graph, &facts);
    facts = owned_facts(0x700, 0x500, 15);
    try offer_fails(graph, &facts, .Cycle);
    // The stored chain walk gives up after 65 steps: an injected loop that never reaches the window.
    inject_deferred(graph, 40, 0xA00);
    inject_deferred(graph, 41, 0xB00);
    graph.deferred[40].record.parent_window = 0xB00;
    graph.deferred[41].record.parent_window = 0xA00;
    try testing_t.expect(call_chain_cycle(graph, 0xC00, 0xA00));
    try testing_t.expect(!call_chain_cycle(graph, 0xC00, 0x100));
    try testing_t.expect(call_chain_cycle(graph, 0x100, 0x100));
}

test "identity change retires the old generation and its descendants and admits a fresh generation" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = try admit_dialog(graph, 0x200, 0x100, 11);
    _ = discard_events(graph);
    var facts = window_facts(0x100, 12);
    facts.process_id = 9999;
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 3, 1, ReasonIncarnationChanged);
    try expect_counts(obs, 3, 1, 2, 0);
    var events: [8]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 3), try drain_events(graph, &events));
    try expect_ev(events[0], EventRetireIntent, 0x200, 2, 0x100, 1, ReasonParentRetired);
    try expect_ev(events[1], EventRetireIntent, 0x100, 1, 0, 0, ReasonIncarnationChanged);
    try expect_ev(events[2], EventAnnounce, 0x100, 3, 0, 0, 0);
    try testing_t.expectEqual(@as(u32, 0), events[0].changed_mask);
    try testing_t.expectEqual(@as(u32, 4242), events[1].process_id);
    try testing_t.expectEqual(@as(u32, 9999), events[2].process_id);
    try testing_t.expectEqual(@as(u32, 3), lookup_probe.calls);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u32, 2), call_count_state(graph, .Retiring));
    try testing_t.expectEqual(status_t.Stale, call_check_action(graph, TestEpoch, 0x100, 1));
    try testing_t.expectEqual(status_t.Ok, call_check_action(graph, TestEpoch, 0x100, 3));
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 2), stats.retire_intents);
    try testing_t.expectEqual(@as(u64, 0), stats.reclassified);
    // The child is not re-deferred: its next offer admits it under the new parent generation.
    facts = owned_facts(0x200, 0x100, 13);
    const child = try offer(graph, &facts);
    try expect_obs(child, DispositionAdmitted, RoleDialog, 4, 0, ReasonNone);
    const ev = try next_ev(graph);
    try expect_ev(ev, EventAnnounce, 0x200, 4, 0x100, 3, 0);
}

test "identity change of a thread, start time or class also forces a replacement" {
    const mutators = [_]*const fn (*facts_t) void{ mutate_thread, mutate_start_time, mutate_class, mutate_start_unknown };
    for (mutators) |mutator| {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x100, 10);
        _ = discard_events(graph);
        var facts = window_facts(0x100, 11);
        mutator(&facts);
        const obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionAdmitted, RoleToplevel, 2, 1, ReasonIncarnationChanged);
    }
}

fn mutate_thread(facts: *facts_t) void {
    facts.thread_id += 1;
}
fn mutate_start_time(facts: *facts_t) void {
    facts.process_start_time += 1;
}
fn mutate_class(facts: *facts_t) void {
    set_class(facts, "Other");
}
fn mutate_start_unknown(facts: *facts_t) void {
    facts.process_start_time = 0;
}

test "identity change keeps the retirement when the replacement cannot be admitted" {
    // Lookup failure.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x100, 10);
        _ = discard_events(graph);
        lookup_probe.mode = .Fail;
        var facts = window_facts(0x100, 11);
        facts.process_id = 9999;
        var obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonIncarnationChanged);
        try expect_counts(obs, 1, 0, 1, 0);
        try testing_t.expectEqual(@as(u64, 1), current_stats(graph).lookup_failures);
        try testing_t.expectEqual(@as(u32, 1), call_count_state(graph, .Retiring));
        try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 0x100));
        // The next offer retries and now admits.
        lookup_probe.mode = .Ok;
        facts.stamp = 12;
        obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionAdmitted, RoleToplevel, 2, 0, ReasonNone);
    }
    // Invalid key.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x100, 10);
        _ = discard_events(graph);
        lookup_probe.mode = .Invalid;
        var facts = window_facts(0x100, 11);
        facts.thread_id = 1;
        const obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonIncarnationChanged);
        try testing_t.expectEqual(@as(u64, 1), current_stats(graph).app_key_invalid);
    }
    // No free slot: the retiring entry keeps its slot.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        try fill_graph(graph, 63);
        _ = try admit_top(graph, 0x100, 10);
        _ = discard_events(graph);
        var facts = window_facts(0x100, 11);
        facts.process_id = 9999;
        var obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionRetired, 0, 0, 64, ReasonIncarnationChanged);
        const stats = current_stats(graph);
        try testing_t.expectEqual(@as(u64, 1), stats.rejected_at_capacity);
        try testing_t.expectEqual(@as(u64, 1), graph.diag_at_capacity);
        try testing_t.expectEqual(@as(u32, 64), MaxEntries - call_count_state(graph, .Free));
        facts.stamp = 12;
        try offer_fails(graph, &facts, .AtCapacity);
        const proof = proof_t{ .capture_stopped = 1, .pins_drained = 1 };
        try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, 0x100, 64, &proof));
        obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionAdmitted, RoleToplevel, 65, 0, ReasonNone);
    }
    // No generation left.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x100, 10);
        _ = discard_events(graph);
        set_generations_issued(graph, std_t.math.maxInt(u64));
        var facts = window_facts(0x100, 11);
        facts.process_id = 9999;
        const obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonIncarnationChanged);
        const stats = current_stats(graph);
        try testing_t.expectEqual(@as(u64, 1), stats.rejected_generation_exhausted);
        try testing_t.expectEqual(@as(u64, 1), graph.diag_generation_exhausted);
        try testing_t.expectEqual(@as(u64, 0), stats.rejected_at_capacity);
    }
    // Self ownership of the replacement.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x100, 10);
        _ = discard_events(graph);
        var facts = owned_facts(0x100, 0x100, 11);
        facts.process_id = 9999;
        const obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonIncarnationChanged);
        try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_cycle);
        try testing_t.expectEqual(@as(u32, 1), lookup_probe.calls);
    }
    // The replacement's new owner is one of the retired descendants.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x100, 10);
        _ = try admit_dialog(graph, 0x200, 0x100, 11);
        _ = discard_events(graph);
        var facts = owned_facts(0x100, 0x200, 12);
        facts.process_id = 9999;
        const obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonIncarnationChanged);
        try expect_counts(obs, 2, 0, 2, 0);
        try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_cycle);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    }
    // The replacement's new owner leads back to the window through a deferred record.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x100, 10);
        _ = discard_events(graph);
        // A capacity-cut leftover of 0x100: deferred although its parent 0x100 is Active.
        inject_deferred(graph, 0, 0x600);
        graph.deferred[0].record.parent_window = 0x100;
        graph.deferred[0].record.app_key_length = 3;
        @memcpy(graph.deferred[0].record.app_key[0..3], "a-x");
        var facts = owned_facts(0x100, 0x600, 14);
        facts.process_id = 9999;
        const obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 1, ReasonIncarnationChanged);
        try expect_counts(obs, 1, 0, 1, 1);
        try testing_t.expectEqual(@as(u32, 1), call_count_deferred(graph));
        try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x600));
        try testing_t.expectEqual(@as(?usize, 0), call_find_deferred(graph, 0x100));
        try testing_t.expectEqual(@as(u64, 0), current_stats(graph).rejected_cycle);
    }
}

test "identity change whose replacement must wait for its parent is deferred with a fresh key" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = discard_events(graph);
    var facts = owned_facts(0x100, 0x777, 11);
    facts.process_id = 9999;
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 1, ReasonIncarnationChanged);
    try expect_counts(obs, 1, 0, 1, 0);
    try testing_t.expectEqual(@as(u32, 2), lookup_probe.calls);
    const record = &graph.deferred[call_find_deferred(graph, 0x100).?].record;
    var key_buffer: [64]u8 = undefined;
    try testing_t.expectEqualStrings(expected_key(&key_buffer, 0x100, 2), record.app_key[0..record.app_key_length]);
    const ev = try next_ev(graph);
    try expect_ev(ev, EventRetireIntent, 0x100, 1, 0, 0, ReasonIncarnationChanged);
}

test "identity replacement drops the full deferred child chain before flushing" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    // These are legal capacity-cut leftovers: cached descendants can wait on an Active parent.
    inject_deferred(graph, 0, 0x200);
    var child_facts = owned_facts(0x200, 0x100, 2000);
    graph.deferred[0].record = record_from_facts(&child_facts, TickBaseStart + 2000, RoleDialog, 0x100);
    graph.deferred[0].record.seq = 1;
    graph.deferred[0].record.app_key_length = 3;
    @memcpy(graph.deferred[0].record.app_key[0..3], "a-x");
    inject_deferred(graph, 1, 0x300);
    graph.deferred[1].record.parent_window = 0x200;
    graph.deferred[1].record.app_key_length = 3;
    @memcpy(graph.deferred[1].record.app_key[0..3], "a-y");
    _ = discard_events(graph);

    var replacement = window_facts(0x100, 13);
    replacement.process_id = 9999;
    const replaced = try offer(graph, &replacement);
    try expect_obs(replaced, DispositionAdmitted, RoleToplevel, 2, 1, ReasonIncarnationChanged);
    try expect_counts(replaced, 2, 1, 1, 2);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try expect_ev(try next_ev(graph), EventRetireIntent, 0x100, 1, 0, 0, ReasonIncarnationChanged);
    try expect_ev(try next_ev(graph), EventAnnounce, 0x100, 2, 0, 0, 0);

    // Freshly queried children can be admitted under the replacement; cached child facts cannot.
    try expect_obs(try admit_dialog(graph, 0x200, 0x100, 14), DispositionAdmitted, RoleDialog, 3, 0, ReasonNone);
    try expect_obs(try admit_dialog(graph, 0x300, 0x200, 15), DispositionAdmitted, RoleDialog, 4, 0, ReasonNone);
    try testing_t.expectEqual(@as(u32, 3), call_count_state(graph, .Active));
}

test "identity replacement drops deferred children of every Active cascade member" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = try admit_dialog(graph, 0x200, 0x100, 11);
    // A capacity-cut flush can leave deferred children keyed to an Active descendant.
    inject_deferred(graph, 0, 0x300);
    graph.deferred[0].record.parent_window = 0x200;
    graph.deferred[0].record.app_key_length = 3;
    @memcpy(graph.deferred[0].record.app_key[0..3], "a-x");
    _ = discard_events(graph);

    var replacement = window_facts(0x100, 12);
    replacement.process_id = 9999;
    const replaced = try offer(graph, &replacement);
    try expect_obs(replaced, DispositionAdmitted, RoleToplevel, 3, 1, ReasonIncarnationChanged);
    try expect_counts(replaced, 3, 1, 2, 1);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try expect_ev(try next_ev(graph), EventRetireIntent, 0x200, 2, 0x100, 1, ReasonParentRetired);
    try expect_ev(try next_ev(graph), EventRetireIntent, 0x100, 1, 0, 0, ReasonIncarnationChanged);
    try expect_ev(try next_ev(graph), EventAnnounce, 0x100, 3, 0, 0, 0);
}

test "deferred parent identity replacement drops its deferred descendant chain" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    try expect_obs(try admit_dialog(graph, 0x100, 0x900, 10), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
    try expect_obs(try admit_dialog(graph, 0x200, 0x100, 11), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
    try expect_obs(try admit_dialog(graph, 0x300, 0x200, 12), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
    _ = discard_events(graph);

    var replacement = owned_facts(0x100, 0x900, 13);
    replacement.process_id = 9999;
    const replaced = try offer(graph, &replacement);
    try expect_obs(replaced, DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
    try expect_counts(replaced, 0, 0, 0, 3);
    try testing_t.expectEqual(@as(u32, 1), call_count_deferred(graph));
    try testing_t.expect(call_find_deferred(graph, 0x100) != null);
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x200));
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x300));

    _ = try admit_top(graph, 0x900, 14);
    try testing_t.expect(call_find_active(graph, 0x100) != null);
    try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 0x200));
    try expect_obs(try admit_dialog(graph, 0x200, 0x100, 15), DispositionAdmitted, RoleDialog, 3, 0, ReasonNone);
    try expect_obs(try admit_dialog(graph, 0x300, 0x200, 16), DispositionAdmitted, RoleDialog, 4, 0, ReasonNone);
}

test "Deferred-only identity shortfalls drop the old subtree and exclude the replacement" {
    // Capacity shortfall with the replacement now under an Active parent.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        try expect_obs(try admit_dialog(graph, 0x100, 0x900, 10), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        try expect_obs(try admit_dialog(graph, 0x200, 0x100, 11), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        try expect_obs(try admit_dialog(graph, 0x300, 0x200, 12), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        _ = try admit_top(graph, 0x901, 20);
        _ = discard_events(graph);
        try fill_graph(graph, MaxEntries - 1);

        var replacement = owned_facts(0x100, 0x901, 40);
        replacement.process_id = 9999;
        const shortfall_result = try offer(graph, &replacement);
        try expect_obs(shortfall_result, DispositionExcluded, 0, 0, 0, ReasonIncarnationChanged);
        try expect_counts(shortfall_result, 0, 0, 0, 3);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
        try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x100));
        try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x200));
        try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x300));
        try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_at_capacity);
        try testing_t.expectEqual(@as(u32, MaxEntries), call_count_state(graph, .Active));

        try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 0x200));
        try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 0x300));
    }

    // Generation shortfall with the replacement under an Active parent.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        try expect_obs(try admit_dialog(graph, 0x100, 0x900, 10), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        try expect_obs(try admit_dialog(graph, 0x200, 0x100, 11), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        try expect_obs(try admit_dialog(graph, 0x300, 0x200, 12), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        _ = try admit_top(graph, 0x901, 20);
        _ = discard_events(graph);
        set_generations_issued(graph, std_t.math.maxInt(u64));

        var replacement = owned_facts(0x100, 0x901, 40);
        replacement.process_id = 9999;
        const shortfall_result = try offer(graph, &replacement);
        try expect_obs(shortfall_result, DispositionExcluded, 0, 0, 0, ReasonIncarnationChanged);
        try expect_counts(shortfall_result, 0, 0, 0, 3);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
        try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_generation_exhausted);
        try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
    }

    // Lookup failure while the replacement still has an absent parent.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        try expect_obs(try admit_dialog(graph, 0x100, 0x900, 10), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        try expect_obs(try admit_dialog(graph, 0x200, 0x100, 11), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        try expect_obs(try admit_dialog(graph, 0x300, 0x200, 12), DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        _ = discard_events(graph);

        lookup_probe.mode = .Fail;
        var replacement = owned_facts(0x100, 0x901, 40);
        replacement.process_id = 9999;
        const shortfall_result = try offer(graph, &replacement);
        try expect_obs(shortfall_result, DispositionExcluded, 0, 0, 0, ReasonIncarnationChanged);
        try expect_counts(shortfall_result, 0, 0, 0, 3);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
        try testing_t.expectEqual(@as(u64, 1), current_stats(graph).lookup_failures);
        try testing_t.expectEqual(@as(u32, 0), graph.ring_len);

        lookup_probe.mode = .Ok;
        _ = try admit_top(graph, 0x900, 41);
        replacement = owned_facts(0x100, 0x900, 42);
        replacement.process_id = 9999;
        try expect_obs(try offer(graph, &replacement), DispositionAdmitted, RoleDialog, 2, 0, ReasonNone);
        try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 0x200));
        try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 0x300));
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
        try expect_obs(try admit_dialog(graph, 0x200, 0x100, 43), DispositionAdmitted, RoleDialog, 3, 0, ReasonNone);
        try expect_obs(try admit_dialog(graph, 0x300, 0x200, 44), DispositionAdmitted, RoleDialog, 4, 0, ReasonNone);
    }
}

test "accepted destruction and NotExported parents invalidate cached descendants" {
    // A stale destroy hint is not discontinuity evidence; an accepted hint is.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x100, 10);
        inject_deferred(graph, 0, 0x200);
        graph.deferred[0].record.parent_window = 0x100;
        graph.deferred[0].record.app_key_length = 3;
        @memcpy(graph.deferred[0].record.app_key[0..3], "a-x");
        inject_deferred(graph, 1, 0x300);
        graph.deferred[1].record.parent_window = 0x200;
        graph.deferred[1].record.app_key_length = 3;
        @memcpy(graph.deferred[1].record.app_key[0..3], "a-y");
        _ = discard_events(graph);
        try expect_obs(try destroy_hint(graph, 0x100, 9, 999), DispositionUnchanged, RoleToplevel, 1, 0, ReasonNone);
        try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));

        fill_ring(graph, EventCapacity);
        const before = state_digest(graph);
        var rejected = sentinel_observation();
        try testing_t.expectEqual(status_t.QueueFull, call_note_destroyed(graph, TestEpoch, 0x100, 13, 1000, &rejected));
        try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&rejected), 0xA5));
        try testing_t.expectEqual(before, state_digest(graph));
        try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
        _ = discard_events(graph);

        const destroyed = try destroy_hint(graph, 0x100, 13, 1000);
        try expect_obs(destroyed, DispositionRetired, 0, 0, 1, ReasonDestroyed);
        try expect_counts(destroyed, 1, 0, 1, 2);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
        try expect_ev(try next_ev(graph), EventRetireIntent, 0x100, 1, 0, 0, ReasonDestroyed);
        _ = try admit_top(graph, 0x100, 14);
        try testing_t.expectEqual(@as(u32, 1), call_count_state(graph, .Active));
    }

    // An unknown NotExported parent still invalidates records waiting on that numeric HWND.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_dialog(graph, 0x200, 0x100, 10);
        _ = try admit_dialog(graph, 0x300, 0x200, 11);
        var excluded_parent = window_facts(0x100, 12);
        set_class(&excluded_parent, "Shell_TrayWnd");
        const excluded_obs = try offer(graph, &excluded_parent);
        try expect_obs(excluded_obs, DispositionExcluded, 0, 0, 0, ReasonExcludedClass);
        try expect_counts(excluded_obs, 0, 0, 0, 2);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
        _ = try admit_top(graph, 0x100, 13);
        try testing_t.expectEqual(@as(u32, 1), call_count_state(graph, .Active));
    }

    // Excluding a known Active parent invalidates descendants of it as well.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x100, 10);
        _ = try admit_dialog(graph, 0x200, 0x777, 11);
        graph.deferred[call_find_deferred(graph, 0x200).?].record.parent_window = 0x100;
        _ = discard_events(graph);
        var excluded_parent = window_facts(0x100, 12);
        set_class(&excluded_parent, "Shell_TrayWnd");
        fill_ring(graph, EventCapacity);
        try offer_fails(graph, &excluded_parent, .QueueFull);
        try testing_t.expectEqual(@as(u32, 1), call_count_deferred(graph));
        _ = discard_events(graph);
        const excluded_obs = try offer(graph, &excluded_parent);
        try expect_obs(excluded_obs, DispositionRetired, 0, 0, 1, ReasonExcludedClass);
        try expect_counts(excluded_obs, 1, 0, 1, 1);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    }
}

test "refresh commit invalidates seen deferred descendants of unseen Active parents" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    try fill_graph(graph, MaxEntries - 1);
    _ = try admit_top(graph, 0x100, 1000);
    inject_deferred(graph, 0, 0x200);
    var child_facts = owned_facts(0x200, 0x100, 2000);
    graph.deferred[0].record = record_from_facts(&child_facts, TickBaseStart + 2000, RoleDialog, 0x100);
    graph.deferred[0].record.seq = 1;
    graph.deferred[0].record.app_key_length = 3;
    @memcpy(graph.deferred[0].record.app_key[0..3], "a-x");
    _ = discard_events(graph);
    try testing_t.expectEqual(status_t.Ok, call_refresh_begin(graph, TestEpoch));
    var index: u32 = 0;
    while (index < MaxEntries - 1) : (index += 1) {
        try testing_t.expectEqual(status_t.Ok, call_refresh_keep(graph, TestEpoch, 0x8000 + index));
    }
    child_facts.stamp = 2001;
    try refresh_offer_fails(graph, &child_facts, .AtCapacity);
    try testing_t.expect(graph.deferred[call_find_deferred(graph, 0x200).?].seen);

    var retired: u32 = 0;
    fill_ring(graph, EventCapacity);
    const before_queuefull = state_digest(graph);
    try testing_t.expectEqual(status_t.QueueFull, call_refresh_commit(graph, TestEpoch, &retired));
    try testing_t.expectEqual(before_queuefull, state_digest(graph));
    try testing_t.expect(graph.deferred[call_find_deferred(graph, 0x200).?].used);
    _ = discard_events(graph);
    try testing_t.expectEqual(status_t.Ok, call_refresh_commit(graph, TestEpoch, &retired));
    try testing_t.expectEqual(@as(u32, 1), retired);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).deferred_dropped);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try expect_ev(try next_ev(graph), EventRetireIntent, 0x100, MaxEntries, 0, 0, ReasonRefreshUnseen);
    const proof = proof_t{ .capture_stopped = 1, .pins_drained = 1 };
    try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, 0x100, MaxEntries, &proof));
    _ = try admit_top(graph, 0x100, 2001);
    try testing_t.expectEqual(@as(u32, MaxEntries), call_count_state(graph, .Active));
    try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 0x200));
}

test "QueueFull preserves planned deferred drops and replacement retry applies them once" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    inject_deferred(graph, 0, 0x200);
    var child_facts = owned_facts(0x200, 0x100, 2000);
    graph.deferred[0].record = record_from_facts(&child_facts, TickBaseStart + 2000, RoleDialog, 0x100);
    graph.deferred[0].record.seq = 1;
    graph.deferred[0].record.app_key_length = 3;
    @memcpy(graph.deferred[0].record.app_key[0..3], "a-x");
    inject_deferred(graph, 1, 0x300);
    graph.deferred[1].record.parent_window = 0x200;
    graph.deferred[1].record.app_key_length = 3;
    @memcpy(graph.deferred[1].record.app_key[0..3], "a-y");
    _ = discard_events(graph);

    var replacement = window_facts(0x100, 13);
    replacement.process_id = 9999;
    fill_ring(graph, EventCapacity);
    const before = state_digest(graph);
    try offer_fails(graph, &replacement, .QueueFull);
    try testing_t.expectEqual(before, state_digest(graph));
    try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
    _ = discard_events(graph);

    const retried = try offer(graph, &replacement);
    try expect_obs(retried, DispositionAdmitted, RoleToplevel, 2, 1, ReasonIncarnationChanged);
    try expect_counts(retried, 2, 1, 1, 2);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
}

test "QueueFull preserves a Deferred replacement subtree until its admission can commit" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    inject_deferred(graph, 0, 0x200);
    var child_facts = owned_facts(0x200, 0x100, 2000);
    graph.deferred[0].record = record_from_facts(&child_facts, TickBaseStart + 2000, RoleDialog, 0x100);
    graph.deferred[0].record.seq = 1;
    graph.deferred[0].record.app_key_length = 3;
    @memcpy(graph.deferred[0].record.app_key[0..3], "a-x");
    inject_deferred(graph, 1, 0x300);
    graph.deferred[1].record.parent_window = 0x200;
    graph.deferred[1].record.app_key_length = 3;
    @memcpy(graph.deferred[1].record.app_key[0..3], "a-y");
    _ = discard_events(graph);

    var replacement = owned_facts(0x200, 0x100, 2001);
    replacement.process_id = 9999;
    fill_ring(graph, EventCapacity);
    const before = state_digest(graph);
    try offer_fails(graph, &replacement, .QueueFull);
    try testing_t.expectEqual(before, state_digest(graph));
    try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
    _ = discard_events(graph);

    const retried = try offer(graph, &replacement);
    try expect_obs(retried, DispositionAdmitted, RoleDialog, 2, 0, ReasonNone);
    try expect_counts(retried, 1, 1, 0, 2);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try expect_ev(try next_ev(graph), EventAnnounce, 0x200, 2, 0x100, 1, 0);
}

test "identity replacement lookup failure still drops cached descendants" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    inject_deferred(graph, 0, 0x200);
    var child_facts = owned_facts(0x200, 0x100, 2000);
    graph.deferred[0].record = record_from_facts(&child_facts, TickBaseStart + 2000, RoleDialog, 0x100);
    graph.deferred[0].record.seq = 1;
    graph.deferred[0].record.app_key_length = 3;
    @memcpy(graph.deferred[0].record.app_key[0..3], "a-x");
    inject_deferred(graph, 1, 0x300);
    graph.deferred[1].record.parent_window = 0x200;
    graph.deferred[1].record.app_key_length = 3;
    @memcpy(graph.deferred[1].record.app_key[0..3], "a-y");
    _ = discard_events(graph);
    lookup_probe.mode = .Fail;

    var replacement = window_facts(0x100, 13);
    replacement.process_id = 9999;
    const retired = try offer(graph, &replacement);
    try expect_obs(retired, DispositionRetired, 0, 0, 1, ReasonIncarnationChanged);
    try expect_counts(retired, 1, 0, 1, 2);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 0x100));
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).lookup_failures);
}

/// Q=0x50 (g1), P=0x100 (g2), C1=0x200 (g3) and C2=0x300 (g4) owned by P, G=0x400 (g5) owned by C1.
fn build_family(graph: *graph_t) !void {
    _ = try admit_top(graph, 0x50, 10);
    _ = try admit_top(graph, 0x100, 11);
    _ = try admit_dialog(graph, 0x200, 0x100, 12);
    _ = try admit_dialog(graph, 0x300, 0x100, 13);
    _ = try admit_dialog(graph, 0x400, 0x200, 14);
    try testing_t.expectEqual(@as(u32, 5), discard_events(graph));
}

/// P gains the owner Q and loses nothing else: role Dialog, parent Q.
fn reclassify_facts(stamp: u64) facts_t {
    return owned_facts(0x100, 0x50, stamp);
}

test "reclassification retires the cascade descending and re-announces it behind the new parent" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    try build_family(graph);
    const lookups = lookup_probe.calls;
    var facts = reclassify_facts(20);
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionReclassified, RoleDialog, 6, 2, ReasonReclassified);
    try expect_counts(obs, 8, 4, 4, 0);
    try testing_t.expectEqual(lookups, lookup_probe.calls);
    var events: [12]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 8), try drain_events(graph, &events));
    try expect_ev(events[0], EventRetireIntent, 0x400, 5, 0x200, 3, ReasonParentRetired);
    try expect_ev(events[1], EventRetireIntent, 0x300, 4, 0x100, 2, ReasonParentRetired);
    try expect_ev(events[2], EventRetireIntent, 0x200, 3, 0x100, 2, ReasonParentRetired);
    try expect_ev(events[3], EventRetireIntent, 0x100, 2, 0, 0, ReasonReclassified);
    try expect_ev(events[4], EventAnnounce, 0x100, 6, 0x50, 1, 0);
    try expect_ev(events[5], EventAnnounce, 0x200, 7, 0x100, 6, 0);
    try expect_ev(events[6], EventAnnounce, 0x300, 8, 0x100, 6, 0);
    try expect_ev(events[7], EventAnnounce, 0x400, 9, 0x200, 7, 0);
    try testing_t.expectEqual(RoleToplevel, events[3].role);
    try testing_t.expectEqual(RoleDialog, events[4].role);
    // Keys are reused: neither the new parent nor its descendants triggered a lookup.
    var key_buffer: [64]u8 = undefined;
    try testing_t.expectEqualStrings(expected_key(&key_buffer, 0x100, 2), events[4].app_key[0..events[4].app_key_length]);
    try testing_t.expectEqualStrings(expected_key(&key_buffer, 0x200, 3), events[5].app_key[0..events[5].app_key_length]);
    try testing_t.expectEqualStrings(expected_key(&key_buffer, 0x400, 5), events[7].app_key[0..events[7].app_key_length]);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    var retiring: [8]retiring_entry_t = undefined;
    var total: u32 = 0;
    try testing_t.expectEqual(status_t.Ok, call_list_retiring(graph, &retiring, 8, &total));
    try testing_t.expectEqual(@as(u32, 4), total);
    try testing_t.expectEqual(@as(u64, 2), retiring[0].generation);
    try testing_t.expectEqual(ReasonReclassified, retiring[0].reason);
    try testing_t.expectEqual(ReasonParentRetired, retiring[3].reason);
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.reclassified);
    try testing_t.expectEqual(@as(u64, 4), stats.retire_intents);
    try testing_t.expectEqual(@as(u64, 9), stats.admitted);
    try testing_t.expectEqual(@as(u64, 13), stats.events_queued);
    // Admission stamps of re-announced descendants are their last accepted facts.
    try testing_t.expectEqual(@as(u64, 12), graph.entries[call_find_active(graph, 0x200).?].record.admit_stamp);
    try testing_t.expectEqual(@as(u64, 20), graph.entries[call_find_active(graph, 0x100).?].record.admit_stamp);
}

test "reclassifying a window under its own descendant retires the cascade and defers nothing" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = try admit_dialog(graph, 0x200, 0x100, 11);
    _ = discard_events(graph);
    var facts = owned_facts(0x100, 0x200, 12);
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonReclassified);
    try expect_counts(obs, 2, 0, 2, 0);
    var events: [4]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 2), try drain_events(graph, &events));
    try expect_ev(events[0], EventRetireIntent, 0x200, 2, 0x100, 1, ReasonParentRetired);
    try expect_ev(events[1], EventRetireIntent, 0x100, 1, 0, 0, ReasonReclassified);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.rejected_cycle);
    try testing_t.expectEqual(@as(u64, 1), stats.reclassified);
    // A plain deferred-chain cycle on a live window is a Cycle status, not a forced retirement.
    _ = try admit_top(graph, 0x700, 13);
    _ = discard_events(graph);
    inject_deferred(graph, 0, 0x800);
    graph.deferred[0].record.parent_window = 0x700;
    graph.deferred[0].record.app_key_length = 3;
    @memcpy(graph.deferred[0].record.app_key[0..3], "a-x");
    facts = owned_facts(0x700, 0x800, 14);
    try offer_fails(graph, &facts, .Cycle);
    try testing_t.expect(call_find_active(graph, 0x700) != null);
}

test "reclassification toward an unannounced parent defers the window and re-defers its descendants" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = try admit_dialog(graph, 0x200, 0x100, 11);
    _ = try admit_dialog(graph, 0x300, 0x100, 12);
    _ = discard_events(graph);
    var facts = owned_facts(0x100, 0x900, 20);
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 1, ReasonReclassified);
    try expect_counts(obs, 3, 0, 3, 0);
    try testing_t.expectEqual(@as(u32, 3), lookup_probe.calls);
    try testing_t.expectEqual(@as(u32, 3), call_count_deferred(graph));
    const seq_p = graph.deferred[call_find_deferred(graph, 0x100).?].record.seq;
    const seq_c1 = graph.deferred[call_find_deferred(graph, 0x200).?].record.seq;
    const seq_c2 = graph.deferred[call_find_deferred(graph, 0x300).?].record.seq;
    try testing_t.expect(seq_p < seq_c1 and seq_c1 < seq_c2);
    try testing_t.expectEqual(@as(u64, 0x900), graph.deferred[call_find_deferred(graph, 0x100).?].record.parent_window);
    try testing_t.expectEqual(@as(u64, 0x100), graph.deferred[call_find_deferred(graph, 0x200).?].record.parent_window);
    try testing_t.expectEqual(@as(u64, 3), discard_events(graph));
    // The missing owner arrives: owner, window, then its children, each after its parent.
    const root = try admit_top(graph, 0x900, 21);
    try expect_counts(root, 4, 4, 0, 0);
    var events: [6]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 4), try drain_events(graph, &events));
    try expect_ev(events[0], EventAnnounce, 0x900, 4, 0, 0, 0);
    try expect_ev(events[1], EventAnnounce, 0x100, 5, 0x900, 4, 0);
    try expect_ev(events[2], EventAnnounce, 0x200, 6, 0x100, 5, 0);
    try expect_ev(events[3], EventAnnounce, 0x300, 7, 0x100, 5, 0);
    var key_buffer: [64]u8 = undefined;
    try testing_t.expectEqualStrings(expected_key(&key_buffer, 0x100, 1), events[1].app_key[0..events[1].app_key_length]);
}

test "reclassification with one free slot admits the window and leaves its descendants deferred" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x50, 9);
    _ = try admit_top(graph, 0x100, 10);
    _ = try admit_dialog(graph, 0x200, 0x100, 11);
    _ = try admit_dialog(graph, 0x300, 0x100, 12);
    try fill_graph(graph, 59);
    _ = discard_events(graph);
    var facts = reclassify_facts(20);
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionReclassified, RoleDialog, 64, 2, ReasonReclassified);
    try expect_counts(obs, 4, 1, 3, 0);
    try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 2), stats.rejected_at_capacity);
    try testing_t.expectEqual(@as(u64, 2), graph.diag_at_capacity);
    try testing_t.expect(graph.deferred[call_find_deferred(graph, 0x200).?].record.seq < graph.deferred[call_find_deferred(graph, 0x300).?].record.seq);
    // Slots return when the coordinator completes the three retirements; the deferred children come back.
    const proof = proof_t{ .capture_stopped = 1, .pins_drained = 1 };
    const lookups = lookup_probe.calls;
    for ([_]u64{ 2, 3, 4 }, [_]u64{ 0x100, 0x200, 0x300 }) |generation, window| {
        try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, window, generation, &proof));
    }
    facts = owned_facts(0x200, 0x100, 21);
    const child = try offer(graph, &facts);
    try expect_obs(child, DispositionAdmitted, RoleDialog, 65, 0, ReasonNone);
    try testing_t.expectEqual(lookups, lookup_probe.calls);
    const ev = try next_ev(graph);
    try expect_ev(ev, EventRetireIntent, 0x300, 4, 0x100, 2, ReasonParentRetired);
}

test "reclassification with a cut flush keeps the unadmitted descendant deferred" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x50, 9);
    _ = try admit_top(graph, 0x100, 10);
    _ = try admit_dialog(graph, 0x200, 0x100, 11);
    _ = try admit_dialog(graph, 0x300, 0x100, 12);
    try fill_graph(graph, 58);
    _ = discard_events(graph);
    var facts = reclassify_facts(20);
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionReclassified, RoleDialog, 63, 2, ReasonReclassified);
    try expect_counts(obs, 5, 2, 3, 0);
    try testing_t.expectEqual(@as(u32, 1), call_count_deferred(graph));
    try testing_t.expect(call_find_deferred(graph, 0x300) != null);
    try testing_t.expect(call_find_active(graph, 0x200) != null);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_at_capacity);
}

test "reclassification with no free slot degrades to retire-only and retries on the next offer" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x50, 9);
    _ = try admit_top(graph, 0x100, 10);
    _ = try admit_dialog(graph, 0x200, 0x100, 11);
    _ = try admit_dialog(graph, 0x300, 0x100, 12);
    try fill_graph(graph, 60);
    _ = discard_events(graph);
    try testing_t.expectEqual(@as(u32, 64), MaxEntries - call_count_state(graph, .Free));
    var facts = reclassify_facts(20);
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionRetired, 0, 0, 2, ReasonReclassified);
    try expect_counts(obs, 3, 0, 3, 0);
    try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_at_capacity);
    try testing_t.expectEqual(@as(u32, 3), discard_events(graph));
    facts.stamp = 21;
    try offer_fails(graph, &facts, .AtCapacity);
    const proof = proof_t{ .capture_stopped = 1, .pins_drained = 1 };
    for ([_]u64{ 2, 3, 4 }, [_]u64{ 0x100, 0x200, 0x300 }) |generation, window| {
        try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, window, generation, &proof));
    }
    const retry = try offer(graph, &facts);
    try expect_obs(retry, DispositionAdmitted, RoleDialog, 65, 0, ReasonNone);
    try expect_counts(retry, 3, 3, 0, 0);
    var events: [4]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 3), try drain_events(graph, &events));
    try expect_ev(events[0], EventAnnounce, 0x100, 65, 0x50, 1, 0);
    try expect_ev(events[1], EventAnnounce, 0x200, 66, 0x100, 65, 0);
    try expect_ev(events[2], EventAnnounce, 0x300, 67, 0x100, 65, 0);
}

test "a role change without a parent change also reclassifies" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = discard_events(graph);
    var facts = window_facts(0x100, 11);
    facts.ex_style = ExStyleNoActivate;
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionReclassified, RoleTooltip, 2, 1, ReasonReclassified);
    try expect_counts(obs, 2, 1, 1, 0);
    // The class is part of the identity, so a class change is an incarnation change instead.
    facts = window_facts(0x100, 12);
    facts.ex_style = ExStyleNoActivate;
    set_class(&facts, "tooltips_class32");
    const other = try offer(graph, &facts);
    try expect_obs(other, DispositionAdmitted, RoleTooltip, 3, 2, ReasonIncarnationChanged);
}

test "retirement for every non-reclassify cause leaves the descendants retired and not deferred" {
    // Classified as not exported.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x100, 10);
        _ = try admit_dialog(graph, 0x200, 0x100, 11);
        _ = try admit_dialog(graph, 0x300, 0x100, 12);
        _ = discard_events(graph);
        var facts = window_facts(0x100, 13);
        facts.flags &= ~FactVisible;
        const obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonNotVisible);
        try expect_counts(obs, 3, 0, 3, 0);
        var events: [4]event_t = undefined;
        try testing_t.expectEqual(@as(usize, 3), try drain_events(graph, &events));
        try expect_ev(events[0], EventRetireIntent, 0x300, 3, 0x100, 1, ReasonParentRetired);
        try expect_ev(events[1], EventRetireIntent, 0x200, 2, 0x100, 1, ReasonParentRetired);
        try expect_ev(events[2], EventRetireIntent, 0x100, 1, 0, 0, ReasonNotVisible);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
        try testing_t.expectEqual(@as(u64, 0), current_stats(graph).reclassified);
        // The child returns through its own next offer, deferred behind the vanished parent.
        const child = try admit_dialog(graph, 0x200, 0x100, 14);
        try expect_obs(child, DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
    }
    // Destroy hint.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_top(graph, 0x100, 10);
        _ = try admit_dialog(graph, 0x200, 0x100, 11);
        _ = discard_events(graph);
        const obs = try destroy_hint(graph, 0x100, 12, 2000);
        try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonDestroyed);
        try expect_counts(obs, 2, 0, 2, 0);
        var events: [4]event_t = undefined;
        try testing_t.expectEqual(@as(usize, 2), try drain_events(graph, &events));
        try expect_ev(events[0], EventRetireIntent, 0x200, 2, 0x100, 1, ReasonParentRetired);
        try expect_ev(events[1], EventRetireIntent, 0x100, 1, 0, 0, ReasonDestroyed);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    }
}

test "a window that is not exported is Excluded and drops its deferred record" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var facts = window_facts(0x100, 10);
    set_class(&facts, "Progman");
    var obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionExcluded, 0, 0, 0, ReasonExcludedClass);
    try expect_counts(obs, 0, 0, 0, 0);
    try testing_t.expectEqual(@as(u32, 0), lookup_probe.calls);
    try testing_t.expectEqual(@as(u64, 10), graph.stamp_high_water);
    facts = owned_facts(0x200, 0x900, 11);
    _ = try offer(graph, &facts);
    try testing_t.expectEqual(@as(u32, 1), call_count_deferred(graph));
    facts = owned_facts(0x200, 0x900, 12);
    facts.width = 0;
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionExcluded, 0, 0, 0, ReasonZeroArea);
    try expect_counts(obs, 0, 0, 0, 1);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).deferred_dropped);
}

test "an off-desktop offer is Suspended and changes no entry, record or event" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = try admit_dialog(graph, 0x200, 0x900, 11);
    _ = discard_events(graph);
    var facts = window_facts(0x100, 20);
    facts.captured_ms = 3000;
    facts.flags &= ~(FactOnInputDesktop | FactVisible);
    facts.width = 0;
    const before = state_digest(graph);
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionSuspended, 0, 0, 0, ReasonNone);
    try expect_counts(obs, 0, 0, 0, 0);
    try testing_t.expectEqual(@as(u64, 0x100), obs.window);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).suspended);
    // Only the stamp high-water mark and the tick base advance (an Ok call).
    try testing_t.expect(before != state_digest(graph));
    try testing_t.expectEqual(@as(u64, 20), graph.stamp_high_water);
    try testing_t.expectEqual(TickBaseStart + 3000, graph.tick_base);
    try testing_t.expectEqual(@as(u64, 10), graph.entries[call_find_active(graph, 0x100).?].record.last_stamp);
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
    try testing_t.expectEqual(@as(u32, 1), call_count_deferred(graph));
    // A stale Suspended offer is rejected before classification.
    facts.stamp = 9;
    try offer_fails(graph, &facts, .StaleFacts);
}

test "64 exported entries refuse a 65th window, evict nothing and free a slot only after completion" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    try fill_graph(graph, 64);
    try testing_t.expectEqual(@as(u32, 64), call_count_state(graph, .Active));
    var facts = window_facts(0x100, 500);
    try offer_fails(graph, &facts, .AtCapacity);
    var stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.rejected_at_capacity);
    try testing_t.expectEqual(@as(u64, 1), graph.diag_at_capacity);
    try testing_t.expectEqual(@as(u32, 64), lookup_probe.calls);
    try testing_t.expectEqual(@as(u32, 64), call_count_state(graph, .Active));
    // Deferral consumes no exported capacity.
    var dialog = owned_facts(0x200, 0x900, 501);
    const deferred = try offer(graph, &dialog);
    try testing_t.expectEqual(DispositionDeferred, deferred.disposition);
    // A destroyed window keeps its slot while quarantined.
    const retired = try destroy_hint(graph, 0x8000, 1000, 5000);
    try expect_obs(retired, DispositionRetired, 0, 0, 1, ReasonDestroyed);
    facts.stamp = 502;
    try offer_fails(graph, &facts, .AtCapacity);
    stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 2), stats.rejected_at_capacity);
    const proof = proof_t{ .capture_stopped = 1, .pins_drained = 1 };
    try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, 0x8000, 1, &proof));
    const admitted = try offer(graph, &facts);
    try expect_obs(admitted, DispositionAdmitted, RoleToplevel, 65, 0, ReasonNone);
    try testing_t.expectEqual(@as(u32, 64), MaxEntries - call_count_state(graph, .Free));
}

test "a flush cut by capacity leaves the youngest children deferred and counts them" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var facts = owned_facts(0x201, 0x100, 10);
    _ = try offer(graph, &facts);
    facts = owned_facts(0x202, 0x100, 11);
    _ = try offer(graph, &facts);
    facts = owned_facts(0x301, 0x201, 12);
    _ = try offer(graph, &facts);
    facts = owned_facts(0x302, 0x202, 13);
    _ = try offer(graph, &facts);
    try fill_graph(graph, 62);
    // Two slots remain: the parent and its oldest child.
    const obs = try admit_top(graph, 0x100, 20);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 63, 0, ReasonNone);
    try expect_counts(obs, 2, 2, 0, 0);
    try testing_t.expect(call_find_active(graph, 0x201) != null);
    try testing_t.expect(call_find_active(graph, 0x202) == null);
    try testing_t.expect(call_find_active(graph, 0x301) == null);
    try testing_t.expectEqual(@as(u32, 3), call_count_deferred(graph));
    // Two candidates were in the worklist when the budget ran out (the sibling and the grandchild).
    try testing_t.expectEqual(@as(u64, 2), current_stats(graph).rejected_at_capacity);
    try testing_t.expectEqual(@as(u32, 64), MaxEntries - call_count_state(graph, .Free));
}

test "a 65th deferred record evicts the oldest deferral" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var index: u64 = 0;
    while (index < MaxDeferred) : (index += 1) {
        var facts = owned_facts(0x1000 + index, 0x9000, 10 + index);
        const obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        try expect_counts(obs, 0, 0, 0, 0);
    }
    try testing_t.expectEqual(@as(u32, 64), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u64, 1), graph.deferred[call_find_deferred(graph, 0x1000).?].record.seq);
    var facts = owned_facts(0x2000, 0x9000, 100);
    var obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
    try expect_counts(obs, 0, 0, 0, 1);
    try testing_t.expectEqual(@as(u32, 64), call_count_deferred(graph));
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x1000));
    try testing_t.expectEqual(@as(u64, 65), graph.deferred[call_find_deferred(graph, 0x2000).?].record.seq);
    var stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.deferred_evicted);
    try testing_t.expectEqual(@as(u64, 0), stats.deferred_dropped);
    try testing_t.expectEqual(@as(u64, 65), stats.deferred_added);
    try testing_t.expectEqual(@as(u64, 1), graph.diag_deferred_evicted);
    // The evicted window comes back through its next offer and evicts the then-oldest record.
    facts = owned_facts(0x1000, 0x9000, 101);
    obs = try offer(graph, &facts);
    try expect_counts(obs, 0, 0, 0, 1);
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x1001));
    try testing_t.expectEqual(@as(u64, 66), graph.deferred[call_find_deferred(graph, 0x1000).?].record.seq);
    try testing_t.expectEqual(@as(u32, 66), lookup_probe.calls);
    stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 2), stats.deferred_evicted);
}

test "lookup failure, invalid keys and re-entry publish nothing" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var facts = window_facts(0x100, 10);
    lookup_probe.mode = .Fail;
    try offer_fails(graph, &facts, .LookupFailed);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).lookup_failures);
    // The failing lookup also guards deferral.
    var dialog = owned_facts(0x200, 0x900, 11);
    try offer_fails(graph, &dialog, .LookupFailed);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    for ([_]lookup_mode_t{ .Invalid, .Empty, .TooLong }) |mode| {
        lookup_probe.mode = mode;
        try offer_fails(graph, &facts, .AppKeyInvalid);
    }
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 2), stats.lookup_failures);
    try testing_t.expectEqual(@as(u64, 3), stats.app_key_invalid);
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
    // A callback that calls back into the graph sees Reentrant for every entry point.
    lookup_probe.mode = .Reenter;
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 1, 0, ReasonNone);
    try testing_t.expectEqual(status_t.Reentrant, lookup_probe.reenter_observe);
    try testing_t.expectEqual(status_t.Reentrant, lookup_probe.reenter_check);
    try testing_t.expectEqual(status_t.Reentrant, lookup_probe.reenter_note);
    try testing_t.expectEqual(status_t.Reentrant, lookup_probe.reenter_next);
    try testing_t.expect(!graph.busy);
    try testing_t.expectEqual(@as(u32, 1), graph.ring_len);
}

test "QueueFull changes nothing and the same offer succeeds after a drain" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    fill_ring(graph, EventCapacity);
    var facts = window_facts(0x100, 10);
    try offer_fails(graph, &facts, .QueueFull);
    try testing_t.expectEqual(@as(u32, 0), lookup_probe.calls);
    var stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.rejected_queue_full);
    try testing_t.expectEqual(@as(u64, 1), graph.diag_queue_full);
    _ = try next_ev(graph);
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 1, 0, ReasonNone);
    _ = discard_events(graph);

    // The whole plan must fit: parent plus three deferred children need four events.
    var child = owned_facts(0x201, 0x300, 11);
    _ = try offer(graph, &child);
    child = owned_facts(0x202, 0x300, 12);
    _ = try offer(graph, &child);
    child = owned_facts(0x203, 0x300, 13);
    _ = try offer(graph, &child);
    fill_ring(graph, EventCapacity - 3);
    var parent = window_facts(0x300, 14);
    try offer_fails(graph, &parent, .QueueFull);
    try testing_t.expectEqual(@as(u32, 3), call_count_deferred(graph));
    _ = try next_ev(graph);
    const flushed = try offer(graph, &parent);
    try expect_counts(flushed, 4, 4, 0, 0);
    _ = discard_events(graph);

    // Retirement of a cascade needs one event per member (three children and the parent).
    fill_ring(graph, EventCapacity - 3);
    var hidden = window_facts(0x300, 20);
    hidden.flags &= ~FactVisible;
    try offer_fails(graph, &hidden, .QueueFull);
    var hint = sentinel_observation();
    const before = state_digest(graph);
    try testing_t.expectEqual(status_t.QueueFull, call_note_destroyed(graph, TestEpoch, 0x300, 50, 9000, &hint));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&hint), 0xA5));
    try testing_t.expectEqual(before, state_digest(graph));
    _ = try next_ev(graph);
    const retired = try offer(graph, &hidden);
    try expect_obs(retired, DispositionRetired, 0, 0, 2, ReasonNotVisible);
    try expect_counts(retired, 4, 0, 4, 0);
    stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 4), stats.rejected_queue_full);
    try testing_t.expectEqual(@as(u64, 4), graph.diag_queue_full);
}

test "QueueFull on a forced retirement counts nothing else and keeps the entry live" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = discard_events(graph);
    fill_ring(graph, EventCapacity);
    var facts = owned_facts(0x100, 0x100, 11);
    facts.process_id = 9999;
    try offer_fails(graph, &facts, .QueueFull);
    try testing_t.expectEqual(@as(u64, 0), current_stats(graph).rejected_cycle);
    try testing_t.expect(call_find_active(graph, 0x100) != null);
    var obs = sentinel_observation();
    const before = state_digest(graph);
    try testing_t.expectEqual(status_t.QueueFull, call_note_destroyed(graph, TestEpoch, 0x100, 50, 9000, &obs));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&obs), 0xA5));
    try testing_t.expectEqual(before, state_digest(graph));
    try testing_t.expectEqual(@as(u64, 2), current_stats(graph).rejected_queue_full);
    try testing_t.expectEqual(@as(u64, 2), graph.diag_queue_full);
}

test "generations grow by one, run out cleanly and leave existing entries working" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    set_generations_issued(graph, std_t.math.maxInt(u64) - 2);
    var obs = try admit_top(graph, 0x100, 10);
    try testing_t.expectEqual(std_t.math.maxInt(u64) - 1, obs.generation);
    obs = try admit_top(graph, 0x200, 11);
    try testing_t.expectEqual(std_t.math.maxInt(u64), obs.generation);
    _ = discard_events(graph);
    var facts = window_facts(0x300, 12);
    try offer_fails(graph, &facts, .GenerationExhausted);
    var stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.rejected_generation_exhausted);
    try testing_t.expectEqual(@as(u64, 1), graph.diag_generation_exhausted);
    try testing_t.expectEqual(@as(u64, 0), stats.rejected_at_capacity);
    // Deferral needs no generation, so it still works.
    var dialog = owned_facts(0x400, 0x900, 13);
    const deferred = try offer(graph, &dialog);
    try testing_t.expectEqual(DispositionDeferred, deferred.disposition);
    // Existing entries keep updating, passing checks and retiring.
    facts = window_facts(0x100, 14);
    facts.x = 50;
    obs = try offer(graph, &facts);
    try testing_t.expectEqual(DispositionUpdated, obs.disposition);
    try testing_t.expectEqual(status_t.Ok, call_check_action(graph, TestEpoch, 0x200, std_t.math.maxInt(u64)));
    obs = try destroy_hint(graph, 0x200, 100, 9000);
    try testing_t.expectEqual(DispositionRetired, obs.disposition);
    stats = current_stats(graph);
    try testing_t.expectEqual(std_t.math.maxInt(u64), stats.generations_issued);
}

test "a flush cut by the last generations counts them as generation exhaustion" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var facts = owned_facts(0x201, 0x100, 10);
    _ = try offer(graph, &facts);
    facts = owned_facts(0x202, 0x100, 11);
    _ = try offer(graph, &facts);
    set_generations_issued(graph, std_t.math.maxInt(u64) - 2);
    const obs = try admit_top(graph, 0x100, 12);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, std_t.math.maxInt(u64) - 1, 0, ReasonNone);
    try expect_counts(obs, 2, 2, 0, 0);
    try testing_t.expect(call_find_deferred(graph, 0x202) != null);
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.rejected_generation_exhausted);
    try testing_t.expectEqual(@as(u64, 0), stats.rejected_at_capacity);
}

test "reusing an HWND always yields a fresh generation and a late completion cannot touch it" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = discard_events(graph);
    _ = try destroy_hint(graph, 0x100, 20, 5000);
    var facts = window_facts(0x100, 21);
    facts.process_id = 777;
    var obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 2, 0, ReasonNone);
    // The same tuple after completing the first quarantine is still a new generation.
    const proof = proof_t{ .capture_stopped = 1, .pins_drained = 1 };
    try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, 0x100, 1, &proof));
    try testing_t.expectEqual(status_t.Stale, call_complete_retire(graph, TestEpoch, 0x100, 1, &proof));
    try testing_t.expectEqual(@as(u64, 2), live_generation(graph, 0x100));
    _ = try destroy_hint(graph, 0x100, 30, 6000);
    obs = try admit_top(graph, 0x100, 31);
    try testing_t.expectEqual(@as(u64, 3), obs.generation);
    try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, 0x100, 2, &proof));
    try testing_t.expectEqual(@as(u64, 3), live_generation(graph, 0x100));
    // The stale-facts reference is the Active entry only: a retired window accepts any stamp.
    _ = try destroy_hint(graph, 0x100, 40, 7000);
    facts = window_facts(0x100, 5);
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 4, 0, ReasonNone);
}

test "stale epochs, invalid facts and stale facts are rejected before any state change" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = discard_events(graph);
    var facts = window_facts(0x100, 11);
    facts.x = 4;
    var obs = sentinel_observation();
    var before = state_digest(graph);
    try testing_t.expectEqual(status_t.StaleEpoch, call_observe(graph, TestEpoch + 1, &facts, &obs));
    try testing_t.expectEqual(status_t.StaleEpoch, call_observe(graph, 0, &facts, &obs));
    try testing_t.expectEqual(status_t.StaleEpoch, call_note_destroyed(graph, TestEpoch + 1, 0x100, 50, 9000, &obs));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&obs), 0xA5));
    try testing_t.expectEqual(before, state_digest(graph));
    try testing_t.expectEqual(@as(u64, 3), current_stats(graph).rejected_stale_epoch);
    // Invalid facts win over staleness.
    facts.stamp = 0;
    try offer_fails(graph, &facts, .InvalidFacts);
    facts = window_facts(0x100, 11);
    facts.dpi = 50;
    try offer_fails(graph, &facts, .InvalidFacts);
    facts = window_facts(0x100, 11);
    facts.owner = 0x50;
    facts.flags |= FactOwnerHidden;
    facts.owner_root = 0;
    try offer_fails(graph, &facts, .InvalidFacts);
    try testing_t.expectEqual(@as(u64, 3), current_stats(graph).rejected_invalid_facts);
    // Older stamps are stale; equal stamps are accepted.
    facts = window_facts(0x100, 9);
    try offer_fails(graph, &facts, .StaleFacts);
    facts.stamp = 10;
    obs = try offer(graph, &facts);
    try testing_t.expectEqual(DispositionUnchanged, obs.disposition);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_stale_facts);
    before = state_digest(graph);
    // note_destroyed argument checks.
    obs = sentinel_observation();
    try testing_t.expectEqual(status_t.InvalidArgument, call_note_destroyed(graph, TestEpoch, 0, 50, 9000, &obs));
    try testing_t.expectEqual(status_t.InvalidArgument, call_note_destroyed(graph, TestEpoch, 0x100, 0, 9000, &obs));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&obs), 0xA5));
    try testing_t.expectEqual(before, state_digest(graph));
}

/// One simulated day in milliseconds.
const DayMs: u64 = 86_400_000;

fn admit_at(graph: *graph_t, window: u64, stamp: u64, tick: u32) !observation_t {
    var facts = window_facts(window, stamp);
    facts.captured_ms = tick;
    const obs = try offer(graph, &facts);
    try testing_t.expectEqual(DispositionAdmitted, obs.disposition);
    return obs;
}

test "a genuine destroy hint retires the live entry and advances the high-water marks" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_at(graph, 0x100, 10, 5000);
    _ = discard_events(graph);
    const obs = try destroy_hint(graph, 0x100, 11, 5100);
    try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonDestroyed);
    try expect_counts(obs, 1, 0, 1, 0);
    try testing_t.expectEqual(@as(u64, 0x100), obs.window);
    try testing_t.expectEqual(@as(u64, 11), graph.stamp_high_water);
    try testing_t.expectEqual(TickBaseStart + 5100, graph.tick_base);
    try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 0x100));
    const ev = try next_ev(graph);
    try expect_ev(ev, EventRetireIntent, 0x100, 1, 0, 0, ReasonDestroyed);
    try testing_t.expectEqual(status_t.Stale, call_check_action(graph, TestEpoch, 0x100, 1));
    // Retiring entries are never touched by a second hint.
    const again = try destroy_hint(graph, 0x100, 12, 5200);
    try expect_obs(again, DispositionUnchanged, 0, 0, 0, ReasonNone);
    try expect_counts(again, 0, 0, 0, 0);
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
    try testing_t.expectEqual(@as(u32, 1), call_count_state(graph, .Retiring));
}

test "a destroy hint older than the admitting facts is ignored" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_at(graph, 0x100, 10, 5000);
    _ = discard_events(graph);
    // Stamp clause: equal and lower stamps carry no authority over the admitting facts.
    var obs = try destroy_hint(graph, 0x100, 10, 6000);
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 1, 0, ReasonNone);
    obs = try destroy_hint(graph, 0x100, 9, 6000);
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 1, 0, ReasonNone);
    try testing_t.expectEqual(@as(u64, 10), graph.stamp_high_water);
    // Tick clause: the event happened 1 and 10 ms before the facts that admitted the entry.
    obs = try destroy_hint(graph, 0x100, 11, 4999);
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 1, 0, ReasonNone);
    obs = try destroy_hint(graph, 0x100, 12, 4990);
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 1, 0, ReasonNone);
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
    try testing_t.expect(call_find_active(graph, 0x100) != null);
    try testing_t.expectEqual(@as(u64, 12), graph.stamp_high_water);
    // Equal ticks count as destroyed (documented conservative churn).
    obs = try destroy_hint(graph, 0x100, 13, 5000);
    try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonDestroyed);
}

test "a destroy hint for a deferred record needs both a newer stamp and a non-older tick" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var facts = owned_facts(0x200, 0x900, 10);
    facts.captured_ms = 5000;
    _ = try offer(graph, &facts);
    var obs = try destroy_hint(graph, 0x200, 10, 6000);
    try expect_obs(obs, DispositionUnchanged, 0, 0, 0, ReasonNone);
    try expect_counts(obs, 0, 0, 0, 0);
    obs = try destroy_hint(graph, 0x200, 11, 4999);
    try expect_counts(obs, 0, 0, 0, 0);
    try testing_t.expectEqual(@as(u32, 1), call_count_deferred(graph));
    obs = try destroy_hint(graph, 0x200, 11, 5000);
    try expect_obs(obs, DispositionUnchanged, 0, 0, 0, ReasonNone);
    try expect_counts(obs, 0, 0, 0, 1);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).deferred_dropped);
    // Nothing known about the window at all.
    obs = try destroy_hint(graph, 0x777, 12, 5000);
    try expect_obs(obs, DispositionUnchanged, 0, 0, 0, ReasonNone);
    try expect_counts(obs, 0, 0, 0, 0);
}

test "tick wrap: a destroy across the 32-bit wrap retires, one just before the admission does not" {
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_at(graph, 0x100, 10, 0xFFFF_FFF0);
        const obs = try destroy_hint(graph, 0x100, 11, 0x0000_0010);
        try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonDestroyed);
        try testing_t.expectEqual(@as(u64, 0x1_0000_0010) + TickBaseStart - 0x1_0000_0000, graph.tick_base);
    }
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        _ = try admit_at(graph, 0x100, 10, 0xFFFF_FFF0);
        const obs = try destroy_hint(graph, 0x100, 11, 0xFFFF_FFE0);
        try expect_obs(obs, DispositionUnchanged, RoleToplevel, 1, 0, ReasonNone);
        // A stale hint 10 ms before an admission close to the wrap point.
        _ = try destroy_hint(graph, 0x100, 12, 0xFFFF_FFE6);
        try testing_t.expect(call_find_active(graph, 0x100) != null);
    }
}

test "destroy hints still work 30 and 60 simulated days after the admission" {
    for ([_]u64{ 30, 60 }) |days| {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        const admit_tick: u64 = 0xFFFF_0000;
        _ = try admit_at(graph, 0x100, 10, @truncate(admit_tick));
        _ = try admit_at(graph, 0x200, 11, @truncate(admit_tick));
        // One tick-carrying Ok call per simulated day (the real cadence is 250 ms).
        var day: u64 = 1;
        while (day <= days) : (day += 1) {
            var keepalive = window_facts(0x200, 11 + day);
            keepalive.captured_ms = @truncate(admit_tick + day * DayMs);
            const obs = try offer(graph, &keepalive);
            try testing_t.expectEqual(DispositionUnchanged, obs.disposition);
        }
        try testing_t.expectEqual(TickBaseStart - 0x1_0000 + days * DayMs, graph.tick_base);
        const final_tick: u32 = @truncate(admit_tick + days * DayMs);
        const obs = try destroy_hint(graph, 0x100, 1000, final_tick);
        try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonDestroyed);
        // A hint from the middle of the long interval would be stale; one from 10 ms ago is not.
        const recent = try destroy_hint(graph, 0x200, 1001, final_tick -% 10);
        try expect_obs(recent, DispositionRetired, 0, 0, 2, ReasonDestroyed);
    }
}

test "with a clock that does not advance, a silence longer than half the range still aliases" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_at(graph, 0x100, 10, 0xFFFF_0000);
    // 30 days later but no Ok call in between and a graph clock that did not move (so the elapsed
    // time estimate is 0): the single 32-bit jump reads as negative and is ignored.
    const obs = try destroy_hint(graph, 0x100, 11, @truncate(0xFFFF_0000 + 30 * DayMs));
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 1, 0, ReasonNone);
    // The next refresh-style offer or hint inside the window of validity corrects it.
    const later = try destroy_hint(graph, 0x100, 12, 0xFFFF_0000);
    try expect_obs(later, DispositionRetired, 0, 0, 1, ReasonDestroyed);
}

test "tick_estimate adds the clock time elapsed since the anchor, never goes below the base and saturates" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    graph.tick_base = TickBaseStart + 1000;
    graph.tick_clock_at_base = 400;
    try testing_t.expectEqual(TickBaseStart + 1000, tick_estimate(graph, 400));
    try testing_t.expectEqual(TickBaseStart + 1600, tick_estimate(graph, 1000));
    // A clock that went backwards (or never moved) counts as 0 elapsed.
    try testing_t.expectEqual(TickBaseStart + 1000, tick_estimate(graph, 399));
    try testing_t.expectEqual(TickBaseStart + 1000, tick_estimate(graph, 0));
    graph.tick_base = std_t.math.maxInt(u64) - 5;
    graph.tick_clock_at_base = 0;
    try testing_t.expectEqual(@as(u64, std_t.math.maxInt(u64)), tick_estimate(graph, 100));
}

test "the estimate anchor follows ticks and clock: newer ticks re-anchor, delayed ticks keep the estimate" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    test_clock.now = 100;
    _ = try admit_at(graph, 0x100, 10, 5000);
    try testing_t.expectEqual(TickBaseStart + 5000, graph.tick_base);
    try testing_t.expectEqual(@as(u64, 100), graph.tick_clock_at_base);
    // 500 ms of clock later the estimate is base + 500. A delayed tick 200 ms behind the estimate
    // still raises the base (monotonic max) but the estimate stays where it was.
    test_clock.now = 600;
    var facts = window_facts(0x100, 11);
    facts.captured_ms = 5300;
    _ = try offer(graph, &facts);
    try testing_t.expectEqual(TickBaseStart + 5300, graph.tick_base);
    try testing_t.expectEqual(TickBaseStart + 5500, tick_estimate(graph, 600));
    // A tick 7 ms ahead of the estimate re-anchors the pair at the current clock reading.
    facts.stamp = 12;
    facts.captured_ms = 5507;
    _ = try offer(graph, &facts);
    try testing_t.expectEqual(TickBaseStart + 5507, graph.tick_base);
    try testing_t.expectEqual(@as(u64, 600), graph.tick_clock_at_base);
    // A delayed tick older than the base changes neither value.
    test_clock.now = 700;
    facts.stamp = 13;
    facts.captured_ms = 5000;
    _ = try offer(graph, &facts);
    try testing_t.expectEqual(TickBaseStart + 5507, graph.tick_base);
    try testing_t.expectEqual(@as(u64, 600), graph.tick_clock_at_base);
    // A clock that went backwards re-anchors at the new reading; the base itself never decreases.
    test_clock.now = 50;
    facts.stamp = 14;
    facts.captured_ms = 5600;
    _ = try offer(graph, &facts);
    try testing_t.expectEqual(TickBaseStart + 5600, graph.tick_base);
    try testing_t.expectEqual(@as(u64, 50), graph.tick_clock_at_base);
    test_clock.now = 10;
    _ = try destroy_hint(graph, 0x777, 20, 5000);
    try testing_t.expectEqual(TickBaseStart + 5600, graph.tick_base);
    try testing_t.expectEqual(@as(u64, 10), graph.tick_clock_at_base);
}

test "a rejected call reads the clock but changes neither tick_base nor its anchor" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_at(graph, 0x100, 10, 5000);
    test_clock.now = 9000;
    var stale = window_facts(0x100, 5);
    stale.captured_ms = 9000;
    try offer_fails(graph, &stale, .StaleFacts);
    var obs = sentinel_observation();
    try testing_t.expectEqual(status_t.InvalidArgument, call_note_destroyed(graph, TestEpoch, 0, 20, 9000, &obs));
    try testing_t.expectEqual(TickBaseStart + 5000, graph.tick_base);
    try testing_t.expectEqual(@as(u64, 0), graph.tick_clock_at_base);
}

test "a graph without a now_ms callback unwraps with its default timer and keeps working" {
    var config = test_config(TestEpoch);
    config.now_ms = null;
    config.now_ms_context = null;
    lookup_probe = .{};
    config.app_key_lookup = probe_lookup;
    const graph = try call_create(testing_t.allocator, &config);
    defer call_destroy(graph);
    lookup_probe.graph = graph;
    _ = try admit_at(graph, 0x100, 10, 0xFFFF_FFF0);
    const obs = try destroy_hint(graph, 0x100, 11, 0x0000_0010);
    try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonDestroyed);
    try testing_t.expectEqual(TickBaseStart + 0x10, graph.tick_base);
}

test "after a silence longer than 2^31 ms of the graph clock, current-tick offers repair the base and a genuine destroy hint retires" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    test_clock.now = 5;
    var first = window_facts(0x100, 10);
    first.captured_ms = 1000;
    _ = try offer(graph, &first);
    // 30 days with no graph call at all (a locked guest). The tick domain and the clock both run on.
    const thirty_days: u64 = 30 * DayMs;
    var clock_now: u64 = 5 + thirty_days;
    var tick_now: u64 = 1000 + thirty_days;
    var stamp: u64 = 11;
    var round: u32 = 0;
    while (round < 14_400) : (round += 1) {
        test_clock.now = clock_now;
        var facts = window_facts(0x100, stamp);
        facts.captured_ms = @truncate(tick_now);
        var obs = sentinel_observation();
        try testing_t.expectEqual(status_t.Ok, call_observe(graph, TestEpoch, &facts, &obs));
        try testing_t.expectEqual(DispositionUnchanged, obs.disposition);
        if (round % 1000 == 0) try check_invariants(graph);
        stamp += 1;
        clock_now += 250;
        tick_now += 250;
    }
    // The base follows the true time although 30 days exceed half the 32-bit range.
    try testing_t.expectEqual(TickBaseStart + 1000 + thirty_days + 14_399 * 250, graph.tick_base);
    test_clock.now = clock_now;
    const obs = try destroy_hint(graph, 0x100, stamp, @truncate(tick_now));
    try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonDestroyed);
}

test "the very first tick-carrying call after a long silence is unwrapped by the clock estimate" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_at(graph, 0x100, 10, 0xFFFF_0000);
    test_clock.now = 30 * DayMs;
    // A genuine destroy hint 30 days after the admission: no offer in between, only the clock.
    var obs = try destroy_hint(graph, 0x100, 11, @truncate(0xFFFF_0000 + 30 * DayMs));
    try expect_obs(obs, DispositionRetired, 0, 0, 1, ReasonDestroyed);
    // A hint for a window re-admitted afterwards, older than its admission by 10 ms, stays ignored.
    var facts = window_facts(0x100, 12);
    facts.captured_ms = @truncate(0xFFFF_0000 + 30 * DayMs);
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 2, 0, ReasonNone);
    obs = try destroy_hint(graph, 0x100, 13, @truncate(0xFFFF_0000 + 30 * DayMs - 10));
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 2, 0, ReasonNone);
}

test "estimate unwrap tolerates a clock that runs ahead of the ticks by less than half the range" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_at(graph, 0x100, 10, 5000);
    // The clock ran 20 days but the offered tick is only 2 days after the base: 18 days behind the
    // estimate, inside +-2^31 ms (24.8 days) of it, so the unwrap is still exact.
    test_clock.now = 20 * DayMs;
    var facts = window_facts(0x100, 11);
    facts.captured_ms = @truncate(5000 + 2 * DayMs);
    _ = try offer(graph, &facts);
    try testing_t.expectEqual(TickBaseStart + 5000 + 2 * DayMs, graph.tick_base);
    try testing_t.expectEqual(TickBaseStart + 5000 + 20 * DayMs, tick_estimate(graph, 20 * DayMs));
}

fn fill_toplevels(graph: *graph_t, first: u64, count: u64, stamp: *u64) !void {
    var index: u64 = 0;
    while (index < count) : (index += 1) {
        stamp.* += 1;
        const facts = window_facts(first + index, stamp.*);
        _ = try offer(graph, &facts);
    }
}

/// Build the capacity-cut leftover state: X (0x300) waits for C (0x200); W (0x100) and then C (a
/// dialog of W) are admitted in the last free slots, so X stays deferred with the Active parent C.
fn cut_leftover_state(graph: *graph_t, stamp: *u64) !void {
    var facts = owned_facts(0x300, 0x200, stamp.*);
    var obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
    try fill_toplevels(graph, 0x10000, 62, stamp);
    stamp.* += 1;
    facts = window_facts(0x100, stamp.*);
    obs = try offer(graph, &facts);
    try testing_t.expectEqual(DispositionAdmitted, obs.disposition);
    stamp.* += 1;
    facts = owned_facts(0x200, 0x100, stamp.*);
    obs = try offer(graph, &facts);
    try testing_t.expectEqual(DispositionAdmitted, obs.disposition);
    try testing_t.expectEqual(@as(u32, 64), call_count_state(graph, .Active));
    try testing_t.expect(call_find_deferred(graph, 0x300) != null);
    _ = discard_events(graph);
}

test "a reclassification whose new parent chain re-enters the cascade through a deferred leftover is retire-only" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var stamp: u64 = 10;
    try cut_leftover_state(graph, &stamp);
    const cycles_before = current_stats(graph).rejected_cycle;
    // W (0x100) is now owned by X (0x300); X is deferred with parent C, and C is W's own dialog.
    // Deferring W would close X -> C -> W -> X; the replacement is rejected, the cascade retires.
    stamp += 1;
    const facts = owned_facts(0x100, 0x300, stamp);
    const obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionRetired, 0, 0, 63, ReasonReclassified);
    try expect_counts(obs, 2, 0, 2, 0);
    try testing_t.expectEqual(cycles_before + 1, current_stats(graph).rejected_cycle);
    try testing_t.expect(call_find_active(graph, 0x100) == null);
    try testing_t.expect(call_find_active(graph, 0x200) == null);
    try testing_t.expect(call_find_deferred(graph, 0x100) == null);
    try testing_t.expect(call_find_deferred(graph, 0x200) == null);
    // X keeps waiting for C; no deferred link was added.
    try testing_t.expectEqual(@as(u32, 1), call_count_deferred(graph));
    try testing_t.expect(call_find_deferred(graph, 0x300) != null);
    try check_invariants(graph);
    // The retirement events are the only events: C (child) first, then W.
    var ev = try next_ev(graph);
    try expect_ev(ev, EventRetireIntent, 0x200, 64, 0x100, 63, ReasonParentRetired);
    ev = try next_ev(graph);
    try expect_ev(ev, EventRetireIntent, 0x100, 63, 0, 0, ReasonReclassified);
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
}

test "the cascade rule follows a longer deferred chain and a plain chain to a non-cascade terminal still defers" {
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        var stamp: u64 = 10;
        try cut_leftover_state(graph, &stamp);
        // Y (0x400) waits for X, so the chain Y -> X -> C ends in W's cascade after two deferred hops.
        stamp += 1;
        var facts = owned_facts(0x400, 0x300, stamp);
        var obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        stamp += 1;
        facts = owned_facts(0x100, 0x400, stamp);
        obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionRetired, 0, 0, 63, ReasonReclassified);
        try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
        try check_invariants(graph);
    }
    {
        // Control: the same deferred leftover X -> C, but the reclassified window is a sibling of C.
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        var stamp: u64 = 10;
        try cut_leftover_state(graph, &stamp);
        // Free one slot (a filler is destroyed), then admit 0x500 as a top-level window.
        stamp += 1;
        var obs = try destroy_hint(graph, 0x10000, stamp, 1000);
        try testing_t.expectEqual(DispositionRetired, obs.disposition);
        _ = discard_events(graph);
        const proof = proof_t{ .capture_stopped = 1, .pins_drained = 1 };
        try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, 0x10000, 1, &proof));
        _ = discard_events(graph);
        stamp += 1;
        var facts = window_facts(0x500, stamp);
        obs = try offer(graph, &facts);
        try testing_t.expectEqual(DispositionAdmitted, obs.disposition);
        // 0x500 becomes owned by X: X -> C(Active, not in 0x500's cascade). Deferred, no cycle.
        stamp += 1;
        facts = owned_facts(0x500, 0x300, stamp);
        obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 65, ReasonReclassified);
        try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
        try check_invariants(graph);
    }
}

test "OwnerHidden promotes an owned window and a later flag change reclassifies it" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var facts = owned_facts(0x200, 0x900, 10);
    var obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
    // The observer found the owner never exportable: the window is a top-level now and uses the stored key.
    facts = owned_facts(0x200, 0x900, 11);
    facts.flags |= FactOwnerHidden;
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 1, 0, ReasonNone);
    try testing_t.expectEqual(@as(u32, 1), lookup_probe.calls);
    var ev = try next_ev(graph);
    try expect_ev(ev, EventAnnounce, 0x200, 1, 0, 0, 0);
    // The owner became exportable again: Dialog under an owner that is not announced.
    facts = owned_facts(0x200, 0x900, 12);
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 1, ReasonReclassified);
    try testing_t.expectEqual(@as(u32, 1), lookup_probe.calls);
    ev = try next_ev(graph);
    try expect_ev(ev, EventRetireIntent, 0x200, 1, 0, 0, ReasonReclassified);
    facts = owned_facts(0x200, 0x900, 13);
    facts.flags |= FactOwnerHidden;
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 2, 0, ReasonNone);
    // Flag changes that leave role and parent alone are not changes at all.
    facts = owned_facts(0x300, 0x900, 14);
    facts.ex_style = ExStyleAppWindow;
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 3, 0, ReasonNone);
    facts.stamp = 15;
    facts.flags |= FactOwnerHidden;
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 3, 0, ReasonNone);
    facts.stamp = 16;
    facts.flags &= ~FactOwnerHidden;
    obs = try offer(graph, &facts);
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 3, 0, ReasonNone);
}

test "an IME preedit window follows its owner and loses the parent when the owner is hidden" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var preedit = owned_facts(0x400, 0x100, 10);
    preedit.flags |= FactAgentOwned | FactAgentPreedit;
    var obs = try offer(graph, &preedit);
    try expect_obs(obs, DispositionDeferred, RoleImePreedit, 0, 0, ReasonParentPending);
    obs = try admit_top(graph, 0x100, 11);
    try expect_counts(obs, 2, 2, 0, 0);
    var events: [4]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 2), try drain_events(graph, &events));
    try expect_ev(events[1], EventAnnounce, 0x400, 2, 0x100, 1, 0);
    try testing_t.expectEqual(RoleImePreedit, events[1].role);
    // The observer now reports the owner as hidden: no parent, so the role is the same but the parent changed.
    preedit = owned_facts(0x400, 0x100, 12);
    preedit.flags |= FactAgentOwned | FactAgentPreedit | FactOwnerHidden;
    obs = try offer(graph, &preedit);
    try expect_obs(obs, DispositionReclassified, RoleImePreedit, 3, 2, ReasonReclassified);
    // An ownerless preedit has no parent either.
    preedit = window_facts(0x500, 13);
    preedit.flags |= FactAgentOwned | FactAgentPreedit;
    obs = try offer(graph, &preedit);
    try expect_obs(obs, DispositionAdmitted, RoleImePreedit, 4, 0, ReasonNone);
}

test "classify_public reports the pure verdict in the C struct" {
    var facts = fixture_facts();
    var verdict: c_t.interop_window_verdict_t = undefined;
    @memset(std_t.mem.asBytes(&verdict), 0xA5);
    try testing_t.expectEqual(status_t.Ok, call_classify_public(&facts, UacWindow, &verdict));
    try testing_t.expectEqual(@as(u32, 1), verdict.kind);
    try testing_t.expectEqual(RoleToplevel, verdict.role);
    try testing_t.expectEqual(@as(u32, 0), verdict.reason);
    try testing_t.expectEqual(@as(u32, 0), verdict.reserved0);
    try testing_t.expectEqual(@as(u64, 0), verdict.parent_window);

    facts.owner = OwnerWindow;
    facts.owner_root = OwnerRootWindow;
    @memset(std_t.mem.asBytes(&verdict), 0xA5);
    try testing_t.expectEqual(status_t.Ok, call_classify_public(&facts, UacAuto, &verdict));
    try testing_t.expectEqual(@as(u32, 1), verdict.kind);
    try testing_t.expectEqual(RoleDialog, verdict.role);
    try testing_t.expectEqual(OwnerRootWindow, verdict.parent_window);
    facts.flags |= FactOwnerHidden;
    try testing_t.expectEqual(status_t.Ok, call_classify_public(&facts, UacAuto, &verdict));
    try testing_t.expectEqual(RoleToplevel, verdict.role);
    try testing_t.expectEqual(@as(u64, 0), verdict.parent_window);

    facts = fixture_facts();
    set_class(&facts, "Progman");
    @memset(std_t.mem.asBytes(&verdict), 0xA5);
    try testing_t.expectEqual(status_t.Ok, call_classify_public(&facts, UacWindow, &verdict));
    try testing_t.expectEqual(@as(u32, 2), verdict.kind);
    try testing_t.expectEqual(@as(u32, 0), verdict.role);
    try testing_t.expectEqual(ReasonExcludedClass, verdict.reason);
    try testing_t.expectEqual(@as(u32, 0), verdict.reserved0);
    try testing_t.expectEqual(@as(u64, 0), verdict.parent_window);

    facts = fixture_facts();
    facts.flags &= ~FactOnInputDesktop;
    facts.owner = OwnerWindow;
    facts.owner_root = OwnerRootWindow;
    @memset(std_t.mem.asBytes(&verdict), 0xA5);
    try testing_t.expectEqual(status_t.Ok, call_classify_public(&facts, UacWindow, &verdict));
    try testing_t.expectEqual(@as(u32, 3), verdict.kind);
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&verdict)[4..], 0));

    // UAC modes reach the classifier.
    facts = fixture_facts();
    set_class(&facts, UacCredentialName);
    set_image(&facts, "consent.exe");
    try testing_t.expectEqual(status_t.Ok, call_classify_public(&facts, UacWindow, &verdict));
    try testing_t.expectEqual(RoleUacConsent, verdict.role);
    try testing_t.expectEqual(status_t.Ok, call_classify_public(&facts, UacNative, &verdict));
    try testing_t.expectEqual(@as(u32, 2), verdict.kind);
    try testing_t.expectEqual(ReasonUacNativeHidden, verdict.reason);
}

test "classify_public rejects bad arguments and invalid facts and leaves the verdict alone" {
    var facts = fixture_facts();
    var verdict: c_t.interop_window_verdict_t = undefined;
    @memset(std_t.mem.asBytes(&verdict), 0xA5);
    try testing_t.expectEqual(status_t.InvalidArgument, call_classify_public(&facts, 3, &verdict));
    try testing_t.expectEqual(status_t.InvalidArgument, call_classify_public(&facts, 0xFFFF_FFFF, &verdict));
    facts.stamp = 0;
    try testing_t.expectEqual(status_t.InvalidFacts, call_classify_public(&facts, UacWindow, &verdict));
    facts = fixture_facts();
    facts.flags |= FactOwnerHidden;
    try testing_t.expectEqual(status_t.InvalidFacts, call_classify_public(&facts, UacWindow, &verdict));
    // The argument check comes first, as for the other pure checks of the C function.
    try testing_t.expectEqual(status_t.InvalidArgument, call_classify_public(&facts, 9, &verdict));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&verdict), 0xA5));
    facts.owner = OwnerWindow;
    facts.owner_root = OwnerRootWindow;
    try testing_t.expectEqual(status_t.Ok, call_classify_public(&facts, UacWindow, &verdict));
}

test "OwnerHidden is valid only together with an owner" {
    var facts = fixture_facts();
    facts.flags |= FactOwnerHidden;
    try testing_t.expect(!call_validate(&facts));
    facts.owner = OwnerWindow;
    facts.owner_root = OwnerRootWindow;
    try testing_t.expect(call_validate(&facts));
    facts.flags &= ~FactOwnerHidden;
    try testing_t.expect(call_validate(&facts));
    try testing_t.expectEqual(@as(u32, 0x1FFF), FactMask);
    try testing_t.expectEqual(@as(u32, 1 << 12), FactOwnerHidden);
}

fn link_child(rec: *record_t, parent_window: u64, parent_generation: u64) void {
    rec.parent_window = parent_window;
    rec.parent_generation = parent_generation;
}

/// A(g5) with children B(g6) and D(g7) and grandchild C(g9); unrelated E, a Retiring lookalike and a wrong-generation lookalike.
fn inject_tree(graph: *graph_t) void {
    _ = inject_entry(graph, 3, .Active, 0xA, 5);
    link_child(inject_entry(graph, 7, .Active, 0xB, 6), 0xA, 5);
    link_child(inject_entry(graph, 1, .Active, 0xC, 9), 0xB, 6);
    link_child(inject_entry(graph, 20, .Active, 0xD, 7), 0xA, 5);
    link_child(inject_entry(graph, 2, .Active, 0xE, 8), 0xF, 1);
    link_child(inject_entry(graph, 4, .Retiring, 0x10, 10), 0xA, 5);
    link_child(inject_entry(graph, 5, .Active, 0x11, 11), 0xA, 4);
    for ([_]usize{ 3, 7, 1, 20, 2, 4, 5 }) |slot| {
        const rec = &graph.entries[slot].record;
        rec.app_key_length = 3;
        @memcpy(rec.app_key[0..3], "a-k");
        rec.dpi = 96;
        rec.width = 10;
        rec.height = 10;
        const title = "title";
        @memcpy(rec.title[0..title.len], title);
        rec.title_length = title.len;
    }
    graph.generations_issued = 11;
}

test "collect_cascade gathers Active descendants by exact parent pair, descending by generation" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    inject_tree(graph);
    var cascade = cascade_t{};
    call_collect_cascade(graph, 3, &cascade);
    try testing_t.expectEqual(@as(u32, 4), cascade.count);
    try testing_t.expectEqualSlices(u8, &[_]u8{ 1, 20, 7, 3 }, cascade.slots[0..4]);
    try testing_t.expect(call_cascade_has_window(graph, &cascade, 0xA));
    try testing_t.expect(call_cascade_has_window(graph, &cascade, 0xC));
    try testing_t.expect(!call_cascade_has_window(graph, &cascade, 0xE));
    try testing_t.expect(!call_cascade_has_window(graph, &cascade, 0x10));
    try testing_t.expect(!call_cascade_has_window(graph, &cascade, 0x11));
    // A leaf is its own cascade.
    call_collect_cascade(graph, 1, &cascade);
    try testing_t.expectEqual(@as(u32, 1), cascade.count);
    try testing_t.expectEqual(@as(u8, 1), cascade.slots[0]);
}

test "retire_cascade keeps records, marks Retiring and queues children before the root" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    inject_tree(graph);
    var cascade = cascade_t{};
    call_collect_cascade(graph, 3, &cascade);
    var tally = tally_t{};
    call_retire_cascade(graph, &cascade, ReasonDestroyed, &tally);
    try testing_t.expectEqual(@as(u32, 4), tally.events);
    try testing_t.expectEqual(@as(u32, 4), tally.retired);
    try testing_t.expectEqual(@as(u32, 0), tally.admitted);
    for ([_]usize{ 3, 7, 1, 20 }) |slot| try testing_t.expectEqual(slot_state_t.Retiring, graph.entries[slot].state);
    try testing_t.expectEqual(slot_state_t.Active, graph.entries[2].state);
    try testing_t.expectEqual(ReasonDestroyed, graph.entries[3].record.retire_reason);
    try testing_t.expectEqual(ReasonParentRetired, graph.entries[7].record.retire_reason);
    var events: [6]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 4), try drain_events(graph, &events));
    const expected = [_][3]u64{ .{ 0xC, 9, ReasonParentRetired }, .{ 0xD, 7, ReasonParentRetired }, .{ 0xB, 6, ReasonParentRetired }, .{ 0xA, 5, ReasonDestroyed } };
    for (expected, events[0..4]) |want, got| {
        try testing_t.expectEqual(want[0], got.window);
        try testing_t.expectEqual(want[1], got.generation);
        try testing_t.expectEqual(@as(u32, @intCast(want[2])), got.reason);
        try testing_t.expectEqual(EventRetireIntent, got.kind);
        try testing_t.expectEqual(@as(u32, 0), got.changed_mask);
        try testing_t.expectEqualStrings("title", got.title[0..got.title_length]);
        try testing_t.expectEqualStrings("a-k", got.app_key[0..got.app_key_length]);
    }
    try testing_t.expectEqual(@as(u64, 4), current_stats(graph).retire_intents);
}

test "event_from_record copies every payload field and leaves the rest zero" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    var facts = fixture_facts();
    facts.min_width = 7;
    facts.min_height = 9;
    var rec = call_record_from_facts(&facts, TickBaseStart + 77, RoleDialog, 0x55);
    rec.generation = 12;
    rec.parent_generation = 4;
    rec.app_key_length = 3;
    @memcpy(rec.app_key[0..3], "p-z");
    const ev = call_event_from_record(graph, &rec, EventUpdate, ChangeDpi, ReasonDestroyed);
    try expect_ev(ev, EventUpdate, facts.window, 12, 0x55, 4, ReasonDestroyed);
    try testing_t.expectEqual(RoleDialog, ev.role);
    try testing_t.expectEqual(ChangeDpi, ev.changed_mask);
    try testing_t.expectEqual(facts.x, ev.x);
    try testing_t.expectEqual(@as(u32, 7), ev.min_width);
    try testing_t.expectEqual(@as(u32, 9), ev.min_height);
    try testing_t.expectEqual(facts.process_id, ev.process_id);
    try testing_t.expectEqual(derive_state(&facts), ev.state);
    try testing_t.expectEqual(@as(u32, 3), ev.app_key_length);
    try testing_t.expectEqualStrings("p-z", ev.app_key[0..3]);
    try testing_t.expectEqualSlices(u8, title_bytes(&facts), ev.title[0..ev.title_length]);
    try testing_t.expect(std_t.mem.allEqual(u8, ev.app_key[3..], 0));
}

test "record_from_facts copies identity and geometry and only the stated title bytes" {
    var facts = fixture_facts();
    facts.title[facts.title_length] = 'Q';
    facts.stamp = 77;
    const rec = call_record_from_facts(&facts, TickBaseStart + 5, RoleToplevel, 0);
    try testing_t.expectEqual(facts.window, rec.window);
    try testing_t.expectEqual(@as(u64, 77), rec.last_stamp);
    try testing_t.expectEqual(@as(u64, 0), rec.admit_stamp);
    try testing_t.expectEqual(TickBaseStart + 5, rec.last_captured_ms);
    try testing_t.expectEqual(@as(u64, 0), rec.admit_captured_ms);
    try testing_t.expectEqual(facts.process_start_time, rec.process_start_time);
    try testing_t.expectEqual(call_hash(call_class_bytes(&facts)), rec.class_hash);
    try testing_t.expect(call_identity(&rec, &facts));
    try testing_t.expectEqual(facts.title_length, rec.title_length);
    try testing_t.expectEqual(@as(u8, 0), rec.title[rec.title_length]);
    try testing_t.expectEqual(@as(u64, 0), rec.generation);
    try testing_t.expectEqual(@as(u64, 0), rec.seq);
    try testing_t.expectEqual(@as(u32, 0), rec.app_key_length);
}

test "defer_insert uses a saturating age and evicts the lowest index among equal ages" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    graph.next_seq = std_t.math.maxInt(u64);
    var tally = tally_t{};
    var index: u64 = 0;
    while (index < MaxDeferred) : (index += 1) {
        var rec = record_init();
        rec.window = 0x100 + index;
        rec.parent_window = 0x9000;
        rec.generation = 55;
        rec.admit_stamp = 3;
        rec.retire_reason = 4;
        rec.app_key_length = 3;
        call_defer_insert(graph, &rec, false, &tally);
    }
    try testing_t.expectEqual(@as(u32, 0), tally.dropped);
    try testing_t.expectEqual(std_t.math.maxInt(u64), graph.next_seq);
    try testing_t.expectEqual(std_t.math.maxInt(u64), graph.deferred[5].record.seq);
    try testing_t.expectEqual(@as(u64, 0), graph.deferred[5].record.generation);
    try testing_t.expectEqual(@as(u64, 0), graph.deferred[5].record.admit_stamp);
    try testing_t.expectEqual(@as(u32, 0), graph.deferred[5].record.retire_reason);
    var rec = record_init();
    rec.window = 0x7777;
    rec.parent_window = 0x9000;
    rec.app_key_length = 3;
    call_defer_insert(graph, &rec, true, &tally);
    try testing_t.expectEqual(@as(u32, 1), tally.dropped);
    try testing_t.expectEqual(@as(u64, 0x7777), graph.deferred[0].record.window);
    try testing_t.expect(graph.deferred[0].seen);
    try testing_t.expectEqual(@as(u64, 0x101), graph.deferred[1].record.window);
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.deferred_evicted);
    try testing_t.expectEqual(@as(u64, 65), stats.deferred_added);
    try testing_t.expectEqual(@as(u64, 0), stats.deferred_dropped);
}

test "drop_deferred empties the slot and counts a drop, not an eviction" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    inject_deferred(graph, 9, 0x300);
    var tally = tally_t{};
    call_drop_deferred(graph, 9, &tally);
    try testing_t.expectEqual(@as(u32, 1), tally.dropped);
    try testing_t.expect(!graph.deferred[9].used);
    try testing_t.expectEqual(@as(u64, 0), graph.deferred[9].record.window);
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.deferred_dropped);
    try testing_t.expectEqual(@as(u64, 0), stats.deferred_evicted);
}

test "mark_seen marks the Active entry and the deferred record but never a Retiring entry" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 0, .Active, 0xA, 1);
    _ = inject_entry(graph, 1, .Retiring, 0xB, 2);
    inject_deferred(graph, 0, 0xC);
    call_mark_seen(graph, 0xA);
    call_mark_seen(graph, 0xB);
    call_mark_seen(graph, 0xC);
    call_mark_seen(graph, 0xD);
    try testing_t.expect(graph.entries[0].seen);
    try testing_t.expect(!graph.entries[1].seen);
    try testing_t.expect(graph.deferred[0].seen);
}

test "bump_by adds and saturates" {
    var counter: u64 = 5;
    call_bump_by(&counter, 3);
    try testing_t.expectEqual(@as(u64, 8), counter);
    call_bump_by(&counter, std_t.math.maxInt(u64));
    try testing_t.expectEqual(std_t.math.maxInt(u64), counter);
}

test "ring_merge_target returns the newest Announce or Update and stops at a RetireIntent" {
    const graph = try test_graph(testing_t.allocator, TestEpoch);
    defer call_destroy(graph);
    try testing_t.expectEqual(@as(?*event_t, null), call_merge_target(graph, 5, 1));
    queue_raw(graph, make_event(EventAnnounce, 5, 1));
    queue_raw(graph, make_event(EventAnnounce, 6, 1));
    queue_raw(graph, make_event(EventUpdate, 5, 1));
    const target = call_merge_target(graph, 5, 1).?;
    try testing_t.expectEqual(EventUpdate, target.kind);
    try testing_t.expectEqual(@as(?*event_t, null), call_merge_target(graph, 5, 2));
    queue_raw(graph, make_event(EventRetireIntent, 5, 1));
    try testing_t.expectEqual(@as(?*event_t, null), call_merge_target(graph, 5, 1));
    try testing_t.expect(call_merge_target(graph, 6, 1) != null);
}

test "seen marks follow entries through re-deferral, promotion and flush" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    try build_family(graph);
    for (&graph.entries) |*entry| entry.seen = false;
    // P is offered with an unannounced owner: P' is a fresh offer (seen), descendants inherit their entries' marks.
    var facts = owned_facts(0x100, 0x900, 20);
    _ = try offer(graph, &facts);
    try testing_t.expect(graph.deferred[call_find_deferred(graph, 0x100).?].seen);
    try testing_t.expect(!graph.deferred[call_find_deferred(graph, 0x200).?].seen);
    try testing_t.expect(!graph.deferred[call_find_deferred(graph, 0x300).?].seen);
    try testing_t.expect(!graph.deferred[call_find_deferred(graph, 0x400).?].seen);
    _ = discard_events(graph);
    // Marked deferred leftovers keep their mark through the flush; promoted entries keep the record's mark.
    graph.deferred[call_find_deferred(graph, 0x300).?].seen = true;
    _ = try admit_top(graph, 0x900, 21);
    try testing_t.expect(graph.entries[call_find_active(graph, 0x900).?].seen);
    try testing_t.expect(graph.entries[call_find_active(graph, 0x100).?].seen);
    try testing_t.expect(!graph.entries[call_find_active(graph, 0x200).?].seen);
    try testing_t.expect(graph.entries[call_find_active(graph, 0x300).?].seen);
    // A plain Ok offer marks the window's Active entry, a retired entry carries no new mark.
    facts = owned_facts(0x200, 0x100, 22);
    _ = try offer(graph, &facts);
    try testing_t.expect(graph.entries[call_find_active(graph, 0x200).?].seen);
    try testing_t.expect(!graph.entries[call_find_active(graph, 0x400).?].seen);
}

test "a deferred record of another incarnation is dropped and its key is never reused" {
    // Admission branch: the window reappears with a different process and an announced owner.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        var facts = owned_facts(0x200, 0x100, 10);
        _ = try offer(graph, &facts);
        _ = try admit_top(graph, 0x50, 11);
        _ = discard_events(graph);
        facts = owned_facts(0x200, 0x50, 12);
        facts.process_id = 9999;
        const obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionAdmitted, RoleDialog, 2, 0, ReasonNone);
        try expect_counts(obs, 1, 1, 0, 1);
        try testing_t.expectEqual(@as(u32, 3), lookup_probe.calls);
        const ev = try next_ev(graph);
        var key_buffer: [64]u8 = undefined;
        try testing_t.expectEqualStrings(expected_key(&key_buffer, 0x200, 3), ev.app_key[0..ev.app_key_length]);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
        try testing_t.expectEqual(@as(u64, 1), current_stats(graph).deferred_dropped);
    }
    // Deferral branch: the new record gets a fresh lookup and a new age position.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        var facts = owned_facts(0x200, 0x100, 10);
        _ = try offer(graph, &facts);
        facts = owned_facts(0x300, 0x100, 11);
        _ = try offer(graph, &facts);
        facts = owned_facts(0x200, 0x100, 12);
        facts.thread_id += 5;
        const obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionDeferred, RoleDialog, 0, 0, ReasonParentPending);
        try expect_counts(obs, 0, 0, 0, 1);
        try testing_t.expectEqual(@as(u32, 3), lookup_probe.calls);
        const record = &graph.deferred[call_find_deferred(graph, 0x200).?].record;
        try testing_t.expectEqual(@as(u64, 3), record.seq);
        try testing_t.expectEqual(@as(u32, 5), record.thread_id - 4243 + 0);
        var key_buffer: [64]u8 = undefined;
        try testing_t.expectEqualStrings(expected_key(&key_buffer, 0x200, 3), record.app_key[0..record.app_key_length]);
        try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
        const stats = current_stats(graph);
        try testing_t.expectEqual(@as(u64, 3), stats.deferred_added);
        try testing_t.expectEqual(@as(u64, 1), stats.deferred_dropped);
    }
    // A lookup failure after a positive mismatch drops the old record and returns Excluded.
    {
        const graph = try probe_graph(testing_t.allocator);
        defer call_destroy(graph);
        var facts = owned_facts(0x200, 0x100, 10);
        _ = try offer(graph, &facts);
        lookup_probe.mode = .Fail;
        facts.process_id = 9999;
        facts.stamp = 11;
        const obs = try offer(graph, &facts);
        try expect_obs(obs, DispositionExcluded, 0, 0, 0, ReasonIncarnationChanged);
        try expect_counts(obs, 0, 0, 0, 1);
        try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
        try testing_t.expectEqual(@as(u64, 1), current_stats(graph).lookup_failures);
    }
}

const consumer_t = struct {
    live: std_t.AutoHashMap(u64, [2]u64),

    fn init(allocator: std_t.mem.Allocator) consumer_t {
        return .{ .live = std_t.AutoHashMap(u64, [2]u64).init(allocator) };
    }

    fn deinit(self: *consumer_t) void {
        self.live.deinit();
    }

    /// Replay one event and check the stream guarantees: parents announced first, children retired first.
    fn apply(self: *consumer_t, ev: *const event_t) !void {
        switch (ev.kind) {
            EventAnnounce => {
                try testing_t.expect(!self.live.contains(ev.generation));
                if (ev.parent_generation != 0) {
                    const parent = self.live.get(ev.parent_generation) orelse return error.TestUnexpectedResult;
                    try testing_t.expectEqual(ev.parent_window, parent[0]);
                    try testing_t.expect(ev.parent_generation < ev.generation);
                } else {
                    try testing_t.expectEqual(@as(u64, 0), ev.parent_window);
                }
                try self.live.put(ev.generation, .{ ev.window, ev.parent_generation });
            },
            EventUpdate => try testing_t.expect(self.live.contains(ev.generation)),
            else => {
                try testing_t.expectEqual(EventRetireIntent, ev.kind);
                try testing_t.expect(self.live.contains(ev.generation));
                var values = self.live.valueIterator();
                while (values.next()) |value| try testing_t.expect(value[1] != ev.generation);
                _ = self.live.remove(ev.generation);
            },
        }
    }

    fn drain(self: *consumer_t, graph: *graph_t, limit: u32) !void {
        var ev = std_t.mem.zeroes(event_t);
        var taken: u32 = 0;
        while (taken < limit and call_next_event(graph, TestEpoch, &ev) == .Ok) : (taken += 1) try self.apply(&ev);
    }
};

const trace_totals_t = struct {
    steps: u32 = 0,
    reclassified: u64 = 0,
    updates: u64 = 0,
    coalesced: u64 = 0,
    evicted: u64 = 0,
    at_capacity: u64 = 0,
    cycle: u64 = 0,
    queue_full: u64 = 0,
    lookup_failures: u64 = 0,
    stale_facts: u64 = 0,
    flushed_children: u64 = 0,
    offers: u64 = 0,
    commits: u64 = 0,
    commit_retired: u64 = 0,
    commit_queue_full: u64 = 0,
    aborts: u64 = 0,
    invalidations: u64 = 0,
    invalidate_queue_full: u64 = 0,
    keeps: u64 = 0,
    unseen_listed: u64 = 0,
};

fn random_trace_facts(random: std_t.Random, variants: []u32, base: u64, stamp: u64, tick: u32) facts_t {
    const index = random.uintLessThan(usize, variants.len);
    if (random.uintLessThan(u32, 9) == 0) variants[index] = random.int(u32);
    var shape_prng = std_t.Random.DefaultPrng.init(@as(u64, variants[index]) *% 0x9E37_79B9 +% index);
    const shape = shape_prng.random();
    const window = base + index;
    var facts = window_facts(window, stamp);
    facts.captured_ms = tick;
    facts.process_id = 4242 + shape.uintLessThan(u32, 2);
    facts.x = shape.intRangeAtMost(i32, -3, 3);
    if (random.uintLessThan(u32, 8) == 0) facts.x += 9;
    if (random.uintLessThan(u32, 8) == 0) set_title(&facts, "other");
    switch (shape.uintLessThan(u32, 14)) {
        0 => facts.flags &= ~FactVisible,
        1 => set_class(&facts, "Progman"),
        2 => facts.width = 0,
        3 => {
            facts.ex_style = ExStyleNoActivate;
            set_class(&facts, "tooltips_class32");
        },
        4 => set_class(&facts, "#32768"),
        else => {},
    }
    if (shape.uintLessThan(u32, 3) != 0) {
        var owner_index = shape.uintLessThan(usize, variants.len);
        if (shape.uintLessThan(u32, 25) != 0 and owner_index == index) owner_index = (owner_index + 1) % variants.len;
        facts.owner = base + owner_index;
        facts.owner_root = facts.owner;
        if (shape.uintLessThan(u32, 8) == 0) facts.flags |= FactOwnerHidden;
        if (shape.uintLessThan(u32, 8) == 0) facts.ex_style |= ExStyleAppWindow;
    }
    return facts;
}

/// One random refresh-engine operation of the trace; `op` is 0..13. Checks the operation's contract
/// against a before/after view of the graph and then the structural invariants.
fn trace_refresh_op(graph: *graph_t, random: std_t.Random, op: u32, base: u64, count: usize, totals: *trace_totals_t) !void {
    const was_open = graph.refresh_open;
    const before = state_digest(graph);
    const before_unmarked = state_digest_unmarked(graph);
    switch (op) {
        0, 1, 2 => {
            const status = call_refresh_begin(graph, TestEpoch);
            try testing_t.expect(graph.refresh_open);
            if (was_open) {
                try testing_t.expectEqual(status_t.RefreshActive, status);
                try testing_t.expectEqual(before, state_digest(graph));
            } else {
                try testing_t.expectEqual(status_t.Ok, status);
                for (&graph.entries) |*entry| try testing_t.expect(!entry.seen);
                for (&graph.deferred) |*slot| try testing_t.expect(!slot.seen);
            }
        },
        3, 4, 5, 6 => {
            const window = base + random.uintLessThan(u64, count);
            const status = call_refresh_keep(graph, TestEpoch, window);
            totals.keeps += 1;
            const active = call_find_active(graph, window);
            const deferred = call_find_deferred(graph, window);
            if (!was_open) {
                try testing_t.expectEqual(status_t.NoRefresh, status);
                try testing_t.expectEqual(before, state_digest(graph));
            } else if (active == null and deferred == null) {
                try testing_t.expectEqual(status_t.None, status);
                try testing_t.expectEqual(before, state_digest(graph));
            } else {
                try testing_t.expectEqual(status_t.Ok, status);
                try testing_t.expectEqual(before_unmarked, state_digest_unmarked(graph));
                if (active) |slot| try testing_t.expect(graph.entries[slot].seen);
                if (deferred) |slot| try testing_t.expect(graph.deferred[slot].seen);
            }
        },
        7, 8, 9 => {
            var retired: u32 = 0xDEAD_BEEF;
            const status = call_refresh_commit(graph, TestEpoch, &retired);
            if (!was_open) {
                try testing_t.expectEqual(status_t.NoRefresh, status);
                try testing_t.expectEqual(@as(u32, 0xDEAD_BEEF), retired);
                try testing_t.expectEqual(before, state_digest(graph));
            } else if (status == .QueueFull) {
                totals.commit_queue_full += 1;
                try testing_t.expectEqual(@as(u32, 0xDEAD_BEEF), retired);
                try testing_t.expect(graph.refresh_open);
                try testing_t.expectEqual(before, state_digest(graph));
            } else {
                try testing_t.expectEqual(status_t.Ok, status);
                totals.commits += 1;
                totals.commit_retired += retired;
                try testing_t.expect(!graph.refresh_open);
                // Everything that survived was seen; every other entry and record is gone.
                for (&graph.entries) |*entry| {
                    if (entry.state == .Active) try testing_t.expect(entry.seen);
                }
                for (&graph.deferred) |*slot| {
                    if (slot.used) try testing_t.expect(slot.seen);
                }
            }
        },
        10 => {
            const status = call_refresh_abort(graph, TestEpoch);
            if (was_open) {
                totals.aborts += 1;
                try testing_t.expectEqual(status_t.Ok, status);
            } else {
                try testing_t.expectEqual(status_t.NoRefresh, status);
            }
            try testing_t.expect(!graph.refresh_open);
            try testing_t.expectEqual(before_unmarked, state_digest_unmarked(graph));
        },
        11 => {
            var retired: u32 = 0xDEAD_BEEF;
            const status = call_invalidate_all(graph, TestEpoch, if (random.boolean()) &retired else null);
            if (status == .QueueFull) {
                totals.invalidate_queue_full += 1;
                try testing_t.expectEqual(@as(u32, 0xDEAD_BEEF), retired);
                try testing_t.expectEqual(before, state_digest(graph));
                try testing_t.expectEqual(was_open, graph.refresh_open);
            } else {
                try testing_t.expectEqual(status_t.Ok, status);
                totals.invalidations += 1;
                try testing_t.expect(!graph.refresh_open);
                try testing_t.expectEqual(@as(u32, 0), call_count_state(graph, .Active));
                try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
            }
        },
        else => {
            var windows: [MaxEntries]u64 = undefined;
            var total: u32 = 0xDEAD_BEEF;
            const status = call_refresh_unseen(graph, TestEpoch, &windows, MaxEntries, &total);
            try testing_t.expectEqual(before, state_digest(graph));
            if (!was_open) {
                try testing_t.expectEqual(status_t.NoRefresh, status);
                try testing_t.expectEqual(@as(u32, 0xDEAD_BEEF), total);
            } else {
                try testing_t.expectEqual(status_t.Ok, status);
                totals.unseen_listed += total;
                var expected: u32 = 0;
                var last_generation: u64 = 0;
                for (&graph.entries) |*entry| {
                    if (entry.state == .Active and !entry.seen) expected += 1;
                }
                try testing_t.expectEqual(expected, total);
                for (windows[0..total]) |window| {
                    const slot = call_find_active(graph, window).?;
                    try testing_t.expect(!graph.entries[slot].seen);
                    try testing_t.expect(graph.entries[slot].record.generation > last_generation);
                    last_generation = graph.entries[slot].record.generation;
                }
            }
        },
    }
    try check_invariants(graph);
}

fn run_random_trace(seed: u64, base: u64, count: usize, steps: u32, drain_above: u32, drain_max: u32, refresh_ops: bool, totals: *trace_totals_t) !void {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    var consumer = consumer_t.init(testing_t.allocator);
    defer consumer.deinit();
    var prng = std_t.Random.DefaultPrng.init(seed);
    const random = prng.random();
    const variants = try testing_t.allocator.alloc(u32, count);
    defer testing_t.allocator.free(variants);
    for (variants) |*variant| variant.* = random.int(u32);
    var stamp: u64 = 100;
    var tick: u32 = 0xFFFF_FF00;
    var step: u32 = 0;
    while (step < steps) : (step += 1) {
        totals.steps += 1;
        stamp += 1;
        const advance = random.uintLessThan(u32, 40);
        tick +%= advance;
        // The graph clock runs with the tick domain, so the unwrap estimate is exercised on every step.
        test_clock.now += advance;
        const choice = random.uintLessThan(u32, 100);
        var before: u64 = 0;
        var status: status_t = .Ok;
        var obs = sentinel_observation();
        var offered_window: u64 = 0;
        const was_open = graph.refresh_open;
        if (choice < 66) {
            var facts = random_trace_facts(random, variants, base, stamp, tick);
            if (random.uintLessThan(u32, 20) == 0) facts.stamp -= 3;
            lookup_probe.mode = if (random.uintLessThan(u32, 30) == 0) .Fail else .Ok;
            before = state_digest_unmarked(graph);
            const use_offer = if (was_open) random.boolean() else random.uintLessThan(u32, 25) == 0;
            if (use_offer) {
                offered_window = facts.window;
                totals.offers += 1;
                status = call_refresh_offer(graph, TestEpoch, &facts, &obs);
            } else {
                status = call_observe(graph, TestEpoch, &facts, &obs);
            }
            lookup_probe.mode = .Ok;
        } else if (choice < 72) {
            const window = base + random.uintLessThan(u64, count);
            before = state_digest_unmarked(graph);
            status = call_note_destroyed(graph, TestEpoch, window, stamp, tick, &obs);
        } else if (choice < 78) {
            var entries: [MaxEntries]retiring_entry_t = undefined;
            var total: u32 = 0;
            try testing_t.expectEqual(status_t.Ok, call_list_retiring(graph, &entries, MaxEntries, &total));
            if (total != 0) {
                const picked = entries[random.uintLessThan(u32, total)];
                const proof = proof_t{ .capture_stopped = 1, .pins_drained = 1 };
                try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, picked.window, picked.generation, &proof));
            }
            try check_invariants(graph);
            continue;
        } else if (choice < 86) {
            try consumer.drain(graph, random.uintLessThan(u32, drain_max + 1));
            try check_invariants(graph);
            continue;
        } else if (!refresh_ops) {
            try consumer.drain(graph, random.uintLessThan(u32, drain_max + 1));
            try check_invariants(graph);
            continue;
        } else {
            try trace_refresh_op(graph, random, choice - 86, base, count, totals);
            if (graph.ring_len > drain_above) try consumer.drain(graph, EventCapacity);
            continue;
        }
        switch (status) {
            .Ok => {
                try testing_t.expect(!std_t.mem.allEqual(u8, std_t.mem.asBytes(&obs), 0xA5));
                try testing_t.expect(obs.events_queued <= MaxCallEvents);
            },
            .Cycle => {},
            .NoRefresh => {
                try testing_t.expect(offered_window != 0 and !was_open);
                try testing_t.expectEqual(before, state_digest_unmarked(graph));
            },
            .AtCapacity, .QueueFull, .LookupFailed, .StaleFacts => try testing_t.expectEqual(before, state_digest_unmarked(graph)),
            else => return error.TestUnexpectedResult,
        }
        if (offered_window != 0 and status != .NoRefresh) {
            // Seen rule: after any outcome whatever exists for the offered window carries the mark.
            try testing_t.expect(was_open);
            if (call_find_active(graph, offered_window)) |slot| try testing_t.expect(graph.entries[slot].seen);
            if (call_find_deferred(graph, offered_window)) |slot| try testing_t.expect(graph.deferred[slot].seen);
        }
        try check_invariants(graph);
        if (graph.ring_len > drain_above) try consumer.drain(graph, EventCapacity);
    }
    try consumer.drain(graph, EventCapacity);
    // After a full drain the consumer's live set is exactly the Active entries.
    try testing_t.expectEqual(@as(u32, @intCast(consumer.live.count())), call_count_state(graph, .Active));
    for (&graph.entries) |*entry| {
        if (entry.state == .Active) try testing_t.expect(consumer.live.contains(entry.record.generation));
    }
    const stats = current_stats(graph);
    try testing_t.expectEqual(stats.events_queued, stats.events_drained);
    try testing_t.expectEqual(@as(u32, 0), stats.events_pending);
    try testing_t.expect(stats.admitted >= stats.retire_intents);
    totals.reclassified += stats.reclassified;
    totals.updates += stats.updates_queued;
    totals.coalesced += stats.events_coalesced;
    totals.evicted += stats.deferred_evicted;
    totals.at_capacity += stats.rejected_at_capacity;
    totals.cycle += stats.rejected_cycle;
    totals.queue_full += stats.rejected_queue_full;
    totals.lookup_failures += stats.lookup_failures;
    totals.stale_facts += stats.rejected_stale_facts;
}

test "random operation traces keep every graph invariant and the event stream ordered" {
    var totals = trace_totals_t{};
    // Few windows: constant reuse, reclassification and incarnation churn.
    try run_random_trace(0x5EED_0001, 0x100, 12, 20000, 150, 5, true, &totals);
    try run_random_trace(0x5EED_0002, 0x100, 12, 20000, 150, 5, true, &totals);
    // Many windows: capacity pressure, deferral overflow and cycles; a lazy consumer provokes QueueFull.
    try run_random_trace(0x5EED_0003, 0x1000, 220, 40000, 150, 5, false, &totals);
    try run_random_trace(0x5EED_0004, 0x1000, 220, 40000, 150, 5, false, &totals);
    try run_random_trace(0x5EED_0005, 0x1000, 90, 30000, 256, 1, true, &totals);
    try run_random_trace(0x5EED_0006, 0x1000, 220, 60000, 150, 5, true, &totals);
    // 210000 steps in total (the invariants are checked after every step).
    try testing_t.expectEqual(@as(u32, 210000), totals.steps);
    // The generator really exercised the interesting paths.
    try testing_t.expect(totals.reclassified > 20);
    try testing_t.expect(totals.updates > 20);
    try testing_t.expect(totals.coalesced > 5);
    // Deferred eviction remains covered by the deterministic 65th-record test above; the
    // randomized traces need not hit that path after accepted incarnation changes now drop subtrees.
    try testing_t.expect(totals.at_capacity > 20);
    try testing_t.expect(totals.cycle > 5);
    try testing_t.expect(totals.queue_full > 0);
    try testing_t.expect(totals.lookup_failures > 5);
    try testing_t.expect(totals.stale_facts > 5);
    // ... and so did the refresh engine.
    try testing_t.expect(totals.offers > 200);
    try testing_t.expect(totals.keeps > 100);
    try testing_t.expect(totals.commits > 20);
    try testing_t.expect(totals.commit_retired > 20);
    try testing_t.expect(totals.commit_queue_full > 0);
    try testing_t.expect(totals.aborts > 5);
    try testing_t.expect(totals.invalidations > 5);
    try testing_t.expect(totals.unseen_listed > 20);
}

test "observe_locked runs the plan on the facts it is given without applying the call guards" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    graph.busy = true;
    defer graph.busy = false;
    var facts = window_facts(0x100, 10);
    var obs = sentinel_observation();
    try testing_t.expectEqual(status_t.Ok, call_observe_locked(graph, &facts, &obs));
    try expect_obs(obs, DispositionAdmitted, RoleToplevel, 1, 0, ReasonNone);
    // The refresh engine hands over its own stack copy: the lookup sees exactly that pointer.
    try testing_t.expectEqual(@intFromPtr(&facts), lookup_probe.last_pointer);
    facts.stamp = 0;
    obs = sentinel_observation();
    try testing_t.expectEqual(status_t.InvalidFacts, call_observe_locked(graph, &facts, &obs));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&obs), 0xA5));
    try testing_t.expectEqual(@as(u64, 1), graph.stats.rejected_invalid_facts);
}

// ---------------------------------------------------------------------------------------------
// Refresh engine tests.
// ---------------------------------------------------------------------------------------------

fn call_refresh_begin(graph: *graph_t, epoch: u64) status_t {
    return @call(.never_inline, refresh_begin, .{ graph, epoch });
}

fn call_refresh_offer(graph: *graph_t, epoch: u64, facts: *const facts_t, out: *observation_t) status_t {
    return @call(.never_inline, refresh_offer, .{ graph, epoch, facts, out });
}

fn call_refresh_keep(graph: *graph_t, epoch: u64, window: u64) status_t {
    return @call(.never_inline, refresh_keep, .{ graph, epoch, window });
}

fn call_refresh_unseen(graph: *graph_t, epoch: u64, windows: ?[*]u64, capacity: u32, out_count: *u32) status_t {
    return @call(.never_inline, refresh_unseen, .{ graph, epoch, windows, capacity, out_count });
}

fn call_refresh_commit(graph: *graph_t, epoch: u64, retired_out: ?*u32) status_t {
    return @call(.never_inline, refresh_commit, .{ graph, epoch, retired_out });
}

fn call_refresh_abort(graph: *graph_t, epoch: u64) status_t {
    return @call(.never_inline, refresh_abort, .{ graph, epoch });
}

fn call_invalidate_all(graph: *graph_t, epoch: u64, retired_out: ?*u32) status_t {
    return @call(.never_inline, invalidate_all, .{ graph, epoch, retired_out });
}

fn call_active_by_generation(graph: *const graph_t, order: *[MaxEntries]u8) u32 {
    return @call(.never_inline, active_by_generation, .{ graph, order });
}

/// Like `state_digest` but blind to the seen marks, for calls that may only change marks.
fn state_digest_unmarked(graph: *const graph_t) u64 {
    var entries = graph.entries;
    for (&entries) |*entry| entry.seen = false;
    var deferred = graph.deferred;
    for (&deferred) |*slot| slot.seen = false;
    var hasher = std_t.hash.Wyhash.init(0);
    hasher.update(std_t.mem.asBytes(&entries));
    hasher.update(std_t.mem.asBytes(&deferred));
    hasher.update(std_t.mem.asBytes(&graph.ring));
    hasher.update(std_t.mem.asBytes(&graph.ring_head));
    hasher.update(std_t.mem.asBytes(&graph.ring_len));
    hasher.update(std_t.mem.asBytes(&graph.generations_issued));
    hasher.update(std_t.mem.asBytes(&graph.next_seq));
    hasher.update(std_t.mem.asBytes(&graph.stamp_high_water));
    hasher.update(std_t.mem.asBytes(&graph.tick_base));
    hasher.update(std_t.mem.asBytes(&graph.tick_clock_at_base));
    return hasher.final();
}

fn refresh_open_ok(graph: *graph_t) !void {
    try testing_t.expectEqual(status_t.Ok, call_refresh_begin(graph, TestEpoch));
    try testing_t.expect(graph.refresh_open);
    try check_invariants(graph);
}

fn refresh_offer_ok(graph: *graph_t, facts: *const facts_t) !observation_t {
    var obs = sentinel_observation();
    try testing_t.expectEqual(status_t.Ok, call_refresh_offer(graph, TestEpoch, facts, &obs));
    try check_invariants(graph);
    return obs;
}

/// Offer inside the open refresh, expect `want`, an untouched observation, and no change but the marks.
fn refresh_offer_fails(graph: *graph_t, facts: *const facts_t, want: status_t) !void {
    var obs = sentinel_observation();
    const before = state_digest_unmarked(graph);
    try testing_t.expectEqual(want, call_refresh_offer(graph, TestEpoch, facts, &obs));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&obs), 0xA5));
    try testing_t.expectEqual(before, state_digest_unmarked(graph));
    try check_invariants(graph);
}

fn refresh_commit_ok(graph: *graph_t) !u32 {
    var retired: u32 = 0xDEAD_BEEF;
    try testing_t.expectEqual(status_t.Ok, call_refresh_commit(graph, TestEpoch, &retired));
    try testing_t.expect(!graph.refresh_open);
    try check_invariants(graph);
    return retired;
}

fn active_seen(graph: *const graph_t, window: u64) bool {
    return graph.entries[call_find_active(graph, window).?].seen;
}

fn deferred_seen(graph: *const graph_t, window: u64) bool {
    return graph.deferred[call_find_deferred(graph, window).?].seen;
}

test "refresh_begin opens the refresh, clears every mark and reports an already open refresh" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = try admit_dialog(graph, 0x900, 0x777, 11);
    try testing_t.expect(active_seen(graph, 0x100));
    try testing_t.expect(deferred_seen(graph, 0x900));
    try testing_t.expectEqual(@as(u32, 0), current_stats(graph).refresh_active);
    try refresh_open_ok(graph);
    try testing_t.expect(!active_seen(graph, 0x100));
    try testing_t.expect(!deferred_seen(graph, 0x900));
    try testing_t.expectEqual(@as(u32, 1), current_stats(graph).refresh_active);
    // A second begin leaves the open refresh and its marks alone.
    try testing_t.expectEqual(status_t.Ok, call_refresh_keep(graph, TestEpoch, 0x100));
    const before = state_digest(graph);
    try testing_t.expectEqual(status_t.RefreshActive, call_refresh_begin(graph, TestEpoch));
    try testing_t.expectEqual(before, state_digest(graph));
    try testing_t.expect(graph.refresh_open);
    try testing_t.expect(active_seen(graph, 0x100));
    try testing_t.expectEqual(@as(u32, 1), current_stats(graph).refresh_active);
    try testing_t.expectEqual(@as(u64, 0), current_stats(graph).refresh_commits);
    try testing_t.expectEqual(@as(u64, 0), current_stats(graph).refresh_aborts);
}

test "offering every live window and committing retires nothing" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x50, 10);
    _ = try admit_top(graph, 0x100, 11);
    _ = try admit_dialog(graph, 0x200, 0x100, 12);
    _ = try admit_dialog(graph, 0x900, 0x777, 13);
    _ = discard_events(graph);
    const lookups = lookup_probe.calls;
    try refresh_open_ok(graph);
    var facts = window_facts(0x50, 20);
    var obs = try refresh_offer_ok(graph, &facts);
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 1, 0, ReasonNone);
    facts = window_facts(0x100, 21);
    obs = try refresh_offer_ok(graph, &facts);
    try expect_obs(obs, DispositionUnchanged, RoleToplevel, 2, 0, ReasonNone);
    facts = owned_facts(0x200, 0x100, 22);
    obs = try refresh_offer_ok(graph, &facts);
    try expect_obs(obs, DispositionUnchanged, RoleDialog, 3, 0, ReasonNone);
    facts = owned_facts(0x900, 0x777, 23);
    obs = try refresh_offer_ok(graph, &facts);
    try testing_t.expectEqual(DispositionDeferred, obs.disposition);
    try expect_counts(obs, 0, 0, 0, 0);
    try testing_t.expectEqual(lookups, lookup_probe.calls);
    try testing_t.expectEqual(@as(u32, 0), try refresh_commit_ok(graph));
    try testing_t.expectEqual(@as(u32, 3), call_count_state(graph, .Active));
    try testing_t.expectEqual(@as(u32, 0), call_count_state(graph, .Retiring));
    try testing_t.expectEqual(@as(u32, 1), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.refresh_commits);
    try testing_t.expectEqual(@as(u64, 0), stats.refresh_aborts);
    try testing_t.expectEqual(@as(u32, 0), stats.refresh_active);
    try testing_t.expectEqual(@as(u64, 0), stats.retire_intents);
    try testing_t.expectEqual(@as(u64, 0), stats.deferred_dropped);
    // The next call without an open refresh is refused.
    try testing_t.expectEqual(status_t.NoRefresh, call_refresh_commit(graph, TestEpoch, null));
}

test "a commit retires every unseen entry children first with reason RefreshUnseen" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    try build_family(graph);
    try refresh_open_ok(graph);
    const retired = try refresh_commit_ok(graph);
    try testing_t.expectEqual(@as(u32, 5), retired);
    var events: [8]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 5), try drain_events(graph, &events));
    try expect_ev(events[0], EventRetireIntent, 0x400, 5, 0x200, 3, ReasonRefreshUnseen);
    try expect_ev(events[1], EventRetireIntent, 0x300, 4, 0x100, 2, ReasonRefreshUnseen);
    try expect_ev(events[2], EventRetireIntent, 0x200, 3, 0x100, 2, ReasonRefreshUnseen);
    try expect_ev(events[3], EventRetireIntent, 0x100, 2, 0, 0, ReasonRefreshUnseen);
    try expect_ev(events[4], EventRetireIntent, 0x50, 1, 0, 0, ReasonRefreshUnseen);
    try testing_t.expectEqual(@as(u32, 0), call_count_state(graph, .Active));
    try testing_t.expectEqual(@as(u32, 5), call_count_state(graph, .Retiring));
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u64, 5), current_stats(graph).retire_intents);
}

test "a seen child of an unseen parent retires with it as ParentRetired and is never re-deferred" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    try build_family(graph);
    try refresh_open_ok(graph);
    // Seen: Q, C1, G. Unseen: P (the parent of C1 and C2) and C2.
    var facts = window_facts(0x50, 20);
    _ = try refresh_offer_ok(graph, &facts);
    facts = owned_facts(0x200, 0x100, 21);
    _ = try refresh_offer_ok(graph, &facts);
    facts = owned_facts(0x400, 0x200, 22);
    _ = try refresh_offer_ok(graph, &facts);
    var windows: [MaxEntries]u64 = undefined;
    var total: u32 = 0;
    try testing_t.expectEqual(status_t.Ok, call_refresh_unseen(graph, TestEpoch, &windows, MaxEntries, &total));
    try testing_t.expectEqual(@as(u32, 2), total);
    try testing_t.expectEqualSlices(u64, &[_]u64{ 0x100, 0x300 }, windows[0..2]);
    const retired = try refresh_commit_ok(graph);
    try testing_t.expectEqual(@as(u32, 4), retired);
    var events: [8]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 4), try drain_events(graph, &events));
    try expect_ev(events[0], EventRetireIntent, 0x300, 4, 0x100, 2, ReasonRefreshUnseen);
    try expect_ev(events[1], EventRetireIntent, 0x400, 5, 0x200, 3, ReasonParentRetired);
    try expect_ev(events[2], EventRetireIntent, 0x200, 3, 0x100, 2, ReasonParentRetired);
    try expect_ev(events[3], EventRetireIntent, 0x100, 2, 0, 0, ReasonRefreshUnseen);
    try testing_t.expectEqual(@as(u32, 1), call_count_state(graph, .Active));
    try testing_t.expect(call_find_active(graph, 0x50) != null);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u32, 4), call_count_state(graph, .Retiring));
}

test "a commit drops unseen deferred records and keeps the seen ones" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_dialog(graph, 0x900, 0x777, 10);
    _ = try admit_dialog(graph, 0xA00, 0x777, 11);
    _ = try admit_dialog(graph, 0xB00, 0x888, 12);
    try testing_t.expectEqual(@as(u32, 3), call_count_deferred(graph));
    try refresh_open_ok(graph);
    try testing_t.expectEqual(status_t.Ok, call_refresh_keep(graph, TestEpoch, 0xA00));
    var facts = owned_facts(0xB00, 0x888, 20);
    const obs = try refresh_offer_ok(graph, &facts);
    try testing_t.expectEqual(DispositionDeferred, obs.disposition);
    try testing_t.expectEqual(@as(u32, 0), try refresh_commit_ok(graph));
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x900));
    try testing_t.expect(call_find_deferred(graph, 0xA00) != null);
    try testing_t.expect(call_find_deferred(graph, 0xB00) != null);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).deferred_dropped);
    try testing_t.expectEqual(@as(u64, 0), current_stats(graph).deferred_evicted);
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
}

test "offers that fail still mark windows seen; accepted Deferred mismatch drops its subtree" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10); // StaleFacts
    _ = try admit_top(graph, 0x200, 10); // InvalidFacts
    _ = try admit_dialog(graph, 0xB00, 0x888, 10); // LookupFailed (deferred record of another incarnation)
    _ = try admit_top(graph, 0x700, 10); // Cycle
    _ = try admit_top(graph, 0x400, 10); // never offered
    _ = try admit_dialog(graph, 0x800, 0x900, 11);
    _ = discard_events(graph);
    // 0x800 is deferred behind 0x900; make it a cycle partner of the live window 0x700.
    graph.deferred[call_find_deferred(graph, 0x800).?].record.parent_window = 0x700;
    try check_invariants(graph);
    try refresh_open_ok(graph);

    var facts = window_facts(0x100, 5);
    try refresh_offer_fails(graph, &facts, .StaleFacts);
    try testing_t.expect(active_seen(graph, 0x100));

    facts = window_facts(0x200, 30);
    facts.stamp = 0;
    try refresh_offer_fails(graph, &facts, .InvalidFacts);
    try testing_t.expect(active_seen(graph, 0x200));

    facts = owned_facts(0xB00, 0x888, 31);
    facts.process_id += 1;
    lookup_probe.mode = .Fail;
    const replacement = try refresh_offer_ok(graph, &facts);
    try expect_obs(replacement, DispositionExcluded, 0, 0, 0, ReasonIncarnationChanged);
    try expect_counts(replacement, 0, 0, 0, 1);
    lookup_probe.mode = .Ok;
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0xB00));

    facts = owned_facts(0x700, 0x800, 32);
    try refresh_offer_fails(graph, &facts, .Cycle);
    try testing_t.expect(active_seen(graph, 0x700));

    try testing_t.expect(!active_seen(graph, 0x400));
    try testing_t.expect(!deferred_seen(graph, 0x800));
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.rejected_stale_facts);
    try testing_t.expectEqual(@as(u64, 1), stats.rejected_invalid_facts);
    try testing_t.expectEqual(@as(u64, 1), stats.lookup_failures);
    try testing_t.expectEqual(@as(u64, 1), stats.rejected_cycle);
    try testing_t.expectEqual(@as(u32, 1), try refresh_commit_ok(graph));
    try testing_t.expectEqual(@as(u32, 3), call_count_state(graph, .Active));
    try testing_t.expectEqual(@as(?usize, null), call_find_active(graph, 0x400));
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x800));
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0xB00));
    try testing_t.expectEqual(@as(u64, 1), stats.deferred_dropped);
    const ev = try next_ev(graph);
    try expect_ev(ev, EventRetireIntent, 0x400, 4, 0, 0, ReasonRefreshUnseen);
    try testing_t.expectEqual(@as(u32, 0), graph.ring_len);
}

test "an offer rejected with QueueFull is not applied but marks the window seen" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x5000, 10);
    _ = try admit_top(graph, 0x5001, 11);
    _ = discard_events(graph);
    fill_ring(graph, EventCapacity);
    try testing_t.expectEqual(@as(u32, 0), call_ring_room(graph));
    try refresh_open_ok(graph);
    var facts = window_facts(0x5000, 20);
    facts.x += 7;
    try refresh_offer_fails(graph, &facts, .QueueFull);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_queue_full);
    try testing_t.expect(active_seen(graph, 0x5000));
    try testing_t.expect(!active_seen(graph, 0x5001));
    try testing_t.expect(graph.entries[call_find_active(graph, 0x5000).?].record.x != facts.x);
    // Keep the other window too: the commit has no victim and needs no ring room.
    try testing_t.expectEqual(status_t.Ok, call_refresh_keep(graph, TestEpoch, 0x5001));
    try testing_t.expectEqual(@as(u32, 0), try refresh_commit_ok(graph));
    try testing_t.expectEqual(@as(u32, 2), call_count_state(graph, .Active));
    try testing_t.expectEqual(@as(u32, EventCapacity), graph.ring_len);
}

test "a flush leftover cut off by capacity survives the commit only when it is offered again" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_dialog(graph, 0x7000, 0x7001, 10);
    _ = try admit_dialog(graph, 0x7002, 0x7001, 11);
    try fill_graph(graph, 63);
    try refresh_open_ok(graph);
    var index: u64 = 0;
    while (index < 63) : (index += 1) {
        try testing_t.expectEqual(status_t.Ok, call_refresh_keep(graph, TestEpoch, 0x8000 + index));
    }
    // The owner takes the 64th slot; both owned windows stay deferred and unseen.
    var facts = window_facts(0x7001, 200);
    const obs = try refresh_offer_ok(graph, &facts);
    try testing_t.expectEqual(DispositionAdmitted, obs.disposition);
    try expect_counts(obs, 1, 1, 0, 0);
    try testing_t.expectEqual(@as(u32, 64), call_count_state(graph, .Active));
    try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
    try testing_t.expect(!deferred_seen(graph, 0x7000));
    try testing_t.expect(!deferred_seen(graph, 0x7002));
    // Offering 0x7000 again hits the full graph: AtCapacity, yet the record is marked.
    facts = owned_facts(0x7000, 0x7001, 201);
    try refresh_offer_fails(graph, &facts, .AtCapacity);
    try testing_t.expect(deferred_seen(graph, 0x7000));
    try testing_t.expectEqual(@as(u32, 0), try refresh_commit_ok(graph));
    try testing_t.expect(call_find_deferred(graph, 0x7000) != null);
    try testing_t.expectEqual(@as(?usize, null), call_find_deferred(graph, 0x7002));
    try testing_t.expectEqual(@as(u32, 64), call_count_state(graph, .Active));
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).deferred_dropped);
}

test "refresh_keep and refresh_unseen: statuses, ordering, truncation and null rules" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    // Without a refresh: the argument check outranks NoRefresh.
    try testing_t.expectEqual(status_t.NoRefresh, call_refresh_keep(graph, TestEpoch, 0x100));
    try testing_t.expectEqual(status_t.InvalidArgument, call_refresh_keep(graph, TestEpoch, 0));
    var total: u32 = 0xABCD;
    var windows = [_]u64{0xEEEE} ** 4;
    try testing_t.expectEqual(status_t.NoRefresh, call_refresh_unseen(graph, TestEpoch, &windows, 4, &total));
    try testing_t.expectEqual(status_t.InvalidArgument, call_refresh_unseen(graph, TestEpoch, null, 1, &total));
    try testing_t.expectEqual(@as(u32, 0xABCD), total);

    // Slot order differs from generation order: 0x200 reuses the slot of 0x400.
    _ = try admit_top(graph, 0x400, 10);
    _ = try admit_top(graph, 0x300, 11);
    _ = try destroy_hint(graph, 0x400, 50, 2000);
    try testing_t.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, 0x400, 1, &ProofOk));
    _ = try admit_top(graph, 0x200, 12);
    _ = try admit_dialog(graph, 0x900, 0x777, 13);
    _ = try admit_top(graph, 0x500, 14);
    _ = try destroy_hint(graph, 0x500, 60, 2000); // Retiring: never listed, never kept
    try testing_t.expectEqual(@as(usize, 0), call_find_active(graph, 0x200).?);
    try testing_t.expectEqual(@as(usize, 1), call_find_active(graph, 0x300).?);
    var order = [_]u8{0xFF} ** MaxEntries;
    try testing_t.expectEqual(@as(u32, 2), call_active_by_generation(graph, &order));
    try testing_t.expectEqualSlices(u8, &[_]u8{ 1, 0, 0xFF }, order[0..3]);

    try refresh_open_ok(graph);
    try testing_t.expectEqual(status_t.InvalidArgument, call_refresh_keep(graph, TestEpoch, 0));
    try testing_t.expectEqual(status_t.None, call_refresh_keep(graph, TestEpoch, 0xBAD));
    try testing_t.expectEqual(status_t.None, call_refresh_keep(graph, TestEpoch, 0x400));
    try testing_t.expectEqual(status_t.None, call_refresh_keep(graph, TestEpoch, 0x500));
    try testing_t.expect(!graph.entries[call_find_exact(graph, 0x500, 4).?].seen);

    try testing_t.expectEqual(status_t.Ok, call_refresh_unseen(graph, TestEpoch, &windows, 4, &total));
    try testing_t.expectEqual(@as(u32, 2), total);
    try testing_t.expectEqualSlices(u64, &[_]u64{ 0x300, 0x200, 0xEEEE, 0xEEEE }, &windows);
    // Truncation: the total stays visible, the rest of the array is untouched.
    windows = [_]u64{0xEEEE} ** 4;
    try testing_t.expectEqual(status_t.Ok, call_refresh_unseen(graph, TestEpoch, &windows, 1, &total));
    try testing_t.expectEqual(@as(u32, 2), total);
    try testing_t.expectEqualSlices(u64, &[_]u64{ 0x300, 0xEEEE, 0xEEEE, 0xEEEE }, &windows);
    // Capacity 0 accepts a null array and still reports the total.
    total = 0;
    try testing_t.expectEqual(status_t.Ok, call_refresh_unseen(graph, TestEpoch, null, 0, &total));
    try testing_t.expectEqual(@as(u32, 2), total);
    // A null array with a nonzero capacity is rejected and leaves the count alone.
    total = 0xABCD;
    try testing_t.expectEqual(status_t.InvalidArgument, call_refresh_unseen(graph, TestEpoch, null, 2, &total));
    try testing_t.expectEqual(@as(u32, 0xABCD), total);

    // keep marks Active entries and deferred records; the listing follows.
    try testing_t.expectEqual(status_t.Ok, call_refresh_keep(graph, TestEpoch, 0x300));
    try testing_t.expectEqual(status_t.Ok, call_refresh_keep(graph, TestEpoch, 0x900));
    try testing_t.expect(active_seen(graph, 0x300));
    try testing_t.expect(deferred_seen(graph, 0x900));
    windows = [_]u64{0xEEEE} ** 4;
    try testing_t.expectEqual(status_t.Ok, call_refresh_unseen(graph, TestEpoch, &windows, 4, &total));
    try testing_t.expectEqual(@as(u32, 1), total);
    try testing_t.expectEqualSlices(u64, &[_]u64{ 0x200, 0xEEEE, 0xEEEE, 0xEEEE }, &windows);
    // An Ok plain observe marks, an offer marks, and the listing is empty at the end.
    var facts = window_facts(0x200, 70);
    _ = try offer(graph, &facts);
    try testing_t.expect(active_seen(graph, 0x200));
    total = 9;
    try testing_t.expectEqual(status_t.Ok, call_refresh_unseen(graph, TestEpoch, &windows, 4, &total));
    try testing_t.expectEqual(@as(u32, 0), total);
    try testing_t.expectEqualSlices(u64, &[_]u64{ 0x200, 0xEEEE, 0xEEEE, 0xEEEE }, &windows);
    try check_invariants(graph);
    // The listing moved no mark and no event.
    try testing_t.expectEqual(@as(u32, 0), try refresh_commit_ok(graph));
}

test "a commit that does not fit the ring changes nothing and succeeds after draining" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x6000, 10);
    _ = try admit_top(graph, 0x6001, 11);
    _ = try admit_top(graph, 0x6002, 12);
    _ = try admit_dialog(graph, 0x6100, 0x7777, 13);
    _ = discard_events(graph);
    fill_ring(graph, EventCapacity - 2);
    try refresh_open_ok(graph);
    const before = state_digest(graph);
    var retired: u32 = 0xDEAD_BEEF;
    try testing_t.expectEqual(status_t.QueueFull, call_refresh_commit(graph, TestEpoch, &retired));
    try testing_t.expectEqual(@as(u32, 0xDEAD_BEEF), retired);
    try testing_t.expectEqual(before, state_digest(graph));
    try testing_t.expect(graph.refresh_open);
    try check_invariants(graph);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_queue_full);
    try testing_t.expectEqual(@as(u64, 0), current_stats(graph).refresh_commits);
    try testing_t.expectEqual(@as(u32, 3), call_count_state(graph, .Active));
    try testing_t.expectEqual(@as(u32, 1), call_count_deferred(graph));
    // One slot freed: room 3 for exactly 3 victims.
    var ev = std_t.mem.zeroes(event_t);
    try testing_t.expectEqual(status_t.Ok, call_next_event(graph, TestEpoch, &ev));
    try testing_t.expectEqual(@as(u32, 3), call_ring_room(graph));
    try testing_t.expectEqual(@as(u32, 3), try refresh_commit_ok(graph));
    try testing_t.expectEqual(@as(u32, 0), call_ring_room(graph));
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).refresh_commits);
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).deferred_dropped);
}

test "refresh_abort closes the refresh without retiring or dropping anything" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = try admit_dialog(graph, 0x900, 0x777, 11);
    _ = discard_events(graph);
    try testing_t.expectEqual(status_t.NoRefresh, call_refresh_abort(graph, TestEpoch));
    try testing_t.expectEqual(@as(u64, 0), current_stats(graph).refresh_aborts);
    try refresh_open_ok(graph);
    const before = state_digest(graph);
    try testing_t.expectEqual(status_t.Ok, call_refresh_abort(graph, TestEpoch));
    try testing_t.expect(!graph.refresh_open);
    try testing_t.expectEqual(before, state_digest(graph));
    try check_invariants(graph);
    var stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.refresh_aborts);
    try testing_t.expectEqual(@as(u64, 0), stats.refresh_commits);
    try testing_t.expectEqual(@as(u32, 0), stats.refresh_active);
    try testing_t.expectEqual(status_t.NoRefresh, call_refresh_commit(graph, TestEpoch, null));
    try testing_t.expectEqual(status_t.NoRefresh, call_refresh_keep(graph, TestEpoch, 0x100));
    var facts = window_facts(0x100, 20);
    var obs = sentinel_observation();
    try testing_t.expectEqual(status_t.NoRefresh, call_refresh_offer(graph, TestEpoch, &facts, &obs));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&obs), 0xA5));
    try testing_t.expectEqual(before, state_digest(graph));
    // A new refresh starts from clean marks and can commit normally.
    try refresh_open_ok(graph);
    try testing_t.expect(!active_seen(graph, 0x100));
    try testing_t.expectEqual(@as(u32, 1), try refresh_commit_ok(graph));
    stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 1), stats.refresh_commits);
    try testing_t.expectEqual(@as(u64, 1), stats.refresh_aborts);
}

test "an offer without an open refresh returns NoRefresh after the argument checks and marks nothing" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    // Invalid facts are not even looked at: the NoRefresh check precedes validation.
    var facts = window_facts(0x100, 0);
    var obs = sentinel_observation();
    const before = state_digest(graph);
    try testing_t.expectEqual(status_t.NoRefresh, call_refresh_offer(graph, TestEpoch, &facts, &obs));
    try testing_t.expectEqual(before, state_digest(graph));
    try testing_t.expectEqual(@as(u64, 0), current_stats(graph).rejected_invalid_facts);
    // Inside a refresh the same facts fail validation and still mark the window.
    try refresh_open_ok(graph);
    try refresh_offer_fails(graph, &facts, .InvalidFacts);
    try testing_t.expect(active_seen(graph, 0x100));
    // Window 0 can not be marked and fails validation without touching any mark.
    facts = window_facts(0, 11);
    try refresh_offer_fails(graph, &facts, .InvalidFacts);
}

test "refresh_offer hands the lookup its own stack copy of the facts" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    try refresh_open_ok(graph);
    var facts = window_facts(0x100, 10);
    const obs = try refresh_offer_ok(graph, &facts);
    try testing_t.expectEqual(DispositionAdmitted, obs.disposition);
    try testing_t.expect(lookup_probe.last_pointer != @intFromPtr(&facts));
    try testing_t.expectEqual(@as(u64, 0x100), lookup_probe.last_window);
    // A new generation is born seen.
    try testing_t.expect(active_seen(graph, 0x100));
    try testing_t.expectEqual(@as(u32, 0), try refresh_commit_ok(graph));
}

test "a reclassification inside a refresh keeps the new generation seen and its re-deferred children unseen" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    try build_family(graph);
    try refresh_open_ok(graph);
    var facts = reclassify_facts(20);
    const obs = try refresh_offer_ok(graph, &facts);
    try testing_t.expectEqual(DispositionReclassified, obs.disposition);
    // Q (0x50) is unseen, the reclassified P and its flushed children inherit the marks of their entries.
    try testing_t.expect(active_seen(graph, 0x100));
    try testing_t.expect(!active_seen(graph, 0x50));
    // Q is the owner of P now: the commit retires Q and with it the whole new subtree.
    _ = discard_events(graph);
    const retired = try refresh_commit_ok(graph);
    try testing_t.expectEqual(@as(u32, 5), retired);
    try testing_t.expectEqual(@as(u32, 0), call_count_state(graph, .Active));
}

test "invalidate_all retires every Active entry children first with reason Invalidated" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    try build_family(graph);
    _ = try admit_dialog(graph, 0x900, 0x777, 20);
    _ = try admit_dialog(graph, 0xA00, 0x888, 21);
    // A quarantined entry stays exactly as it is.
    _ = try destroy_hint(graph, 0x50, 30, 2000);
    _ = discard_events(graph);
    try refresh_open_ok(graph);
    // Room for 3 of the 4 victims: QueueFull and nothing changed, the refresh stays open.
    fill_ring(graph, EventCapacity - 3);
    const before = state_digest(graph);
    var retired: u32 = 0xDEAD_BEEF;
    try testing_t.expectEqual(status_t.QueueFull, call_invalidate_all(graph, TestEpoch, &retired));
    try testing_t.expectEqual(@as(u32, 0xDEAD_BEEF), retired);
    try testing_t.expectEqual(before, state_digest(graph));
    try testing_t.expect(graph.refresh_open);
    try testing_t.expectEqual(@as(u32, 2), call_count_deferred(graph));
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).rejected_queue_full);
    try testing_t.expectEqual(@as(u64, 0), current_stats(graph).refresh_aborts);
    try check_invariants(graph);
    _ = discard_events(graph);
    try testing_t.expectEqual(status_t.Ok, call_invalidate_all(graph, TestEpoch, &retired));
    try testing_t.expectEqual(@as(u32, 4), retired);
    try check_invariants(graph);
    var events: [8]event_t = undefined;
    try testing_t.expectEqual(@as(usize, 4), try drain_events(graph, &events));
    try expect_ev(events[0], EventRetireIntent, 0x400, 5, 0x200, 3, ReasonInvalidated);
    try expect_ev(events[1], EventRetireIntent, 0x300, 4, 0x100, 2, ReasonInvalidated);
    try expect_ev(events[2], EventRetireIntent, 0x200, 3, 0x100, 2, ReasonInvalidated);
    try expect_ev(events[3], EventRetireIntent, 0x100, 2, 0, 0, ReasonInvalidated);
    try testing_t.expectEqual(@as(u32, 0), call_count_state(graph, .Active));
    try testing_t.expectEqual(@as(u32, 5), call_count_state(graph, .Retiring));
    try testing_t.expectEqual(ReasonDestroyed, graph.entries[call_find_exact(graph, 0x50, 1).?].record.retire_reason);
    try testing_t.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try testing_t.expect(!graph.refresh_open);
    const stats = current_stats(graph);
    try testing_t.expectEqual(@as(u64, 2), stats.deferred_dropped);
    try testing_t.expectEqual(@as(u64, 1), stats.refresh_aborts);
    try testing_t.expectEqual(@as(u64, 0), stats.refresh_commits);
    try testing_t.expectEqual(@as(u32, 0), stats.refresh_active);
    try testing_t.expectEqual(status_t.NoRefresh, call_refresh_commit(graph, TestEpoch, null));
    // Repeating it on an empty Active set is Ok, retires nothing and aborts nothing more.
    try testing_t.expectEqual(status_t.Ok, call_invalidate_all(graph, TestEpoch, &retired));
    try testing_t.expectEqual(@as(u32, 0), retired);
    try testing_t.expectEqual(status_t.Ok, call_invalidate_all(graph, TestEpoch, null));
    try testing_t.expectEqual(@as(u64, 1), current_stats(graph).refresh_aborts);
}

test "every refresh function rejects a stale epoch and changes only the rejection counter" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    _ = discard_events(graph);
    try refresh_open_ok(graph);
    const before = state_digest(graph);
    var facts = window_facts(0x100, 20);
    var obs = sentinel_observation();
    var windows = [_]u64{0xEEEE};
    var total: u32 = 0xABCD;
    var retired: u32 = 0xDEAD_BEEF;
    var rejected: u64 = 0;
    for ([_]u64{ TestEpoch + 1, 0 }) |stale| {
        try testing_t.expectEqual(status_t.StaleEpoch, call_refresh_begin(graph, stale));
        try testing_t.expectEqual(status_t.StaleEpoch, call_refresh_offer(graph, stale, &facts, &obs));
        try testing_t.expectEqual(status_t.StaleEpoch, call_refresh_keep(graph, stale, 0x100));
        try testing_t.expectEqual(status_t.StaleEpoch, call_refresh_unseen(graph, stale, &windows, 1, &total));
        try testing_t.expectEqual(status_t.StaleEpoch, call_refresh_commit(graph, stale, &retired));
        try testing_t.expectEqual(status_t.StaleEpoch, call_refresh_abort(graph, stale));
        try testing_t.expectEqual(status_t.StaleEpoch, call_invalidate_all(graph, stale, &retired));
        rejected += 7;
        try testing_t.expectEqual(rejected, current_stats(graph).rejected_stale_epoch);
    }
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&obs), 0xA5));
    try testing_t.expectEqual(@as(u64, 0xEEEE), windows[0]);
    try testing_t.expectEqual(@as(u32, 0xABCD), total);
    try testing_t.expectEqual(@as(u32, 0xDEAD_BEEF), retired);
    try testing_t.expectEqual(before, state_digest(graph));
    try testing_t.expect(graph.refresh_open);
    try testing_t.expect(!graph.busy);
}

const refresh_thread_probe_t = struct {
    graph: *graph_t,
    statuses: [7]status_t = [_]status_t{.Ok} ** 7,
};

fn refresh_thread_body(probe: *refresh_thread_probe_t) void {
    var facts = window_facts(0x100, 20);
    var obs = sentinel_observation();
    var windows = [_]u64{0xEEEE};
    var total: u32 = 0;
    var retired: u32 = 0;
    probe.statuses[0] = call_refresh_begin(probe.graph, TestEpoch);
    probe.statuses[1] = call_refresh_offer(probe.graph, TestEpoch, &facts, &obs);
    probe.statuses[2] = call_refresh_keep(probe.graph, TestEpoch, 0x100);
    probe.statuses[3] = call_refresh_unseen(probe.graph, TestEpoch, &windows, 1, &total);
    probe.statuses[4] = call_refresh_commit(probe.graph, TestEpoch, &retired);
    probe.statuses[5] = call_refresh_abort(probe.graph, TestEpoch);
    probe.statuses[6] = call_invalidate_all(probe.graph, TestEpoch, &retired);
}

test "every refresh function from a foreign thread returns WrongThread and touches nothing" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    try refresh_open_ok(graph);
    // Busy is set too: WrongThread must outrank Reentrant, and a wrong epoch must not be counted.
    graph.busy = true;
    const before = state_digest(graph);
    const stats_before = graph.stats;
    var probe = refresh_thread_probe_t{ .graph = graph };
    const thread = try std_t.Thread.spawn(.{}, refresh_thread_body, .{&probe});
    thread.join();
    for (probe.statuses) |status| try testing_t.expectEqual(status_t.WrongThread, status);
    try testing_t.expectEqual(before, state_digest(graph));
    try testing_t.expectEqualSlices(u8, std_t.mem.asBytes(&stats_before), stats_bytes(graph));
    try testing_t.expect(graph.busy);
    graph.busy = false;
    try testing_t.expect(graph.refresh_open);
}

test "every refresh function called while the graph is busy returns Reentrant and touches nothing" {
    const graph = try probe_graph(testing_t.allocator);
    defer call_destroy(graph);
    _ = try admit_top(graph, 0x100, 10);
    try refresh_open_ok(graph);
    graph.busy = true;
    const before = state_digest(graph);
    const stats_before = graph.stats;
    var facts = window_facts(0x100, 20);
    var obs = sentinel_observation();
    var windows = [_]u64{0xEEEE};
    var total: u32 = 0xABCD;
    var retired: u32 = 0xDEAD_BEEF;
    try testing_t.expectEqual(status_t.Reentrant, call_refresh_begin(graph, TestEpoch));
    try testing_t.expectEqual(status_t.Reentrant, call_refresh_offer(graph, TestEpoch, &facts, &obs));
    try testing_t.expectEqual(status_t.Reentrant, call_refresh_keep(graph, TestEpoch, 0x100));
    try testing_t.expectEqual(status_t.Reentrant, call_refresh_unseen(graph, TestEpoch, &windows, 1, &total));
    try testing_t.expectEqual(status_t.Reentrant, call_refresh_commit(graph, TestEpoch, &retired));
    try testing_t.expectEqual(status_t.Reentrant, call_refresh_abort(graph, TestEpoch));
    try testing_t.expectEqual(status_t.Reentrant, call_invalidate_all(graph, TestEpoch, &retired));
    try testing_t.expect(std_t.mem.allEqual(u8, std_t.mem.asBytes(&obs), 0xA5));
    try testing_t.expectEqual(@as(u32, 0xABCD), total);
    try testing_t.expectEqual(@as(u32, 0xDEAD_BEEF), retired);
    try testing_t.expectEqual(before, state_digest(graph));
    try testing_t.expectEqualSlices(u8, std_t.mem.asBytes(&stats_before), stats_bytes(graph));
    graph.busy = false;
}
