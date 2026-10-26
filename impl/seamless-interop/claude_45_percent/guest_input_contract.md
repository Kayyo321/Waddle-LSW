# Guest Keyboard Executor Contract (interop path)

Scope: the guest side of the keyboard feature. It consumes host keyboard frames that the codec
has already decoded and performs Win32 keyboard actions through an injected table. It owns the
guest lease epoch and the guest's view of the host serial stream. The host side is
`session_input_contract.md` (this file is its guest counterpart). Parent specs: IMPL_DESC 3.6.2
(lease, serials), 3.6.3 (keyboard: repeat, lock, layout, scan table), 6.3 (input safety), and
`impl/input-lease-recovery/IMPL_DESC.md` sections 4 (single-owner serial), 5 (guest transition
table) and 6 (host barrier, for the frames the guest must not answer early).

## 1. Files

| Path | Role |
|---|---|
| `src/interop/guest_win32_compat.h` | Win32 type and constant subset for non-Windows builds. On Windows it includes `<windows.h>`. |
| `src/interop/guest_input.h` | Public API: status codes, Win32 table, state, frame entry points, release paths. |
| `src/interop/guest_input.c` | Executor. Typed mapping and repeat policy copied from `av_input.c`. Native table (Windows only). |
| `tests/interop/guest_input_test.c` | Linux test with a Win32 double, sanitizers, equivalence and a seeded randomized sequence. |
| `tests/interop/guest_input_coverage.py` | Coverage gate for `guest_input.c` (line and branch taken, at least 90%). |
| `tests/interop/guest_input_native_test.c` | Windows smoke program (cross-built here, run on a Windows CI runner). |
| `scripts/interop_lane_keyboard_guest.mk` | Make targets `interop-keyboard-guest-test` and `interop-keyboard-guest-native-cross`. |

## 2. Win32 table and header approach

`guest_input_win32_t` holds one function pointer per Win32 call the executor makes: SendInput,
GetKeyState, LoadKeyboardLayoutW, PostMessageW, GetForegroundWindow and
GetWindowThreadProcessId. Parameter types are the real Win32 types. The table is passed at
`guest_input_init()` and borrowed for the state's lifetime.

Why real types with a compat header: the table is part of the public contract, so its signatures
must match Windows exactly. `guest_win32_compat.h` provides `<windows.h>` on Windows. On Linux it
defines the same names, widths and INPUT/KEYBDINPUT layout (x86-64: INPUT 40 bytes, KEYBDINPUT 24
bytes) plus the constants used. No Win32 function is declared on Linux, so the Linux build can only
call doubles. Win32 identifiers (`SendInput`, `KEYEVENTF_*`, `wVk`, ...) keep their external
spelling; the naming rules do not apply to them.

`activate_keyboard_layout` is deliberately absent. The layout path loads with `KLF_ACTIVATE` and
never calls `ActivateKeyboardLayout` (IMPL_DESC 3.6.3 names only the load and the post).

## 3. Lease, epoch and serial rules as implemented

Serial admission (`guest_input_admit_serial`, used by every serial-bearing frame and intended for
LeaseAck). Order follows input-lease-recovery section 4, steps 1 to 3:

1. A zero serial, a serial not greater than the highest accepted serial, a zero lease, or a lease
   greater than the current epoch is a protocol violation: terminal. This includes a host wrap
   (a low serial after UINT32_MAX), because it is not greater than the high-water mark.
2. The serial is recorded before the epoch check, so a stale frame still consumes it (3.6.2:
   "old epoch frames consume serial and have no effect"). A replay of a stale serial as a current
   frame is therefore terminal (section 4: "replay/regression is terminal even when stale").
3. A lease below the current epoch returns `GuestInputStaleEpoch`. The caller must not act on the
   frame.

