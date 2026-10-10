# Revocable input leases and bounded focus recovery

Implementation specification, 2026-10-10. All state is scoped to one connection on `feature/software-vgpu-slicing`. This specification freezes the minimal Revoked/Ack/Ready contract. Verification status is tracked separately; design requirements are not test-pass claims.

## 1. Scope, minimality and acceptance warning

Recover after an **observed** native foreground transition between already-successfully-exported, live, ordinary ownerless windows of the selected process. Use one guest-issued epoch, one ordered host serial, Revoked/Ack/Ready and an ordered Wayland barrier followed by a genuinely new focus event.

**Acceptance warning:** Requires an isolated interactive guest desktop. `SendInput` is desktop-global. Foreground checks and WinEvent observations are not an atomic HWND-bound security boundary. Silent OS notification loss, delayed observations, wholly unobserved leave-and-return, concurrent unrelated guest input and same-process HWND reuse without observed lifecycle evidence remain unqualified. Successful insertion does not establish delivery to the intended application. [Microsoft SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)

Hook observations and actual foreground/eligibility checks are both required. Hook installation failure, locally detected loss, observation-buffer overflow and ambiguous reentrant sequencing are terminal. No reliable general API loss counter proves that all OS hook events arrived. Current foreground checks also cannot detect every intermediate transition.

No owned-dialog recovery, new capture admission, process-family expansion, automatic focus restoration, reconnect manager, desktop creation/switch, privilege workaround, GPU breadth or new input devices are included.

The optional visibility handshake in revision 2 is eliminated: there is **no FocusResult opcode, PendingFocus phase, pending-focus record/tombstone, focus-result correlation or focus-result deadline**. This is not a loss of guest authorization. `av_peer_pump` dispatches received frames synchronously in FIFO order: focus validation/activation completes before the next input frame. Guest owns the actual active pair; host only knows what focus it requested. Obsolete-pair data has no effect, native activation failure terminates before following frames, and a detected transition revokes before subsequent input authorization. No replacement acknowledgment or retry mechanism is introduced.

## 2. Wire format and compatibility

Keep the 328-byte envelope: u16 LE magic `0x574c` at 0, u16 LE opcode at 2, u32 LE payload length 320 at 4. Existing opcodes 1–13 retain their exact meanings and historical acceptance. The following new opcodes have never been published and are numbered coherently after removing the optional result:

| Opcode | Name | Direction |
|---|---|---|
| 14 | MsgWindowCreateV3 | Guest → host |
| 15 | MsgInputEpochRevoked | Guest → host |
| 16 | MsgInputEpochAck | Host → guest |
| 17 | MsgInputEpochReady | Guest → host |
| 18 | MsgInputFocusV3 | Host → guest |
| 19 | MsgInputKeyV3 | Host → guest |
| 20 | MsgInputPointerV3 | Host → guest |
| 21 | MsgInputButtonV3 | Host → guest |
| 22 | MsgInputWheelV3 | Host → guest |
| 23 | MsgInputReleaseV3 | Host → guest |

CreateV3 uses CreateV2 geometry, pool, nonzero immutable incarnation and title fields/validation, additionally requiring zero damage fields. `buffer_index` is the pool 0–15. CreateV3 carries no epoch.

Add independent logical `uint64_t lease_generation` to `av_message_t`; require zero for opcodes 1–14. For opcodes 15–23 require nonzero epoch and zero logical damage fields, and map epoch only to bytes 56–63. Do not interpret these bytes as damage. Preserve old wire acceptance/encoding without retroactively tightening legacy padding.

| Offset | Size | Meaning for 15–23 |
|---|---:|---|
| 8 | 8 | window_id, u64 LE |
| 16/20 | 4 each | x/y, i32 LE |
| 24 | 4 | code/width, u32 LE |
| 28 | 4 | zero |
| 32 | 4 | flags, u32 LE |
| 36/40 | 4 each | zero |
| 44 | 4 | host serial or exact Ack-serial echo, u32 LE |
| 48 | 8 | immutable window incarnation, u64 LE |
| 56 | 8 | guest input epoch, u64 LE |
| 64–327 | 264 | zero, including title and padding |

Exact message validation:

