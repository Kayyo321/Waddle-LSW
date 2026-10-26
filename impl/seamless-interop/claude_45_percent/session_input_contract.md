# Session Input Contract (host keyboard feature, TODO #15 live halves)

Scope: the host-side keyboard feature that the session runtime (`session_control_host`
feature ops, see `host_feature_runtime_contract.md`) calls through `session_handler_t`.
It covers lock sync, layout sync, shortcut inhibit and repeat on the interop path.
Parent specs: IMPL_DESC 3.6.1 (routing and focus), 3.6.2 (lease, serials, epochs),
3.6.3 (keyboard), 8.45 (capability map). The lease handshake is specified in
`impl/input-lease-recovery/IMPL_DESC.md` section 6 (host barrier).

## 1. Files

| Path | Role |
|---|---|
| `src/interop/session_input_handler.h/.c` | Handler state, lease barrier, queue, repeat, lock, layout, inhibit hooks. No Wayland. |
| `src/interop/session_input_wayland.h/.c` | Wayland glue: `wl_keyboard` listener, surface table, shortcut-inhibit objects. |
| `src/interop/keymap_names.zig/.h` | Bounded keymap parser: first-group layout and variant, Caps/Num/Scroll indices. |
| `tests/interop/session_input_handler_test.c` | Handler tests through the real `session_handler_t` callbacks. |
| `tests/interop/session_input_wayland_test.c` | Glue tests with link-wrap doubles. |
| `tests/interop/session_input_coverage.py`, `session_input_wayland_coverage.py` | Coverage gates. |
| `scripts/interop_lane_keyboard.mk` | Lane rules (sanitizers, Zig tests, coverage). |

## 2. Capabilities and owned frames

- Handler capabilities: `InteropCapInput | InteropCapKeyboardLayout`.
- Owned inbound types: `LeaseRevoked` (0x0213), `LeaseReady` (0x0215), `CaretRect` (0x0212).
  `CaretRect` is consumed and ignored in this tranche.
- Outbound frames, all from the host, in ascending tag order:
  - `InputFocus` (0x0200): Window, Generation, Serial, Pressed, Lease.
  - `InputKey` (0x0201): Window, Generation, Serial, Pressed, Key, Lease.
  - `InputRelease` (0x020E): Window, Generation, Serial, Lease.
  - `InputLockState` (0x020F): Serial, Locks, Lease.
  - `InputRepeatInfo` (0x0210): IntervalMs (= wl delay, ms), Rate (per second).
  - `KeyboardLayout` (0x0211): Layout (8 hex digits, the KLID bytes).
  - `LeaseAck` (0x0214): Serial, Lease.
- Gating: input frames need `Input`; `InputRepeatInfo` needs `Input`; `KeyboardLayout`
  needs only `KeyboardLayout` (8.45). A frame whose capability is absent is never sent.

## 3. Serials and epochs

- Host serial: 32-bit counter, starts at 0, the first sent frame gets 1. It advances only
  when a serial-bearing frame is actually queued by the peer (`HostFeatureSuccess`).
  A `QueueFull` retry does not consume a serial. A send that would exceed UINT32_MAX fails
  the run. `InputRepeatInfo` and `KeyboardLayout` carry no serial.
- Lease epoch: the host stores the highest accepted guest epoch, starting at 0.
  Every input frame carries it.

## 4. Lease barrier (host, IMPL_DESC 3.6.2 and input-lease-recovery section 6)

| Phase | Event | Result |
|---|---|---|
| AwaitInitialRevocation | Revoked(1) | Epoch 1. Clear forwarding, queue and held keys. Arm 2000 ms deadline. Send LeaseAck (serial S). Phase AwaitReady. |
| AwaitInitialRevocation | Revoked(other) | Fail the run. |
| Ready | Revoked(epoch+1) | As above. Revoke discards pending and held input and sends no release (see section 9). |
| Ready | Revoked(other) | Fail the run (duplicate, regressed or skipped epoch). |
| Barrier / AwaitReady | any Revoked | Fail the run (overlap). |
| Barrier | LeaseAck send refused (`QueueFull`) | Stay in Barrier; retry on tick. The deadline keeps running. |
| AwaitReady | Ready(epoch = current, serial = S, before deadline) | Phase Ready. Clear the deadline. |
| any | Ready(older epoch) | No-op. |
| any | Ready(newer epoch) | Fail the run. |
| AwaitReady | Ready(wrong serial) | Fail the run. |
| not AwaitReady | Ready(current epoch) | Fail the run. |
| Barrier / AwaitReady | deadline reached (checked on tick, fds, frames and Ready) | Fail the run. |

