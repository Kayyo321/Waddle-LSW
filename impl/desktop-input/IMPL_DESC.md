# Ordinary-window input milestone

## 1. Scope and limits

This milestone joins the existing host Wayland seat, bounded AV control socket,
and Windows user-mode `SendInput` path for the currently exported ordinary
window. It adds pointer motion, left/right/middle buttons, bounded wheel motion,
and physical PC keyboard presses/releases. Guest keyboard layout interprets the
scan codes. This does not implement host XKB text/layout translation, IME,
compositor key repeat, cursor imagery, relative pointer, tablet, clipboard,
process-family/modal-window admission, or physical acceptance. These remain
roadmap B gates. Host and guest must be built from this protocol revision.

No driver or privilege escalation is introduced. Windows foreground restrictions
and UIPI apply. Activation/injection failures fail the AV session and trigger
release cleanup. `SendInput` is desktop-global: foreground and process checks
immediately precede injection but are not an atomic OS security boundary against
an unrelated foreground change. This prototype therefore requires an isolated,
interactive guest desktop and is not qualified for concurrent unrelated guest
input or elevated/secure-desktop applications.

## 2. Components and ownership

`av_wayland.c` owns one seat (first advertised), keyboard and pointer proxies,
focus pointers, and an increasing input serial on its existing event thread.
Callbacks queue canonical `av_message_t` values through the existing bounded
64-frame peer queue. Delivery failure is terminal, never silently dropping a
release. Socket closure makes guest teardown release held inputs.

`av_codec.zig` validates untrusted fixed-size frames without allocation and leaves
output unchanged on error. `av_input.c` is a portable, caller-owned bounded state
machine. It delegates tracked-window checks/activation and one-event injection
to `av_windows.c`; it owns no heap or OS handles. Guest tracker, input state and
control receive callbacks share the existing guest message/capture thread.
No locks, cross-thread state, shared-memory input ring, or new dependencies exist.

## 3. Wire format and state

The 328-byte AV envelope and legacy message layouts are unchanged. New IDs are
7 focus, 8 key, 9 pointer, 10 button, 11 wheel, 12 release. Input `window_id`
(offset 8, u64 LE) is the announced HWND; `sequence` (48, u64) is a nonzero
window incarnation from its Create.sequence; `buffer_index` (44, u32) is a
strictly increasing nonzero input serial across the connection. Wrapping fails
closed. Create.sequence increases per guest tracker admission, even for a reused
HWND. The host retains this incarnation separately from video frame sequences.
Every input message has zero height, DPI, process ID, damage fields and title.

- Focus: flags 1 activates, 0 releases/clears; x/y/width are zero.
- Key: width is Linux evdev physical code 1..127; flags 1 down/0 up; x/y zero.
  Unsupported PC scan-code mappings fail closed. Held state is 128 bounded bytes.
- Pointer: x/y are surface-local integer pixels 0..8191; width/flags zero.
  Guest checks against current capture bounds and converts virtual-desktop pixels
  to Windows absolute coordinates using 64-bit arithmetic. Current surface scale
  is one and current attached pixel dimensions bound host coordinates.
- Button: width 1 left, 2 right, 3 middle; flags down/up; x/y zero.
- Wheel: x horizontal and y vertical Windows wheel units, each -1200..1200,
  nonzero vector; width/flags zero. Wayland vertical direction is reversed.
- Release: flags 1 keys, 2 buttons, 3 both; x/y/width zero, keeps keyboard focus.

All unused bytes are rejected, including nonzero title padding. Valid input still
must match both the live focus ID and incarnation before injection. Replayed or
out-of-order serials, stale incarnations and unknown focus fail the connection.
The window incarnation is session-owned identity, not authentication. Existing
VSOCK control access remains the transport trust boundary.

## 4. Execution

On keyboard enter, release the former focus, activate the new exported window,
and start with empty held state. Keys already held on host enter are suppressed
until their release; they are never imported from an unrelated Linux app.
Physical modifier key events follow the same press/release path. Keymap FDs are
closed without parsing because this mode intentionally uses guest layout.
On pointer enter/motion, forward coordinates only when pointer and keyboard
focus refer to the same live surface. Buttons/wheel obey the same check. The
initial activating click can be ignored if keyboard enter has not arrived yet.
Pointer leave releases buttons; keyboard leave releases all and clears focus.
Seat capability removal, seat global removal, window retirement and disconnect
also clear appropriate state. Capability addition creates fresh proxies.

On the guest, validate frame, serial, tracked HWND/process/incarnation and
foreground before injection. Only successful injections enter held state.
Releases for keys/buttons not held are ignored. Focus transfer releases old held
state before activating the next target. Removing/hiding a tracked window clears
its input before retiring the registry entry. Session cleanup retries failed
releases and reports failures rather than claiming balanced input.

## 5. Concurrency and failure

All input paths are synchronous on one owner thread per peer. The fixed peer
queue bounds memory. No event pointer is retained. Serial overflow, malformed
input, queue exhaustion and OS errors terminate the session. A failed release
retains its held bit for cleanup retry; other releases are still attempted.
Disconnect does not depend on successfully transmitting a final host release.
No reconnect reuses old focus state; a fresh connection creates a fresh state.
Stale geometry causes motion rejection rather than out-of-bounds injection.

## 6. Verification

Portable fixtures cover focus transfer, held-key/button accounting, unmatched
release, replay, stale window incarnation, rejected activation, injection failure,
release retry, disconnect reset and scan-code boundaries. Zig fixtures cover all
new types, canonical bytes, invalid ranges, output immutability and truncation.
Host callback fixtures use no live compositor and cover focus ownership, held
enter suppression, pointer bounds, leave and delivery failure. Run C fixtures
under ASan/LSan/UBSan and record coverage honestly. Build the Windows target as
cross-compilation evidence only. Actual Windows foreground/UIPI behavior,
compositor lifecycle, layouts, resize/scale and saved text workflows require an
authorized physical run and are not established by these tests.