Epoch. The executor starts at epoch 0. `guest_input_revoke()` reserves epoch+1 and is the only
writer of the epoch. The first revocation, reserving epoch 1, is the one the lease component
queues on the first successful CreateV3 (section 5, Initial row). Epoch UINT64_MAX cannot be
advanced again, so a further revocation is terminal (`GuestInputEpochExhausted`).

Pair and window checks. Frames for a pair that is not the active pair are ordered no-ops
(`GuestInputIgnored`, serial consumed). Section 5 rows "Active + nonmatching/retired-pair data",
"UnfocusedReady + data/off/release".

## 4. Focus, keys, release

- `InputFocus pressed=1` on a new pair: release-all of the old pair's held keys, discard the lock
  queue, record the pair, and arm lock sync. This matches section 5 "Active + live current Focus-on".
  The same pair while active is a no-op. The executor does not call the foreground table here.
- `InputFocus pressed=0` on the active pair: release-all, clear the pair and the lock state
  (section 5 "Active + exact-pair Focus-off"; 6.3 focus loss).
- `InputRelease` on the active pair: release-all of held keys, pair kept (section 5 "Active +
  exact-pair Release").
- `InputKey`: suppressed (`KEY_POWER` 116, `KEY_SLEEP` 142) and unsupported codes are never
  injected (`GuestInputNotInjected`, not an error; 6.3). A down injects and marks the key held. A
  repeated down for a held repeatable key injects a plain down with no intervening up (3.6.3). A
  repeated down for any other held key is not injected. An up for a key that is not held is
  ignored (`GuestInputIgnored`).
- Injection form matches `av_windows.c` `keyboard_input()`: scan code with `KEYEVENTF_SCANCODE`
  (plus `KEYEVENTF_EXTENDEDKEY` for E0 keys); Hangul 0x15 and Hanja 0x19 by `wVk` only; Pause as
  `wVk` 0x13 with scan 0x45 and no scan flag; the key-up flag for every class.
- Release-all (`release_held`) walks codes 1..767 in ascending order. A failed key keeps its held
  bit and the walk continues. A failed release during a focus transition or revocation is terminal.

Revocation (`guest_input_revoke`), input-lease-recovery section 5 row "Active or anchored
UnfocusedReady + unexpected eligible exported foreground transition": release all holds first,
then reserve the next epoch and clear the pair. A failed release is terminal and reserves no
epoch. The caller queues LeaseRevoked with the returned epoch.

Disconnect (`guest_input_disconnect`) and agent exit (`guest_input_release_all`): release-all,
usable in any state. Disconnect also latches terminal (6.3, disconnect).

Terminal latch (`fail()`). Every protocol or injection failure latches terminal, discards the lock
queue, and attempts release-all. Later frames return `GuestInputTerminal`. `release_all` and
`disconnect` remain allowed as cleanup.

## 5. Lock sync (3.6.3, section 5 "UnfocusedReady + live current Focus-on")

- After a focus enter, InputKey and InputRelease frames for the active pair are queued in arrival
  order (`GuestInputQueued`) until InputLockState arrives. Nothing is injected in that window.
- `InputLockState` (current epoch, active pair): the guest reads GetKeyState for VK_CAPITAL,
  VK_NUMLOCK and VK_SCROLL (bit 0 of each), computes the plan with
  `waddle_keyboard_lock_guest_update()` (keyboard_lock.h, not reimplemented), injects a press and a
  release for each differing lock, then drains the queue in order.
- Queue capacity: `GuestInputPendingCapacity` = 64 entries. Overflow is terminal
  (`GuestInputQueueOverflow`), with no injection of the queued entries.
- The planner's own serial gate is wrap-safe within an epoch. Since the executor admits only
  strictly increasing serials, the planner refuses only a gap of 2^31 or more. That case is a
  terminal protocol violation (documented; tested).

## 6. Layout sync (3.6.3)

`guest_input_keyboard_layout()`: validate the 8-hex-digit KLID with
`waddle_keyboard_layouts_validate_klid()` (keyboard_layouts.h). An equal recorded KLID returns
`GuestInputLayoutUnchanged` with no Win32 call. Otherwise: foreground window, then its thread
(GetWindowThreadProcessId, zero means no window), then LoadKeyboardLayoutW with
`KLF_ACTIVATE | KLF_SETFORPROCESS`, then PostMessageW(WM_INPUTLANGCHANGEREQUEST) to that window.
The recorded KLID changes only after the post succeeds, so a failed step can be retried.

Decision not in the spec: with no foreground window the executor does not load the layout at all.
A load without a window to notify would change the process layout with nothing told. The next
frame retries.

KeyboardLayout carries no serial and no lease (3.6.2). It is accepted in any epoch.

## 7. Typed mapping path

Path taken: reimplemented. The table is copied from `src/av/av_input.c` (66 entries, generated
from that file, not retyped), with codes 1..83 as the identity scan map. The repeat list is copied
from `av_input.c` `NonRepeatKeys`.

Why not linked: `av_input.c` depends on `av_control_encode()` (AV protocol codec, Zig), and its
header pulls in `waddle/av_protocol.h`. The guest agent does not carry the AV codec. The test
compiles `av_input.c` and links `build/av_codec.o` to prove equivalence:
`av_input_translate_key()` against `guest_input_translate_key()` for codes 0..768 (kind, scan,
extended, virtual key; Pause checked by kind and by the injected VK/scan), and
`av_input_key_repeatable()` against `guest_input_key_repeatable()` for 0..768 and 0xFFFFFFFF.

Not compared: `av_input_key_injectable()`. Its 1..127 admission is AV's legacy wire bound and is
not the executor's policy. The executor injects typed keys up to 767.

## 8. Tests and measured results

Linux build (`make interop-keyboard-guest-test`):

```
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O1 -g -fsanitize=address,leak,undefined \
   -fno-omit-frame-pointer -Iinclude -Isrc/interop -Isrc/av tests/interop/guest_input_test.c \
   src/interop/guest_input.c src/av/av_input.c build/av_codec.o build/interop_keyboard_lock.o \
   build/interop_keyboard_layouts.o -o build/guest_input_test
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build/guest_input_test
```

Result: all checks passed; no sanitizer reports; leak detection on.

Scenario coverage: init and NULL arguments; mapping and repeat equivalence; exact INPUT forms;
lock sync order and all-bit plan; queue overflow; release queued during sync; lock-state edges
including the planner gap; serial rules (zero, replay, regression, wrap); epoch rules (stale,
newer, zero); stale focus-off inert; pair transitions; key rules (repeat, non-repeatable,
suppressed, unsupported, out-of-range); release frame; revoke release order; revoke failures;
release failure on leave; injection failure; lock injection failure; drain failure; disconnect;
serial-free frames; KeyboardLayout (invalid, no window, thread zero, load failure, post failure,
success, unchanged); admit hook; randomized sequence.

Randomized sequence: seed fixed (xorshift64*). 2000 events; 70% of frames name the first pair; one
frame in four is key 30. The run reports 529 injected records, 263 key runs and 3 repeat downs.
Invariant checked after disconnect: no up without a preceding down; every key is up at the end.

Coverage (`make interop-keyboard-guest-test` runs `tests/interop/guest_input_coverage.py`):

- `guest_input.c`: line 99.19% (244 of 246 lines executed); branch taken at least once 92.86%
  (221 of 238). Gate is 90% on both.
- Uncovered lines are defensive: the `default` arm of `inject_key()` (unreachable, since callers
  inject only translated keys) and a few arms reachable only with invalid inputs.

Windows cross build (`make interop-keyboard-guest-native-cross`):

- `build/interop_guest_input_native_test.exe`: `zig cc -target x86_64-windows-gnu -std=c11
  -Wall -Wextra -Wpedantic -Werror`. Passes. Output: PE32+ console x86-64 executable. The Zig
  modules are built for the same target with `zig build-obj`.
- `build/guest_input_mingw.o`: `x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Wpedantic -Werror
  -c src/interop/guest_input.c`. Passes.
- Also checked by hand, not a make target: mingw compile of the smoke program passes. A mingw link
  needs `-lntdll` for the Zig std objects (`RtlWaitOnAddress`, `NtClose`). zig cc links them itself.

## 9. Native smoke program (not executed)

Not run. This container is Linux and cannot create a Windows desktop. Expected CI run:
`build/interop_guest_input_native_test.exe` on a Windows runner with an interactive desktop
session. Checks, in order:

1. `RegisterClassW`/`CreateWindowExW` succeed; `SetForegroundWindow` returns nonzero and
   `GetForegroundWindow()` equals the window within 1 s (fails with a desktop message otherwise).
2. `guest_input_init`, then `guest_input_revoke` returns epoch 1.
3. `InputFocus` enter is accepted; `InputLockState` with the current toggles is accepted.
4. `a` down and up (evdev 30) produce WM_KEYDOWN and WM_KEYUP with scan 0x1E, in order.
5. Left Shift (42), `b` down/up (48), Shift up produce scan 0x2A and 0x30 messages in that order.
   Under layout 00000409 also WM_CHAR `a` and `B`; under another layout the char checks are skipped
   and a note is printed.
6. F13 (183) produces WM_KEYDOWN with scan 0x64.
7. Num Lock is planned to the flipped state through the executor and must read flipped within
   1.5 s, then restored and must read restored.
8. `guest_input_keyboard_layout` with the current KLID (GetKeyboardLayoutNameW) is accepted and the
   layout name is unchanged afterwards.
9. Disconnect releases cleanly with `held_count == 0`.

Exit code 0 when all checks pass; 1 otherwise, one stderr line per failed check.

## 10. Spec ambiguities resolved here

1. Lock state with an active pair and no pending sync: applied (the planner's natural semantics).
   Without an active pair: consumed no-op. The host sends one per enter, so the case is a resync.
2. Focus-off or a new pair during lock sync: queued entries are discarded. Nothing was injected,
   so nothing is owed.
3. Same-pair focus-on while active: no-op (the host never sends it, per 3.6.1 "enter for the same
   pair is a no-op").
4. Key-up for an unheld key and suppressed keys: no injection, not terminal.
5. KeyboardLayout with no foreground window: no Win32 load, nothing recorded, retried on the next
   frame.
6. KeyboardLayout post failure after a successful load: the process layout has changed, but the
   KLID is not recorded, so the next frame can retry.
7. Epoch 2^64 - 1 is reachable once; a revocation from it is terminal (input-lease-recovery 4).
8. Revocation release failure: terminal, no epoch reserved.
9. A repeat-info value outside the codec bounds returns InvalidArgument with no state change.

## 11. Limits and open items

- No real guest agent. Tests use a Win32 double. The native smoke was not executed.
- The LeaseRevoked, LeaseAck and LeaseReady handshake phases are not implemented. The AwaitAck
  rule ("other current Focus/input before Ready is terminal", input-lease-recovery 5) is not
  enforced here. The lease component must gate frames until Ready and must call
  `guest_input_admit_serial()` for LeaseAck. Until it does, a host that sends input before Ready
  is accepted.
- No real foreground activation semantics. Focus-on records the pair. Foreground verification
  before each injection (input-lease-recovery 8.2) is not implemented.
- No real keyboard layout verification on Linux. LoadKeyboardLayoutW and the post are doubles here.
- Pointer, touch, tablet, text and cursor frames are out of scope.
- `interop-keyboard-guest-native-cross` needs both zig and `x86_64-w64-mingw32-gcc` (the
  `build/guest_input_mingw.o` prerequisite). `make interop-test` runs it, so CI needs both.
