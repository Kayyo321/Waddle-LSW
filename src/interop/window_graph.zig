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

/// Application-key lookup callback type (`interop_window_app_key_lookup_t`); optional C function pointer.
pub const app_key_lookup_t = c.interop_window_app_key_lookup_t;
/// Millisecond clock callback type (`interop_window_now_ms_t`); optional C function pointer.
pub const now_ms_t = c.interop_window_now_ms_t;
/// Drain sink callback type (`interop_window_event_sink_t`); optional C function pointer.
pub const event_sink_t = c.interop_window_event_sink_t;

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
    allocator: std.mem.Allocator,
    /// Copy of the creation parameters (the pointers inside are borrowed from the caller).
    config: config_t,
    /// Thread that created the graph; the only thread allowed to call it.
    owner_thread: std.Thread.Id,
    /// True between a successful `enter` and `leave`; rejects re-entry.
    busy: bool,
    /// True while a refresh is open.
    refresh_open: bool,
    /// True after the first successful capacity diagnostic.
    have_diag: bool,
    /// Monotonic timer of the default clock; null when the platform has none (clock reads 0).
    timer: ?std.time.Timer,
    /// Last generation handed out; 0 before the first admission.
    generations_issued: u64,
    /// Largest stamp accepted by an Ok call.
    stamp_high_water: u64,
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
pub fn create(allocator: std.mem.Allocator, config: *const config_t) error{ OutOfMemory, InvalidArgument }!*graph_t {
    if (config.session_epoch == 0 or config.reserved0 != 0) return error.InvalidArgument;
    if (config.uac_mode > UacNative or config.app_key_lookup == null) return error.InvalidArgument;
    const graph = try allocator.create(graph_t);
    @memset(std.mem.asBytes(graph), 0);
    graph.allocator = allocator;
    graph.config = config.*;
    graph.owner_thread = std.Thread.getCurrentId();
    graph.timer = std.time.Timer.start() catch null;
    graph.next_seq = 1;
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
    if (graph.owner_thread != std.Thread.getCurrentId()) return .WrongThread;
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

/// Queue an event, merging an Announce or Update into a pending one when the header allows.
///
/// For kind Announce or Update the ring is scanned newest first for the first event with the
/// same (window, generation). If that event is an Announce or Update its payload (x, y, width,
/// height, state, dpi, min_width, min_height, title and title_length) is overwritten in place,
/// its kind is kept (an Announce stays an Announce), its changed_mask is OR-ed with the new
/// one and `events_coalesced` is bumped. If it is a RetireIntent the scan stops: events never
/// merge across a retirement. Otherwise the event is appended and `events_queued` is bumped
/// (`updates_queued` as well for an Update). RetireIntent is always appended.
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
        var offset = graph.ring_len;
        while (offset > 0) {
            offset -= 1;
            const queued = &graph.ring[ring_index(graph, offset)];
            if (queued.window != ev.window or queued.generation != ev.generation) continue;
            if (queued.kind == EventRetireIntent) break;
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
    if (graph.timer) |*timer| return timer.read() / std.time.ns_per_ms;
    return 0;
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

// Planning engine: appended by the next stage.
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
    var config = std.mem.zeroes(config_t);
    config.session_epoch = epoch;
    config.uac_mode = UacWindow;
    config.app_key_lookup = stub_lookup;
    config.now_ms = fake_now;
    config.now_ms_context = &test_clock;
    return config;
}

/// Create a graph with the stub lookup and the settable fake clock (reset to 0).
fn test_graph(allocator: std.mem.Allocator, epoch: u64) !*graph_t {
    test_clock = .{};
    const config = test_config(epoch);
    return call_create(allocator, &config);
}

fn call_create(allocator: std.mem.Allocator, config: *const config_t) error{ OutOfMemory, InvalidArgument }!*graph_t {
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
    var ev = std.mem.zeroes(event_t);
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
    return std.mem.asBytes(&graph.stats);
}

test "create validates every configuration field and builds a zeroed graph" {
    var config = test_config(TestEpoch);
    const bad_epoch = blk: {
        var bad = config;
        bad.session_epoch = 0;
        break :blk bad;
    };
    try testing.expectError(error.InvalidArgument, call_create(testing.allocator, &bad_epoch));
    var bad = config;
    bad.reserved0 = 1;
    try testing.expectError(error.InvalidArgument, call_create(testing.allocator, &bad));
    bad = config;
    bad.uac_mode = UacNative + 1;
    try testing.expectError(error.InvalidArgument, call_create(testing.allocator, &bad));
    bad.uac_mode = 0xFFFF_FFFF;
    try testing.expectError(error.InvalidArgument, call_create(testing.allocator, &bad));
    bad = config;
    bad.app_key_lookup = null;
    try testing.expectError(error.InvalidArgument, call_create(testing.allocator, &bad));

    for ([_]u32{ UacWindow, UacAuto, UacNative }) |mode| {
        config.uac_mode = mode;
        const accepted = try call_create(testing.allocator, &config);
        try testing.expectEqual(mode, accepted.config.uac_mode);
        call_destroy(accepted);
    }

    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    try testing.expectEqual(TestEpoch, graph.config.session_epoch);
    try testing.expectEqual(std.Thread.getCurrentId(), graph.owner_thread);
    try testing.expect(!graph.busy and !graph.refresh_open and !graph.have_diag);
    try testing.expectEqual(@as(u64, 1), graph.next_seq);
    try testing.expectEqual(@as(u64, 0), graph.generations_issued);
    try testing.expectEqual(@as(u32, 0), graph.ring_len);
    try testing.expectEqual(@as(u32, MaxEntries), call_count_state(graph, .Free));
    try testing.expectEqual(@as(u32, 0), call_count_deferred(graph));
    try testing.expect(std.mem.allEqual(u8, stats_bytes(graph), 0));
    try testing.expect(std.mem.allEqual(u8, std.mem.asBytes(&graph.ring[EventCapacity - 1]), 0));
    try testing.expect(@sizeOf(graph_t) <= MaxGraphBytes);
}

test "create copies the configuration by value" {
    var config = test_config(TestEpoch);
    const graph = try call_create(testing.allocator, &config);
    defer call_destroy(graph);
    config.session_epoch = 99;
    config.uac_mode = UacNative;
    try testing.expectEqual(TestEpoch, graph.config.session_epoch);
    try testing.expectEqual(UacWindow, graph.config.uac_mode);
}

test "create under allocator failure leaves no graph and leaks nothing" {
    const config = test_config(TestEpoch);
    var fail_index: usize = 0;
    while (true) : (fail_index += 1) {
        try testing.expect(fail_index < 4);
        var failing = std.testing.FailingAllocator.init(testing.allocator, .{ .fail_index = fail_index });
        if (call_create(failing.allocator(), &config)) |graph| {
            try testing.expectEqual(@as(usize, 1), failing.allocations);
            call_destroy(graph);
            try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
            break;
        } else |err| {
            try testing.expectEqual(error.OutOfMemory, err);
            try testing.expectEqual(@as(usize, 0), failing.allocated_bytes);
            try testing.expectEqual(failing.allocated_bytes, failing.freed_bytes);
        }
    }
    // The graph needs exactly one allocation: index 0 fails, index 1 succeeds.
    try testing.expectEqual(@as(usize, 1), fail_index);
}

test "create takes the default monotonic clock when no now_ms is given" {
    var config = test_config(TestEpoch);
    config.now_ms = null;
    config.now_ms_context = null;
    const graph = try call_create(testing.allocator, &config);
    defer call_destroy(graph);
    graph.diag_queue_full = 2;
    var diag = std.mem.zeroes(diagnostic_t);
    try testing.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
    try testing.expectEqual(@as(u64, 2), diag.queue_full);
    graph.diag_queue_full = 1;
    try testing.expectEqual(status_t.None, call_take_diag(graph, &diag));
    graph.timer = null;
    graph.have_diag = false;
    try testing.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
    try testing.expectEqual(@as(u64, 0), graph.last_diag_ms);
}

test "enter applies thread, busy and epoch rules in order" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);

    try testing.expectEqual(@as(?status_t, null), call_enter(graph, null));
    try testing.expect(graph.busy);
    try testing.expectEqual(@as(?status_t, status_t.Reentrant), call_enter(graph, null));
    // Reentrant outranks a stale epoch and bumps nothing.
    try testing.expectEqual(@as(?status_t, status_t.Reentrant), call_enter(graph, 0));
    try testing.expectEqual(@as(u64, 0), graph.stats.rejected_stale_epoch);
    try testing.expectEqual(@as(?status_t, status_t.Reentrant), call_enter_read(graph));
    try testing.expect(graph.busy);
    call_leave(graph);
    try testing.expect(!graph.busy);

    try testing.expectEqual(@as(?status_t, null), call_enter(graph, TestEpoch));
    try testing.expect(graph.busy);
    call_leave(graph);

    try testing.expectEqual(@as(?status_t, status_t.StaleEpoch), call_enter(graph, TestEpoch + 1));
    try testing.expectEqual(@as(?status_t, status_t.StaleEpoch), call_enter(graph, 0));
    try testing.expect(!graph.busy);
    try testing.expectEqual(@as(u64, 2), graph.stats.rejected_stale_epoch);
    var expected = std.mem.zeroes(stats_t);
    expected.rejected_stale_epoch = 2;
    try testing.expectEqualSlices(u8, std.mem.asBytes(&expected), stats_bytes(graph));

    try testing.expectEqual(@as(?status_t, null), call_enter_read(graph));
    try testing.expect(!graph.busy);
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
    var ev = std.mem.zeroes(event_t);
    var stats = std.mem.zeroes(stats_t);
    var diag = std.mem.zeroes(diagnostic_t);
    probe.enter_status = call_enter(probe.graph, 0);
    probe.enter_null_status = call_enter(probe.graph, null);
    probe.read_status = call_enter_read(probe.graph);
    probe.next_status = call_next_event(probe.graph, 0, &ev);
    probe.stats_status = call_get_stats(probe.graph, &stats);
    probe.diag_status = call_take_diag(probe.graph, &diag);
}

