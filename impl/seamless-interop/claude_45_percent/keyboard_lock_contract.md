# Keyboard lock-state reducer (TODO #15 lock sync)

`src/interop/keyboard_lock.zig` with C header `src/interop/keyboard_lock.h`
implements the pure lock-state logic of IMPL_DESC 3.6.3 "Lock state". It owns
no sockets, no Wayland or Win32 objects, no timers and no allocation. It does
not read `wl_keyboard`, does not call `GetKeyState` and does not inject
input. The host event loop and the guest message pump call it with values they
already decoded.

## Scope

In scope:

- Host: derive the three-bit lock mask from `wl_keyboard.modifiers` locked bits
  and decide when one `InputLockState` (0x020F) must be queued.
- Guest: validate an `InputLockState` update against the epoch and serial
  staleness gate, then produce the ordered VK toggle plan that makes
  `GetKeyState` agree with the desired mask.

Out of scope (owned by other TODO #15 subtasks): keymap parsing, layout
(KLID) mapping, shortcut inhibitor objects, the actual `keybd_event`/`SendInput`
calls, and wire encoding of `InputLockState` (already in `interop_codec.zig`).

## Protocol facts used

- Tag `Locks` is `0x0021`, one byte, bit0 Caps, bit1 Num, bit2 Scroll. Codec
  value range is 0 through 7 (`interop_codec.zig` `InteropTagLocks`).
- Message `InputLockState` is `0x020F`, host to guest, fields `serial`
  (tag 0x0010, u32, nonzero), `locks` (tag 0x0021), `lease_epoch` (tag 0x0045,
  u64, nonzero).
- Lock mask source on the host is the `locked` mask of `wl_keyboard.modifiers`.
  Caps Lock is the XKB `Lock` modifier; Num Lock and Scroll Lock use the
  keymap's modifier index for `NumLock`/`ScrollLock` (caller supplies it).
  `depressed` and `latched` are never used for lock bits.

## Public API (Zig)

| Symbol | Kind | Meaning |
|---|---|---|
| `LockCaps`, `LockNum`, `LockScroll`, `LockAll` | const u8 | Lock bits; `LockAll` = 0x07 |
| `MaxModifierIndex` | const u8 | 31 |
| `ModifierAbsent` | const u8 | 0xFF; keymap has no such lock modifier |
| `VkCapital`, `VkNumlock`, `VkScroll` | const u16 | 0x14, 0x90, 0x91 |
| `MaxToggleActions` | const usize | 6 (three keys, press and release) |
| `HalfSerialSpace` | const u32 | 0x80000000 |
| `error_t` | error set | `InvalidIndex`, `InvalidMask`, `InvalidSerial`, `InvalidEpoch` |
| `modifiers_t` | extern struct | `depressed`, `latched`, `locked` (u32 each), by value |
| `indices_t` | extern struct | `caps`, `num`, `scroll` (u8 each), by value |
| `host_state_t` | extern struct | `focused: bool`, `last_mask: u8`. Zero = valid initial value |
| `host_decision_t` | extern struct | `send: bool`, `mask: u8` |
| `guest_state_t` | extern struct | `accepted: bool`, `epoch: u64`, `serial: u32`. Zero = valid initial value |
| `toggle_action_t` | extern struct | `vk: u16`, `pressed: bool` |
| `toggle_plan_t` | extern struct | `count: u8`, `actions: [6]toggle_action_t` |
| `update_t` | extern struct | `applied: bool`, `plan: toggle_plan_t` |
| `status_t` | enum(c_int) | `Ok`=0, `NullArgument`=1, `InvalidIndex`=2, `InvalidMask`=3, `InvalidSerial`=4, `InvalidEpoch`=5 |
| `lock_mask(mods, indices)` | fn | Returns the mask, or `InvalidIndex` |
| `host_enter`, `host_observe`, `host_leave` | fn | Host reducer (below) |
| `serial_newer(candidate, last)` | fn | Wrap-safe serial order (below) |
| `plan_toggles(desired, current)` | fn | Ordered toggle plan (below) |
| `guest_update(state, epoch, serial, desired, current)` | fn | Guest gate plus plan |

C ABI (`export fn`, `callconv(.C)`, all pointer parameters are nullable and a
NULL pointer returns `NullArgument`): `waddle_keyboard_lock_mask`,
`waddle_keyboard_lock_host_enter`, `waddle_keyboard_lock_host_observe`,
`waddle_keyboard_lock_host_leave`, `waddle_keyboard_lock_guest_update`,
`waddle_keyboard_lock_plan_toggles`. Output structs are written only when the
call returns `Ok`. The C header mirrors the struct layouts exactly; the Zig
test `ABI struct sizes match the C header layouts` pins sizes 12, 3, 2, 2, 24,
4, 26 and 28, and the C consumer checks them with `_Static_assert`.

## Host reducer

Mask: `lock_mask` reads bit `index` of `locked` for each lock modifier. An
index equal to `ModifierAbsent` contributes 0. An index in 32 through 254 is
`InvalidIndex`. Validation of all three indices happens before any state is
read, so an invalid call never changes state and never sends.

`host_enter(state, mods, indices)`: computes the mask, sets `focused = true`,
records `last_mask`, and returns `send = true`. Every wl_keyboard enter sends
exactly one `InputLockState`, including a second enter without an intervening
leave and an enter whose mask equals the previous one. The spec requires one
message per `InputFocus{enter}`.

`host_observe(state, mods, indices)`: computes the mask. If not focused, or the
mask equals `last_mask`, returns `send = false`. Otherwise records the new
mask and returns `send = true`. A duplicate mask is never re-sent without a
focus enter. While unfocused the recorded mask is not updated.

`host_leave(state)`: clears `focused`. `last_mask` is retained but is not used
until the next enter, which always sends.

## Guest gate and planner

Staleness: `serial_newer(candidate, last)` is true when
`candidate != last` and `(candidate -% last) < 0x80000000`. This is RFC 1982
style serial arithmetic. A forward distance of exactly 2^31 is treated as
older, so no distance is ambiguous.

`guest_update(state, epoch, serial, desired, current)`:

1. `epoch == 0` returns `InvalidEpoch`. `serial == 0` returns `InvalidSerial`.
2. `plan_toggles(desired, current)` validates both masks (`InvalidMask`).
3. If `state.accepted` and (`epoch < state.epoch` or not
   `serial_newer(serial, state.serial)`), the update is stale. Return
   `applied = false` with an empty plan. State is unchanged.
4. Otherwise store `(accepted = true, epoch, serial)` and return
   `applied = true` with the plan.

Every rejection in steps 1 and 2 leaves state unchanged. The first update
after zero state is always applied.

Because the host serial is strictly increasing across all host-to-guest
messages (IMPL_DESC 3.6.2), an update whose serial is older than the last
accepted one is always stale, even when its lease epoch is newer. This is the
stricter reading of "lease_epoch or serial is older than the last accepted".

`plan_toggles(desired, current)`: `differing = desired XOR current`. Iterates
bits in the fixed order Caps, Num, Scroll and appends press then release for
each differing bit. Result count is 0, 2, 4 or 6. Equal masks give an empty
plan. Current toggle state is what the caller reads from `GetKeyState(...) & 1`
for each key; this module never calls that API.

The host sends `desired` as the protocol mask. The planner does not
interpret keyboard focus, so the caller must inject the plan before forwarding
any other key (IMPL_DESC 3.6.3).

## Ownership, lifetime and threading

- Every state is caller-owned storage of the sizes in the header. Zero-filled
  storage is a valid initial value. There is no init or free function, since
  nothing is allocated.
- Inputs are by value or borrowed only for the call. Outputs are written only
  on success.
- `host_state_t` is owned by the single host event-loop thread that receives
  focus and modifier events. `guest_state_t` is owned by the single guest
  owner thread that receives `InputLockState`. Neither may be shared.
- `lock_mask`, `plan_toggles` and `serial_newer` are pure and callable from
  any thread.
- No function blocks, allocates, logs or performs I/O.

## Error codes

| Status | Zig error | Trigger | State change |
|---|---|---|---|
| `NullArgument` | n/a | C NULL pointer | none |
| `InvalidIndex` | `error.InvalidIndex` | index outside 0..31 and not ModifierAbsent | none |
| `InvalidMask` | `error.InvalidMask` | mask bit outside 0x07 | none |
| `InvalidSerial` | `error.InvalidSerial` | serial 0 | none |
| `InvalidEpoch` | `error.InvalidEpoch` | epoch 0 | none |

A stale `guest_update` is not an error. It returns `Ok` with `applied = false`.

## Verification

Zig built-in tests in `keyboard_lock.zig` (run by `make interop-keyboard-lanes-test`):

- Lock mask: each bit, absent index, index 31, ignored `depressed`/`latched`,
  rejection of 32, 33, 254 at each of the three positions.
- Host: enter sends; observe sends only on change while focused; duplicate
  mask suppressed; unfocused observe never sends; leave then enter sends;
  repeated enter sends each time; invalid indices leave state and output
  unchanged.
- Serial: equal, older, newer, wrap from 0xFFFFFFFF to 1, distance 2^31 - 1
  newer, distance 2^31 and above older.
- Plan: equal masks empty; single key press-release; all keys in order; each
  unknown-bit rejection in either argument.
- Guest: first update applied; equal serial stale; older serial stale; older
  epoch stale; newer serial with older epoch stale; newer epoch and newer serial
  applied; zero epoch, zero serial and unknown bits rejected with no mutation.
- C ABI: every NULL argument position and every status code; outputs unchanged
  on error.
- Randomized sequence of 20000 host and guest operations (valid and invalid
  inputs, serials near the wrap point) checked against an independent reference
  that uses signed-distance serial ordering and set-bit arithmetic.
- ABI struct sizes equal the C header layouts.

`keyboard_lock_abi.c` is an independent C consumer that asserts the same
contract through the header. It is built with
`-fsanitize=address,leak,undefined` and run with
`ASAN_OPTIONS=detect_leaks=1` (`make interop-keyboard-lanes-sanitizers`).

Coverage (`make interop-keyboard-lanes-coverage`, gate >= 90% production line and
taken-branch), measured at this commit: line 98.68% (75/76), branch 90.00%
(36/40). One compiler panic guard is excluded by the existing coverage tool.

Zig sanitizer runs are not claimed. The Zig suite uses no allocator, so
`std.testing.allocator` and `FailingAllocator` sweeps do not apply to this module.

## Deviations and open items

- The lock mask uses only the `locked` modifier bits, as IMPL_DESC 3.6.3
  specifies. The `depressed` and `latched` fields are accepted and ignored.
- Scroll Lock uses the keymap's ScrollLock modifier index when the caller
  supplies one. A keymap without that modifier is passed as `ModifierAbsent`,
  so Scroll is reported off. The caller decides which index to supply.
- Ordering of `InputLockState` relative to `InputFocus` on the wire is owned by
  the caller and the message pump. This module only validates each message it
  is given.
