# Window-State Native Adapter Contract (guest, Win32)

Scope: the C adapter that executes the reducer's guest-side actions on a real
top-level HWND, and produces the guest observation used for initial sync and for
resynchronization. It is the native half of TODO #13 "native adapters" (25%).
The reducer and its C ABI are fixed by `window_state_contract.md` and
`window_state_abi_contract.md`; this document does not change their rules.

Files owned: `src/interop/window_state_adapter_windows.h`,
`src/interop/window_state_adapter_windows.c`,
`tests/interop/window_state_adapter_double.h`,
`tests/interop/window_state_adapter_suite.c`,
`tests/interop/window_state_adapter_integration.c`,
`tests/interop/window_state_adapter_native_windows.c`,
`tests/interop/window_state_adapter_coverage.py`, and the adapter targets in
`scripts/interop_lane_coordinator.mk`.

## 1. Scope and boundaries

Inside scope:

- Guest execution of the five guest actions the reducer can emit to the native
  window and the two host-side calls the guest makes for it (section 4).
- The guest observation (state bits and capability bits) from native facts.
- A bounded per-window table of admitted windows and of saved fullscreen state.
- A native API table with a real Win32 implementation (under `_WIN32`) and a
  test double (Linux and Windows).

Outside scope:

- The host side: `xdg-shell`, the wire protocol, the compositor connection.
- WinEvent hooks, window classification, admission policy, and dirty tracking.
  The adapter is called by the window coordinator on its owner thread.
- Retirement proof, frame capture, DWM, and input injection.
- DestroyWindow, TerminateProcess, and any call that removes a window. The adapter
  never destroys a window or a process.
- Changes to the reducer or to its ABI.

## 2. Native API table

`window_state_native_api_t` (declared in the header) is a struct of function
pointers. Every entry takes `void *context` first and a `uintptr_t window` (the
HWND value) second. Every entry is required (non-NULL). Every entry except
`current_thread_id` is called only on the adapter's owner thread. The table is copied into the adapter by value at create;
`context` must stay valid until destroy.

Each mutating or querying entry returns 1 on success and 0 on failure. The two
window-state queries (`is_iconic`, `is_zoomed`) report their value through an
out parameter, because the Win32 calls return a plain boolean with no failure
channel of their own; `is_window` returns 1 when the window exists and never
fails. On failure
the implementation has already captured the native error, which
`last_error` returns. A failing entry never leaves the window partially modified
by itself, except where the operation documents otherwise (for example
`SetWindowPos` and `SetWindowLongPtrW` are single calls).

| Entry | Win32 call (real implementation) | Notes |
|:--|:--|:--|
| `is_window` | `IsWindow` | |
| `window_thread` | `GetWindowThreadProcessId` | Returns 0 when the thread id is 0. |
| `is_iconic` | `IsIconic` | Writes 1 to `*iconic` when minimized. |
| `is_zoomed` | `IsZoomed` | Writes 1 to `*zoomed` when maximized. |
| `get_style` | `GetWindowLongPtrW(GWL_STYLE)` | Last error cleared before the call; 0 with a set error means failure. |
| `set_style` | `SetWindowLongPtrW(GWL_STYLE)` | |
| `show_window` | `ShowWindow` (synchronous) | Command is `SW_MINIMIZE`, `SW_RESTORE` or `SW_MAXIMIZE`. |
| `set_window_pos` | `SetWindowPos` | Flags are passed through. SetGeometry passes `SWP_NOMOVE \| SWP_NOZORDER \| SWP_NOACTIVATE`; fullscreen passes `SWP_FRAMECHANGED \| SWP_NOZORDER \| SWP_NOACTIVATE`. |
| `get_placement` | `GetWindowPlacement` | |
| `set_placement` | `SetWindowPlacement` | |
| `get_rect` | `GetWindowRect` | Window rectangle in screen coordinates. |
| `monitor_rect` | `MonitorFromWindow(MONITOR_DEFAULTTONEAREST)` then `GetMonitorInfoW` | Returns `rcMonitor`. |
| `foreground_window` | `GetForegroundWindow` | Returns 0 when none. Not in the brief; required by the activation fallback. |
| `set_foreground_window` | `SetForegroundWindow` | |
| `set_active_window` | `SetActiveWindow` | Used only by the activation fallback. |
| `attach_thread_input` | `AttachThreadInput(from, to, attach)` | |
| `post_message` | `PostMessageW` | The adapter uses it only with `WM_CLOSE`. |
| `current_thread_id` | `GetCurrentThreadId` | Read at create to define the owner thread, and on every entry to check the caller. The only entry any thread may call. |
| `last_error` | `GetLastError` | Read after a failed entry. |
| `extended_frame_bounds` | `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)` | The visible frame, without the invisible resize border. A failing HRESULT is stored as the last error with `SetLastError`, so `last_error` reports the HRESULT. |