test "a foreign thread gets WrongThread and touches nothing" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    queue_raw(graph, make_event(EventAnnounce, 1, 1));
    graph.diag_at_capacity = 5;
    // Busy is set too: WrongThread must still outrank Reentrant.
    graph.busy = true;
    const before = graph.stats;
    var probe = thread_probe_t{ .graph = graph };
    const thread = try std.Thread.spawn(.{}, thread_body, .{&probe});
    thread.join();
    try testing.expectEqual(@as(?status_t, status_t.WrongThread), probe.enter_status);
    try testing.expectEqual(@as(?status_t, status_t.WrongThread), probe.enter_null_status);
    try testing.expectEqual(@as(?status_t, status_t.WrongThread), probe.read_status);
    try testing.expectEqual(status_t.WrongThread, probe.next_status);
    try testing.expectEqual(status_t.WrongThread, probe.stats_status);
    try testing.expectEqual(status_t.WrongThread, probe.diag_status);
    try testing.expectEqualSlices(u8, std.mem.asBytes(&before), stats_bytes(graph));
    try testing.expectEqual(@as(u32, 1), graph.ring_len);
    try testing.expectEqual(@as(u64, 5), graph.diag_at_capacity);
    try testing.expect(graph.busy);
    graph.busy = false;
}

test "ring wraps around over many more pushes than its capacity" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    const total: u64 = 3 * EventCapacity + 11;
    const backlog: u64 = 100;
    var out = std.mem.zeroes(event_t);
    var sequence: u64 = 0;
    while (sequence < total) : (sequence += 1) {
        const ev = make_event(EventAnnounce, sequence + 1, 1);
        try testing.expect(call_push(graph, &ev));
        if (sequence >= backlog) {
            try testing.expectEqual(status_t.Ok, call_next_event(graph, TestEpoch, &out));
            try testing.expectEqual(sequence - backlog + 1, out.window);
        }
    }
    try testing.expectEqual(@as(u32, backlog), graph.ring_len);
    try testing.expect(graph.ring_head < EventCapacity);
    var expected_window = total - backlog + 1;
    while (graph.ring_len > 0) : (expected_window += 1) {
        try testing.expectEqual(status_t.Ok, call_next_event(graph, TestEpoch, &out));
        try testing.expectEqual(expected_window, out.window);
    }
    try testing.expectEqual(total + 1, expected_window);
    try testing.expectEqual(total, graph.stats.events_queued);
    try testing.expectEqual(total, graph.stats.events_drained);
}

