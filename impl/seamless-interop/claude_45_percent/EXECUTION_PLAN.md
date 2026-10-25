# Seamless interop: 45 percent execution plan and acceptance ledger

Branch `feat/seamless-45-percent`, worktree `/home/dev/worktrees/seamless-45`.
Baseline `origin/feat/seamless-interop` = `f5178d3` (verified to contain the
30 percent checkpoint; the remote had not advanced). Accepted completion at the
baseline is **30.95%** with the 34 parent weights unchanged. Overall completion
is `sum(weight x progress / 100)`. This plan and ledger earn **no progress**;
credit is recorded in `TRACKER.md` only after the implementation and the
acceptance gates named here pass, each attributed to one atomic commit.

Target: at least 45.00%, i.e. at least **14.05 points** above the baseline.

## Rules applied to every allocation

1. A subtask earns credit only for an implemented runtime path whose acceptance
   gates (below) passed on the exact commit. Disconnected helpers, stubs or
   planning documents earn nothing.
2. Every allocation inside a parent is a percentage of that parent's own
   weight; parent weights never change. Points = parent weight x subtask %.
3. A native, installed-session, compositor, hardware or comprehensive-crash
   qualification line is **not earnable here** unless actually demonstrated and
   stays open. Portable fixtures (fake Wayland proxies, fake Win32 API, real
   AF_UNIX sockets, rooted temp directories) never earn native credit.
4. No double counting: each behavior is credited under exactly one subtask. The
   cross-references below name the owner.
5. Gates: every new Zig module >= 90% production line and taken-arc coverage
   via `tests/av/coverage.py` (markers in `tests/av/coverage_modes/`) with the
   Zig 0.13 testing allocator and allocation-failure sweeps; every new C module
   >= 90% line and branch via gcov with a dedicated coverage script; applicable
   C suites under ASan/LSan/UBSan with `detect_leaks=1`; Windows-only code
   through the mingw cross build and the `windows-2022` jobs in
   `.github/workflows`. No thresholds weakened, no new exclusions.
6. Zig sanitizer execution is never claimed; Zig ownership is proven by
   `std.testing.allocator`. Windows-provider code gets Windows functionality and
   coverage evidence, not Linux sanitizers.

## Detailed allocations for previously unsplit tasks

### #11 Host toplevel/popup/parent mapping (weight 4%, was Pending/unsplit)

| ID | Subtask | % | Pts | Notes |
|---|---|---|---|---|
| 11a | Pure bounded `(window, generation)` mapping table: parent-before-child, deferred children (64), stale-generation rejection, parent-retire cascade | 15 | 0.60 | Zig, no Wayland |
| 11b | Toplevel/dialog realization: `waddle.<dev>.<app_key>` app_id once, title on change only, `set_parent`, `set_min_size` ceil(value/scale) | 15 | 0.60 | C over Wayland ops seam |
| 11c | Pure positioner geometry (anchor, gravity, constraint adjustment, offsets) after the contract amendment in IMPL_DESC | 10 | 0.40 | amendment lands first |
| 11d | Popup lifecycle: positioner, `get_popup`, grab only with a live trigger serial, ungrabbed fallback, `popup_done`, reposition/dismiss events | 15 | 0.60 | |
| 11e | Decoration negotiation for toplevel/dialog only; popups excluded; late configure handling | 10 | 0.40 | |
| 11f | Teardown order decoration, popup, toplevel, xdg_surface, wl_surface; stale callbacks are no-ops | 10 | 0.40 | |
| 11g | Codec-to-mapping fixtures over `WindowAnnounce/Update/Retire/Configure/PopupRepositioned/PopupDismissed` | 5 | 0.20 | |
| 11h | Coverage and sanitizer gates for the new module | 5 | 0.20 | |
| 11i | GNOME and KDE (and Sway) real-compositor acceptance | 10 | open | native, not earnable |
| 11j | Live app_id/desktop-entry association with running windows | 5 | open | shares #28 native; not earnable |

Earnable: 85% = **3.40**.

### #29 `waddle open` (weight 3%, was Pending/unsplit)