`ShowWindowAsync` is not in the table. Asynchronous show returns before the state
change is applied, so a completed count would not mean that the change happened.
The adapter therefore uses the synchronous call only.

The Win32 implementation is returned by `window_state_native_api()` under
`_WIN32` and returns NULL on other platforms. The Win32 types never appear in the
header; `HWND` values travel as `uintptr_t`.

## 3. Identity, admission and the saved-state table

- The adapter holds a fixed table of 64 entries. Each in-use entry binds one
  `(window_id, generation)` to one HWND, its process id and its thread id. The
  identity is captured at `admit` by `window_thread` and `is_window`.
- `admit` fails with `AlreadyAdmitted` if the pair or the HWND is already bound,
  with `CapacityExhausted` if no entry is free, and with `NotFound` if the HWND
  does not exist. A thread id of 0 is `NativeFailure`.
- Before every native call that acts on the window (every action, `observe`,
  and the identity check itself), the adapter checks:
  1. `is_window(window)` is nonzero;
  2. `window_thread(window)` returns the admitted process id and thread id.
  If either check fails the window is stale. The adapter returns `StaleHandle`,
  removes the entry and any saved fullscreen state for it (the saved values are
  discarded and never applied), and makes no further native call for that
  window. A later call with that pair returns `NotAdmitted`.
- A failing identity query (the entry returns 0 for a reason other than the
  window being absent) is `NativeFailure` and changes nothing.
- The HWND argument of `observe` and `execute` must equal the admitted HWND. A
  different value is `InvalidArgument` and changes nothing; the adapter does not
  look up a second window.
- `release(window_id, generation)` removes an admitted entry. It returns `Busy`
  when the entry has a saved fullscreen state, so the owner must leave fullscreen
  first. Release makes no native call.
- A refused release is sticky. A `Busy` release keeps the entry, its HWND binding and
  its saved state, so the same pair is still admitted: `admit` answers
  `AlreadyAdmitted` and `observe` reports Fullscreen. The owner retries after
  LeaveFullscreen succeeds. A failed LeaveFullscreen keeps the record (section 4,
  item 5), so the retry is `Busy` again until that leave succeeds. A failed release
  never removes an entry.

Saved fullscreen state per entry: `saved_style` (the `GWL_STYLE` value), `saved_rect`
(`GetWindowRect`), and `saved_placement` (`GetWindowPlacement`). The presence of
this state is the only record of fullscreen; Windows has no native fullscreen
flag, so `observe` reports Fullscreen exactly when the record is present.

## 4. Guest actions

`execute` processes `actions[0..count)` in order. For each action, argument and
saved-state checks run first; they make no native call. Then the identity check
(section 3) runs before the first native call of the action.
If an action fails, execution stops: no later action is attempted, and
`*out_completed` is the number of actions before the failing one. If every action
succeeds, `*out_completed` equals `count`. A failure of an action that has already
changed the window (`EnterFullscreen` after its style change) is handled as
stated in each item below, so that the window and the saved state always agree
with `observe`.

Action kinds are the `waddle_window_state_action_kind_t` values of
`window_state.h`. Host-side kinds (1 to 5) and unknown kinds (0 and above 13)
are `InvalidAction` at their index; they are never executed by the guest.

1. **Minimize** (kind 8, `GuestMinimize`): `show_window(SW_MINIMIZE)`.
2. **Restore** (kind 6, `GuestRestore`): `show_window(SW_RESTORE)`. Restore does
   **not** leave fullscreen. A window with a saved fullscreen state stays
   fullscreen-stripped and its record stays in place. The reducer emits
   `LeaveFullscreen` when fullscreen clears, and that action restores the style.
   This is a deliberate deviation from the brief, which asked Restore to leave
   fullscreen when needed. Leaving inside Restore would make the reducer's order
   (restore, then fullscreen transition) observable as a different sequence.