test "a full ring refuses a new event and changes nothing" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    var index: u64 = 0;
    while (index < EventCapacity) : (index += 1) {
        const ev = make_event(EventAnnounce, index + 1, 1);
        try testing.expect(call_push(graph, &ev));
    }
    try testing.expectEqual(@as(u32, 0), call_ring_room(graph));
    const before = graph.stats;
    const extra = make_event(EventAnnounce, 9999, 1);
    try testing.expect(!call_push(graph, &extra));
    try testing.expectEqual(@as(u32, EventCapacity), graph.ring_len);
    try testing.expectEqualSlices(u8, std.mem.asBytes(&before), stats_bytes(graph));
    // A merge still works on a full ring.
    var update = make_event(EventUpdate, EventCapacity, 1);
    update.x = 77;
    update.changed_mask = ChangeRect;
    try testing.expect(!call_push(graph, &update));
    try testing.expectEqual(@as(u32, EventCapacity), graph.ring_len);
    try testing.expectEqual(@as(u64, 1), graph.stats.events_coalesced);
    var out = std.mem.zeroes(event_t);
    try testing.expectEqual(status_t.Ok, call_next_event(graph, TestEpoch, &out));
    try testing.expectEqual(@as(u32, 1), call_ring_room(graph));
    try testing.expect(call_push(graph, &extra));
    try testing.expectEqual(@as(u32, 0), call_ring_room(graph));
}

fn pop_all(graph: *graph_t, events: []event_t) !usize {
    var count: usize = 0;
    var out = std.mem.zeroes(event_t);
    while (call_next_event(graph, TestEpoch, &out) == .Ok) : (count += 1) events[count] = out;
    return count;
}

test "announce and update merge keep the announce kind and OR the change mask" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    const announce = make_event(EventAnnounce, 10, 3);
    try testing.expect(call_push(graph, &announce));
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
    try testing.expect(!call_push(graph, &update));
    try testing.expectEqual(@as(u32, 1), graph.ring_len);
    try testing.expectEqual(@as(u64, 1), graph.stats.events_queued);
    try testing.expectEqual(@as(u64, 1), graph.stats.events_coalesced);
    try testing.expectEqual(@as(u64, 0), graph.stats.updates_queued);

    var events: [4]event_t = undefined;
    try testing.expectEqual(@as(usize, 1), try pop_all(graph, &events));
    const merged = events[0];
    try testing.expectEqual(EventAnnounce, merged.kind);
    try testing.expectEqual(ChangeMask, merged.changed_mask);
    try testing.expectEqual(@as(i32, -5), merged.x);
    try testing.expectEqual(@as(i32, 6), merged.y);
    try testing.expectEqual(@as(u32, 321), merged.width);
    try testing.expectEqual(@as(u32, 123), merged.height);
    try testing.expectEqual(StateMaximized, merged.state);
    try testing.expectEqual(@as(u32, 144), merged.dpi);
    try testing.expectEqual(@as(u32, 50), merged.min_width);
    try testing.expectEqual(@as(u32, 40), merged.min_height);
    try testing.expectEqual(@as(u32, title.len), merged.title_length);
    try testing.expectEqualSlices(u8, title, merged.title[0..title.len]);
    // Identity-bound fields are not rewritten.
    try testing.expectEqual(@as(u32, 77), merged.process_id);
    try testing.expectEqual(RoleToplevel, merged.role);
    try testing.expectEqual(@as(u64, 0), merged.parent_window);
    try testing.expectEqual(@as(u32, 6), merged.app_key_length);
}

test "update and update merge OR their masks and count only appended updates" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    var first = make_event(EventUpdate, 10, 3);
    first.changed_mask = ChangeRect;
    first.x = 1;
    var second = make_event(EventUpdate, 10, 3);
    second.changed_mask = ChangeTitle | ChangeDpi;
    second.x = 2;
    try testing.expect(call_push(graph, &first));
    try testing.expect(!call_push(graph, &second));
    var events: [4]event_t = undefined;
    try testing.expectEqual(@as(usize, 1), try pop_all(graph, &events));
    try testing.expectEqual(EventUpdate, events[0].kind);
    try testing.expectEqual(ChangeRect | ChangeTitle | ChangeDpi, events[0].changed_mask);
    try testing.expectEqual(@as(i32, 2), events[0].x);
    try testing.expectEqual(@as(u64, 1), graph.stats.updates_queued);
    try testing.expectEqual(@as(u64, 1), graph.stats.events_queued);
    try testing.expectEqual(@as(u64, 1), graph.stats.events_coalesced);
}