| ID | Subtask | % | Pts |
|---|---|---|---|
| 29a | Frozen grammar parser, classification via `pe_header`, exit codes | 12 | 0.36 |
| 29b | Canonicalize, device mount translation, quoting, CUI delegation decision | 8 | 0.24 |
| 29c | `LocalLaunch` request/response handling on the session listener (listener itself is #7 local control) | 10 | 0.30 |
| 29d | CLI auto-spawn (setsid), 120 s retry, 1.5 s start notice | 8 | 0.24 |
| 29e | Activation token capture and the 30 s `(token, process_id)` store | 8 | 0.24 |
| 29f | Host `LaunchRequest`/`LaunchResult` dispatch and correlation on the session | 12 | 0.36 |
| 29g | File Explorer seed and Seed/System flags in catalog candidates | 6 | 0.18 |
| 29h | End-to-end fake-guest fixtures: cold, warm, timeout, exit codes | 10 | 0.30 |
| 29i | Guest ShellExecute/ActivateApplication/msiexec/CreateProcess handler | 10 | open (native) |
| 29j | `xdg_activation_v1.activate` on first commit | 6 | open (needs #11 native) |
| 29k | Native cold/warm launch and Explorer on GNOME/KDE | 10 | open (native) |

Earnable: 74% = **2.22**.

### #33 commands (weight 3%): split of the existing "commands/all producers 20%"

| ID | Subtask | % | Pts |
|---|---|---|---|
| 33a | install/remove/repair/status/migrate over the rooted engine for the existing desktop-entry/menu producer; modified-file preservation, retained backups, explicit restore, retryable recovery | 12 | 0.36 |
| 33b | All other producers (autostart, MIME defaults, shortcuts, guest policies, binfmt) integrated | 8 | open |

The "all producers" milestone is **not claimed**. Comprehensive crash/native/
full-removal 15% also stays open.

### #31 MIME handler (weight 2%): existing 40% accepted; remaining split

| ID | Subtask | % | Pts |
|---|---|---|---|
| 31a | Journaled handler artifacts (handler desktop entry + MIME defaults through the manifest engine) | 20 | 0.40 |
| 31b | `exe_handler` configuration and route integration into `waddle open` | 20 | 0.40 |
| 31c | Integrated/native qualification | 20 | open |

### #16 Pointer (weight 3%, Pending, **reserve** lane)

| ID | Subtask | % | Pts |
|---|---|---|---|
| 16a | Typed XBUTTON/value120/scroll translation and guest injection with fake SendInput | 30 | 0.90 |
| 16b | Activating click, cursor shape/image sync, native | 70 | open |

Reserve only; earned only if the main lanes fall short.

## Existing allocations targeted (from TRACKER subtask tables)

| Task | Subtask | % | Pts | Evidence required |
|---|---|---|---|---|
| #10 (4%) | C1 coordinator: observer pump to staged graph events, admission and retire barriers | 8 | 0.32 | C fixture, gcov >= 90%, ASan/LSan |
| #10 | C2 slot admission/publish seam with a fake capture | 6 | 0.24 | same |
| #10 | C3 wire encode of Announce/Update/Retire/FrameReady | 6 | 0.24 | same |
| #7 (4%) | Local session command control (`session.sock`, peer credentials, `LocalStop`) | 5 | 0.20 | socketpair/real unix fixture |
| #7 | AV/Wayland module boundary (shared Wayland binding module used by AV and interop) | 20 | 0.80 | `av-test`, `av-sanitizers`, new gcov mode |
| #7 | Reconnect (re-admit with 250 ms to 4 s schedule) 10% and autostart entry 5% | 15 | 0.60 | fixtures |
| #13 (3%) | Native adapters (ShowWindow/SetWindowPos/PostMessage) behind a C ABI of the accepted reducer | 25 | 0.75 | windows-2022 run plus fake-API unit tests |
| #13 | Wire/compositor: configure/state-request messages to reducer and host xdg state calls | 15 | 0.45 | fake compositor |
| #15 (3%) | Typed injection 15, live repeat 10, lock sync 15, layout sync 20, shortcut inhibit 10 | 70 | 2.10 | see lane K |
| #27 (4%) | Watch/enrichment 10 (portable share of 15), wire/runtime 8 (portable share of 10), catalog-to-manifest integration 4 | 22 | 0.88 | fixtures |
| #28 (4%) | Runtime/association 20, portable desktop mirror 7 | 27 | 1.08 | fixtures |
| #33 (3%) | see 33a | 12 | 0.36 | |
| #31 (2%) | see 31a/31b | 40 | 0.80 | |

## Ledger totals

| Source | Points |
|---|---|
| #11 | 3.40 |
| #29 | 2.22 |
| #15 | 2.10 |
| #7 | 1.60 |
| #28 | 1.08 |
| #13 | 1.20 |
| #27 | 0.88 |
| #10 | 0.80 |
| #31 | 0.80 |
| #33 | 0.36 |
| **Main lanes** | **14.44** |
| #16 reserve | 0.90 |
| **Ceiling with reserve** | **15.34** |

The main lanes exceed the required 14.05 by only 0.39 points. Any rejected
subtask is replaced from the reserve or the shortfall is reported honestly.

## Lanes, ownership and merge procedure

Each lane works in its own git worktree under `/home/dev/worktrees/` on a
branch `w/<lane>` cut from the lead branch, commits atomically, and never edits
files outside its ownership. The lead cherry-picks accepted commits onto
`feat/seamless-45-percent`, re-runs the gates, and is the only writer of
`TRACKER.md`, `IMPL_DESC.md`, the workflows and shared Makefiles. Lane Makefile
targets live in `scripts/interop_lane_<name>.mk`; Zig coverage modes register
as marker files in `tests/av/coverage_modes/`.

| Lane | Tasks | Exclusive files |
|---|---|---|
| K keyboard | #15 (#16 reserve) | `src/interop/keyboard_*.zig`, `src/av/av_input.c`, `src/av/av_windows.c` key paths, `src/av/av_wayland.c` keyboard code, `tests/interop/keyboard_*`, `tests/av/input*`, `scripts/interop_lane_keyboard.mk` |
| M manifest | #33a, #31 | `src/cli/integration_commands.zig`, `src/interop/manifest_store.zig`, `src/interop/mimeapps.zig`, `tests/interop/integration_*`, `scripts/interop_lane_manifest.mk` |
| W window map | #11 | `src/interop/host_window_map.zig`, `src/interop/host_wayland_windows.c/.h`, `tests/interop/host_window_*`, `scripts/interop_lane_window_map.mk` |
| L launch/catalog | #29, #27, #28 | `src/cli/open_command.zig`, `src/interop/catalog_runtime.zig`, `src/interop/launch_runtime.zig`, `tests/interop/open_*`, `tests/interop/catalog_runtime*`, `scripts/interop_lane_launch.mk` |
| C coordinator | #10, #13 | `src/interop/window_coordinator.c/.h`, `src/interop/window_state_abi.zig`, `src/interop/window_state_adapter_windows.c`, `tests/interop/window_coordinator*`, `scripts/interop_lane_coordinator.mk` |
| S session | #7 | `src/interop/session_control_host.c/.h`, `session_main.c`, `session_host.c`; sequenced after K finishes `av_wayland.c` |
| Lead | shared | tracker, docs, GNUmakefile, workflows, `src/cli/host.c` dispatch, feature-dispatch ABI |

Dependency order: K, M, W (pure parts), L (CLI/pure parts) and C start in
parallel. S and the shared feature-dispatch registration in
`session_control_host.c` follow, because #29f, #27 wire/runtime, #11g and #10 C3
register handlers there. The `av_wayland.c` boundary extraction (#7) waits for K
to merge its keyboard edits.

## Runners and resources

Shared `build/` is per worktree, so lanes do not collide. `make clean`,
`make test-sanitizers` and broad build-tree cleanup are never run; the main
checkout and its VM assets are not touched. Native execution is limited to the
GitHub `windows-2022` jobs; no local QEMU or swtpm is started for this
milestone. CI is triggered with `workflow_dispatch` against this branch.

## Review

Opus reviewers audit each merged lane for stale generations, disconnect/
reconnect, cancellation, partial publication and ownership before credit is
recorded. Findings are fixed in follow-up commits; credit that review
invalidates is removed from the tracker.