3. **Maximize** (kind 7, `GuestMaximize`): `show_window(SW_MAXIMIZE)`.
4. **EnterFullscreen** (kind 9, `GuestEnterFullscreen`):
   - If a saved fullscreen state exists: `AlreadyFullscreen`, no change.
   - Read `saved_style`, `saved_rect`, `saved_placement`, and the monitor
     rectangle. Any failure is `NativeFailure` with no change.
   - `set_style(saved_style & ~(WS_CAPTION | WS_THICKFRAME))`. Failure is
     `NativeFailure` with no change.
   - `set_window_pos(monitor, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE)`.
     If this fails, the style is restored with `set_style(saved_style)` and the
     result is `NativeFailure`. If that restore also fails, the record is kept
     and the result is still `NativeFailure`, so that `LeaveFullscreen` can undo
     the stripped style and `observe` reports the fullscreen state that the
     window actually has.
   - On success, the record is stored.
5. **LeaveFullscreen** (kind 10, `GuestLeaveFullscreen`):
   - No saved state: `NoSavedState`, no change. The adapter never invents a style
     or a rectangle.
   - `set_style(saved_style)`, then `set_window_pos(saved_rect, SWP_FRAMECHANGED |
     SWP_NOZORDER | SWP_NOACTIVATE)`, then `is_iconic` and `is_zoomed` read the
     current state, then `set_placement` with the saved placement whose
     `show_command` is replaced by the current state: SW_SHOWMINIMIZED when
     minimized, SW_SHOWMAXIMIZED when maximized, otherwise SW_SHOWNORMAL. The
     placement therefore restores the saved normal rectangle and leaves the
     minimized and maximized state alone. The reducer decides that state with its
     own actions, and they run before this one. Re-applying the saved show command
     would undo them: the integration test found exactly that, where a configure
     restored the window and then LeaveFullscreen maximized it again. Any failure
     is `NativeFailure` and keeps the record, so a retry repeats these calls. Each
     of them is idempotent.
   - On success, the record is removed.
6. **Activate** (kind 11, `GuestActivate`):
   - `set_foreground_window(window)`. If it succeeds, the action completes.
   - Otherwise, the fallback: `foreground = foreground_window()`; if it is 0,
     `NativeFailure`. `from = current_thread_id()`. `to = window_thread(foreground)`.
     If `to != from`, `attach_thread_input(from, to, 1)`; failure is
     `NativeFailure`. Then `set_foreground_window(window)`, `set_active_window(window)`,
     and, if the input was attached, `attach_thread_input(from, to, 0)` is always
     attempted. The first failure among the foreground, active and detach calls
     is the result; a detach failure after success is `NativeFailure` too.
7. **SetGeometry** (kind 12, `GuestSetGeometry`): width and height must each be
   in `1..16384`, otherwise `InvalidGeometry` with no native call (the reducer
   already omits zero geometry). Then the adapter applies the visible-frame size
   with the DWM border adjustment of `av_windows.c` (lines 516 to 527):
   - `get_rect(window)` gives the outer rectangle, and `extended_frame_bounds(window)`
     the visible frame. Either failing is `NativeFailure` with no change.
   - `outer_width = width + (outer.right - outer.left) - (visible.right - visible.left)`,
     and the same for height. The result must lie in `1..16384`; otherwise
     `InvalidGeometry`, with no `set_window_pos` call.
   - `set_window_pos(0, 0, outer_width, outer_height, SWP_NOMOVE | SWP_NOZORDER |
     SWP_NOACTIVATE)`. The `x` and `y` of the action are not applied: the host
     sends them as zero (IMPL 4.5, WindowConfigure rect(0,0,w,h)), so the window keeps
     its origin. Failure is `NativeFailure`.
   The `maximized` field is accepted and does not change the call. The work-area
   origin for a maximized configure is not applied (IMPL 4.5 places a maximized
   window at the work-area origin). That needs the monitor work rectangle, which the
   table does not supply; section 11 lists it as an open item.