Deadline: 2000 ms of `CLOCK_MONOTONIC`, armed at Revoked receipt, never restarted by
traffic. `next_deadline_ms` reports it while Barrier or AwaitReady.

## 5. Forwarding and focus

- Physical focus is always recorded (`post_focus_enter` / `post_focus_leave`).
- Forwarding starts only when the phase is Ready and Input is negotiated, and only on a
  fresh enter. An enter received in any other phase is never replayed after Ready
  (`input-lease-recovery` section 6: "a genuinely new post-Ready enter is required").
- Starting forwarding queues `InputFocus{pressed=1}` for the pair and arms lock sync and the
  repeat reducer for that pair.
- Stopping forwarding (focus leave, pair change, keymap rejection) queues
  `InputRelease` for the pair when any key is held, then `InputFocus{pressed=0}`.
- Enter for the same pair while focused is a no-op. Enter for another pair first stops
  forwarding for the old pair.
- Keys: `post_key` forwards only while forwarding. A key-down for a held code is ignored.
  A key-up for a code that is not held (for example a key held before the enter) is ignored,
  so no stray up reaches the guest. Codes outside 1..767 fail with `InvalidKey`.

## 6. Pending queue and overflow

- Capacity: 256 entries (`SessionInputPendingCapacity`), FIFO. Entry kinds: Key, Focus,
  Release, Locks. Serials are assigned at send time, not at enqueue time.
- Flush order per tick or callback: release-all (if flagged), pending entries in order,
  then KeyboardLayout, then InputRepeatInfo. A `QueueFull` on any step stops the flush and
  retries on the next tick (the runtime ticks at least every 100 ms). A `Draining` or
  `Closed` result stops without failing the run.
- Overflow (queue full on enqueue): all queued Key entries are dropped, the held set is
  cleared and one release-all is flagged. The release-all is sent first on flush. A new Key
  entry is not stored (release-all covers it). Non-key entries are stored after the
  compaction. If the queue is still full (only focus and lock entries), the run fails. This
  is the only case where the handler fails for capacity; it never loses a release.
- Counters: `overflows`, `repeats_dropped`.

## 7. Repeat

- The repeat reducer (`keyboard_repeat_abi`) sees forwarded key transitions only, with the
  repeatable flag from the reducer's policy (16 non-repeat codes).
- Timing: `CLOCK_MONOTONIC` arrival time, in ns. `next_deadline_ms` rounds up to whole ms.
- A due repeat is sent as `InputKey{pressed=1}` only when: forwarding, the queue is empty,
  and the code is still held. A blocked queue drops the repeat and counts it. The reducer
  never retries a repeat.
- A clock discontinuity (time moves backwards) makes the reducer forget its arming. The
  handler re-focuses the reducer while the pair is still forwarded (`repeat_restart`).
  Keys held across the step are not repeated until pressed again. Found by the handler
  tests: `cancel` clears the window, so without the re-focus repeats stopped for the rest
  of the session.
- Repeat info (`rate`, `delay`) from `wl_keyboard.repeat_info` is stored and sent once per
  change as diagnostics. A rate of 0 disables the reducer's repeat.

## 8. Lock sync, layout sync, inhibit

- Lock state: `waddle_keyboard_lock_host_enter` on each forwarding start. The first
  `modifiers` event after that produces at most one `InputLockState` mask (Caps bit 0,
  Num bit 1, Scroll bit 2). The indices come from the keymap; with no keymap the fallback
  caps=1, num=4 applies and scroll is absent.
- Layout sync: `session_input_post_keymap` parses the keymap with `keymap_names`
  (libxkbcommon is not installed and is not a dependency). The layout and variant feed
  `keyboard_layouts`. A changed KLID is sent as `KeyboardLayout`. An unmapped name changes
  nothing. A malformed keymap (layout or variant outside the charset) is rejected and
  stops forwarding (focus-off queued for the pair when forwarding).
- Inhibit: focus enter and leave, and the posted surface and inhibitor events, go to
  `keyboard_inhibit`. The reducer's actions reach `session_input_wayland_apply_inhibit`,
  which creates or destroys one `zwp_keyboard_shortcuts_inhibitor_v1` for the mapped surface.
  Reducer refusals (for example a full 16-window table) never gate forwarding. A generation
  above UINT32_MAX skips inhibit for that focus, and the glue refuses to map it.

