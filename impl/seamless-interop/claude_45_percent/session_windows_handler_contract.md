# Session Windows Handler Contract

Owner: TODO #11g (live wiring of the window lifecycle), #12 (host half of move, resize and state requests) and #13
(host half of the wire window path). Modules: `src/interop/session_windows_handler.c/.h`,
`src/interop/host_wayland_core.c/.h`, and the windows layer `src/interop/host_wayland_windows.c/.h`. Parent contracts:
`host_feature_runtime_contract.md` (the feature ops table and admission), `peer_feature_send_contract.md` (request
identifier classes and send results), `host_wayland_windows_contract.md`, `host_wayland_core_contract.md`,
`window_state_abi_contract.md`. Normative rules: IMPL_DESC 3.2.5, 4.3, 4.5, 4.7 and 8.45.

## 1. Limits (read first)

- **Frames are not built.** FrameReady is answered at once with FrameRelease for the exact window, generation, slot
  and frame sequence, so the guest does not leak a slot. Nothing is attached or committed. Each such reply counts in
  `frames_dropped_no_buffer_layer`. The 11 credit covers the window lifecycle only; it does not cover frames.
- **Error is never sent by this handler.** Error is a control-class message (`control_peer.zig`, `is_control_type`), and
  a feature sender refuses control types. Refusals are counted: `errors_reported` for windows-layer refusals, and the
  per-request ignored counters for guest requests. The run continues.
- **WindowIconChanged is ignored and counted** (`icons_ignored`). The catalog lane owns icons.
- **Not wired into the session runtime.** `session_main` still passes NULL ops (`host_feature_runtime_contract.md`
  section 1). The handler is composed by the session application runtime, which is not part of this lane.
- **The Wayland layer is tested against a recording double.** No libwayland wire bytes and no real compositor are
  involved. Real compositor acceptance (11i, 11j) is open.

## 2. Public surface

| Function | Purpose |
|---|---|
| `session_windows_handler_init(handler, config)` | Storage and configuration; no connection. Returns 0, or nonzero for a bad device name or scale. |
| `session_windows_handler_descriptor(handler)` | The `session_handler_t` table. Context is the handler. Capabilities: `InteropCapWindows`. |
| `session_windows_record_press(handler, input)` | The input glue records a press (lease, kind, button, seat, window pair, clock). |
| `session_windows_release_press(handler, T)` | The input glue removes the press for T on release or cancel. |
| `session_windows_set_lease_epoch(handler, epoch)` | Sets the lease that presses and guest requests must belong to; drops other leases' presses. |
| `session_windows_set_seat_listener(handler, listener, user)` | The input lane's seat hook. The handler forgets removed seats first, then forwards the event. |
| `session_windows_handler_counters(handler, out)` | Copies the counters (section 6). |

Config: device (1..64 bytes), display name or an injected `wl_display` (tests), scale fraction (zero means 1/1), and an
optional monotonic clock in milliseconds (NULL uses `CLOCK_MONOTONIC`).

## 3. Lifecycle

1. `init` stores the configuration. Nothing is connected.
2. `on_ready(negotiated)`: when `InteropCapWindows` is negotiated, the core connects (or uses the injected display),
   binds the globals, and the windows layer is initialized from them. A missing required global or a failed init
   returns nonzero, which fails the run with `HostControlFeatureFailed`. Without Windows negotiated, nothing happens.
3. Steady state: `poll_fds` reports the Wayland descriptor (POLLIN, plus POLLOUT while output is pending). `on_fds`
   dispatches the core, then flushes pending frames and Wayland output. `on_tick` flushes. `on_frame` handles one
   admitted frame, flushing pending frames first and Wayland output last.
4. `on_closing(reason)` runs exactly once: pending frames are counted as dropped (Draining), the windows layer
   resets (every window destroyed in teardown order, section 5), each reducer is destroyed, then the core closes.
   No frame is sent while closing.

A reconnect is a fresh `init` with fresh storage; no state survives an epoch.

## 4. Inbound frames (on_frame)

Every inbound frame must have request identifier 0 (unsolicited, 8.45). A nonzero identifier returns nonzero and fails
the run, as the peer contract requires of the dispatcher. `owns_type` covers WindowAnnounce, WindowUpdate,
WindowRetire, FrameReady, WindowMoveRequest, WindowResizeRequest, WindowStateRequest and WindowIconChanged.

| Frame | Behaviour |
|---|---|
| WindowAnnounce | Create the reducer from the state field (refused state drops the frame). Realize the window through the windows layer. On refusal drop the reducer. On success plan an initial GuestObservation and execute its host actions. |
| WindowUpdate | Apply through the windows layer. On success plan GuestObservation from the state field, execute its host actions, and complete (or complete the successful prefix on a failed action; the reducer then resynchronizes). |
| WindowRetire | Retire through the windows layer. Every reducer whose window pair is no longer realized is planned with DestroyObserved, completed, and dropped. |
| FrameReady | Send FrameRelease at once (section 1). |
| WindowMoveRequest | Section 7. |
| WindowResizeRequest | Section 7. |
| WindowStateRequest | Section 8. |
| WindowIconChanged | Ignored and counted. |