test "events never merge across a retire intent" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    const announce = make_event(EventAnnounce, 10, 3);
    var retire = make_event(EventRetireIntent, 10, 3);
    retire.reason = ReasonDestroyed;
    var update = make_event(EventUpdate, 10, 3);
    update.changed_mask = ChangeRect;
    try testing.expect(call_push(graph, &announce));
    try testing.expect(call_push(graph, &retire));
    try testing.expect(call_push(graph, &update));
    // A second update merges into the newest one (after the retire), never into the announce.
    var again = make_event(EventUpdate, 10, 3);
    again.changed_mask = ChangeState;
    try testing.expect(!call_push(graph, &again));
    var events: [8]event_t = undefined;
    try testing.expectEqual(@as(usize, 3), try pop_all(graph, &events));
    try testing.expectEqual(EventAnnounce, events[0].kind);
    try testing.expectEqual(ChangeMask, events[0].changed_mask);
    try testing.expectEqual(EventRetireIntent, events[1].kind);
    try testing.expectEqual(@as(u32, 0), events[1].changed_mask);
    try testing.expectEqual(ReasonDestroyed, events[1].reason);
    try testing.expectEqual(EventUpdate, events[2].kind);
    try testing.expectEqual(ChangeRect | ChangeState, events[2].changed_mask);
    try testing.expectEqual(@as(u64, 1), graph.stats.events_coalesced);
}

test "retire intents are always appended and an older update stays unmerged" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    var update = make_event(EventUpdate, 10, 3);
    update.changed_mask = ChangeRect;
    const retire = make_event(EventRetireIntent, 10, 3);
    try testing.expect(call_push(graph, &update));
    try testing.expect(call_push(graph, &retire));
    // After the retire, an Announce for the same identity (defensive) starts a new event.
    const later = make_event(EventAnnounce, 10, 3);
    try testing.expect(call_push(graph, &later));
    try testing.expectEqual(@as(u32, 3), graph.ring_len);
    try testing.expectEqual(@as(u64, 0), graph.stats.events_coalesced);
    try testing.expectEqual(@as(u64, 3), graph.stats.events_queued);
}

test "events of another generation or window do not merge" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    const announce = make_event(EventAnnounce, 10, 3);
    const other_generation = make_event(EventUpdate, 10, 4);
    const other_window = make_event(EventUpdate, 11, 3);
    try testing.expect(call_push(graph, &announce));
    try testing.expect(call_push(graph, &other_generation));
    try testing.expect(call_push(graph, &other_window));
    try testing.expectEqual(@as(u32, 3), graph.ring_len);
    try testing.expectEqual(@as(u64, 0), graph.stats.events_coalesced);
    // The newest event of the same identity wins even when other identities were queued after it.
    var update = make_event(EventUpdate, 10, 3);
    update.changed_mask = ChangeMinSize;
    update.min_width = 12;
    try testing.expect(!call_push(graph, &update));
    try testing.expectEqual(@as(u32, 3), graph.ring_len);
    var events: [4]event_t = undefined;
    try testing.expectEqual(@as(usize, 3), try pop_all(graph, &events));
    try testing.expectEqual(@as(u32, 12), events[0].min_width);
    try testing.expectEqual(EventAnnounce, events[0].kind);
    try testing.expectEqual(@as(u32, 0), events[1].min_width);
}

test "next_event reports None on an empty queue and leaves the output alone" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    var out = std.mem.zeroes(event_t);
    out.window = Unset;
    try testing.expectEqual(status_t.None, call_next_event(graph, TestEpoch, &out));
    try testing.expectEqual(Unset, out.window);
    try testing.expectEqual(@as(u64, 0), graph.stats.events_drained);
    try testing.expect(!graph.busy);
    // Wrong epoch (including zero) is rejected before the queue is looked at.
    queue_raw(graph, make_event(EventAnnounce, 5, 1));
    try testing.expectEqual(status_t.StaleEpoch, call_next_event(graph, 0, &out));
    try testing.expectEqual(status_t.StaleEpoch, call_next_event(graph, TestEpoch + 1, &out));
    try testing.expectEqual(@as(u32, 1), graph.ring_len);
    try testing.expectEqual(@as(u64, 2), graph.stats.rejected_stale_epoch);
    try testing.expectEqual(Unset, out.window);
    try testing.expectEqual(status_t.Ok, call_next_event(graph, TestEpoch, &out));
    try testing.expectEqual(@as(u64, 5), out.window);
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
    var scratch = std.mem.zeroes(event_t);
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
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    fill_ring(graph, 5);
    var probe = sink_probe_t{ .graph = graph };
    var delivered: u32 = 99;
    try testing.expectEqual(status_t.Ok, call_drain(graph, TestEpoch, probe_sink, &probe, 3, &delivered));
    try testing.expectEqual(@as(u32, 3), delivered);
    try testing.expectEqualSlices(u64, &[_]u64{ 1, 2, 3 }, probe.seen[0..3]);
    try testing.expect(probe.busy_inside);
    try testing.expect(!graph.busy);
    try testing.expectEqual(@as(u32, 2), graph.ring_len);
    try testing.expectEqual(status_t.Ok, call_drain(graph, TestEpoch, probe_sink, &probe, 10, &delivered));
    try testing.expectEqual(@as(u32, 2), delivered);
    try testing.expectEqualSlices(u64, &[_]u64{ 4, 5 }, probe.seen[3..5]);
    try testing.expectEqual(@as(u64, 5), graph.stats.events_drained);
    try testing.expectEqual(@as(u64, 0), graph.stats.sink_failures);
}

test "drain on an empty queue reports None and delivered 0" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    var probe = sink_probe_t{ .graph = graph };
    var delivered: u32 = 99;
    try testing.expectEqual(status_t.None, call_drain(graph, TestEpoch, probe_sink, &probe, 4, &delivered));
    try testing.expectEqual(@as(u32, 0), delivered);
    try testing.expectEqual(@as(u32, 0), probe.calls);
    try testing.expect(!graph.busy);
}

