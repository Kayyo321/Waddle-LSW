# Read-only Win32 observer review

Reviewed the Claude-returned observer at worktree tip `3714cf2` in
`/home/dev/.codex/worktrees/seamless-window-graph/Waddle-LSW`. No checkout files,
build outputs, or runtime state were changed. The review covered
`src/interop/window_observer_windows.c/.h`, the API double and lifecycle tests,
and the open-finding text in `impl/seamless-interop/claude_window_graph/IMPL_DESC.md`
and `HANDOFF_RETURN.md`.

## Actionable finding

**O-OBS-1 — stop releases the process-wide singleton before its hook teardown is complete (lifecycle contract violation; high priority).**

At `src/interop/window_observer_windows.c:502`, `stop_locked` calls
`release_slot`; that function clears `g_active_observer` at lines 279–285 and
waits for the hint count to reach zero. Only after it returns does stop remove
the three WinEvent hooks at lines 504–508 and change observer state at lines
514–518. A second observer's `start_locked` can therefore win the NULL singleton
CAS at line 575 while the first observer is still `Started` and its hooks are
still installed.

Reproduction: create/start observer A on its owner thread; begin
`window_observer_stop(A)`; interleave `window_observer_start(B)` on B's owner
thread after A's compare-exchange clears the slot (the wait's `yield_thread()`
at line 283 is a practical interleave point); then allow A to continue. B can
return `Ok` before A has removed its hooks or changed state. During overlap both
sets of hooks are active, but every callback goes through the single global
`win_event_proc` and `window_observer_note_hint` path (lines 1360–1364, 720–728),
so A's still-installed hooks feed the active B observer. This contradicts the
header promise at `window_observer_windows.h:27–33` and the start contract at
lines 500–513 that only one observer may be Started at a time. It can also keep
A's process-wide in-flight wait busy under sustained B callbacks.

The observer API double tests waiting for one in-flight hint
(`tests/interop/window_observer_windows.c:2278–2292`) and tests one-observer
hook lifecycle, but has no second-observer start interleaved between singleton
release and hook removal. The lifecycle reservation needs to remain owned until
all hooks are removed and A is no longer Started; a global stop/start gate or an
equivalent stopping reservation would close this window.

## Unhook failure ownership

**O-OBS-2 — failed native unhook is an explicit process-lifetime leak, with a conditional module-unload hazard.**

`native_remove_hook` correctly returns failure when `UnhookWinEvent` returns
FALSE (`window_observer_windows.c:1379–1382`). However, stop records failure but
zeros every saved hook token regardless of the return value
(`window_observer_windows.c:504–508`). It then enters terminal Failed state
(`515–516`). Destroy of that Failed observer does not retry stop; it releases
the slot and frees the observer at `538–559`. The existing test explicitly
expects this behavior and status at `tests/interop/window_observer_windows.c:2731–2740`;
the header documents that destroy frees the owner while a hook is leaked until
process exit (`window_observer_windows.h:490–495`).

I do **not** see an immediate use-after-free of the observer object: the active
slot is cleared before freeing it, and the static callback carries no observer
context; later callbacks load a NULL slot and do not dereference the freed
observer. The concrete consequence is an unretryable native hook leak. If the
observer code is in a dynamically unloadable module, the still-registered
callback can outlive its function code and become a process crash; the API has
no module reference/lifetime contract that prevents unload. If this is always
linked into a process-lifetime executable, the documented leak-to-exit is a
known bounded-by-process-lifetime limitation rather than an object UAF. Keep
the module loaded until process exit after `UnhookFailed`, or change the
ownership/API to retain failed hook tokens and prevent destruction/unload until
unhook succeeds. The Linux API double cannot establish native failure semantics.

## Open findings: independent disposition

- **O5 — identical-tuple HWND reuse after a lost/dropped destroy hint:** confirmed
  as a real undetectable case under the current identity inputs. If the same HWND
  is reused with the same process/thread/start-time/class tuple and the destroy
  hint is lost (including table overflow), refresh produces identical facts and
  cannot infer a new generation. This is not repaired by the 250 ms refresh. It
  is an explicit design limitation, but a blocker if integration requires strict
  generation separation for every HWND incarnation. The observer has no
  independent Win32 generation source to fix it.
- **O24 — child-window event flood:** confirmed as a bounded-work/performance
  concern, not an export-correctness defect. The callback accepts every
  OBJID_WINDOW/CHILDID_SELF event; up to 512 distinct handles are queried, and
  overflow causes a full refresh rather than graph invalidation. Repeated
  distinct-child floods can repeatedly trigger up to the bounded enumeration
  work. Native load measurement is needed before treating the cost as acceptable;
  no native prefilter is implemented.
- **O25 — destroy timing and deferred-child residuals:** both stated consequences
  are plausible from the code/design. Pump applies a destroy before requerying
  (`window_observer_windows.c:893–918`); the observer samples the current tick
  before native query and has no after-query sample. Equal ticks or a destroy
  between the sample and read can therefore churn a live incarnation. Separately,
  the IMPL_DESC documents that graph flush after an IncarnationChanged admission
  can emit ghost deferred-child Announce/RetireIntent pairs until the next commit
  (IMPL_DESC lines 1296 and 1342). The text correctly bounds but does not eliminate
  these behaviors. They need explicit integration acceptance if transient stale
  child announcements can trigger host work.
- **O26 — `GetWindow(GW_OWNER)` last-error assumption:** the code clears last
  error before the call and treats NULL plus nonzero last error as Failed
  (`window_observer_windows.c:1772–1775`). The documented pattern is defensible:
  Microsoft documents NULL for “no related window” and directs callers to
  GetLastError for extended error information
  ([GetWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindow)).
  The API documentation does not fully establish the no-owner last-error value
  on every supported Windows build, so this remains a native smoke-test item,
  not a demonstrated source defect.

## Other native/lifetime observations and limits

The in-flight hint protocol itself is sound for a single observer lifecycle:
`window_observer_note_hint` increments before loading the active pointer and
decrements after use (`720–728`); clearing the slot and waiting drains callers
that could have loaded the observer before free. The owner-thread check at
`666–670` avoids cross-thread writes to the dirty table. The busy guard covers
owner-thread reentry through observer entry points; callback delivery on the
installing thread is consistent with OUTOFCONTEXT hooks and the required message
loop. These protections do not close the separate-instance stop/start window
above.

The source's native checks are not native acceptance evidence. The return report
says the Win32 section was cross-compiled/linked but never executed. Windows
runner acceptance is still required for real hook install/remove failure and
callback timing, DPI-context resolution, input desktop/UAC transitions, process
query and Toolhelp fallback errors, title/class/owner API behavior, and the
tick/event-time base. In particular, test the O26 no-owner case on supported
Windows versions and exercise HWND destroy/reuse under load. Native API doubles
can validate control flow but cannot establish these OS behaviors.