8. **PostClose** (kind 13, `GuestPostClose`): `post_message(window, WM_CLOSE, 0, 0)`.
   Failure is `NativeFailure`. Nothing else is called.

No action reads or writes any window other than the admitted one.

## 5. Observation

`observe` performs the identity check, then:

- Minimized bit: `is_iconic`.
- Maximized bit: `is_zoomed`.
- Fullscreen bit: present when a saved fullscreen state exists.
- Capability bits: `get_style` (`GWL_STYLE`). Resizable from `WS_THICKFRAME`,
  Maximizable from `WS_MAXIMIZEBOX`, Minimizable from `WS_MINIMIZEBOX`. While a
  saved fullscreen state exists, the capability bits are read from `saved_style`,
  because the stripped style has lost the frame. The record's style is the window's
  real style before fullscreen.
- Activated is never set.

The result is `WindowState` bits (`WaddleWindowStateMinimized` and the others of
`window_state.h`); any native failure is `NativeFailure` with `*out` untouched.

`observe` is the only function used for initial sync and for resynchronization.
After a failed completion the owner calls `observe` and passes the result to the
reducer as a GuestObservation plan.

## 6. Memory and lifecycle

- `window_state_adapter_create` allocates exactly one object (the adapter, which
  contains the 64-entry table inline) with `malloc`. Create returns `OutOfMemory`
  when allocation fails and writes nothing to the output.
- `window_state_adapter_destroy` frees that object and sets the caller's slot to
  NULL. It returns `Busy`, frees nothing, and leaves the slot unchanged when any entry
  is still admitted, fullscreen or not. The adapter stays valid, so the owner releases
  each entry (fullscreen entries first through LeaveFullscreen) and calls destroy again.
  It makes no native call. A caller that frees its own memory after a Busy destroy
  leaks the adapter; the owner must retry, not drop the adapter.
- No other allocation exists. No function retains a caller pointer after it
  returns. The adapter does not allocate for actions, observations or
  placements; all of them are fixed-size locals or table entries.
- A NULL slot or a NULL adapter is `InvalidArgument`.

## 7. Threading and re-entrancy

- The owner thread is the thread that calls `create`, identified by
  `current_thread_id` at that time. Every other function called from a different
  thread returns `WrongThread` and touches nothing.
- Native calls made by an action may dispatch messages to the owner thread
  (`SendMessage`-based calls into another process can do this). A busy flag is
  set for the duration of every adapter entry. An entry called while the flag is
  set returns `Reentrant` and touches nothing.
- WinEvent callbacks must not call the adapter. The adapter cannot detect that
  it is running in a hook procedure. The coordinator's contract is that hooks only
  post work to its own queue.

## 8. Errors and last error

- `window_state_adapter_last_error` returns the native error captured by the most
  recent failed native entry, or 0 when none has failed.
- No function writes an output on any status other than `Ok`, except `observe`
  and `execute`, whose outputs are documented above.

## 9. Verification

Double suite (`tests/interop/window_state_adapter_suite.c`, 28 tests), built plain and
under `-fsanitize=address,leak,undefined` with `ASAN_OPTIONS=detect_leaks=1`. It runs
on every platform against `tests/interop/window_state_adapter_double.h`, which logs every
native call and can fail any call by count or at random:

- The NULL-member contract: each of the 19 table members, set to NULL in turn, makes
  `create` return InvalidArgument with no output.
- Exact native call sequence of every action, including the fullscreen enter and leave
  sequences, the activation direct path, and the activation fallback.
- Every failure point: for four action sequences, each call of each entry fails once
  (with and without a fullscreen state entered beforehand). The completed count, the
  status, and the observed Minimized, Maximized and Fullscreen bits are checked after each.
- Stale handles: a destroyed window, an HWND reused with another process or thread
  identity, and an identity query that fails. A stale window's saved state is never
  applied.
- Fullscreen: round trips, enter twice, leave without a saved state, rollback when the
  placement call fails, rollback when the rollback also fails, and a leave that fails
  at each call while the record is kept.
- Host-only and unknown kinds, count and NULL-array rules, and geometry bounds.
- Admission: capacity of 64 (the 65th refused), duplicate pairs and HWNDs, zero
  identity components on their own, NULL adapter in every entry, release and destroy
  while fullscreen, and destroy with a NULL slot.