test "drain stops at a refused event and redelivers it first next time" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    fill_ring(graph, 5);
    var probe = sink_probe_t{ .graph = graph, .fail_on_call = 3 };
    var delivered: u32 = 99;
    try testing.expectEqual(status_t.SinkFailed, call_drain(graph, TestEpoch, probe_sink, &probe, 10, &delivered));
    try testing.expectEqual(@as(u32, 2), delivered);
    try testing.expectEqual(@as(u32, 3), probe.calls);
    try testing.expectEqual(@as(u32, 3), graph.ring_len);
    try testing.expectEqual(@as(u64, 2), graph.stats.events_drained);
    try testing.expectEqual(@as(u64, 1), graph.stats.sink_failures);
    try testing.expect(!graph.busy);
    // The refused event is the first one of the next drain (here refused again at once).
    probe.fail_on_call = probe.calls + 1;
    try testing.expectEqual(status_t.SinkFailed, call_drain(graph, TestEpoch, probe_sink, &probe, 10, &delivered));
    try testing.expectEqual(@as(u32, 0), delivered);
    try testing.expectEqual(@as(u64, 3), probe.seen[probe.calls - 1]);
    try testing.expectEqual(@as(u32, 3), graph.ring_len);
    try testing.expectEqual(@as(u64, 2), graph.stats.sink_failures);
    probe.fail_on_call = 0;
    try testing.expectEqual(status_t.Ok, call_drain(graph, TestEpoch, probe_sink, &probe, 10, &delivered));
    try testing.expectEqual(@as(u32, 3), delivered);
    try testing.expectEqual(@as(u32, 0), graph.ring_len);
}

test "drain rejects bad arguments after the common rules and changes nothing" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    fill_ring(graph, 2);
    var probe = sink_probe_t{ .graph = graph };
    var delivered: u32 = 99;
    try testing.expectEqual(status_t.InvalidArgument, call_drain(graph, TestEpoch, probe_sink, &probe, 0, &delivered));
    try testing.expectEqual(status_t.InvalidArgument, call_drain(graph, TestEpoch, null, &probe, 1, &delivered));
    try testing.expectEqual(status_t.StaleEpoch, call_drain(graph, 0, null, &probe, 0, &delivered));
    try testing.expectEqual(@as(u32, 99), delivered);
    try testing.expectEqual(@as(u32, 2), graph.ring_len);
    try testing.expectEqual(@as(u32, 0), probe.calls);
    try testing.expectEqual(@as(u64, 1), graph.stats.rejected_stale_epoch);
    try testing.expect(!graph.busy);
}

test "graph calls made from inside the sink are Reentrant and nothing is delivered twice" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    fill_ring(graph, 3);
    var probe = sink_probe_t{ .graph = graph };
    var delivered: u32 = 99;
    try testing.expectEqual(status_t.Ok, call_drain(graph, TestEpoch, reentrant_sink, &probe, 10, &delivered));
    try testing.expectEqual(@as(u32, 3), delivered);
    try testing.expectEqual(@as(?status_t, status_t.Reentrant), probe.reentry_read);
    try testing.expectEqual(@as(?status_t, status_t.Reentrant), probe.reentry_next);
    try testing.expectEqual(@as(?status_t, status_t.Reentrant), probe.reentry_drain);
    try testing.expectEqualSlices(u64, &[_]u64{ 1, 2, 3 }, probe.seen[0..3]);
    try testing.expectEqual(@as(u64, 3), graph.stats.events_drained);
    try testing.expect(!graph.busy);
}

test "check_action is Ok only for the exact Active pair" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 0, .Active, 500, 10);
    _ = inject_entry(graph, 1, .Retiring, 500, 9);
    _ = inject_entry(graph, 2, .Retiring, 600, 4);
    try testing.expectEqual(status_t.Ok, call_check_action(graph, TestEpoch, 500, 10));
    try testing.expectEqual(@as(u64, 0), graph.stats.rejected_stale);
    // Retiring, older generation, newer generation, unknown window: all Stale.
    try testing.expectEqual(status_t.Stale, call_check_action(graph, TestEpoch, 500, 9));
    try testing.expectEqual(status_t.Stale, call_check_action(graph, TestEpoch, 600, 4));
    try testing.expectEqual(status_t.Stale, call_check_action(graph, TestEpoch, 500, 11));
    try testing.expectEqual(status_t.Stale, call_check_action(graph, TestEpoch, 700, 1));
    try testing.expectEqual(@as(u64, 4), graph.stats.rejected_stale);
    // Zero arguments are InvalidArgument and bump nothing; a stale epoch outranks them.
    try testing.expectEqual(status_t.InvalidArgument, call_check_action(graph, TestEpoch, 0, 10));
    try testing.expectEqual(status_t.InvalidArgument, call_check_action(graph, TestEpoch, 500, 0));
    try testing.expectEqual(@as(u64, 4), graph.stats.rejected_stale);
    try testing.expectEqual(status_t.StaleEpoch, call_check_action(graph, 0, 0, 0));
    try testing.expectEqual(status_t.StaleEpoch, call_check_action(graph, TestEpoch + 1, 500, 10));
    try testing.expectEqual(@as(u64, 2), graph.stats.rejected_stale_epoch);
    try testing.expect(!graph.busy);
    try testing.expectEqual(@as(u32, 1), call_count_state(graph, .Active));
}