- Revoked: zero window_id/incarnation/serial and event fields.
- Ack: zero window_id/incarnation/event fields; fresh nonzero host serial.
- Ready: zero window_id/incarnation/event fields; serial echoes exactly the Ack being answered.
- FocusV3: nonzero pair/serial; flags 1 on, 0 off; x/y/code zero. Neither operation has a reply.
- KeyV3: nonzero pair/serial; code 1–127, flags 0/1, x/y zero; unsupported physical scan mapping fails before emission.
- PointerV3: nonzero pair/serial; flags/code zero; x/y 0–8191, additionally checked against current native geometry.
- ButtonV3: nonzero pair/serial; code 1–3, flags 0/1, x/y zero.
- WheelV3: nonzero pair/serial; flags/code zero; x/y -1200..1200, nonzero vector.
- ReleaseV3: nonzero pair/serial; flags 1 keys, 2 buttons, 3 both; x/y/code zero.

Codec remains allocation-free Zig, with transactional failure. Check canonicality, direction and mode before window/pool lookup.

First Create permanently selects V1, V2 or V3; mixed Create modes fail. Modern incarnation tokens remain nonzero, increasing and independent of epoch/serial. Existing pool/HWND alias rejection remains.

- New guest sends CreateV3 for every window. Old host rejects first opcode 14 before admitting a surface or performing partial input semantics; no fallback.
- New host + V1 is explicitly view-only regardless of incidental nonzero sequence.
- New host + V2 retains existing input semantics and terminal native foreground loss; no recovery claim.
- V3 rejects old input 7–12; V1/V2 reject input/lease 15–23.

## 3. Startup-only unarmed state

Install required hooks and preserve current native initialization restrictions. First successfully queued CreateV3 precedes Revoked(epoch=1). Commit successfully exported metadata before using it for native authorization. If Revoked enqueue fails after Create, the session fails; first-window publication is not readiness. No handshake timer runs before any window is exported. Enumeration order never chooses an anchor or activates a window.

The independently approved startup distinction preserves shell launch before any forwarded ownership exists. Start with sticky `ever_anchored=false`, no native anchor, no active pair and empty held arrays. **No SendInput is allowed in this interval.** Ack/Ready establish only protocol readiness. Unrelated shell/tool/NULL source foreground before first ownership is not ownership loss. Hook registration, local observation loss, protocol, clock and queue failures remain terminal; unavailable receiving-input desktop is not bypassed.

Only a fresh post-Ready user keyboard.enter producing a canonical, serial/epoch-valid current Focus-on may attempt initial ownership. Guest probes the already-successfully-exported requested pair. An obsolete pair is an ordered no-op and remains unarmed, with no acknowledgment. A live target must pass current ownerless ordinary-window, visibility/enabled/nonminimized and input-desktop checks. Existing SetForegroundWindow restrictions apply. Do not enumerate/admit a target or capture the unrelated source window.

After explicit activation succeeds, reconcile reentrant observations and verify actual foreground equals the exact requested target. Immediately establish the native anchor and sticky flag, then the actual active input pair, before returning from that synchronous focus operation. No following input can execute before this success boundary. Activation/check failure is terminal, never a retry into startup.

Observations received before ownership cannot authorize input. Process them while still unarmed; they do not make arbitrary source foreground an ownership loss. Callbacks delivered after anchoring remain subject to fail-closed monitoring even if likely generated before startup. No guessed age/event_time cutoff is permitted; delayed startup callbacks can conservatively terminate the session.

Focus-off, destruction, revocation, absent active pair and last-window disappearance never reset `ever_anchored`. Only a genuinely fresh connection after successful teardown starts unarmed. This supersedes only the original review’s unconditional initial eligible-foreground requirement.

## 4. Single-owner state and serial validation

Keep existing event-thread ownership. Guest phases: Initial, AwaitAck, UnfocusedReady, Active, Terminal. Store one epoch, one host serial high-water, one actual active pair/held arrays and a separate native anchor. Active is actual guest authorization. Host phases: AwaitInitialRevocation, Barrier, AwaitReady, UnfocusedReady, Forwarding, Terminal. Forwarding means the host successfully queued a focus request, **not** that guest activation has been acknowledged.

