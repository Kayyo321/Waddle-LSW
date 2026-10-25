# Live keyboard repeat and typed injection (TODO #15 live repeat, typed injection)

Scope of this contract: the host-owned typematic repeat composed from
`src/interop/keyboard_repeat.zig` (pure reducer), its C ABI, the Wayland host
adapter in `src/av/av_wayland.c`, the guest admission rule in `src/av/av_input.c`,
and the Windows native injection in `src/av/av_windows.c`. The pure reducer's
contract is `keyboard_repeat_contract.md`. The evdev-to-Set-1 table is
`keyboard_scan_contract.md`.

## 1. Guest admission of host repeats (`src/av/av_input.c`)

IMPL_DESC 3.6.3 requires that every host repeat is injected as a down without an
intervening up. The guest therefore admits a duplicate down for a key that is
already held when the key is repeatable.

- `av_input_key_repeatable(key)` returns 1 for keys 1..767 except the non-repeat
  set {29, 42, 54, 56, 58, 69, 70, 97, 100, 116, 119, 125, 126, 142}: the four
  modifier pairs (Shift, Ctrl, Alt, Meta), CapsLock, NumLock, ScrollLock, Pause,
  and the suppressed power and sleep keys. The set is the same list
  `keyboard_repeat.zig` `policy_repeatable` uses. It is duplicated in C because
  the guest sees only evdev codes and cannot call the host reducer. The same
  14-key golden list appears in `tests/av/input.c` (`repeat_policy`) and in the
  reducer's Zig tests. A change to the policy in either place fails that suite's
  golden, so both lists must change together.
- Rule in `av_input_apply_admitted`: a key down for a held key is injected again
  only when the key is repeatable. Duplicate downs for non-repeatable keys stay
  suppressed. Releases and first presses are unchanged. A failed repeat emit
  leaves the key held and returns -1, as for any failed emit.
- The held bit is not a count. One release clears the key regardless of how many
  repeats were injected.

## 2. Repeat policy reducer (host)

Defined by `keyboard_repeat_contract.md`. Section 3 is its C ABI. The Wayland
adapter (section 4) drives it.

## 3. C ABI (`src/interop/keyboard_repeat_abi.zig`, `keyboard_repeat_abi.h`)

Caller-owned opaque storage of `WaddleKeyboardRepeatStorageBytes` (1024 bytes,
8-byte aligned). Zero-filled storage is the valid initial state; `init` writes the
same state. The reducer allocates nothing. Storage is private: callers pass it only
to these functions, from one owner thread.

| Function | Inputs | Outputs | Errors |
|---|---|---|---|
| `init` | storage | — | NullArgument |
| `configure` | `enabled` (0/1), `rate` (0..1000), `delay_ms` (0..60000) | — | NullArgument, InvalidFlag, InvalidTiming |
| `focus` | `window` (nonzero), `generation` (nonzero) | — | NullArgument, InvalidFocus |
| `cancel` | — | — | NullArgument (idempotent) |
| `key` | `code` (1..767), `pressed` (0/1), `repeatable` (0/1), `now_ns` | — | NullArgument, InvalidFlag, InvalidKey, ClockDiscontinuity |
| `take_due` | `now_ns` | `out_code` (0 = none) | NullArgument, ClockDiscontinuity |
| `next_deadline` | — | `out_present` (0/1), `out_deadline` | NullArgument |

Each flag byte is validated before use. A status other than Ok changes no state.
A repeat returned by `take_due` is consumed. The caller queues it or cancels focus;
it is never retried. `take_due` returns at most one repeat per call, matching the
reducer.

## 4. Wayland host adapter (`src/av/av_wayland.c`)

- **Storage.** `av_wayland_t` owns one `waddle_keyboard_repeat_t` (1024 bytes),
  initialized in `av_wayland_init`. It is freed with the client. Nothing else allocates.
- **repeat_info.** `keyboard_repeat` rejects negative rate or delay without changing
  stored timing. A valid pair is passed to `configure(enabled = rate > 0, rate, delay)`.
  A rate of zero disables repeat. A rate above 1000 Hz or a delay above 60000 ms is
  rejected by the ABI and leaves the stored timing unchanged. A successful configure
  disarms an active candidate, as the reducer specifies.
- **Feeding the reducer.** `keyboard_key` calls `repeat_key` only after `send_input`
  has delivered the key to the guest. A suppressed key (held at focus enter, or not
  yet released after a snapshot) never reaches the reducer, so it never arms a repeat.
  `repeatable` is always passed as 1. The reducer owns the modifier, lock, Pause and
  suppressed-key exclusions; the adapter does not duplicate them. If delivery of a key
  fails, the repeat is cancelled.
- **Focus.** `keyboard_enter` calls `focus(window_id, incarnation)` after the focus
  message is delivered, in both legacy and lease modes. Zero identities are skipped.
- **Cancel.** `repeat_cancel` runs on every route teardown: `clear_keyboard_focus`
  (leave, keymap change, seat loss), `disable_routes` (lease revocation, terminal
  failure, lease-mode fail), `mark_delivery_failed`, retirement of the focused window,
  and a failed repeat delivery. A repeat that fires while the lease is not in the
  forwarding phase, or while the window is not controllable, cancels instead of sending.
- **Firing.** `av_wayland_timeout` runs first on every call, including the
  `maximum = 0` calls around the host poll. It takes each due repeat with
  `take_due(now_ns)` and sends it as a key down through `send_input`. This uses a
  serial exactly like a physical key. The loop stops when `take_due` returns 0.
  A failed send returns -1 (the same failure path as any input).
- **Poll timeout.** The poll timeout is the minimum of the caller's maximum, the
  lease deadline (lease mode), and the time to the next repeat. The repeat time is
  the remaining nanoseconds rounded up to whole milliseconds, so the poll never wakes
  early and misses the deadline. The fake clock in the tests is whole milliseconds,
  so the ceiling is exercised exactly.
- **Clock.** `monotonic_ns` reads `CLOCK_MONOTONIC`. A read failure yields 0, which
  cancels nothing and fires nothing for that call.

## 5. Verification (`tests/av/wayland_state.c`)

- Timing: delay 100 ms (`timeout = 100`), first repeat at exactly the delay, then
  period `ceil(1e9/25)` = 40 ms. Rate 3 uses 333333334 ns, so the poll waits 334 ms
  and does not fire at 333 ms. A key up stops the repeat.
- repeat_info: negative rate and delay, rate 1001, and delay 60001 each leave the
  stored timing unchanged; rate zero disables.
- Cancel: leave, keymap format change, failed repeat delivery, and lease revocation
  (`disable_routes`) each leave no repeat armed and send no further key event.
- Suppression and rollback: a key held at enter never arms a repeat; a clock moving
  backward disarms the repeat, so no repeat is delivered afterwards.
- Randomized: 2000 steps of key down and up, focus enter and leave, repeat_info
  changes, and time advances. Each step's key events are compared with a reference
  model in order, and the poll timeout is compared after every advance. At least 20
  repeats fired (34 in the committed seed).
