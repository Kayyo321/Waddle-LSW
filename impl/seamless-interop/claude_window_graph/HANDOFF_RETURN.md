# Claude Code return: guest window graph

## Assignment status

- Status: **Partial** for global TODO #10 (this slice is complete as code plus Linux/API-double evidence; nothing was executed on Windows). Task-local tracker completion is 92.25%; the remaining 7.75% is native Windows execution and Codex review, which this session cannot provide.
- Base: e86cf8e344c6bafea5e562f04129a408e19629ff
- Worktree: /home/dev/.codex/worktrees/seamless-window-graph/Waddle-LSW
- Branch: feat/seamless-window-graph
- Returned HEAD: see "Commits and exact files" (the final HEAD is the commit whose message starts `docs(interop): record the window graph handoff return and final tracker`; `git rev-parse feat/seamless-window-graph` on the worktree gives its full SHA; the last verified code HEAD before the return/tracker commits is `0c7127521deba72571eaf8707f1c779aa7ed7346`)
- Working tree: clean at the time of writing (`git status --short` empty). Generated output lives only in the gitignored `build/` directory.
- File ownership: **explicitly released to Codex.** No sub-agent or process of this session is running or will edit anything after this commit.

## Concrete delivered behavior

A bounded, session-wide guest window graph and its Win32 observation adapter, as three layers:

1. **Zig engine** (`src/interop/window_graph.zig`): validates portable window facts, classifies windows with the exact IMPL_DESC 4.4 precedence (all UAC modes, shell-host-specific CoreWindow exclusion, preedit exception, NOACTIVATE binding to the Menu style clause only, owned+APPWINDOW, disabled windows kept, zero-area/alpha-0 exclusion), assigns monotonic `(session_epoch, window, generation)` identities (generation starts at 1, never reused, never wraps; exhaustion is an explicit error), keeps up to 64 exported entries (Active + Retiring quarantine) and 64 deferred records (oldest evicted), announces parents before children, retires children first, quarantines retirements until the coordinator proves capture stopped and pins drained, emits bounded value events (Announce / Update / RetireIntent) through a 256-event ring with in-place coalescing, supports refresh (begin/offer/keep/unseen/commit/abort) and `invalidate_all`, rate-limits capacity diagnostics to once per 60 s with an injectable clock, and unwraps 32-bit ticks against a 64-bit base estimated from the graph clock.
2. **C ABI** (`include/waddle/interop_window_graph.h`, `src/interop/window_graph_abi.zig`): 21 `interop_window_graph_*` exports (symbol list is checked against the header by `window-graph-symbols`), opaque handle, one heap allocation freed by `destroy`, every C pointer validated before use, outputs staged and copied only on documented statuses.
3. **Win32 observer** (`src/interop/window_observer_windows.[ch]`): out-of-context WinEvent hooks (three ranges including cloak and parent change) are hints only; they update a bounded 512-key coalescing table and nothing else. The owner thread pops hints, re-queries current facts through an injectable `window_observer_api_t`, and feeds the graph. It implements the refresh/keep/unseen algorithm, the 250 ms cadence with a once-per-latch-event rule, the desktop-inactive suspension, overflow recovery by a full refresh, an owner sub-query that sets `OwnerHidden`, a busy/Reentrant guard, and a native `_WIN32` section (cross-compiled only).

Behaviours worth knowing before integrating (details: IMPL_DESC sections 4, 6, 9):
- **OwnerHidden promotion is a refinement of 4.4** that needs Codex sign-off (a window whose owner root can never be exported is treated as unowned; otherwise Java/VCL/hidden-owner dialogs would be deferred forever). The one-line disable is: the observer never sets the flag.
- Reclassification and parent change always retire + announce a new generation (no parent in `WindowUpdate`).
- A refresh marks every *offered* window seen whatever the status; failed queries are kept (`refresh_keep`), unseen Active windows are re-queried individually before commit, so a window that fails forever or is not enumerated by `EnumWindows` is not retired and cannot starve the windows behind it.
- Overflow of the hint table now causes one full refresh, **not** `invalidate_all` (child-window event floods otherwise churn every exported window). Cost: a destroy+recreate of an HWND with an identical identity tuple whose destroy hint was lost in an overflow stays undetected (documented residual risk O5).
- The destroy-hint rule narrows but does not literally prove "a delayed destroy cannot retire a newer live object": equal ticks or a destroy between the tick sample and the read can retire-and-readmit a newer incarnation (churn, never permanent loss). Codex must accept this (O25, decision 24).