Exactly one guest component validates/advances serial for Ack and V3 input. Do not consume serial in both a wrapper and old full `av_input_apply`. Refactor/provide an already-admitted core with an explicit boundary; V2 semantics stay unchanged. Serial never resets on release, transfer or revocation.

For each V3 host message:

1. Terminal does no further work. Validate canonical bytes, direction and mode.
2. Validate nonzero strictly increasing host serial; consume once, including stale-epoch messages.
3. Old epoch is an ordered no-op: no target, activation, release, emission or request-triggered reconciliation. Replay/regression is terminal even when stale. Future epoch is terminal.
4. Dispatch exact-current epoch by phase. Independent native observation/deadline processing may stop the session; stale traffic itself produces no native effects.
5. For live current operations, after applicable identity/pair filtering, check deadline/observations/current native evidence before effects; if reconciliation revokes, the triggering old-epoch operation emits nothing.

`av_peer_pump` must keep callbacks synchronous: fully resolve Focus, native activation/checks, held release or revocation before returning and admitting the next received frame. Never defer a focus operation while allowing subsequent key/button frames to overtake it. Terminal failure stops pumping further effects. This existing ordered dispatcher is the authoritative focus/data boundary.

## 5. Guest exact phase table

| State/event | Required outcome |
|---|---|
| Initial + first successfully queued CreateV3 | Reserve epoch 1, queue Revoked; success enters AwaitAck and starts deadline |
| AwaitAck + exact-current Ack before deadline | Recheck applicable anchored/unarmed native conditions, queue Ready echoing exact Ack serial; success enters UnfocusedReady |
| AwaitAck + other current Focus/input/Release | Terminal; input before readiness forbidden |
| UnfocusedReady + obsolete current Focus-on | Ordered no-op; no activation/release; remain UnfocusedReady |
| UnfocusedReady + live current Focus-on | Validate, explicitly activate/check; establish anchor if first ownership; enter Active before dispatch returns |
| Active + obsolete current Focus-on | Ordered no-op preserving existing actual pair/holds |
| Active + live current Focus-on | Successfully release all old holds before explicit activation/check; establish new actual pair before dispatch returns |
| Active + exact-pair Focus-off | Release all holds, clear active pair, enter UnfocusedReady; retain native anchor |
| Active + exact-pair Release | Release requested mask, remain Active |
| Active + exact-pair key/pointer/button/wheel | Authorize and check each native insertion before injection |
| Active + nonmatching/retired-pair data/off/release | Ordered no-op; no native effects |
| UnfocusedReady + data/off/release | Ordered no-op; no actual active pair |
| Active or anchored UnfocusedReady + unexpected eligible exported foreground transition | Suspend authorization, release all holds, recheck candidate, reserve next epoch, queue Revoked; success enters AwaitAck |
| **Anchored AwaitAck** + further distinct unexpected native transition | Terminal; do not coalesce or start another handshake |
| Initial unanchored AwaitAck + source foreground change | Process observations without input authority; source change alone is not ownership loss; hook/local-loss/protocol/queue/deadline rules still apply |
| Any anchored monitored state + unknown/NULL/tool/owned/cross-process/disabled/minimized/invalid/noninteractive foreground | Terminal |
| Any queue/clock/release/injection/protocol failure | Terminal; no later Ready or normal input effects |

A second current Ack after readiness is a protocol error. Probe obsolete Focus identity before releasing anything. Ordinary host transfer always sends prior Focus-off before the new Focus-on; therefore an obsolete new target cannot unintentionally leave old guest holds armed in the normal flow.

On anchor removal, suspend authorization and reconcile actual foreground without forgetting anchor evidence. Exclude retiring pair from candidate eligibility. A current eligible already-exported replacement can revoke; unknown/NULL/ineligible foreground terminates. Do not readmit/enumerate a candidate to legalize recovery. Keep required lifecycle release cleanup bounded.

Anchored observed A→B→A during AwaitAck is terminal/disarmed. Confirmation of unchanged anchor is not another transition. Ambiguous delayed observations fail closed or conservatively revoke, never restore authorization just because actual foreground returned.

Epoch starts at 1, advances exactly once per attempted revocation and never rolls back after failed publication. Incarnation, epoch and serial are separate. UINT32_MAX/UINT64_MAX may be consumed once; the next generation attempt is terminal. No retransmission or same-epoch Ack replay protocol.