## 9. Decisions that differ from the wording of the tranche brief (for lead review)

1. Revoke does not send `InputRelease`. The brief asked for "release-all on
   revoke/leave/disconnect". Re-verified against the spec text (tranche 5 review):
   - `impl/input-lease-recovery/IMPL_DESC.md` section 5, transition table row
     "Active or anchored UnfocusedReady + unexpected eligible exported foreground
     transition": "Suspend authorization, release all holds, recheck candidate, reserve
     next epoch, queue Revoked". The guest releases its own holds before it sends Revoked.
   - `impl/input-lease-recovery/IMPL_DESC.md` section 4 item 3: "Old epoch is an ordered
     no-op: no target, activation, release, emission or request-triggered reconciliation."
     A host release for the revoked epoch therefore has no guest effect, and a release
     sent with the new epoch targets a pair that is not active (the same no-op rule).
   - `impl/input-lease-recovery/IMPL_DESC.md` section 6, host Barrier: "On valid Revoked
     disable forwarding first, clear requested keyboard/pointer routes, preserve physical
     suppression". The spec names no host release on revoke.
   - `impl/seamless-interop/IMPL_DESC.md` 6.3 first bullet ("On focus loss, lease
     revocation, window retirement, disconnect and agent exit, every held key ... is
     released") is satisfied by the guest release above, not by a host frame.
   The handler therefore drops pending and held input on revoke without a frame. Leave
   still sends release-all, and so does the overflow path. Disconnect is covered by the
   guest's release on disconnect (6.3).
2. Disconnect (`on_closing`) sends nothing and clears local state. Nothing can be sent
   after `Draining`, and the connection loss releases guest input.
3. A stale epoch is handled as the frozen spec states, not as "ignored" in every frame
   type. A stale Ready is a no-op. A stale or regressed Revoked fails the run ("duplicate,
   regressed, skipped or overlapping Revoked terminates").
4. `KeyboardLayout` and `InputRepeatInfo` are sent on the next tick, not inside the
   post call. Posts never send, so every frame is sent from a callback that has a sender.
5. Layout parsing uses `keymap_names` instead of libxkbcommon (IMPL_DESC 3.6.3 names
   libxkbcommon). The Caps, Num and Scroll indices come from modifier_map statements, with a
   documented fallback.

## 10. Verification

- `make interop-keyboard-lanes-test` passes (exit 0). It runs the sanitized handler test, the
  sanitized glue test, the existing keyboard lane tests, and the Zig keymap tests.
- Handler test (`session_input_handler_test`, ASan, LSan and UBSan clean): descriptor and
  init guards, the lease handshake, forwarding and key rules, release on leave, lock once per
  enter, keymap and layout frames, repeat timing and drops, revoke and deadline failures,
  hostile lease frames and payloads, overflow release-all, queue capacity failure, serial
  wrap, draining, inhibit actions, dispatch and poll, error arms, and a 2000-event randomized
  sequence checked against a guest model (no key left down at the end).
- Glue test (`session_input_wayland_test`): keymap format, size and fd handling, enter, leave,
  key, modifiers, repeat info, inhibit create and destroy, inhibitor events, table full,
  unknown unmap, and a randomized sequence.
- Coverage (`session_input_coverage.py`, `session_input_wayland_coverage.py`):
  - `session_input_handler.c`: 99.26% line (538 of 542 lines in the gcov block), 93.10% branch
    taken at least once (324 of 348). Both at or above the 90% gate.
  - `session_input_wayland.c`: 100.00% line, 100.00% branch taken.
  - `keymap_names.zig`: 100% line under kcov (263 of 263, reported by the subagent that wrote
    it). Zig branch coverage is not measured by the available tooling, so the 90% branch
    requirement is not verified for this file.
- Naming scan of new C and Zig files: no camelCase identifiers (only Zig std names).

## 11. Limits and open items

- The runtime is not started by these tests. Frames are checked against the handler
  callbacks and the sender (`interop_host_feature_send`, link-wrapped). A fake guest peer
  through `interop_host_control_run` is not built here. That end-to-end test is open.
- Not in this tranche: guest-side injection of input frames, guest lock compare (`GetKeyState`),
  guest layout load (`LoadKeyboardLayoutW`), the activating click and cursor sync (3.6.1),
  pointer input, and the shortcut-inhibit policy source (surface fullscreen and pointer lock
  come from the window tracker through `session_input_post_surface_state`).
- The session runtime is not wired. `session_app` must register the handler
  (`session_input_make_handler`, `session_input_set_display`) and the glue must be attached to
  the seat's `wl_keyboard`. Both are owned by the lead.
- Keymap parsing: the modifier indices are discovered from `modifier_map` text. A keymap with
  no recognized `modifier_map` gets the documented fallback, which is not verified against a
  real compositor keymap.
- Wayland seat serials and times are not used. Protocol serials are the host counter only.
- Generation values above UINT32_MAX are not inhibited (the reducer's field is 32-bit). The
  handler still forwards input for them.
- Repeat across a clock step is not restored for held keys (section 7).

## 12. Integration (tranche 5)

Scope: the handler on the real session runtime path. The runtime is `session_app_run_detailed`
(handler table, union offer, pump, drain and closing) over `interop_host_control_run_detailed`.
A fake guest thread speaks the wire protocol over the control-host fixture's socketpair seam.
Files: `tests/interop/session_input_integration.c` (test), `scripts/interop_lane_keyboard_integration.mk`
(target `interop-keyboard-integration-test`, added to `InteropLaneTests` and `InteropLaneSanitizers`).
Run: `make interop-keyboard-integration-test`. It builds `build/session_input_integration_test` with the
keyboard lane's sanitizer flags and runs it with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1`
and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.

### 12.1 Composition and seam

- Runtime: `session_app_run_detailed` with one table, a copy of `session_input_make_handler`. The
  runtime supplies the offer (Input|KeyboardLayout). `control.feature_ops` is NULL, the admission
  `local_capabilities` is zero, `run_for_ms` is zero (unbounded). Stop is SIGTERM to the pump
  thread through the fixture's signalfd (`stop_fd`).
- Seam: the handler's display descriptor is the read end of a nonblocking pipe
  (`session_input_set_display`). For each scripted step the guest writes one byte. The dispatch
  hook (`kb_dispatch`, pump thread, from `on_fds`) drains the pipe, calls exactly one post function
  and returns zero. The handler then flushes inside the same callback. Frames from a step are
  therefore sent before the pump runs anything else. The guest waits until the step counter has
  advanced before it triggers the next step. Posts never run on the guest thread.
- Sync: the test copies the handler table and wraps two members. `on_frame` runs the handler
  and then increments a counter, so the guest knows the handler returned after each owned frame
  it sends (LeaseRevoked, LeaseReady, CaretRect). `on_closing` counts the call and then calls the
  original. The handler source is unchanged.
- Guest: it validates and decodes every host frame with the codec (`interop_header_decode`,
  `interop_payload_validate` with `InteropFromHost`, `interop_field_*`). It answers Ping with
  Pong. It checks that every serial-bearing frame's serial is strictly greater than the previous
  one. Its held-key model is set by InputKey, and cleared by InputRelease, by InputFocus with
  pressed 0, and by the guest's own LeaseRevoked (decision 1).

### 12.2 Main scenario (frames a to m)

Serials are exact where the sequence is fixed. S is the LeaseAck serial of the second epoch.

| Step | Guest sends or posts | Host frames, in order |
|---|---|---|
| a | Hello (Input and KeyboardLayout offered) | HelloAck; `Capabilities` equals Input\|KeyboardLayout |
| b | LeaseRevoked(epoch 1); LeaseReady(serial 1, epoch 1) | LeaseAck(serial 1, lease 1) |
| c | focus enter (window 0x1000, generation 7) | InputFocus(window, generation, pressed 1, serial 2, lease 1) |
| d | key 30 down, key 30 up, modifiers locked 0x12, same modifiers again | InputKey(pressed 1, key 30, serial 3, lease 1); InputKey(pressed 0, key 30, serial 4, lease 1); InputLockState(locks 0x03, serial 5, lease 1); no frame for the repeat of the modifiers (quiet window 80 ms) |
| e | keymap `include "pc+de+inet(evdev)"` | KeyboardLayout(Layout = KLID of "de", from `waddle_keyboard_layouts_observe` on a zero state); no serial, no lease |
| f | repeat info (rate 50, delay 100 ms); key 30 down; release after six repeats | InputRepeatInfo(IntervalMs 100, Rate 50); InputKey(pressed 1, key 30, serial 6, lease 1); six InputKey(pressed 1, key 30) with serials 7 to 12; InputKey(pressed 0, key 30, next serial, lease 1) |
| g | quiet window 250 ms after the release | no frame (a release cancels repeats) |
| h | LeaseRevoked(epoch 2); LeaseReady(S, epoch 2) | LeaseAck(serial S, lease 2). No InputRelease on revoke. |
| i | after Ready: key 30 down, key 30 up, focus leave (no frames, quiet 80 ms each); focus enter; modifiers; key 30 down | InputFocus(pressed 1, serial S+1, lease 2); InputLockState(0x03, S+2, lease 2); InputKey(pressed 1, key 30, S+3, lease 2) |
| j | focus leave while key 30 is held | InputRelease(window, generation, serial S+4, lease 2); InputFocus(pressed 0, serial S+5, lease 2). The guest model is empty. |
| m | SIGTERM to the pump | Goodbye is the only frame drained; the run returns HostControlCancelled (7); on_closing runs once with reason 7 |

Frame count of the main run: 21 host frames, with keyboard frames as in the table. The full
sequence is asserted in order, with type, window, generation, serial, pressed, key, lease, locks,
layout and interval or rate checked on each frame.

### 12.3 Timing (real CLOCK_MONOTONIC on the guest side)

- First repeat: arrival time minus the trigger time for the key-down step. Asserted >= 100 ms with
  no early tolerance. The host computes the due time from its own clock inside the post, which
  is later than the trigger time, and the pump wakes at a millisecond-rounded deadline (round up,
  never early). Measured in three runs: 100.15, 100.16 and 100.19 ms.
- Consecutive repeats: asserted >= 15 ms (the nominal period is 20 ms; the allowance is 5 ms).
  Measured minimum gap 21.06 to 21.08 ms, maximum 21.08 to 21.10 ms in three runs. The period is
  about 21 ms because the reducer reschedules at `now + period` after each take, and the pump
  wakes at millisecond-rounded deadlines, so the rate drifts to about 47 per second. No upper
  bound is asserted. This is the reducer's current scheduling, reported for lead review; it is not
  changed here.
- Repeat frames that arrive before the release frame are accepted by the loop. None occurred in the
  recorded runs.

### 12.4 Hostile and other runs (outcomes the runtime returned)

- CaretRect with a wrong window (0x999, generation 7) and with a wrong generation (0x1000,
  generation 8): no host frame in each 80 ms window. A following key down and up are forwarded
  with serials 3 and 4. Outcome HostControlCancelled (7).
- Stale epoch: after epoch 2 is Ready, LeaseRevoked(1). The handler returns SessionAppFailure,
  and the run fails. Outcome HostControlFeatureFailed (11). The host sent one Goodbye. on_closing
  ran once with reason 11.
- Unassigned type 0x0FF0 from the guest (header only, empty payload). The frame is rejected by
  the stream codec, not by feature admission. The host sends Error (0x0006), then drains. Outcome
  HostControlPeerFailed (4), not UnexpectedFeature. `detail.guest_error` stays 0, because that flag
  is set only on the Error and Draining admission path. on_closing ran once with reason 4.
- Known feature type the negotiated set does not admit (WindowAnnounce 0x0100; no Windows
  capability). Outcome HostControlUnexpectedFeature (5). The host sent Error (0x0006), then drained.
  on_closing ran once with reason 5.
- Disconnect while key 30 is held (the guest closes its socket end without Goodbye). Outcome
  HostControlPeerFailed (4). on_closing ran once with reason 4. Nothing is sent after the closing
  call.

### 12.5 Verification

- `make interop-keyboard-integration-test` passes (exit 0) under ASan, LSan and UBSan with
  `halt_on_error`. The run takes about 3.4 s. Three consecutive runs passed.
- `make interop-keyboard-lanes-test` still passes (exit 0), including its sanitizer phase.

### 12.6 Limits

- No real guest agent. The guest is a test thread. Its held-key and serial model is the codec
  decoder's view, not a guest key-state or toggle implementation (`GetKeyState`, `LoadKeyboardLayoutW`).
- The scroll bit (2) is not exercised. The keymap in the scenario has no `modifier_map`, so the fallback
  indices apply (caps 1, num 4, scroll absent), and the only mask asserted is 0x03.
- One window pair and one key (evdev 30). No pointer, text, caret position or inhibit path is run.
- The stop is a SIGTERM to the pump thread through the fixture's signalfd, not a process signal.
- The test includes `session_control_host.c`, so the fixture's vsock, mapping and admission seams are
  active (the same seams the session tests use). The handler itself is not wrapped.
- Mutation testing of the handler was not run. The assertions are exact frame and serial checks, so a
  change in the frame sequence fails the test.