test "lookup helpers distinguish Active, Retiring and deferred records" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 3, .Active, 500, 10);
    _ = inject_entry(graph, 4, .Retiring, 500, 9);
    _ = inject_entry(graph, 5, .Retiring, 600, 3);
    inject_deferred(graph, 7, 800);
    try testing.expectEqual(@as(?usize, 3), call_find_active(graph, 500));
    try testing.expectEqual(@as(?usize, null), call_find_active(graph, 600));
    try testing.expectEqual(@as(?usize, null), call_find_active(graph, 800));
    try testing.expectEqual(@as(?usize, 3), call_find_exact(graph, 500, 10));
    try testing.expectEqual(@as(?usize, 4), call_find_exact(graph, 500, 9));
    try testing.expectEqual(@as(?usize, 5), call_find_exact(graph, 600, 3));
    try testing.expectEqual(@as(?usize, null), call_find_exact(graph, 500, 8));
    try testing.expectEqual(@as(?usize, null), call_find_exact(graph, 0, 0));
    try testing.expectEqual(@as(?usize, 7), call_find_deferred(graph, 800));
    try testing.expectEqual(@as(?usize, null), call_find_deferred(graph, 500));
    graph.deferred[7].used = false;
    try testing.expectEqual(@as(?usize, null), call_find_deferred(graph, 800));
    try testing.expectEqual(@as(u32, 1), call_count_state(graph, .Active));
    try testing.expectEqual(@as(u32, 2), call_count_state(graph, .Retiring));
    try testing.expectEqual(@as(u32, 0), call_count_deferred(graph));
}

test "find_live describes only Active entries" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    const record = inject_entry(graph, 0, .Active, 500, 10);
    record.parent_window = 400;
    record.parent_generation = 7;
    record.role = RoleDialog;
    record.state = StateMinimized | StateResizable;
    record.process_id = 31337;
    _ = inject_entry(graph, 1, .Retiring, 600, 3);

    var live = std.mem.zeroes(live_t);
    live.reserved0 = 0xFFFF;
    try testing.expectEqual(status_t.Ok, call_find_live(graph, 500, &live));
    try testing.expectEqual(@as(u64, 500), live.window);
    try testing.expectEqual(@as(u64, 10), live.generation);
    try testing.expectEqual(@as(u64, 400), live.parent_window);
    try testing.expectEqual(@as(u64, 7), live.parent_generation);
    try testing.expectEqual(RoleDialog, live.role);
    try testing.expectEqual(StateMinimized | StateResizable, live.state);
    try testing.expectEqual(@as(u32, 31337), live.process_id);
    try testing.expectEqual(@as(u32, 0), live.reserved0);

    live = std.mem.zeroes(live_t);
    live.window = Unset;
    try testing.expectEqual(status_t.None, call_find_live(graph, 600, &live));
    try testing.expectEqual(status_t.None, call_find_live(graph, 12345, &live));
    try testing.expectEqual(status_t.InvalidArgument, call_find_live(graph, 0, &live));
    try testing.expectEqual(Unset, live.window);
    graph.busy = true;
    try testing.expectEqual(status_t.Reentrant, call_find_live(graph, 500, &live));
    graph.busy = false;
    try testing.expect(std.mem.allEqual(u8, stats_bytes(graph), 0));
}

test "list_retiring orders by generation, reports the total and truncates" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    inject_entry(graph, 5, .Retiring, 300, 30).retire_reason = ReasonReclassified;
    _ = inject_entry(graph, 2, .Retiring, 100, 10);
    inject_entry(graph, 9, .Retiring, 200, 20).role = RoleMenu;
    _ = inject_entry(graph, 1, .Active, 400, 40);

    var entries: [MaxEntries]retiring_entry_t = undefined;
    var count: u32 = Unset & 0xFFFF;
    try testing.expectEqual(status_t.Ok, call_list_retiring(graph, &entries, MaxEntries, &count));
    try testing.expectEqual(@as(u32, 3), count);
    try testing.expectEqual(@as(u64, 100), entries[0].window);
    try testing.expectEqual(@as(u64, 10), entries[0].generation);
    try testing.expectEqual(ReasonDestroyed, entries[0].reason);
    try testing.expectEqual(@as(u64, 20), entries[1].generation);
    try testing.expectEqual(RoleMenu, entries[1].role);
    try testing.expectEqual(@as(u64, 30), entries[2].generation);
    try testing.expectEqual(ReasonReclassified, entries[2].reason);

    var small: [2]retiring_entry_t = undefined;
    small[1] = .{ .window = Unset, .generation = Unset, .reason = 0, .role = 0 };
    count = 0;
    try testing.expectEqual(status_t.Ok, call_list_retiring(graph, &small, 1, &count));
    try testing.expectEqual(@as(u32, 3), count);
    try testing.expectEqual(@as(u64, 10), small[0].generation);
    try testing.expectEqual(Unset, small[1].window);

    count = 0;
    try testing.expectEqual(status_t.Ok, call_list_retiring(graph, null, 0, &count));
    try testing.expectEqual(@as(u32, 3), count);

    count = 77;
    try testing.expectEqual(status_t.InvalidArgument, call_list_retiring(graph, null, 4, &count));
    try testing.expectEqual(@as(u32, 77), count);
    graph.busy = true;
    try testing.expectEqual(status_t.Reentrant, call_list_retiring(graph, &entries, MaxEntries, &count));
    graph.busy = false;
    try testing.expectEqual(@as(u32, 77), count);

    for (&graph.entries) |*entry| entry.state = .Free;
    try testing.expectEqual(status_t.Ok, call_list_retiring(graph, &entries, MaxEntries, &count));
    try testing.expectEqual(@as(u32, 0), count);
}

const ProofOk = proof_t{ .capture_stopped = 1, .pins_drained = 1 };

