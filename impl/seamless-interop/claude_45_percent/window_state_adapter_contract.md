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
HWND value) second. Every entry is required (non-NULL) and is called only on the
adapter's owner thread. The table is copied into the adapter by value at create;
`context` must stay valid until destroy.

Each mutating or querying entry returns 1 on success and 0 on failure. On failure
the implementation has already captured the native error, which
`last_error` returns. A failing entry never leaves the window partially modified
by itself, except where the operation documents otherwise (for example
`SetWindowPos` and `SetWindowLongPtrW` are single calls).

| Entry | Win32 call (real implementation) | Notes |
|:--|:--|:--|
| `is_window` | `IsWindow` | |
| `window_thread` | `GetWindowThreadProcessId` | Returns 0 when the thread id is 0. |
| `is_iconic` | `IsIconic` | Returns 1 when minimized. |
| `is_zoomed` | `IsZoomed` | Returns 1 when maximized. |
| `get_style` | `GetWindowLongPtrW(GWL_STYLE)` | Last error cleared before the call; 0 with a set error means failure. |
| `set_style` | `SetWindowLongPtrW(GWL_STYLE)` | |
| `show_window` | `ShowWindow` (synchronous) | Command is `SW_MINIMIZE`, `SW_RESTORE` or `SW_MAXIMIZE`. |
| `set_window_pos` | `SetWindowPos` | Flags are passed through. |
| `get_placement` | `GetWindowPlacement` | |
| `set_placement` | `SetWindowPlacement` | |
| `get_rect` | `GetWindowRect` | Window rectangle in screen coordinates. |
| `monitor_rect` | `MonitorFromWindow(MONITOR_DEFAULTTONEAREST)` then `GetMonitorInfoW` | Returns `rcMonitor`. |
| `foreground_window` | `GetForegroundWindow` | Returns 0 when none. Not in the brief; required by the activation fallback. |
| `set_foreground_window` | `SetForegroundWindow` | |
| `set_active_window` | `SetActiveWindow` | Used only by the activation fallback. |
| `attach_thread_input` | `AttachThreadInput(from, to, attach)` | |
| `post_message` | `PostMessageW` | The adapter uses it only with `WM_CLOSE`. |
| `current_thread_id` | `GetCurrentThreadId` | Read at create only; defines the owner thread. |
| `last_error` | `GetLastError` | Read after a failed entry. |

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

Saved fullscreen state per entry: `saved_style` (the `GWL_STYLE` value), `saved_rect`
(`GetWindowRect`), and `saved_placement` (`GetWindowPlacement`). The presence of
this state is the only record of fullscreen; Windows has no native fullscreen
flag, so `observe` reports Fullscreen exactly when the record is present.

## 4. Guest actions

`execute` processes `actions[0..count)` in order. For each action it first
performs the identity check (section 3), then the native calls of that action.
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
     SWP_NOZORDER | SWP_NOACTIVATE)`, then `set_placement(saved_placement)`. Any
     failure is `NativeFailure` and keeps the record, so a retry repeats these
     calls. Each of them is idempotent.
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
   already omits zero geometry). Then `set_window_pos(x, y, width, height,
   SWP_NOZORDER | SWP_NOACTIVATE)`. The `maximized` field is accepted and does not
   change the call. The rectangle is caller-prepared, as the reducer contract
   states; the adapter does not compute maximized placement.
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
  NULL. It returns `Busy` and frees nothing when any entry has a saved fullscreen
  state. It makes no native call.
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

Linux double suites (`window_state_adapter_suite.c`), under
`-fsanitize=address,leak,undefined` with `ASAN_OPTIONS=detect_leaks=1`:

- Every action kind, with its exact native call sequence.
- Every failure point: each entry is made to fail at each of its calls in turn,
  for each action that uses it, and the completed count, the window state, and
  the saved state are checked.
- Stale handles: a destroyed window, and an HWND reused with another process or
  thread identity. Each drops the saved state and the entry.
- Fullscreen enter and leave round trips, including leave without saved state,
  enter twice, and failures between the style change and the placement change.
- Host-only and unknown kinds.
- Capacity (64 admissions, the 65th fails), duplicate admission, release with and
  without saved state, destroy with and without saved state.
- Threading: a call from another thread returns `WrongThread`; a call made during
  an entry returns `Reentrant`.

Integration (`window_state_adapter_integration.c`), with the real reducer through
`window_state_abi.h` and the double:

- Plan, execute, complete, observe, and then a GuestObservation plan that
  converges: after any partial failure, the reducer's applied guest state equals
  the adapter's observation.
- Random sequences of inputs with random native faults. After each step the
  invariant `applied guest state == observe()` holds whenever the machine is
  synchronized, and every native fault is reported by its status.

Native smoke (`tests/interop/window_state_adapter_native_windows.c`), cross-built
with mingw and executed only on Windows CI: see section 10.

Coverage: `tests/interop/window_state_adapter_coverage.py` runs the double suites
and the integration test under gcov and requires at least 90% line and at least
90% branch coverage of `src/interop/window_state_adapter_windows.c`. The `_WIN32`
block is preprocessed out on Linux and is not counted; the native smoke covers it
on Windows.

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
9. Stale handle: after the smoke destroys the window, `execute` on the same HWND
   returns `StaleHandle`, and a second window created afterwards with the same
   HWND value (if the system reuses it) also returns `StaleHandle` for the admitted
   identity. If the system does not reuse the value, the second window is not
   checked for reuse and the smoke reports that case as not exercised.

The smoke writes one line per step and exits nonzero on any mismatch.

## 11. Known limits

- The adapter cannot make Windows accept a foreground change; the activation
  fallback is the documented route and may still fail under foreground locks.
- Fullscreen is an adapter convention, not a Windows state. Another process that
  changes the stripped style or the placement is outside what the saved state
  can repair.
- The maximized flag of SetGeometry does not change the call (section 4, item 7).
