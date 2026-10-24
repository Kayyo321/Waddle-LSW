//! Session-wide guest window graph, layer 1: pure value rules.
//!
//! This file holds the stateless part of the graph engine (TODO #10): the C ABI
//! type aliases, the PascalCase constants asserted against
//! `include/waddle/interop_window_graph.h`, the persistent record layout, facts
//! validation, the window classifier (header steps 0 to 8), identity comparison
//! and the small arithmetic helpers (FNV-1a class hash, wrap-safe tick order,
//! saturating counters). Nothing here allocates, keeps state, calls the
//! operating system or touches a thread. The stateful engine (entries, deferred
//! records, event ring, refresh, retirement) is appended below by the next stage.
//!
//! Every function documented here is pure and therefore safe to call from any
//! thread concurrently with other pure calls on distinct or shared read-only
//! arguments. Test blocks live below the `Test-only` delimiter line at the end
//! of the production area, which is the split point of the coverage tool.

const std = @import("std");

/// The committed public C header, imported verbatim. The engine uses these C
/// structs directly: there are no hand-written mirrors that could drift.
pub const c = @cImport({
    @cInclude("waddle/interop_window_graph.h");
});

/// One native window snapshot supplied by the observer (`interop_window_facts_t`).
pub const facts_t = c.interop_window_facts_t;
/// One queued event (`interop_window_event_t`); a value copy with no pointers.
pub const event_t = c.interop_window_event_t;
/// Result summary of observe, refresh_offer and note_destroyed.
pub const observation_t = c.interop_window_observation_t;
/// Description of the Active entry for one window.
pub const live_t = c.interop_window_live_t;
/// One quarantined entry as listed by list_retiring.
pub const retiring_entry_t = c.interop_window_retiring_entry_t;
/// Coordinator proof that ends a quarantine.
pub const proof_t = c.interop_window_retire_proof_t;
/// Cumulative counters and gauges of one graph.
pub const stats_t = c.interop_window_graph_stats_t;
/// Rate-limited capacity diagnostic.
pub const diagnostic_t = c.interop_window_capacity_diagnostic_t;
/// Graph creation parameters (callbacks, epoch, UAC mode).
pub const config_t = c.interop_window_graph_config_t;

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
/// Mask of every defined fact flag; any other bit is invalid facts.
pub const FactMask: u32 = 0x0FFF;

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
        .{ MaxEntries, c.InteropMaxWindows },
        .{ MaxDeferred, c.InteropWindowGraphMaxDeferred },
        .{ EventCapacity, c.InteropWindowGraphEventCapacity },
        .{ MaxCallEvents, c.InteropWindowGraphMaxCallEvents },
        .{ RefreshIntervalMs, c.InteropWindowGraphRefreshIntervalMs },
        .{ DiagnosticIntervalMs, c.InteropWindowGraphDiagnosticIntervalMs },
        .{ MaxClassBytes, c.InteropWindowGraphMaxClassBytes },
        .{ MaxImageBytes, c.InteropWindowGraphMaxImageBytes },
        .{ MaxTitleBytes, c.InteropWindowGraphMaxTitleBytes },
        .{ MaxTitleBytes, c.InteropMaxTitleBytes },
        .{ MaxAppKeyBytes, c.InteropWindowGraphMaxAppKeyBytes },
        .{ MaxAppKeyBytes, c.InteropMaxAppKeyBytes },
        .{ MinDpi, c.InteropWindowGraphMinDpi },
        .{ MaxDpi, c.InteropWindowGraphMaxDpi },
        .{ MaxExtent, c.InteropWindowGraphMaxExtent },
        .{ FactVisible, c.InteropWindowFactVisible },
        .{ FactEnabled, c.InteropWindowFactEnabled },
        .{ FactIconic, c.InteropWindowFactIconic },
        .{ FactZoomed, c.InteropWindowFactZoomed },
        .{ FactLayered, c.InteropWindowFactLayered },
        .{ FactLayeredAlphaKnown, c.InteropWindowFactLayeredAlphaKnown },
        .{ FactFullscreen, c.InteropWindowFactFullscreen },
        .{ FactAgentOwned, c.InteropWindowFactAgentOwned },
        .{ FactAgentPreedit, c.InteropWindowFactAgentPreedit },
        .{ FactOnInputDesktop, c.InteropWindowFactOnInputDesktop },
        .{ FactClassTruncated, c.InteropWindowFactClassTruncated },
        .{ FactImageUnknown, c.InteropWindowFactImageUnknown },
        .{ FactMask, c.InteropWindowFactMask },
        .{ ChangeRect, c.InteropWindowChangeRect },
        .{ ChangeState, c.InteropWindowChangeState },
        .{ ChangeDpi, c.InteropWindowChangeDpi },
        .{ ChangeTitle, c.InteropWindowChangeTitle },
        .{ ChangeMinSize, c.InteropWindowChangeMinSize },
        .{ ChangeMask, c.InteropWindowChangeMask },
        .{ EventAnnounce, c.InteropWindowEventAnnounce },
        .{ EventUpdate, c.InteropWindowEventUpdate },
        .{ EventRetireIntent, c.InteropWindowEventRetireIntent },
        .{ UacWindow, c.InteropWindowUacWindow },
        .{ UacAuto, c.InteropWindowUacAuto },
        .{ UacNative, c.InteropWindowUacNative },
        .{ DispositionUnchanged, c.InteropWindowDispositionUnchanged },
        .{ DispositionAdmitted, c.InteropWindowDispositionAdmitted },
        .{ DispositionUpdated, c.InteropWindowDispositionUpdated },
        .{ DispositionDeferred, c.InteropWindowDispositionDeferred },
        .{ DispositionRetired, c.InteropWindowDispositionRetired },
        .{ DispositionReclassified, c.InteropWindowDispositionReclassified },
        .{ DispositionExcluded, c.InteropWindowDispositionExcluded },
        .{ DispositionSuspended, c.InteropWindowDispositionSuspended },
        .{ ReasonNone, c.InteropWindowReasonNone },
        .{ ReasonNotVisible, c.InteropWindowReasonNotVisible },
        .{ ReasonExcludedClass, c.InteropWindowReasonExcludedClass },
        .{ ReasonExcludedCoreWindow, c.InteropWindowReasonExcludedCoreWindow },
        .{ ReasonExcludedAgent, c.InteropWindowReasonExcludedAgent },
        .{ ReasonExcludedUacDummy, c.InteropWindowReasonExcludedUacDummy },
        .{ ReasonUacNativeHidden, c.InteropWindowReasonUacNativeHidden },
        .{ ReasonChild, c.InteropWindowReasonChild },
        .{ ReasonZeroArea, c.InteropWindowReasonZeroArea },
        .{ ReasonTransparent, c.InteropWindowReasonTransparent },
        .{ ReasonParentPending, c.InteropWindowReasonParentPending },
        .{ ReasonDestroyed, c.InteropWindowReasonDestroyed },
        .{ ReasonIncarnationChanged, c.InteropWindowReasonIncarnationChanged },
        .{ ReasonReclassified, c.InteropWindowReasonReclassified },
        .{ ReasonParentRetired, c.InteropWindowReasonParentRetired },
        .{ ReasonRefreshUnseen, c.InteropWindowReasonRefreshUnseen },
        .{ ReasonInvalidated, c.InteropWindowReasonInvalidated },
        .{ RoleToplevel, c.InteropRoleToplevel },
        .{ RoleDialog, c.InteropRoleDialog },
        .{ RoleMenu, c.InteropRoleMenu },
        .{ RoleTooltip, c.InteropRoleTooltip },
        .{ RoleUacConsent, c.InteropRoleUacConsent },
        .{ RoleImePreedit, c.InteropRoleImePreedit },
        .{ StateMinimized, c.InteropStateMinimized },
        .{ StateMaximized, c.InteropStateMaximized },
        .{ StateFullscreen, c.InteropStateFullscreen },
        .{ StateActivated, c.InteropStateActivated },
        .{ StateResizable, c.InteropStateResizable },
        .{ StateMaximizable, c.InteropStateMaximizable },
        .{ StateMinimizable, c.InteropStateMinimizable },
        .{ StateMask, c.InteropStateMask },
        .{ StylePopup, c.InteropWindowStylePopup },
        .{ StyleCaption, c.InteropWindowStyleCaption },
        .{ StyleThickFrame, c.InteropWindowStyleThickFrame },
        .{ StyleMaximizeBox, c.InteropWindowStyleMaximizeBox },
        .{ StyleMinimizeBox, c.InteropWindowStyleMinimizeBox },
        .{ ExStyleToolWindow, c.InteropWindowExStyleToolWindow },
        .{ ExStyleAppWindow, c.InteropWindowExStyleAppWindow },
        .{ ExStyleLayered, c.InteropWindowExStyleLayered },
        .{ ExStyleNoActivate, c.InteropWindowExStyleNoActivate },
        .{ @intFromEnum(status_t.Ok), c.InteropWindowGraphStatusOk },
        .{ @intFromEnum(status_t.None), c.InteropWindowGraphStatusNone },
        .{ @intFromEnum(status_t.InvalidArgument), c.InteropWindowGraphStatusInvalidArgument },
        .{ @intFromEnum(status_t.InvalidFacts), c.InteropWindowGraphStatusInvalidFacts },
        .{ @intFromEnum(status_t.WrongThread), c.InteropWindowGraphStatusWrongThread },
        .{ @intFromEnum(status_t.Reentrant), c.InteropWindowGraphStatusReentrant },
        .{ @intFromEnum(status_t.NoMemory), c.InteropWindowGraphStatusNoMemory },
        .{ @intFromEnum(status_t.StaleEpoch), c.InteropWindowGraphStatusStaleEpoch },
        .{ @intFromEnum(status_t.Stale), c.InteropWindowGraphStatusStale },
        .{ @intFromEnum(status_t.StaleFacts), c.InteropWindowGraphStatusStaleFacts },
        .{ @intFromEnum(status_t.QueueFull), c.InteropWindowGraphStatusQueueFull },
        .{ @intFromEnum(status_t.AtCapacity), c.InteropWindowGraphStatusAtCapacity },
        .{ @intFromEnum(status_t.Cycle), c.InteropWindowGraphStatusCycle },
        .{ @intFromEnum(status_t.LookupFailed), c.InteropWindowGraphStatusLookupFailed },
        .{ @intFromEnum(status_t.AppKeyInvalid), c.InteropWindowGraphStatusAppKeyInvalid },
        .{ @intFromEnum(status_t.GenerationExhausted), c.InteropWindowGraphStatusGenerationExhausted },
        .{ @intFromEnum(status_t.RefreshActive), c.InteropWindowGraphStatusRefreshActive },
        .{ @intFromEnum(status_t.NoRefresh), c.InteropWindowGraphStatusNoRefresh },
        .{ @intFromEnum(status_t.ProofIncomplete), c.InteropWindowGraphStatusProofIncomplete },
        .{ @intFromEnum(status_t.NotRetiring), c.InteropWindowGraphStatusNotRetiring },
        .{ @intFromEnum(status_t.SinkFailed), c.InteropWindowGraphStatusSinkFailed },
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
    /// `captured_ms` of the admitting facts (wrap-safe tick domain).
    admit_captured_ms: u32,
    /// `captured_ms` of the newest accepted facts.
    last_captured_ms: u32,
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
    return std.mem.zeroes(record_t);
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
    if (std.mem.indexOfScalar(u8, bytes, 0) != null) return false;
    return std.unicode.utf8ValidateSlice(bytes);
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
        if (std.ascii.toLower(left) != std.ascii.toLower(right)) return false;
    }
    return true;
}