test "complete_retire releases only the exact Retiring generation" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 0, .Retiring, 500, 9);
    _ = inject_entry(graph, 1, .Active, 500, 10);
    _ = inject_entry(graph, 2, .Retiring, 600, 4);

    // Wrong epoch (even with zero ids) first; counters other than the epoch stay put.
    try testing.expectEqual(status_t.StaleEpoch, call_complete_retire(graph, TestEpoch + 1, 500, 9, &ProofOk));
    try testing.expectEqual(status_t.StaleEpoch, call_complete_retire(graph, 0, 0, 0, &ProofOk));
    try testing.expectEqual(@as(u64, 2), graph.stats.rejected_stale_epoch);
    try testing.expectEqual(@as(u32, 2), call_count_state(graph, .Retiring));

    // Zero window or generation.
    try testing.expectEqual(status_t.InvalidArgument, call_complete_retire(graph, TestEpoch, 0, 9, &ProofOk));
    try testing.expectEqual(status_t.InvalidArgument, call_complete_retire(graph, TestEpoch, 500, 0, &ProofOk));

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
        try testing.expectEqual(status_t.ProofIncomplete, call_complete_retire(graph, TestEpoch, 500, 9, &proof));
        try testing.expectEqual(@as(u64, round), graph.stats.proof_incomplete);
        try testing.expectEqual(slot_state_t.Retiring, graph.entries[0].state);
        try testing.expectEqual(@as(u64, 9), graph.entries[0].record.generation);
    }
    try testing.expectEqual(@as(u64, 0), graph.stats.retire_completed);

    // Stale outranks ProofIncomplete; NotRetiring outranks it too and bumps nothing.
    const bad_proof = proof_t{ .capture_stopped = 0, .pins_drained = 0 };
    try testing.expectEqual(status_t.Stale, call_complete_retire(graph, TestEpoch, 500, 8, &bad_proof));
    try testing.expectEqual(@as(u64, 1), graph.stats.rejected_stale);
    try testing.expectEqual(status_t.NotRetiring, call_complete_retire(graph, TestEpoch, 500, 10, &bad_proof));
    try testing.expectEqual(status_t.NotRetiring, call_complete_retire(graph, TestEpoch, 500, 10, &ProofOk));
    try testing.expectEqual(@as(u64, 1), graph.stats.rejected_stale);
    try testing.expectEqual(@as(u64, 6), graph.stats.proof_incomplete);
    try testing.expectEqual(slot_state_t.Active, graph.entries[1].state);

    // Success scrubs and frees exactly that slot; the newer Active generation is untouched.
    try testing.expectEqual(status_t.Ok, call_complete_retire(graph, TestEpoch, 500, 9, &ProofOk));
    try testing.expectEqual(slot_state_t.Free, graph.entries[0].state);
    try testing.expect(std.mem.allEqual(u8, std.mem.asBytes(&graph.entries[0].record), 0));
    try testing.expectEqual(@as(u64, 1), graph.stats.retire_completed);
    try testing.expectEqual(@as(?usize, 1), call_find_active(graph, 500));
    try testing.expectEqual(@as(u64, 10), graph.entries[1].record.generation);
    try testing.expectEqual(slot_state_t.Retiring, graph.entries[2].state);

    // Duplicate completion and a late stale one while a newer Active entry exists are Stale.
    try testing.expectEqual(status_t.Stale, call_complete_retire(graph, TestEpoch, 500, 9, &ProofOk));
    try testing.expectEqual(@as(u64, 2), graph.stats.rejected_stale);
    try testing.expectEqual(@as(u64, 1), graph.stats.retire_completed);
    try testing.expectEqual(slot_state_t.Active, graph.entries[1].state);
    try testing.expect(!graph.busy);

    // A freed slot no longer counts as exported.
    try testing.expectEqual(@as(u32, 1), call_count_state(graph, .Retiring));
    try testing.expectEqual(@as(u32, 1), call_count_state(graph, .Active));
}

test "get_stats fills counters and recomputed gauges" {
    const graph = try test_graph(testing.allocator, TestEpoch);
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

    var stats = std.mem.zeroes(stats_t);
    stats.reserved0 = 0xFFFF;
    try testing.expectEqual(status_t.Ok, call_get_stats(graph, &stats));
    try testing.expectEqual(TestEpoch, stats.session_epoch);
    try testing.expectEqual(@as(u64, 17), stats.generations_issued);
    try testing.expectEqual(@as(u64, 4242), stats.stamp_high_water);
    try testing.expectEqual(@as(u32, 2), stats.active_count);
    try testing.expectEqual(@as(u32, 1), stats.retiring_count);
    try testing.expectEqual(@as(u32, 3), stats.deferred_count);
    try testing.expectEqual(@as(u32, 4), stats.events_pending);
    try testing.expectEqual(@as(u32, 1), stats.refresh_active);
    try testing.expectEqual(@as(u32, 0), stats.reserved0);
    try testing.expectEqual(@as(u64, 6), stats.admitted);
    try testing.expectEqual(@as(u64, 2), stats.suspended);
    try testing.expectEqual(@as(u64, 3), stats.rejected_stale_epoch);
    // The stored counters never receive the gauges.
    try testing.expectEqual(@as(u32, 0), graph.stats.active_count);
    try testing.expectEqual(@as(u64, 0), graph.stats.session_epoch);

    graph.refresh_open = false;
    try testing.expectEqual(status_t.Ok, call_get_stats(graph, &stats));
    try testing.expectEqual(@as(u32, 0), stats.refresh_active);

    var untouched = std.mem.zeroes(stats_t);
    untouched.admitted = Unset;
    graph.busy = true;
    try testing.expectEqual(status_t.Reentrant, call_get_stats(graph, &untouched));
    graph.busy = false;
    try testing.expectEqual(Unset, untouched.admitted);
}

test "stamp high water is read through the guard and the plain getter" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    var value: u64 = Unset;
    try testing.expectEqual(status_t.Ok, call_stamp_high_water(graph, &value));
    try testing.expectEqual(@as(u64, 0), value);
    graph.stamp_high_water = 777;
    try testing.expectEqual(@as(u64, 777), call_stamp_high_water_of(graph));
    try testing.expectEqual(status_t.Ok, call_stamp_high_water(graph, &value));
    try testing.expectEqual(@as(u64, 777), value);
    value = Unset;
    graph.busy = true;
    try testing.expectEqual(status_t.Reentrant, call_stamp_high_water(graph, &value));
    graph.busy = false;
    try testing.expectEqual(Unset, value);
}