Refusals by the windows layer are not failures of the run. They are counted and dropped.

## 5. Teardown order

`on_closing` runs the windows reset first (each window: decoration, then toplevel or popup, then xdg_surface, then
wl_surface; children before parents), then the reducers are destroyed, then the core closes (seats, decoration
manager, shm, xdg_wm_base, compositor, registry, and the display only when owned). This order is asserted by the
handler test and by the control runtime test.

## 6. Outbound frames and backpressure

Every outbound frame has request identifier 0 and direction host.

- Sends go through `interop_host_feature_send` on the sender of the running callback. A sender is never kept past its
  callback.
- `HostFeatureSuccess` counts `frames_sent`. `HostFeatureDraining` drops the frame and counts `frames_dropped_draining`.
- `HostFeatureQueueFull` stores the frame in the pending list (at most `SessionWindowsMaxPending` = 128). Later frames
  queue behind it, so FIFO order holds. The list flushes at the start of each callback and on each tick, and stops at
  the first QueueFull.
- A WindowConfigure for a window pair that already has a pending WindowConfigure replaces it (the latest is kept;
  `configures_coalesced`).
- A full pending list fails the run (`pending_overflow`). Nothing is silently dropped to make room.
- Any other send result fails the run.
- Pending frames for a window pair that the windows layer no longer realizes are purged before the next flush
  (`frames_dropped_retired`). See the deviation in section 10.

Outbound frames:

| Frame | Source | Fields |
|---|---|---|
| WindowConfigure | windows `on_configure` (after the xdg_surface acknowledgement) | window, generation, rect (0, 0, w x scale, h x scale) only when both sizes are nonzero and at most 16384, state (Activated and the window state bits) |
| WindowClose | windows `on_close` | window, generation |
| PopupRepositioned | windows popup configure | window, generation, rect |
| PopupDismissed | windows `popup_done` | window, generation |
| FrameRelease | inbound FrameReady | window, generation, slot, frame_sequence |

## 7. Move and resize (4.5)

A guest request names the protocol serial T of a press. The handler resolves T through the press table and acts only
when every condition holds; otherwise it ignores the request and counts it (`moves_ignored`, `resizes_ignored`).