- Threading and re-entry: a call from another thread touches nothing; a call made from
  inside a native entry (ShowWindow) returns Reentrant for observe, destroy and execute.
- Observation bits, including capabilities read from the saved style while fullscreen,
  and a failed observation leaving its output untouched.
- A seeded random run: 24 rounds of 60 steps with 12 per cent native faults and
  occasional window destruction. Every OK status completes every action, every other
  status completes fewer, a destroyed window is reported stale and removed, and the
  observation matches the double's flags after each step.

Reducer integration (`tests/interop/window_state_adapter_integration.c`), built plain and
under the same sanitizers and linked with `build/window_state_abi/window_state_abi.o`:
the real reducer through `window_state.h`, the adapter, and the double. 200 rounds of 80
steps with 15 per cent native faults. Every guest plan is executed and completed with its
completed count; a failed completion is followed by a resynchronization from a fresh
observation. A stale result caused by a fault is recovered by re-admitting the same
window. The invariant, checked after every step with faults disabled: once the machine is
synchronized, its applied guest window bits equal the adapter's observation. The run
completes 287 guest actions, observes 183 native failures and 107 stale recoveries. The
test found the LeaveFullscreen fault described in section 4, item 5.

Coverage (`tests/interop/window_state_adapter_coverage.py`, run by
`make interop-window-state-adapter-coverage`): gcov line and branch on the portable logic,
driven by the double suite. Measured: line 100.00% (298 of 298), branch 100.00% (293 of
293). The gate is 90 percent. The `_WIN32` table is outside this measure by design.

Mutation checks, not committed, each run and restored: skipping the EnterFullscreen
rollback; leaving fullscreen without a record; dropping the activation fallback; making
Restore leave fullscreen; ignoring the thread id in the identity check; continuing after a
failed action. Each fails the suite.

## 10. Native smoke (Windows, executed by CI only)

Target `build/interop_window_state_adapter_native_test.exe`, cross-built by
`x86_64-w64-mingw32-gcc` from the adapter source and the smoke source. The smoke
creates its own top-level window on the interactive desktop, owns it on its main
thread, and asserts through `window_state_adapter_observe` and the real Win32 calls:

1. Minimize: observe reports Minimized; the window is iconic.
2. Restore: observe reports neither Minimized nor Maximized.
3. Maximize: observe reports Maximized.
4. Restore again, then EnterFullscreen: observe reports Fullscreen, the window
   rectangle equals the monitor rectangle, and the style lacks WS_CAPTION and
   WS_THICKFRAME.
5. LeaveFullscreen: observe no longer reports Fullscreen, the style and the
   rectangle equal their values before step 4.
6. SetGeometry to 320 by 240 at a fixed position: `GetWindowRect` equals it.
7. Activate: `GetForegroundWindow` equals the window. This depends on the desktop
   granting foreground rights to the smoke process; a failure is reported as
   such and fails the smoke.
8. PostClose: the window receives `WM_CLOSE`, which the smoke observes through a
   window procedure that records it and does not call `DestroyWindow`. The smoke
   then destroys the window itself, as its owner.
9. Stale handle: the smoke destroys the window as its owner. `execute` on the old HWND
   returns `StaleHandle` with no action completed, and `observe` on it then returns
   `NotAdmitted`. A second window is created and admitted under the same pair.
   `execute` with the old HWND value returns `InvalidArgument`, because the value is
   not the admitted HWND. If the system reuses the old value for the second window,
   that step is printed as a note and not counted; the stale identity check cannot be
   told apart from a fresh admission within one process and thread.

The smoke requires per-monitor DPI awareness v2 on its thread; without it the
rectangles are not comparable and the smoke fails at its first step.
The smoke writes one line per step and exits nonzero on any mismatch.

## 11. Known limits

- The adapter cannot make Windows accept a foreground change; the activation
  fallback is the documented route and may still fail under foreground locks.
- Fullscreen is an adapter convention, not a Windows state. Another process that
  changes the stripped style or the placement is outside what the saved state
  can repair.
- The maximized flag of SetGeometry does not change the call (section 4, item 7).
