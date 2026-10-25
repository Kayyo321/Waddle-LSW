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
  reducer's Zig tests, so a change to either side fails the other suite.
- Rule in `av_input_apply_admitted`: a key down for a held key is injected again
  only when the key is repeatable. Duplicate downs for non-repeatable keys stay
  suppressed. Releases and first presses are unchanged. A failed repeat emit
  leaves the key held and returns -1, as for any failed emit.
- The held bit is not a count. One release clears the key regardless of how many
  repeats were injected.

## 2. Repeat policy reducer (host)

Defined by `keyboard_repeat_contract.md`. The ABI in section 3 exposes it. The
Wayland adapter in section 4 drives it.