## 6. Host barrier and optimistic forwarding

Dispatch lease controls before any window/pool/frame lookup. Host rules:

- First Revoked must be epoch 1. Later Revoked must equal current+1 without wrap and arrive after prior Ready was received. Duplicate, regressed, skipped or overlapping Revoked terminates.
- On valid Revoked disable forwarding first, clear requested keyboard/pointer routes, preserve physical suppression, and own one dedicated sync callback plus one handshake deadline. Enter Barrier.
- The exact live callback object/owned epoch/Barrier phase alone may queue Ack with the next host serial. Ignore callback_data. Check deadline. Queue success enters AwaitReady without restarting it.
- Ready must match current epoch and the saved Ack serial in AwaitReady before expiry. Clear handshake deadline, enter UnfocusedReady. Ready never replays focus or pointer input.
- Canonical older-epoch Ready is a no-op; future Ready is terminal. Same-epoch Ready in the wrong phase or with wrong Ack serial terminates.
- A fresh valid keyboard.enter after Ready may start forwarding. Parse its key snapshot first. Queue old requested pair's Focus-off if any, then new pair's Focus-on. Only successful FIFO enqueues establish the host requested/Forwarding pair; any control enqueue failure is terminal.
- Following key/button/pointer/wheel frames carry that requested pair, current epoch and increasing serial. Guest synchronously validates/activates focus before processing them. No host assertion of actual guest activation is made.
- Keyboard leave, capability loss or invalid keymap queues appropriate Focus-off for the requested pair, then clears forwarding. Guest destruction retires routes without redundant cleanup input because guest removal already handles release. Same-HWND replacement uses a new incarnation and cannot inherit routes.
- Revocation clears requested routes. Any queued old-epoch Focus/data still in transport is ignored by guest after serial validation, even if HWND/incarnation are unchanged.

The lease callback is separate from the existing latency sync and uses the same event queue as seat callbacks. No input forwarding before both barrier completion and matching Ready. A keyboard.enter observed between them updates physical bookkeeping but never becomes a cached request. A genuinely new post-Ready enter is required. Wayland sync orders protocol messages; it does not timestamp physical user actions. [Wayland display sync](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_display)

## 7. Suppression and pointer synchronization

Dropping blocked events means **no forwarding**, not skipping local bookkeeping. Track observed-down supported keys/buttons independently of routing and guest state.

- On revoke/focus/route disable, suppress all relevant known-down inputs. Never indiscriminately zero suppression during focus clear or window retirement.
- While Barrier, AwaitReady or UnfocusedReady blocks forwarding, supported down sets observed-down and suppression; up clears both without forwarding. Bookkeep before rejecting for NULL route or disabled forwarding.
- Validate every keyboard.enter keys array, including blocked/unrecognized-surface enters, using bounded size/alignment/accessibility checks. Treat supported keys as authoritative snapshot: suppress present keys and remove stale observed/suppressed keys absent from the array. Never synthesize downs from it. Unsupported keys are not guessed. [Wayland keyboard protocol](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_keyboard)
- Focus-on queue success does not replay key/button/wheel events. Suppression persists until up or authoritative key snapshot absence.
- Pointer.enter has no button snapshot. Retain button suppression until observed up; a later first full click may conservatively be swallowed when an off-surface release was unseen.
- After revoke only fresh pointer.enter **after Ready** establishes pointer route identity. Motion provides coordinates but no surface identity and cannot restore a cleared route. Pre-Ready route/coordinates never authorize pointer effects.
- A fresh post-Ready keyboard.enter may queue Focus-on followed immediately by PointerV3 for a matching fresh pointer route, using the current valid coordinates. This ordering belongs to the fresh enter, never to Ready alone. Subsequent data follows in the same FIFO.
- For each fresh, unsuppressed, valid post-Ready button-down or wheel callback while Forwarding, queue a current-route PointerV3 first, then that button/wheel with a higher serial. Require route equals requested keyboard pair and coordinates are valid. This synchronizes native cursor position before the click/scroll and avoids relying on a dropped earlier motion. Queue failure of either is terminal. The coordinate snapshot is used only because a new user callback is being processed; no blocked click/wheel is replayed.
- Button-up does not require a new pointer move; a release must not be lost solely because coordinates moved outside bounds. Preserve existing guest-held release rules.
- Pointer leave queues guest button Release when appropriate before clearing route, suppresses observed-down buttons and never fabricates a later press. Future ups clear suppression.
- Ordinary old-off→new-on transfer carries suppression and route identity checks; it imports no held key from another Linux application.