1. T is a live press (not released, not expired after 300 ms of the caller's clock, not dropped by a lease change).
2. The press's window pair equals the request's window pair (a stale generation is ignored).
3. The press is pointer, button evdev `BTN_LEFT` (0x110). The press exists only while the button is held, because the
   input glue removes it on release.
4. The press is the latest live press of its window pair.
5. The press belongs to the current lease epoch.
6. The press's seat is still recorded by the core and registered in the windows layer (a removed seat is forgotten).
7. Resize only: the reducer's last observation carried the Resizable capability, and the edge is one of 1, 2, 4, 5, 6,
   8, 9, 10. These values are the xdg-shell edge values and the interop edge values of 3.2, so the wire value is passed
   through unchanged.

Then `xdg_toplevel.move(seat, S)` or `xdg_toplevel.resize(seat, S, edge)` is sent with the press's Wayland serial S.

## 8. State requests (8.45 and 4.5)

WindowStateRequest asks for the Maximized bit to be flipped (caption double click). The handler plans a
HostStateRequest on the window's reducer with `maximize` set from the requested bit. The reducer refuses the request
when the window lacks the Maximizable capability (`UnsupportedStateRequest`) or is unsynchronized. On success the
returned maximize or unmaximize actions run on the windows layer, and the plan is completed. Only a request that
changed the host maximize state counts as applied (`state_requests_applied`).

Guest observations (Announce and Update state fields) plan GuestObservation. The reducer returns host actions for the
observed minimize, maximize and fullscreen transitions. Minimize is one-shot (`set_minimized`). Maximize and fullscreen
are persistent (`set_maximized` / `unset_maximized`, `set_fullscreen` / `unset_fullscreen`). Guest actions in a plan are
the guest's and count as completed.

Compositor configures (windows `on_configure`) plan HostConfigure with the state bits (Activated, Maximized,
Fullscreen; the capability bits never come from the host) and the physical size when it is valid. The completed plan
counts as the guest having been told. An unsynchronized window plans nothing and the configure is still sent.

Reducers are keyed by the window pair (at most 64, equal to the windows table). The reducer ABI takes a 32-bit
generation; a generation at or above 2^32 is refused by the reducer and counted as a refused announce (documented in the
header).

## 9. Failure model

A callback returns nonzero (and the run fails with `HostControlFeatureFailed`) only when the handler cannot continue:

- a Wayland display error (dispatch or flush), or a required global missing at ready;
- a send result other than success, QueueFull or Draining;
- a pending-list overflow;
- an inbound frame with a nonzero request identifier, or a frame before ready.

The failure is sticky: after a fatal condition every later callback returns nonzero, and the runtime then drains and
calls `on_closing` once.

Guest requests that cannot be honoured, refused windows-layer operations and malformed fields are not failures. They
are counted (section 11).

## 10. Deviations from the lead's rules, and why

- **Pending window frames of retired windows are dropped.** The rule "never drop WindowClose or Popup*" applies to
  overflow. A pending close, configure or popup frame for a window the guest has already lost (retired by a guest
  retire or by a failed delivery) would describe a window the guest no longer has. It is purged, counted, and never
  sent. The randomized run found a case where it was sent (seed 0x9e3779b97f4a7c15, step 239).
- **Error frames are not sent** (section 1).
- **FrameRelease is sent for any window pair**, including one not realized, because the guest's slot must be released
  whatever the pair.
- **A guest Goodbye ends the run as `HostControlPeerFailed`**, not as a clean success. This is the runtime's mapping of
  a Draining reported with reason LocalRequest (`session_control_host`), not a handler decision.
- **A window frame without Windows negotiated** is refused by the peer with Error(UnsupportedType) and ends the run with
  `HostControlUnexpectedFeature`, before the handler is consulted.

## 11. Counters

`frames_received`, `frames_sent`, `frames_queued`, `frames_dropped_draining`, `frames_dropped_no_buffer_layer`,
`frames_dropped_retired`, `configures_coalesced`, `pending_overflow`, `announces_realized`, `announces_refused`,
`updates_applied`, `retires_applied`, `moves_applied`, `moves_ignored`, `resizes_applied`, `resizes_ignored`,
`state_requests_applied`, `state_requests_ignored`, `guest_observations_applied`, `host_actions_failed`,
`configures_sent`, `closes_sent`, `popup_frames_sent`, `icons_ignored`, `errors_reported`, `frame_fields_refused`.

## 12. Verification

- `interop-session-windows-handler-test` (`tests/interop/session_windows_handler_test.c`, combined double
  `session_windows_handler_fake.h`): 30 tests under ASan, LSan and UBSan with `-Werror`. They cover request sequences
  for each window kind, every outbound frame's fields, the move and resize validation matrix, state requests,
  FrameReady, QueueFull coalescing and overflow, stale generations, display errors, teardown order, a missing global,
  a missing decoration manager, and a randomized sequence of 3000 steps for three fixed seeds. The random run checks
  that every outbound frame that names a window names a realized one, that children are destroyed before parents, and
  that the pending list never exceeds 128.
- `interop-session-windows-handler-coverage` (`tests/interop/session_windows_handler_coverage.py`): gcov line and branch
  coverage of `session_windows_handler.c`, no exclusions. Measured: lines 99.23 percent (516/520), branches 92.31
  percent (372/403). The uncovered lines are unreachable by construction (a guest-action switch default, an encode
  failure for an already-realized window, and a window-init failure after validation).
- `interop-session-windows-control-test` (`tests/interop/session_windows_handler_control_test.c`): the handler through
  the real `interop_host_control_run` with an in-process guest peer and a single-handler ops adapter written in the
  test. Eight cases: admission and announce (10 xdg requests), configure reaching the guest (one WindowConfigure with
  state Activated and rect 0,0,800,600), move validation (two refusals, one applied), FrameReady releasing the slot,
  retire followed by a late close (no close sent), display error mid-run (`HostControlFeatureFailed`, one `on_closing`),
  guest Goodbye (`HostControlPeerFailed`, one `on_closing`), and no Windows negotiated (`HostControlUnexpectedFeature`).
- `interop-host-wayland-core-test` and `interop-host-wayland-core-coverage`: 100.00 percent line (169/169) and branch
  (178/178) for `host_wayland_core.c` (see `host_wayland_core_contract.md`).

## 13. Honest gaps

- No libwayland wire bytes and no compositor. The double checks requests, arguments, event delivery and destroy order.
- The control runtime test uses a constant clock for presses, so no press expires by time there. Expiry is covered by
  the handler test with an injected clock.
- The late WindowClose case in the control test is dropped by the double's dead-object rule, not by the handler. The
  handler's purge (section 10) covers the pending case.
- Coverage is measured for the handler and the core, not for the control runtime test.
- The handler is not composed into `session_main` or a session application runtime. Wiring it there is the next step
  and is outside this lane's ownership.
- Frames (attach, commit, buffer release) are not implemented; FrameReady is only released (section 1).
- Removal of compositor, xdg_wm_base, shm or decoration globals is ignored by the core (`host_wayland_core_contract.md`).