test "capacity diagnostic is None without reading the clock when every counter is zero" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    test_clock.now = 5_000_000;
    var diag = std.mem.zeroes(diagnostic_t);
    diag.at_capacity = Unset;
    try testing.expectEqual(status_t.None, call_take_diag(graph, &diag));
    try testing.expectEqual(@as(u32, 0), test_clock.reads);
    try testing.expectEqual(Unset, diag.at_capacity);
    try testing.expect(!graph.have_diag);
    try testing.expect(!graph.busy);
}

test "capacity diagnostic follows the sixty second rate limit" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    _ = inject_entry(graph, 0, .Active, 1, 1);
    _ = inject_entry(graph, 1, .Retiring, 2, 2);
    inject_deferred(graph, 0, 9);
    fill_ring(graph, 2);
    var diag = std.mem.zeroes(diagnostic_t);

    // First take is immediate, elapsed 0, counters reset, gauges filled.
    test_clock.now = 1000;
    graph.diag_at_capacity = 3;
    graph.diag_deferred_evicted = 4;
    graph.diag_queue_full = 5;
    graph.diag_generation_exhausted = 6;
    try testing.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
    try testing.expectEqual(@as(u64, 3), diag.at_capacity);
    try testing.expectEqual(@as(u64, 4), diag.deferred_evicted);
    try testing.expectEqual(@as(u64, 5), diag.queue_full);
    try testing.expectEqual(@as(u64, 6), diag.generation_exhausted);
    try testing.expectEqual(@as(u64, 0), diag.elapsed_ms);
    try testing.expectEqual(@as(u32, 1), diag.active_count);
    try testing.expectEqual(@as(u32, 1), diag.retiring_count);
    try testing.expectEqual(@as(u32, 1), diag.deferred_count);
    try testing.expectEqual(@as(u32, 2), diag.events_pending);
    try testing.expect(graph.have_diag);
    try testing.expectEqual(@as(u64, 1000), graph.last_diag_ms);
    try testing.expectEqual(@as(u64, 0), graph.diag_at_capacity | graph.diag_deferred_evicted | graph.diag_queue_full | graph.diag_generation_exhausted);

    // Counters all zero again: None without a clock read.
    const reads = test_clock.reads;
    try testing.expectEqual(status_t.None, call_take_diag(graph, &diag));
    try testing.expectEqual(reads, test_clock.reads);

    // Within the interval: None, counters and baseline kept.
    graph.diag_at_capacity = 2;
    test_clock.now = 30000;
    try testing.expectEqual(status_t.None, call_take_diag(graph, &diag));
    test_clock.now = 1000 + DiagnosticIntervalMs - 1;
    try testing.expectEqual(status_t.None, call_take_diag(graph, &diag));
    try testing.expectEqual(@as(u64, 2), graph.diag_at_capacity);
    try testing.expectEqual(@as(u64, 1000), graph.last_diag_ms);

    // Backwards clock counts as zero elapsed time and moves nothing.
    test_clock.now = 10;
    try testing.expectEqual(status_t.None, call_take_diag(graph, &diag));
    try testing.expectEqual(@as(u64, 1000), graph.last_diag_ms);
    try testing.expectEqual(@as(u64, 2), graph.diag_at_capacity);

    // Exactly the interval is enough; gauges follow the current state.
    graph.entries[1].state = .Free;
    test_clock.now = 1000 + DiagnosticIntervalMs;
    try testing.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
    try testing.expectEqual(@as(u64, DiagnosticIntervalMs), diag.elapsed_ms);
    try testing.expectEqual(@as(u64, 2), diag.at_capacity);
    try testing.expectEqual(@as(u64, 0), diag.queue_full);
    try testing.expectEqual(@as(u32, 0), diag.retiring_count);
    try testing.expectEqual(@as(u64, 1000 + DiagnosticIntervalMs), graph.last_diag_ms);
    try testing.expectEqual(@as(u64, 0), graph.diag_at_capacity);

    // Each counter alone makes the diagnostic due once the interval passes again.
    const fields = [_][]const u8{ "diag_at_capacity", "diag_deferred_evicted", "diag_queue_full", "diag_generation_exhausted" };
    inline for (fields) |name| {
        test_clock.now += DiagnosticIntervalMs;
        @field(graph, name) = 1;
        try testing.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
        try testing.expectEqual(@as(u64, 0), @field(graph, name));
    }
}

test "capacity diagnostic takes its first sample even at clock zero and guards the clock callback" {
    const graph = try test_graph(testing.allocator, TestEpoch);
    defer call_destroy(graph);
    graph.diag_generation_exhausted = 1;
    test_clock.probe = graph;
    var diag = std.mem.zeroes(diagnostic_t);
    try testing.expectEqual(status_t.Ok, call_take_diag(graph, &diag));
    try testing.expectEqual(@as(u32, 1), test_clock.reads);
    try testing.expectEqual(@as(?status_t, status_t.Reentrant), test_clock.probe_status);
    try testing.expectEqual(@as(u64, 1), diag.generation_exhausted);
    try testing.expectEqual(@as(u64, 0), diag.elapsed_ms);
    try testing.expect(graph.have_diag);
    try testing.expect(!graph.busy);
    test_clock.probe = null;
    graph.busy = true;
    graph.diag_queue_full = 1;
    try testing.expectEqual(status_t.Reentrant, call_take_diag(graph, &diag));
    graph.busy = false;
    try testing.expectEqual(@as(u64, 1), graph.diag_queue_full);
}