/// Order two 32-bit tick values in a wrap-safe way.
///
/// @param[in] a First tick.
/// @param[in] b Second tick.
/// @return true iff the signed 32-bit difference `a - b` is negative, that is `a`
///         is earlier than `b` within half the tick range. A difference of exactly
///         2^31 is negative in both directions (ambiguous, treated as earlier).
/// @note Pure; thread-safe.
pub fn time_before(a: u32, b: u32) bool {
    const difference: i32 = @bitCast(a -% b);
    return difference < 0;
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
/// LayeredAlphaKnown requires Layered; ImageUnknown set if and only if
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

/// Outcome of classification.
pub const verdict_t = union(enum) {
    /// The window is exportable with this role and parent.
    eligible: eligible_t,
    /// The window is not exported; the payload is a classification `Reason*` code (1..9).
    not_exported: u32,
    /// The input desktop is not Default: nothing may be admitted, retired or updated.
    suspended: void,
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

/// Step 8 gate shared by every role.
fn gate(facts: *const facts_t, role: u32, parent_window: u64) verdict_t {
    if (facts.width == 0 or facts.height == 0) return .{ .not_exported = ReasonZeroArea };
    if (flag_set(facts, FactLayered) and flag_set(facts, FactLayeredAlphaKnown) and facts.alpha == 0) {
        return .{ .not_exported = ReasonTransparent };
    }
    return .{ .eligible = .{ .role = role, .parent_window = parent_window } };
}

/// Classify a facts snapshot (header steps 0 to 8; the first matching step wins).
///
/// Step 0: OnInputDesktop clear is `suspended`; Visible clear is NotVisible.
/// Step 1: excluded classes, CoreWindow of the three shell-host images, then the
/// agent's own windows (preedit selects ImePreedit and skips to step 8).
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
    if (!flag_set(facts, FactOnInputDesktop)) return .suspended;
    if (!flag_set(facts, FactVisible)) return .{ .not_exported = ReasonNotVisible };
    if (class_is_excluded(facts)) return .{ .not_exported = ReasonExcludedClass };
    if (class_is(facts, CoreWindowClass) and image_is_shell_host(facts)) {
        return .{ .not_exported = ReasonExcludedCoreWindow };
    }
    if (flag_set(facts, FactAgentOwned)) {
        if (!flag_set(facts, FactAgentPreedit)) return .{ .not_exported = ReasonExcludedAgent };
        return gate(facts, RoleImePreedit, 0);
    }
    if (image_is(facts, ConsentImage)) {
        if (class_is(facts, UacDummyClass)) return .{ .not_exported = ReasonExcludedUacDummy };
        if (class_is(facts, UacCredentialClass)) {
            if (uac_mode != UacWindow and uac_mode != UacAuto) return .{ .not_exported = ReasonUacNativeHidden };
            return gate(facts, RoleUacConsent, 0);
        }
    }
    if (facts.root != facts.window) return .{ .not_exported = ReasonChild };
    const owned = facts.owner != 0;
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

// Stateful engine: appended by the next stage.
// Test-only fixtures.

const testing = std.testing;

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

fn call_before(a: u32, b: u32) bool {
    return @call(.never_inline, time_before, .{ a, b });
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
    var facts = std.mem.zeroes(facts_t);
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
    return .{ .eligible = .{ .role = role, .parent_window = parent_window } };
}

fn excluded(reason: u32) verdict_t {
    return .{ .not_exported = reason };
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
    .{ .name = "off input desktop is suspended", .flags_clear = FactOnInputDesktop, .expect = .suspended },
    .{ .name = "off input desktop beats invisible", .flags_clear = FactOnInputDesktop | FactVisible, .expect = .suspended },
    .{ .name = "off input desktop beats excluded class", .class = "Progman", .flags_clear = FactOnInputDesktop, .expect = .suspended },
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
    .{ .name = "preedit skips owner and menu steps", .flags_set = AgentPreeditFlags, .style = PopupNoCaption, .owned = true, .expect = eligible(RoleImePreedit, 0) },
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
        testing.expect(call_validate(&facts)) catch |err| {
            std.debug.print("matrix row has invalid facts: {s}\n", .{row.name});
            return err;
        };
        const got = call_classify(&facts, row.uac);
        testing.expectEqual(row.expect, got) catch |err| {
            std.debug.print("matrix row failed: {s}\n", .{row.name});
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
        try testing.expectEqual(excluded(ReasonExcludedClass), call_classify(&facts, UacWindow));
        var mangled: [64]u8 = undefined;
        for (name, 0..) |byte, index| {
            mangled[index] = if (index % 2 == 0) std.ascii.toUpper(byte) else std.ascii.toLower(byte);
        }
        set_class(&facts, mangled[0..name.len]);
        try testing.expectEqual(excluded(ReasonExcludedClass), call_classify(&facts, UacNative));
        set_class(&facts, name[0 .. name.len - 1]);
        try testing.expectEqual(eligible(RoleToplevel, 0), call_classify(&facts, UacWindow));
        @memcpy(mangled[0..name.len], name);
        mangled[name.len] = 'x';
        set_class(&facts, mangled[0 .. name.len + 1]);
        try testing.expectEqual(eligible(RoleToplevel, 0), call_classify(&facts, UacWindow));
        set_class(&facts, name);
        facts.flags |= FactClassTruncated;
        try testing.expectEqual(eligible(RoleToplevel, 0), call_classify(&facts, UacWindow));
    }
}

test "classification parents follow owner_root, never the raw owner" {
    var facts = fixture_facts();
    facts.owner = OwnerWindow;
    facts.owner_root = OwnerRootWindow;
    try testing.expectEqual(eligible(RoleDialog, OwnerRootWindow), call_classify(&facts, UacWindow));
    facts.ex_style = ExStyleNoActivate;
    try testing.expectEqual(eligible(RoleTooltip, OwnerRootWindow), call_classify(&facts, UacWindow));
    set_class(&facts, "#32768");
    try testing.expectEqual(eligible(RoleMenu, OwnerRootWindow), call_classify(&facts, UacWindow));
    // Self ownership is the graph's Cycle case: the classifier still reports the parent as given.
    facts.owner_root = facts.window;
    try testing.expectEqual(eligible(RoleMenu, facts.window), call_classify(&facts, UacWindow));
}

test "classification never reads past an unvalidated class or image length" {
    var facts = fixture_facts();
    facts.class_length = 0xFFFF_FFFF;
    facts.image_length = 0xFFFF_FFFF;
    facts.title_length = 0xFFFF_FFFF;
    try testing.expectEqual(@as(usize, MaxClassBytes), call_class_bytes(&facts).len);
    try testing.expectEqual(@as(usize, MaxImageBytes), call_image_bytes(&facts).len);
    try testing.expectEqual(@as(usize, MaxTitleBytes), call_title_bytes(&facts).len);
    try testing.expectEqual(eligible(RoleToplevel, 0), call_classify(&facts, UacWindow));
    try testing.expect(!call_validate(&facts));
    facts.class_length = 0;
    facts.image_length = 0;
    facts.title_length = 0;
    try testing.expectEqual(@as(usize, 0), call_class_bytes(&facts).len);
    try testing.expectEqual(@as(usize, 0), call_image_bytes(&facts).len);
    try testing.expectEqual(@as(usize, 0), call_title_bytes(&facts).len);
    set_class(&facts, "Notepad");
    set_image(&facts, "notepad.exe");
    set_title(&facts, "Untitled");
    try testing.expectEqualStrings("Notepad", call_class_bytes(&facts));
    try testing.expectEqualStrings("notepad.exe", call_image_bytes(&facts));
    try testing.expectEqualStrings("Untitled", call_title_bytes(&facts));
}

test "valid facts: defaults, maxima and boundaries" {
    var facts = fixture_facts();
    try testing.expect(call_validate(&facts));
    facts.window = std.math.maxInt(u64);
    facts.root = std.math.maxInt(u64);
    facts.stamp = std.math.maxInt(u64);
    facts.owner = 1;
    facts.owner_root = std.math.maxInt(u64);
    facts.dpi = MinDpi;
    try testing.expect(call_validate(&facts));
    facts.dpi = MaxDpi;
    facts.width = MaxExtent;
    facts.height = MaxExtent;
    facts.min_width = MaxExtent;
    facts.min_height = MaxExtent;
    facts.alpha = 0;
    facts.flags = FactMask & ~(FactAgentPreedit | FactLayeredAlphaKnown | FactImageUnknown);
    facts.style = std.math.maxInt(u32);
    facts.ex_style = std.math.maxInt(u32);
    facts.x = std.math.minInt(i32);
    facts.y = std.math.maxInt(i32);
    try testing.expect(call_validate(&facts));
    // Maximal strings of non-ASCII text: 128 two-byte characters fill the class array.
    const two_byte = "\xC3\xA9" ** 128;
    @memcpy(facts.class_name[0..two_byte.len], two_byte);
    facts.class_length = MaxClassBytes;
    @memset(&facts.image_name, 'i');
    facts.image_length = MaxImageBytes;
    facts.flags &= ~FactImageUnknown;
    @memset(&facts.title, 't');
    facts.title_length = MaxTitleBytes;
    try testing.expect(call_validate(&facts));
    // Preedit with agent ownership and layered with known alpha are valid combinations.
    facts.flags |= FactAgentOwned | FactAgentPreedit | FactLayered | FactLayeredAlphaKnown;
    try testing.expect(call_validate(&facts));
    // Empty title and unknown image are valid.
    facts = fixture_facts();
    set_title(&facts, "");
    set_image(&facts, "");
    try testing.expect(call_validate(&facts));
    try testing.expect(facts.flags & FactImageUnknown != 0);
}

test "validate_facts rejects each violated rule and nothing else" {
    const base = fixture_facts();
    try testing.expect(call_validate(&base));

    var facts = base;
    facts.window = 0;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.root = 0;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.stamp = 0;
    try testing.expect(!call_validate(&facts));

    facts = base;
    facts.owner = 5;
    facts.owner_root = 0;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.owner = 0;
    facts.owner_root = 5;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.owner = 5;
    facts.owner_root = 6;
    try testing.expect(call_validate(&facts));

    var bit: u5 = 12;
    while (true) : (bit += 1) {
        facts = base;
        facts.flags |= @as(u32, 1) << bit;
        try testing.expect(!call_validate(&facts));
        if (bit == 31) break;
    }

    facts = base;
    facts.flags |= FactAgentPreedit;
    try testing.expect(!call_validate(&facts));
    facts.flags |= FactAgentOwned;
    try testing.expect(call_validate(&facts));
    facts = base;
    facts.flags |= FactLayeredAlphaKnown;
    try testing.expect(!call_validate(&facts));
    facts.flags |= FactLayered;
    try testing.expect(call_validate(&facts));

    facts = base;
    facts.flags |= FactImageUnknown;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.image_length = 0;
    try testing.expect(!call_validate(&facts));
    facts.flags |= FactImageUnknown;
    try testing.expect(call_validate(&facts));

    for ([_]u32{ 0, 1, MinDpi - 1, MaxDpi + 1, std.math.maxInt(u32) }) |dpi| {
        facts = base;
        facts.dpi = dpi;
        try testing.expect(!call_validate(&facts));
    }
    facts = base;
    facts.width = MaxExtent + 1;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.height = MaxExtent + 1;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.min_width = MaxExtent + 1;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.min_height = std.math.maxInt(u32);
    try testing.expect(!call_validate(&facts));

    facts = base;
    facts.class_length = 0;
    try testing.expect(!call_validate(&facts));
    facts.class_length = MaxClassBytes + 1;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.image_length = MaxImageBytes + 1;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.title_length = MaxTitleBytes + 1;
    try testing.expect(!call_validate(&facts));

    for (0..3) |index| {
        facts = base;
        facts.reserved0[index] = 1;
        try testing.expect(!call_validate(&facts));
    }
}

test "validate_facts checks text only over the stated length" {
    const base = fixture_facts();
    var facts = base;
    facts.class_name[3] = 0;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.image_name[0] = 0;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.title[2] = 0;
    try testing.expect(!call_validate(&facts));

    facts = base;
    facts.class_name[1] = 0xFF;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.image_name[4] = 0xC0;
    try testing.expect(!call_validate(&facts));
    facts = base;
    facts.title[5] = 0xED;
    facts.title[6] = 0xA0;
    facts.title[7] = 0x80;
    try testing.expect(!call_validate(&facts));
    // A multi-byte sequence cut by the stated length is invalid.
    facts = base;
    set_title(&facts, "ab\xC3\xA9");
    facts.title_length = 3;
    try testing.expect(!call_validate(&facts));
    facts.title_length = 4;
    try testing.expect(call_validate(&facts));

    // Garbage after every stated length is ignored.
    facts = base;
    facts.class_name[facts.class_length] = 0;
    facts.class_name[facts.class_length + 1] = 0xFF;
    facts.image_name[facts.image_length] = 0xC0;
    facts.title[facts.title_length] = 0;
    facts.title[MaxTitleBytes - 1] = 0xFF;
    try testing.expect(call_validate(&facts));
    set_title(&facts, "");
    facts.title[0] = 0xFF;
    try testing.expect(call_validate(&facts));
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
    for (valid) |bytes| try testing.expect(call_utf8(bytes));
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
    for (invalid) |bytes| try testing.expect(!call_utf8(bytes));
}

test "app_key_valid matches the frozen codec rule and the 3..200 length rule" {
    try testing.expect(call_app_key("a-x"));
    try testing.expect(call_app_key("p-x"));
    try testing.expect(call_app_key("a-Notepad_Plus-Plus-1234abcd"));
    try testing.expect(call_app_key("p-0123456789_-ABCxyz"));
    try testing.expect(call_app_key("a--"));
    try testing.expect(call_app_key("a-_"));
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
    for (invalid) |bytes| try testing.expect(!call_app_key(bytes));

    var key: [MaxAppKeyBytes + 1]u8 = undefined;
    @memset(&key, 'k');
    key[0] = 'a';
    key[1] = '-';
    try testing.expect(call_app_key(key[0 .. 2 + 1]));
    try testing.expect(call_app_key(key[0 .. MaxAppKeyBytes - 1]));
    try testing.expect(call_app_key(key[0..MaxAppKeyBytes]));
    try testing.expect(!call_app_key(key[0 .. MaxAppKeyBytes + 1]));
    try testing.expect(!call_app_key(key[0..2]));

    // Every single byte value at every position of a short key.
    var byte: usize = 0;
    while (byte < 256) : (byte += 1) {
        var probe = [_]u8{ 'a', '-', 'k', 'k' };
        const expected = (byte >= 'A' and byte <= 'Z') or (byte >= 'a' and byte <= 'z') or
            (byte >= '0' and byte <= '9') or byte == '_' or byte == '-';
        for (2..4) |position| {
            probe = [_]u8{ 'a', '-', 'k', 'k' };
            probe[position] = @intCast(byte);
            try testing.expectEqual(expected, call_app_key(&probe));
        }
        probe = .{ 'a', '-', 'k', 'k' };
        probe[0] = @intCast(byte);
        try testing.expectEqual(byte == 'a' or byte == 'p', call_app_key(&probe));
        probe = .{ 'a', '-', 'k', 'k' };
        probe[1] = @intCast(byte);
        try testing.expectEqual(byte == '-', call_app_key(&probe));
    }
}

test "class_hash_of follows FNV-1a 64 and distinguishes case" {
    try testing.expectEqual(FnvOffsetBasis, call_hash(""));
    try testing.expectEqual(@as(u64, 0xcbf29ce484222325), call_hash(""));
    try testing.expectEqual(@as(u64, 0xaf63dc4c8601ec8c), call_hash("a"));
    try testing.expectEqual(@as(u64, 0x85944171f73967e8), call_hash("foobar"));
    try testing.expectEqual(@as(u64, 0x8842b206a7bfc712), call_hash("Notepad"));
    try testing.expectEqual(@as(u64, 0x62bde74ad4d13eb2), call_hash("notepad"));
    try testing.expectEqual(@as(u64, 0xdebead0ba0919302), call_hash("#32768"));
    try testing.expect(call_hash("Notepad") != call_hash("notepad"));
    // The hash covers the exact stated bytes: a trailing byte changes it.
    try testing.expect(call_hash("Notepad") != call_hash("Notepad\x00"));
}

test "ascii_eql_ignore_case folds only ASCII letters over the full length" {
    try testing.expect(call_eql_fold("", ""));
    try testing.expect(call_eql_fold("Shell_TrayWnd", "SHELL_TRAYWND"));
    try testing.expect(call_eql_fold("abcXYZ019_-", "ABCxyz019_-"));
    try testing.expect(!call_eql_fold("abc", "abcd"));
    try testing.expect(!call_eql_fold("abcd", "abc"));
    try testing.expect(!call_eql_fold("", "a"));
    try testing.expect(!call_eql_fold("a", "b"));
    // '@' (0x40) and '`' (0x60) differ by 0x20 but are not letters.
    try testing.expect(!call_eql_fold("@", "`"));
    try testing.expect(!call_eql_fold("[", "{"));
    // Non-ASCII bytes compare exactly.
    try testing.expect(call_eql_fold("\xC3\xA9", "\xC3\xA9"));
    try testing.expect(!call_eql_fold("\xC3\x89", "\xC3\xA9"));
    var byte: usize = 0;
    while (byte < 256) : (byte += 1) {
        const left = [_]u8{@intCast(byte)};
        const right = [_]u8{@intCast(byte ^ 0x20)};
        const letter = (byte >= 'A' and byte <= 'Z') or (byte >= 'a' and byte <= 'z');
        try testing.expectEqual(letter, call_eql_fold(&left, &right));
    }
}

test "time_before is wrap-safe signed 32-bit order" {
    try testing.expect(call_before(0, 1));
    try testing.expect(!call_before(1, 0));
    try testing.expect(!call_before(5, 5));
    try testing.expect(!call_before(0, 0));
    try testing.expect(call_before(0xFFFF_FFFE, 1));
    try testing.expect(!call_before(1, 0xFFFF_FFFE));
    try testing.expect(call_before(0xFFFF_FFFF, 0));
    try testing.expect(!call_before(0, 0xFFFF_FFFF));
    try testing.expect(call_before(0x7FFF_FFF0, 0x8000_0010));
    try testing.expect(!call_before(0x8000_0010, 0x7FFF_FFF0));
    try testing.expect(call_before(0, 0x7FFF_FFFF));
    try testing.expect(!call_before(0x7FFF_FFFF, 0));
    // Exactly half the range apart is negative in both directions (documented ambiguity).
    try testing.expect(call_before(0, 0x8000_0000));
    try testing.expect(call_before(0x8000_0000, 0));
}

test "derive_state maps facts to protocol bits and never sets Activated" {
    var facts = fixture_facts();
    facts.style = 0;
    try testing.expectEqual(@as(u32, 0), call_state(&facts));
    facts.flags |= FactIconic;
    try testing.expectEqual(StateMinimized, call_state(&facts));
    facts.flags = FactVisible | FactOnInputDesktop | FactZoomed;
    try testing.expectEqual(StateMaximized, call_state(&facts));
    facts.flags = FactVisible | FactOnInputDesktop | FactFullscreen;
    try testing.expectEqual(StateFullscreen, call_state(&facts));
    facts.flags = FactVisible | FactOnInputDesktop;
    facts.style = StyleThickFrame;
    try testing.expectEqual(StateResizable, call_state(&facts));
    facts.style = StyleMaximizeBox;
    try testing.expectEqual(StateMaximizable, call_state(&facts));
    facts.style = StyleMinimizeBox;
    try testing.expectEqual(StateMinimizable, call_state(&facts));
    facts.style = DefaultStyle;
    try testing.expectEqual(StateResizable | StateMaximizable | StateMinimizable, call_state(&facts));
    facts.flags = FactMask;
    facts.style = std.math.maxInt(u32);
    facts.ex_style = std.math.maxInt(u32);
    try testing.expectEqual(StateMask & ~StateActivated, call_state(&facts));
    try testing.expectEqual(@as(u32, 0), call_state(&facts) & StateActivated);
}

test "identity_equal compares process, thread, start time and class hash" {
    var facts = fixture_facts();
    var rec = call_record_init();
    rec.process_id = facts.process_id;
    rec.thread_id = facts.thread_id;
    rec.process_start_time = facts.process_start_time;
    rec.class_hash = call_hash(call_class_bytes(&facts));
    try testing.expect(call_identity(&rec, &facts));
    // Title, rect, style and stamps are not part of the identity.
    facts.x += 100;
    facts.width += 1;
    facts.stamp += 9;
    facts.style = 0;
    set_title(&facts, "another title");
    try testing.expect(call_identity(&rec, &facts));
    // Class bytes past the stated length are not part of it either.
    facts.class_name[facts.class_length] = 'Z';
    try testing.expect(call_identity(&rec, &facts));

    var other = facts;
    other.process_id += 1;
    try testing.expect(!call_identity(&rec, &other));
    other = facts;
    other.thread_id += 1;
    try testing.expect(!call_identity(&rec, &other));
    other = facts;
    other.process_start_time += 1;
    try testing.expect(!call_identity(&rec, &other));
    other = facts;
    other.process_start_time = 0;
    try testing.expect(!call_identity(&rec, &other));
    other = facts;
    set_class(&other, "notepad");
    try testing.expect(!call_identity(&rec, &other));
    set_class(&other, "Notepad2");
    try testing.expect(!call_identity(&rec, &other));
    set_class(&other, "Notepad");
    try testing.expect(call_identity(&rec, &other));
}

test "record_init returns an all-zero record" {
    const rec = call_record_init();
    try testing.expectEqual(@as(u64, 0), rec.window);
    try testing.expectEqual(@as(u64, 0), rec.generation);
    try testing.expectEqual(@as(u64, 0), rec.parent_window);
    try testing.expectEqual(@as(u64, 0), rec.parent_generation);
    try testing.expectEqual(@as(u64, 0), rec.process_start_time);
    try testing.expectEqual(@as(u64, 0), rec.class_hash);
    try testing.expectEqual(@as(u64, 0), rec.admit_stamp);
    try testing.expectEqual(@as(u64, 0), rec.last_stamp);
    try testing.expectEqual(@as(u64, 0), rec.seq);
    try testing.expectEqual(@as(i32, 0), rec.x);
    try testing.expectEqual(@as(i32, 0), rec.y);
    try testing.expectEqual(@as(u32, 0), rec.width);
    try testing.expectEqual(@as(u32, 0), rec.height);
    try testing.expectEqual(@as(u32, 0), rec.role);
    try testing.expectEqual(@as(u32, 0), rec.state);
    try testing.expectEqual(@as(u32, 0), rec.dpi);
    try testing.expectEqual(@as(u32, 0), rec.min_width);
    try testing.expectEqual(@as(u32, 0), rec.min_height);
    try testing.expectEqual(@as(u32, 0), rec.process_id);
    try testing.expectEqual(@as(u32, 0), rec.thread_id);
    try testing.expectEqual(@as(u32, 0), rec.admit_captured_ms);
    try testing.expectEqual(@as(u32, 0), rec.last_captured_ms);
    try testing.expectEqual(@as(u32, 0), rec.retire_reason);
    try testing.expectEqual(@as(u32, 0), rec.title_length);
    try testing.expectEqual(@as(u32, 0), rec.app_key_length);
    try testing.expect(std.mem.allEqual(u8, &rec.title, 0));
    try testing.expect(std.mem.allEqual(u8, &rec.app_key, 0));
    try testing.expectEqual(@as(usize, MaxTitleBytes), rec.title.len);
    try testing.expectEqual(@as(usize, MaxAppKeyBytes), rec.app_key.len);
}

test "bump increments and saturates without wrapping" {
    var counter: u64 = 0;
    call_bump(&counter);
    try testing.expectEqual(@as(u64, 1), counter);
    call_bump(&counter);
    try testing.expectEqual(@as(u64, 2), counter);
    counter = std.math.maxInt(u64) - 1;
    call_bump(&counter);
    try testing.expectEqual(std.math.maxInt(u64), counter);
    call_bump(&counter);
    try testing.expectEqual(std.math.maxInt(u64), counter);
}

test "status values and constants equal the committed header" {
    try testing.expectEqual(@as(c_int, 0), @intFromEnum(status_t.Ok));
    try testing.expectEqual(@as(c_int, 1), @intFromEnum(status_t.None));
    try testing.expectEqual(@as(c_int, 20), @intFromEnum(status_t.SinkFailed));
    try testing.expectEqual(@as(usize, 21), @typeInfo(status_t).Enum.fields.len);
    try testing.expectEqual(@as(usize, 64), MaxEntries);
    try testing.expectEqual(@as(usize, 192), MaxCallEvents);
    try testing.expectEqual(@sizeOf(c.interop_window_facts_t), @sizeOf(facts_t));
    try testing.expectEqual(@as(usize, 1656), @sizeOf(facts_t));
    try testing.expectEqual(@as(usize, 1328), @sizeOf(event_t));
    try testing.expectEqual(@as(usize, 56), @sizeOf(observation_t));
    try testing.expectEqual(@as(usize, 48), @sizeOf(live_t));
    try testing.expectEqual(@as(usize, 24), @sizeOf(retiring_entry_t));
    try testing.expectEqual(@as(usize, 8), @sizeOf(proof_t));
    try testing.expectEqual(@as(usize, 256), @sizeOf(stats_t));
    try testing.expectEqual(@as(usize, 56), @sizeOf(diagnostic_t));
    try testing.expectEqual(@as(usize, 48), @sizeOf(config_t));
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
    var prng = std.Random.DefaultPrng.init(0x57AD_D1E0);
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
        try testing.expectEqual(codec_app_key_oracle(key), call_app_key(key));
    }
}
