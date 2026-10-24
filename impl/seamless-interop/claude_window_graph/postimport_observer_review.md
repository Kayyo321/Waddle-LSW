# Post-import Win32 observer review

Read-only review of `src/interop/window_observer_windows.c/.h`,
`tests/interop/window_observer_native_windows.c`, and
`impl/seamless-interop/claude_window_graph/IMPL_DESC.md`. No repository files,
builds, or native execution were performed. The known O5 discarded-destroy,
Starting-callback, and O25 descendant issues are excluded.

## Findings

### P2: Native fact reads can return a hybrid identity snapshot after HWND reuse

`native_query_facts_inner` captures `process_id` and `thread_id` once at
`src/interop/window_observer_windows.c:1855-1861`, then reads relations, styles,
visibility, geometry, DPI, class/title and process metadata through separate
Win32 calls. Its final identity/liveness guard at `:1927-1930` checks only
`IsWindow(handle)`. If process A destroys the HWND during the query and Windows
reuses that numeric HWND for process B, the final `IsWindow` can succeed while
`facts.process_id/thread_id` still identify A and class/rect/title (or other
window facts) now describe B. `native_fill_process(process_id, ...)` also queries
A's image/start time. The adapter then returns `Exists` and the graph may accept
the mixed snapshot, including classifying B using A's executable identity.

This is narrower than the documented O5 limit: it is detectable when the
recycled window belongs to a different process or thread. The final guard should
re-read `GetWindowThreadProcessId` and return `Failed` if either identity field
differs from the captured pair. This does not solve same-process/same-thread
reuse with an identical tuple, which remains the documented limitation.

### P2: A failed monitor-info query is treated as a known non-fullscreen fact

`native_is_fullscreen` at `src/interop/window_observer_windows.c:1778-1791`
returns `0` both when `MonitorFromWindow` finds no monitor and when it finds one
but `GetMonitorInfoW` fails (`:1783-1787`). The latter can occur if display
topology changes between those calls. `native_query_facts_inner` interprets
that `0` as a confirmed clear `InteropWindowFactFullscreen` bit and returns
`Exists` (`:1903-1910`, `:1930`), so the observer can publish a false state
update instead of preserving the prior snapshot on an indeterminate query.
Distinguish “no monitor covers this rect” from “monitor lookup failed”; the
second case should make the window query `Failed` (and therefore non-retiring,
per the existing query contract).

## Audited areas with no additional finding

- Process handles are closed on the image-buffer failure and normal paths in
  `native_fill_process` (`:1719-1743`); the optional long image-path allocation
  is freed before return (`:1682-1705`). Snapshot scopes close on the standalone
  query wrapper and the enclosing pump/refresh paths (`:1593-1607`, `:1049-1052`,
  `:1216-1229`, `:1942-1948`). The close-result values are ignored; I did not
  confirm a normal execution path that loses a valid handle as a result.
- `window_observer_stop` clears callback publication before unhooking, retains
  every failed hook token for retry, and only releases the lifecycle reservation
  when no hook remains (`:303-315`, `:525-541`). The native callback is delivered
  on the installing thread, and Microsoft documents that it is not called after
  `UnhookWinEvent` returns, so I found no old-hook callback path that can be
  attributed to a subsequently started observer. References:
  [SetWinEventHook](https://learn.microsoft.com/en-us/windows/desktop/api/winuser/nf-winuser-setwineventhook),
  [UnhookWinEvent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-unhookwinevent).
- The 32-bit HWND encoding is consistent with Microsoft’s documented user-object
  handle interoperability rule that only the lower 32 bits are significant:
  [Interprocess Communication](https://learn.microsoft.com/en-us/windows/win32/winprog64/interprocess-communication).
- The native smoke's cleanup attempts HWND destruction, retries observer
  destruction without discarding a retained owner, and restores the prior DPI
  context (`tests/interop/window_observer_native_windows.c:97-117`). Its current
  assertion checks the exact fixture HWND and Create/Show kind, rather than an
  aggregate counter.

## Qualification limits

The Windows-only fixture was not executed here. It covers an own-process,
ownerless HWND, so it does not exercise foreign-process identity changes,
Toolhelp fallback/refresh, monitor topology failure, DWM transitions, or handle
reuse during a query. No conclusions are made about those runtime frequencies.