## Commits and exact files

All 12 owned paths were created in this session; no other file changed (`git diff --name-status e86cf8e..HEAD` shows only the owned paths plus Codex's handoff documents from the base).

| Commit | Files | Behavior / task-local progress |
|---|---|---|
| da898fc | include/waddle/interop_window_graph.h | Public C ABI header (structs, enums, prototypes, static asserts) |
| fe1e5c4 | src/interop/window_observer_windows.h | Observer API header |
| df749f8 | src/interop/window_graph.zig | Pure validation + classification layer, 18 tests |
| ec5ffef | impl/.../claude_window_graph/IMPL_DESC.md | Detailed task-local design |
| b583c90 | src/interop/window_observer_windows.c | Observer implementation (portable + native) |
| 059d02c | src/interop/window_graph.zig | Graph state, ring, quarantine, diagnostics, 47 tests |
| 30620cb | .../TRACKER.md | Tracker log |
| 2831282 | include/waddle/interop_window_graph.h | ABI amended after Opus design review |
| 467bbba | src/interop/window_observer_windows.h | Observer API amended after design review |
| 29fbfb7 | src/interop/window_observer_windows.c | Review fixes: refresh keep/unseen, busy flag, latch, 3 hooks, OwnerHidden |
| 1aab2e2 | src/interop/window_graph.zig | Planning engine (observe, note_destroyed, retire cascade, deferral), 109 tests |
| 5d0c7ce | .../IMPL_DESC.md | Design aligned with reviewed headers |
| c496c2b | src/interop/window_graph.zig | Refresh, keep/unseen, invalidate_all, 127 tests |
| 6dd8aa3 | src/interop/window_graph_abi.zig | Zig C ABI exports + 14 tests |
| 02ee43c | tests/interop/window_graph.c | C ABI test, 26 tests incl. 24000-op random trace |
| 65b2032 | src/interop/window_observer_windows.c | Latch consumed once per event even with a frozen clock |
| 0ecc2e6 | tests/interop/window_observer_api_double.h, window_observer_windows.c | API double + 40 observer tests |
| d267b49 | scripts/interop_window_graph.mk | Standalone targets |
| f7eb737 | tests/interop/window_observer_windows.c, window_observer_api_double.h | Observer coverage tests (53) |
| 8af4d5c | src/interop/window_graph.zig | Review fixes R3 (deferred cycle through leftovers) and R4 (tick estimate after silence), 136 tests |
| 9a9ff8a | include/waddle/interop_window_graph.h | Header text for the tick estimate |
| 09bc75e | src/interop/window_observer_windows.[ch] | Review fixes R1/R5-R8/R11: overflow=refresh, truncated enumeration, yielding wait, reduced hidden-window query |
| 00a84e4 | tests/interop/window_observer_windows.c, window_observer_api_double.h | Flood, fuzz (convergence), calloc-failure, in-flight tests; 63 tests |
| 221fb74 | scripts/interop_window_graph.mk | ReleaseSafe runs, symbol check, calloc wrap |
| 0c71275 | .../IMPL_DESC.md | Design aligned with review results; open findings O24-O26 |
| (final) | .../HANDOFF_RETURN.md, .../TRACKER.md | This return and the final tracker |

Untracked/generated files: none outside `build/` (gitignored). To recover everything: `git log e86cf8e..feat/seamless-window-graph`; every commit is self-contained and builds with the standalone makefile.

## API, memory and concurrency

- **Graph**: `interop_window_graph_create(config, &graph)` (nonzero `session_epoch`, `uac_mode` 0..2, required `app_key_lookup`, optional `now_ms`), `destroy(&graph)`; `observe`, `note_destroyed`, `refresh_begin/offer/keep/unseen/commit/abort`, `invalidate_all`, `complete_retire`, `check_action`, `next_event`, `drain`, `take_capacity_diagnostic`, `get_stats`, `stamp_high_water`, `find_live`, `list_retiring`, and the pure `classify`. Full per-status behaviour: header comments and IMPL_DESC 6.1.
- **ABI sizes**: facts 1656, event 1328, observation 56, live 48, retiring entry 24, proof 8, diagnostic 56, stats 256, config 48, verdict 24 (all static-asserted in C and compile-time-asserted in Zig). Wire/layout structs, codec and `interop_protocol.h` are untouched.
- **Ownership**: the graph is one heap block (about 504 KiB, compile-time asserted at most 524288 bytes; IMPL_DESC 3.2) owned by create/destroy; events/facts are value copies; callbacks (`app_key_lookup`, drain sink, `now_ms`) receive borrowed pointers and must not retain them or call into the graph (Reentrant). The observer is one calloc block owned by create/destroy. Neither frees heap runs, unmaps memory or produces frame-drain proof.
- **Threading**: both objects are bound to their creating thread; other threads get WrongThread with nothing touched. The only cross-thread entry is `window_observer_note_hint` (wrong-thread hints only bump an atomic counter). An in-flight counter makes stop/destroy wait (with yield) for an off-thread hint before freeing.
- **Identity/epoch**: every mutating call takes the captured `session_epoch`; a mismatch is `StaleEpoch` (only a counter changes). A late completion for an old epoch or generation cannot touch a newer entry (`complete_retire` matches the exact `(window, generation)`).
- **Capture-drain boundary**: `RetireIntent` blocks new actions and quarantines the entry; only `complete_retire(proof{capture_stopped=1,pins_drained=1})` ends it. No API treats a lost connection as proof. Before destroying the graph the coordinator must call `list_retiring` and hand the pairs to the slot runtime.
- **Failure outputs**: documented per status; on every non-Ok/None status all state and outputs are unchanged except diagnostic counters and the documented exceptions (Cycle drops a stale deferred record; SinkFailed keeps earlier consumed events; forced retirement returns Ok/Retired).

## Verification actually performed

All commands run from the worktree root with `PATH=$HOME/.local/bin:$PATH PYTHONDONTWRITEBYTECODE=1` (the makefile also sets `ZIG_LOCAL_CACHE_DIR`/`ZIG_GLOBAL_CACHE_DIR` under `build/window_graph/`). Toolchain: zig 0.13.0, gcc 14.2, clang-19.1.7 (coverage tool), mingw-w64 gcc 14, Linux x86_64.

| Command | cwd / relevant environment | Exit | Evidence artifact |
|---|---|---|---|
| `make -f scripts/interop_window_graph.mk window-graph-all` (test, symbols, sanitizers, coverage, windows in that order) | worktree root; no main-checkout build used | 0 | `build/window_graph/all_run.log` (sha256 prefix 8ec1c4079f196b23e61e) |
| `... window-graph-test` | as above | 0 | included in the log above: engine 136, ABI module 150, C ABI 26, observer 63 tests passed; also `-O ReleaseSafe` runs |
| `... window-graph-sanitizers` | `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1`; gcc `-fsanitize=address,leak,undefined` | 0 | same log |
| `... window-graph-coverage` | PYTHONPATH=tests/av; sed copy of tests/av/coverage.py with the two new modes (tracked script unchanged) | 0 | same log; Zig reports under `build/coverage/interop/window_graph*/run-*` |
| `... window-graph-symbols` | `nm` on `build/window_graph/window_graph_abi.o` | 0 | exactly 21 T symbols equal to the header |
| `... window-graph-windows` | `zig cc -target x86_64-windows-gnu` and `x86_64-w64-mingw32-gcc` | 0 | **cross-compiled and linked only; never executed** |

Earlier gate logs (same targets, earlier code): `build/window_graph/{test_run,san_run,win_run,cov_run,cov_run2}.log`. The two Opus review reports are session-local scratch files: `design_review.md` (pre-implementation) and `code_review.md` (post-implementation) under the session scratchpad; their findings and dispositions are summarised below and in IMPL_DESC 9.

- Production coverage (denominator = functions defined above `// Test-only fixtures.`; compiler panic/stack-canary guard branches are excluded by the repo tool and counted in its output): `window_graph.zig` line 764/765 (99.87%), branch 505/514 (98.25%); `window_graph_abi.zig` line 141/142 (99.30%), branch 56/58 (96.55%); portable part of `window_observer_windows.c` (gcov, `-O0`) line 614/633 (97.0%), branch 350/374 (93.6%).
- Exclusions: the `#ifdef _WIN32` native section of the observer (not compiled on Linux, so not in the gcov denominator); Zig test code; the 25 observer branches that need saturated counters, a concurrent hint thread in specific interleavings or defensive full/empty-table paths (listed in the message of commit f7eb737 and in IMPL_DESC 7.5).
- Sanitizers/allocator: C tests (graph ABI, observer) under ASan+LSan+UBSan with leak detection enabled; zero reported. Zig tests use `std.testing.allocator` and a `FailingAllocator` fail_index sweep for create (zero bytes leaked); the observer calloc-failure path is tested through a Linux-only `--wrap=calloc` seam.
- Stress: engine seeded random traces total 210,000 steps with an invariant checker after every step and an event-order consumer model; C ABI random trace 24,000 operations; observer convergence fuzzers 40 seeds x 3000 steps plus 8 stressed seeds x 3000 steps (144,000 steps in total across the two fuzz tests).
- Windows: **cross-compiled and linked only** (`zig cc` and mingw, flags `-U_WIN32_WINNT -D_WIN32_WINNT=0x0A00 -UWINVER -DWINVER=0x0A00 -DUNICODE -D_UNICODE`, `-ldwmapi`). No API-double test was run on Windows either.
- Native VM/GPU/compositor use: none. No VM, QEMU, GPU, compositor, submodule update, or main-checkout build was used. API doubles are not real-session or desktop acceptance; nothing claims UIAccess or secure-desktop proof.

## Failed attempts and open findings

Sub-agent stalls and re-dispatches (process only): a single-agent IMPL_DESC attempt stalled and was split into three parts; an amendment pass stalled on an oversized read and was re-run with chunked reads. No code was lost.

Findings fixed (design review, then code review): hidden/never-exported owners no longer defer forever (OwnerHidden); refresh no longer retires live windows that fail a query or are not enumerated and cannot starve admissions; refresh latch spin and same-millisecond latch loss; 32-bit tick wrap and long-silence tick repair; deferred-record reuse across incarnations; observer re-entrancy (busy flag, no messages to own-process windows); UAC image-name fallback via Toolhelp; IME preedit parent; deferred cycles (including through capacity-cut leftovers); child-window hint floods churning every window; truncated enumeration; use-after-free guard test for the in-flight counter.

Open (full text IMPL_DESC 9.2, O1-O26); the ones Codex must decide or confirm natively:
1. **OwnerHidden promotion** needs Codex sign-off as a refinement of 4.4.
2. **Destroy rule** narrows, not literally proves, the "cannot retire a newer live object" claim (O25); equal-tick churn rate and the GetTickCount-vs-dwmsEventTime base must be measured natively (8.6 item 4).
3. **Residual identical-tuple HWND recycling** with a lost destroy hint (now also after overflow, O5) is undetectable.
4. **Native-mode UAC window handle** is only counted (`uac_native_hidden`), not exposed to the coordinator.
5. **Quarantine can pin capacity** if the coordinator never calls `complete_retire`.
6. **Child-window hint flood** is bounded but not pre-filtered natively (O24); `GetWindow(GW_OWNER)` last-error assumption needs native confirmation (O26); the reduced hidden-window query, stack-then-heap image path, Toolhelp fallback, DPI-awareness check and `GetDpiForWindow` resolution are compile-checked only.
7. Min size (`WM_GETMINMAXINFO`) is never queried (always 0); per-pixel/click-through overlays are exported as opaque (O20); known-vs-0 `process_start_time` flips churn (O21); refresh cost needs measurement (O22); app key is never re-looked-up; Dialog/Toplevel may get a popup-role parent.
8. Test evidence **not provided**: separate reference-model file, replay hash, `alc_operations_do_not_allocate`, leak-probe control, observer counter-saturation in C (listed in IMPL_DESC 7.5).

## Codex integration checklist

1. Add the standalone targets to the real build (do not copy flags blindly): `include/waddle/interop_window_graph.h`, `src/interop/window_graph_abi.zig` (-> `window_graph_abi.o`, Windows `.lib`), `src/interop/window_observer_windows.c`; include paths `-Iinclude -Isrc/interop`; Windows compile defines above and `-ldwmapi`; `scripts/interop_window_graph.mk` was intentionally **not** included by `GNUmakefile`/`scripts/interop.mk`. No submodule was added.
2. Guest session main loop order (IMPL_DESC 8.2-8.4): graph create (epoch from Hello, `uac_mode` from config, `app_key_lookup`, optional clock) -> observer create with `first_stamp` above `interop_window_graph_stamp_high_water` and the *same* `uac_mode`, own pid, `preedit_window` -> start -> loop: message pump -> `window_observer_pump` -> `drain` events to the wire encoder (Announce/Update/RetireIntent only; `FrameReady` and wire serialization remain yours) -> on RetireIntent stop capture, drain pins, send `WindowRetire` when slots are released, then `complete_retire` -> every 250 ms `window_observer_refresh_due`/`refresh`. After `GraphQueueFull`, call pump/refresh again only after a drain delivered at least one event (else wait for transport writability).
3. Owner thread must run a message loop and be `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` (observer `create` returns `Unsupported` otherwise) on Windows 10 1607+.
4. Shutdown/transport reset: observer stop -> `list_retiring` hand-off to the slot runtime -> graph destroy -> observer destroy; a new epoch means a new graph and a new observer; stale callbacks from the old epoch are rejected by epoch.
5. Required native-runner evidence before awarding global #10 credit (IMPL_DESC 8.6): run the double-driven suites on Windows (`window_graph_test.exe`, `window_observer_test.exe` from `window-graph-windows`), then installed-agent qualification: classification fixtures (toplevel, owned dialog, `#32768` menu, tooltip, tool window, consent.exe in all three UAC modes), delayed destroy + HWND recycle, hidden/cloaked windows, `EnumWindows` vs hint-only windows, desktop switch, hook teardown, access-denied image names, tick-base check.
6. Preserved versions: protocol v2, layout v3, manifest schema 1, codec, `interop_protocol.h` unchanged. Remaining global #10 work: session dispatch, capture/slot integration, wire serialization of the events, native installed-agent qualification, UAC native pipeline using the hidden consent window, min-size query.

## Recovery and handback

- Rollback: `git revert` is not needed; Codex can cherry-pick selected commits from `git log e86cf8e..feat/seamless-window-graph` (each is atomic and listed above) or take the final tree for the 12 owned paths. All commits are on the isolated branch; nothing was pushed, merged or rebased.
- Owned processes: all sub-agents (research, implementation, two Opus reviewers) finished or were stopped; no background job, hook, VM or server from this session is running. Throwaway harnesses live only in the session scratchpad, never in the repository.
- Not touched: `README.md`, `GNUmakefile`, `scripts/interop.mk`, `tests/av/coverage.py` (a sed copy is generated under `build/`), the global TRACKER/IMPL_DESC, shared wire/layout/codec headers, CLI/daemon/common paths, submodules, legacy `src/av`, the main checkout `/home/dev/Waddle-LSW`. No secrets, credentials or environment dumps are in this document.
- **Ownership is explicitly released to Codex.** I will not edit these files again while Codex reviews this revision.
