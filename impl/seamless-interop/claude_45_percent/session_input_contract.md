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
   revoke/leave/disconnect". The frozen spec clears routes on revoke and leaves guest
   release to the guest, which already releases its holds when it leaves the active pair
   (`input-lease-recovery` section 5: "Release all holds"). Old-epoch frames are ignored
   by the guest after serial validation, so a release sent after revoke has no effect. The
   handler therefore drops pending and held input on revoke without a frame. Leave still
   sends release-all, and so does the overflow path.
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