## 8. Native observations, eligibility and insertion accounting

### 8.1 Bounded observation and reentrancy boundary

Install an out-of-context EVENT_SYSTEM_FOREGROUND hook with process/thread filters zero, on tracker owner thread, without skipping own process/thread. Existing target-filtered lifecycle hooks are separate. Validate hook origin. Installation failure is terminal. Same-thread delivery does not prevent reentry during native calls. [SetWinEventHook](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook), [Reentrancy guidance](https://learn.microsoft.com/en-us/windows/win32/winauto/guarding-against-reentrancy-in-hook-functions)

Use one bounded value-only native observation mechanism: 64 records plus operation/callback guards and terminal/authorization-suspended latches. Store hook, event kind, numeric HWND, needed object/child/native event metadata and local receipt order; no owned native pointers. A snapshot of known incarnation is local evidence, not proof against delayed HWND reuse.

Callbacks append observations and suspend normal authorization; they never recursively reset, emit, mutate lifecycle registry, activate or publish. Overflow or ambiguous reentrant ordering terminates; no silent overwrite/drop. Resolve effects at outer owner boundary in receipt order. Newly queued observations during reconciliation must be resolved before another effect or cause terminal ambiguity, not recursive mutation. While startup is unarmed, observation processing retains no-input authority and the source exception in §3.

Before a new native effect require no unprocessed observations and valid operation guard. Reconcile at owner-loop boundaries and before authorized current operations, rather than relying on 250 ms refresh. Stale input callbacks themselves run no effectful reconciliation. Actual foreground checks remain mandatory before every insertion and after committed insertion/accounting.

An expected-activation marker covers only the exact explicit operation and requested pair. Never suppress callbacks for an allowlist of recently requested HWNDs. An intervening observed unexpected transition invalidates established ownership even if actual foreground returns. Close marker at outer boundary; ambiguous delayed callbacks fail closed/revoke. Same-anchor confirmation is harmless. No event_time freshness guess.

### 8.2 Dedicated eligibility and desktop checks

Use existing successfully exported registry plus retained target-process lifetime evidence, matching current PID, live HWND/root identity, visibility, enabled state, not minimized, no WS_EX_TOOLWINDOW/WS_EX_NOACTIVATE, and `GW_OWNER == NULL`. Recheck native target immediately before activation/insertion; old capture eligibility is insufficient. Current capture `eligible()` admits some owned APPWINDOW windows, so it is not this recovery predicate. Capture admission stays unchanged.

Read-only receiving-input desktop verification: borrowed `GetThreadDesktop(GetCurrentThreadId())`, then correctly sized BOOL via `GetUserObjectInformationW(..., UOI_IO, ...)`. Error/false is terminal. Do not close borrowed desktop, switch desktop or infer current interactivity from OpenInputDesktop success (which can occur in a disconnected session). [GetThreadDesktop](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getthreaddesktop), [UOI_IO](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getuserobjectinformationw), [OpenInputDesktop](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openinputdesktop)

Actual foreground must equal authorized pair immediately before insertion. NULL can be transient but remains fail-closed after ownership or when verifying activation; only pre-ownership source observation has the explicit startup exception. [GetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getforegroundwindow)

### 8.3 Distinct insertion outcomes and release accounting

- **Inserted:** commit exact successful down/up held-bit change before post-insertion reconciliation. Do not return generic failure after success in a way that loses a held down.
- **Not inserted, recognized eligible transition:** no held-bit update for unsent event; outer owner boundary revokes/releases. Never recursively reset from emit.
- **Terminal native failure:** failed/partial insertion, authorization refusal or ambiguous sequence; no normal event retry.

Check every individual SendInput. Horizontal wheel already inserted followed by vertical preparation/check/insertion failure or transition is terminal partial operation, never retried/replayed as an unissued whole event.

Synthetic release is mandatory cleanup and bypasses foreground authorization; it never activates. Successful releases clear only their held bits; failed bits remain and other releases are attempted. Observation during cleanup can latch Terminal but never recursively clean up. Only existing bounded teardown release retry remains permitted after failure.

## 9. Control routing, queue semantics and deadlines

Route lease/control opcodes before pool/window lookup on both endpoints. Guest notification must bypass existing pool/identity lookup. Host lease controls must bypass frame indexing. Serial larger than AvVideoBuffers is normal and never a buffer index; `find_window(client,0)` can match an empty slot and is not a lease dispatcher.

Every control enqueue result other than zero terminates, including Create/Revoked/Ack/Ready/Focus/data. Only MsgFrameReady preserves nonterminal full-queue drop. First Create success plus Revoked failure is session failure, not partial recovery. No Ready or normal effect escapes after terminal native/queue evidence.

There are exactly two handshake deadline ownership cases, both fixed monotonic 2 seconds:

- Guest AwaitAck: from successful Revoked enqueue.
- Host Barrier + AwaitReady: one unchanged interval from valid Revoked receipt.

No focus-acceptance operation/deadline exists. Check expiry before accepting Ack/Ready or any new native effect while a handshake deadline applies, and around event-loop work. Expired matching traffic cannot rescue the operation. Traffic, EINTR and observations do not restart deadlines; completion or terminal teardown clears them. Adapt poll/wait to nearest deadline; monotonic failure/overflow terminates. They are cooperative event-loop deadlines, not interruption of a stalled synchronous OS call. Initial/idle unfocused states without a handshake have no artificial timeout.

Flush the new Wayland sync before the next poll. Existing host flush precedes guest-message pumping, so flush again after Revoked processing. EAGAIN uses writable polling and retains deadline; do not leave the sync buffered across the next ordinary 1000 ms wait.

## 10. Shutdown and connection lifetime

Host marks Terminal/shutting_down and disables publication first. Cancel/destroy lease callback and clear routes before buffer-drain dispatch or surface/context destruction. Late drain callbacks cannot Ack or forward. Existing buffer/other-callback ownership remains unchanged.

Guest disables normal native callback effects/publication before bounded mandatory release cleanup. Unhook on registering thread, check every UnhookWinEvent result and clear observations. Failed unhook is terminal teardown failure and prohibits restart. Do not treat recycled numeric hook handle or mutable global generation as proof of arbitrary old callback identity. Successful same-thread unhook is the boundary. [UnhookWinEvent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-unhookwinevent)

Current executable accepts one connection; no reconnect manager. A later authorized genuinely new session requires successful prior teardown, fresh hooks/peer/registry/mode/epoch/serial and full capability/barrier sequence. No active pair, requested routes, holds or observations carry over; no automatic focus restoration. General silent OS loss remains undetectable.

## 11. Required verification matrix

1. Golden bytes for 14–23, epoch/incarnation/serial offsets and padding; transactional errors; logical epoch zero for old types without tightening old wire behavior.
2. Historical V2 codec rejects first CreateV3 before surface admission; new-host V1 view-only and V2 original behavior remain distinct; no fallback.
3. Mode/direction/reserved failures before lookup; large lease serial never indexes video; global zero-ID lease messages bypass registry.
4. Single serial authority, stale epoch ordered no-effects consumption, repeated stale serial/future epoch rejection, exhaustion without wrap.
5. AwaitAck rejects current Focus/data/Release; exact Ack/Ready only once; inactive off/release/nonmatching data no-ops; no focus response exists.
6. Ordered old-off→new-on→pointer/key/button dispatch: first focus completes activation/release checks before next frame; obsolete new target leaves following data inert; failed activation stops further effects.
7. Actual foreground changes between received frames and immediately before emission revoke/disarm before subsequent input; old-epoch same-HWND/incarnation Focus/key/button have zero effects.
8. Active and anchored UnfocusedReady A→B revoke; anchored AwaitAck A→B→A terminates. **Unanchored initial AwaitAck** shell/tool/NULL transitions alone do not terminate or authorize input; local loss/hook/protocol/deadline failures still terminate.
9. Startup obsolete Focus remains unarmed/no-effect; live first explicit activation verifies target then establishes sticky anchor; no SendInput before success; failure terminal; no later focus-off/destruction/revoke returns to startup. Late startup callback after arming is not timestamp-filtered.
10. Anchor destruction reconciles current eligible exported replacement or fails unknown/NULL/ineligible; no new capture admission; reused HWND/incarnation does not inherit routing.
11. Old Wayland input before barrier, enter between sync/Ready and Ready alone never arm. Fresh post-Ready enter sends focus; old-off precedes new-on on ordinary transfer.
12. Known down before revoke, blocked down/up with NULL routes, authoritative enter snapshot absence/presence, button held across changes; no swallowed forever key or stale down after fresh focus.
13. Motion cannot restore old pointer route; fresh post-Ready pointer.enter identity only. New keyboard enter queues focus then optional matching-route pointer. Fresh button-down/wheel always queues matching current coordinates first; verify first click after focus uses correct guest cursor without replay from Ready. Queue failure of either frame is terminal.
14. Pointer-up/outside-bounds and pointer-leave cleanup preserve held release; button snapshot is not invented; suppressed off-surface button may consume first full click conservatively.
15. Reentrant API doubles during activation/down/up/release/lifecycle preserve receipt ordering and held accounting, with no recursive mutations or post-failure normal effect.
16. Pre-insertion transition emits nothing; successful down then observation is accounted then released; successful up remains clear; failed release retains bit. First wheel-axis success then transition is terminal, no replay.
17. Hook install/known local loss/64-record overflow/wrong hook/ambiguous reentry fail; tests do not claim silent OS loss detection. Unhook failure blocks restart; callback during host drain cannot Ack/forward.
18. Matching Ack/Ready after expiry fails; clock overflow/failure, traffic and EINTR do not extend; sync flush occurs before poll and EAGAIN retains deadline.
19. Fragmentation/partial EOF/cross-direction stale-input FIFO/queue-full failures disarm; first Create then failed Revoked terminates; video-only drops remain separate; no automatic reconnect.
20. ASan/LSan/UBSan and >=90% production codec/lease line and branch coverage, strict host link and Windows/native fixture cross-link are gates to execute, not claimed passes.
21. Separate authorized native two-window recovery, tool/owned/cross-process/desktop-loss rejection, suppression and teardown qualification. Models, doubles and cross-compilation do not substitute for this native evidence.

## 12. Implementation boundaries and memory ownership

`av_protocol.h` defines the logical message; the envelope remains 328 bytes. The appended logical `lease_generation` changes the in-process structure only. `av_codec.zig` is allocation-free and transactionally decodes new epoch fields without changing old wire acceptance. Frozen V1 and V2 fixtures are isolated test-only modules.

`av_lease.c` owns portable guest/host phase transitions. Guest serial admission occurs only in `av_lease_guest_apply`; the already-admitted input core never advances it again. `av_input_apply` preserves V2 validation and calls that same held-accounting core. Input callback return zero means successfully inserted, positive means suspended without insertion, negative is terminal. Held updates precede post-operation observation reconciliation. Mandatory cleanup attempts all held releases and retains failed bits for the bounded teardown retry.

Guest adapters provide synchronous target, emit, publish, reconciliation and checked monotonic clock callbacks. Pointer arguments are borrowed only for each callback. State is caller-owned and zero initialized once per successful new session. The portable module owns no heap, native handles, callbacks or threads. Transition revalidates the eligible candidate after releases and before Revoked publication. No recursive input mutation or native cleanup is permitted in emit callbacks.

The Wayland adapter owns a separate lease-sync proxy, distinct from diagnostic latency sync. Its event-thread context destroys that proxy before drain callbacks or surface teardown. The native Windows adapter owns same-thread hooks, a retained target-process lifetime handle and 64 value-only observations. Host/guest peers own their existing bounded FIFO transport queues; only frames may be dropped on full queue. A required control failure latches terminal status and stops further effects.

Build gates include the existing AV test and coverage aggregates, host real link, Windows guest and fixture cross-links. Physical SendInput/UIPI, receiving-desktop and Wayland acceptance require separately authorized native runs; this container cannot qualify them.
