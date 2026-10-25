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
