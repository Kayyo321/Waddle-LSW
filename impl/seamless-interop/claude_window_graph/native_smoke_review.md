# Native observer smoke review

## Finding: hook-delivery assertion can pass on an unrelated desktop event (P2)

`tests/interop/window_observer_native_windows.c:72-79` treats any increase in `after.hints_received` as proof that creating this test's HWND reached the WinEvent hook. The observer is desktop-wide (`SetWinEventHook` uses process/thread filters 0 in `src/interop/window_observer_windows.c:1391-1400`), and the counter records every accepted hint regardless of HWND/event (`src/interop/window_observer_windows.c:719-740`). Thus an unrelated window event on the Windows runner during the ten-second wait satisfies the check even if this HWND's create/show event was never delivered. The printed success claim at lines 94-95 inherits this ambiguity.

A stronger smoke should correlate a captured hint with `window_key` and one of the expected object create/show events, or constrain the observer callback/test seam so it can assert the target key. If the public observer deliberately exposes only aggregate counters, label this assertion as “some hook traffic received” and add a separate target-correlated native seam rather than claiming delivery for the created HWND.

## Scope and remaining limits

I found no concrete cleanup defect in the smoke's normal/failure paths: it attempts HWND destruction, retries observer destruction without freeing an observer whose hook teardown still fails, and restores the prior thread DPI context (`tests/interop/window_observer_native_windows.c:97-117`). The native query wrapper also gives direct native-table queries a temporary scan scope and closes that scope on every ordinary return (`src/interop/window_observer_windows.c:1942-1948`; scope close at 1593-1607); pump/refresh bracket their full call with the same scope (1049-1051, 1216-1229).

The native smoke exercises real Win32 ownerless facts, visibility, destroyed-HWND classification, hook setup/traffic, and observer teardown. It does not exercise Toolhelp fallback/second-snapshot behavior: its HWND belongs to the current process, for which the normal `OpenProcess` path is expected to succeed (`src/interop/window_observer_windows.c:1719-1743`). The make target cross-compiles the native test on Linux, and the separate `windows-2022` job executes it (`scripts/interop_window_graph.mk:177-179`, `.github/workflows/interop_window_graph.yml:41-56`); this is real Windows runtime evidence, but no ASan/LSan instrumentation is applied to that Windows executable, and the Linux sanitizer targets cover only the portable/double observer path (`scripts/interop_window_graph.mk:136-141`).
