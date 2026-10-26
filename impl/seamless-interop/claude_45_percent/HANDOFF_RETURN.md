# Handoff: seamless-interop 45 percent attempt (stopped at 36.79%)

**The 45% target was not reached.** Accepted completion is **36.79%** (baseline 30.95%,
+5.84 points). The user paused the session on 2026-10-26; this is a recoverable stopping
point, not a completed milestone. The feature stays `In Progress`, `Time Ended: TBD`;
no PR or merge is eligible.

## 1. Git state

| Item | Value |
|---|---|
| Branch | `feat/seamless-45-percent` (pushed, no force) |
| Worktree | `/home/dev/worktrees/seamless-45` |
| Baseline | `origin/feat/seamless-interop` = `f5178d3` (verified; the remote had not advanced) |
| Final code revision | see section 5 (the last code commit before the docs-only commits) |
| Working tree | clean except untracked `.zig-cache/` (ignored scratch) |
| Lane branches (local worktrees `/home/dev/worktrees/w-*`) | `w/keyboard` `w/manifest` `w/window_map` `w/launch` `w/session` `w/peer` are fully merged into the head. **Not merged**: `w/coordinator` (head `fc3155f2`, 13 commits ahead) and `w/catalog` (head `7407c8dc`, 8 commits ahead). Both are pushed to `origin` as `seamless-45-lanes/coordinator` and `seamless-45-lanes/catalog`. |

Lane worktrees are scratch checkouts; their branches share the main repository's object
store (refs only, no checkout change). Remove with `git worktree remove` when done.

## 2. Completion

Starting 30.95%, final **36.79%**. Per-task increments (commit attribution is in
[TRACKER.md](../TRACKER.md) "45 percent milestone"):

| Task | Before | After | Increment | What earned it |
|---|---|---|---|---|
| #11 | 0% | 80% | +3.20 | mapping table, positioner, xdg toplevel/dialog/popup realization, decoration, teardown order, gates |
| #13 | 50% | 75% | +0.75 | native Win32 adapter, real-desktop CI run |
| #15 | 20% | 55% | +1.05 | live host repeat (AV path), pure lock/layout/inhibit reducers |
| #29 | 0% | 28% | +0.84 | parser/classification, translation/quoting, token store |

Calculation: `sum(weight x progress / 100)` over the 34 unchanged weights = 36.79.

### Implemented but NOT credited (what accepting each would add)

| Slice | Points | Why not credited | Where |
|---|---|---|---|
| #15 typed Windows injection 15% | 0.45 | `av_passthrough` CI at the final head not confirmed at write time (see section 5); first-pass review found the forms sound | merged (`8dd7392b`) |
| #15 live lock 7% / layout 8% / inhibit 5% (interop input handler + guest executor) | 0.60 | handler not registered in `waddle-session`; second pass TODO | merged (`session_input_handler.c`, `guest_input.c`) |
| #16 16a XBUTTON/value120 30% | 0.90 | never reviewed | merged (`b4d437c`, `371cd3d`, `d361b8f`) |
| #7 local control 5% + reconnect 10% | 0.60 | second pass TODO (client exit 75 handling unconfirmed) | merged (`session_local_handler.c`, supervisor) |
| #11 11g handler wiring 5% | 0.20 | handler unreviewed and not registered | merged (`session_windows_handler.c`) |
| #29 29c/29d/29f/29h/29g/29i | up to 1.68 | second pass TODO; 29i has a real-desktop run (guest_launch, unreviewed) | merged |
| #33 33a commands 12% + #31 31a 20% | 0.76 | CLI unreviewed; store fixes reviewed | merged (`integration_commands.zig`, store) |
| #10 coordinator 20% | 0.80 | five High review findings fixed only on the lane branch; gate 88.78% < 90% | `seamless-45-lanes/coordinator` |
| #27/#28 catalog runtime | ~1.9 | four High findings fixed only on the lane branch; arc gate 87.17% < 90% | `seamless-45-lanes/catalog` |

Accepting all of the above would give about 44.7% - still under 45%. Native, installed-
session, compositor, hardware and crash qualification lines are untouched and open.

## 3. Concrete runtime behavior delivered

* `waddle open [...]` client: grammar, PE classification, share translation, quoting, token
  capture, cold-start spawn of `waddle-session` with retry/notice/fast-fail, LocalLaunch over
  `session.sock`; `waddle session stop` client; `waddle integration install|remove|repair|status|migrate`
  for the handler desktop entry and MIME default (journaled, rooted, restoring prior values).
* `waddle-session` (default supervised): host feature runtime (ops table, sender, request ids,
  capability map per IMPL 8.45), supervisor with backoff, builtin registry with the launch handler,
  `session.sock` listener, LocalStop with the reply after lock release, LaunchRequest/Result correlation.
* Host xdg-shell realization of interop windows (toplevel/dialog/popup, grab rules, decoration,
  reposition, teardown order) with strict protocol-double tests; host key repeat and XBUTTON/value120
  on the live AV path; interop host input handler and guest executors for input, window state and launch.
* **Not wired into the shipped binary**: windows handler, input handler, catalog handler (only
  the launch handler is registered).

## 4. Evidence and reproduction

Run from `/home/dev/worktrees/seamless-45` (`export ZIG_LOCAL_CACHE_DIR=<scratch>` if Zig reports
`FileNotFound` on C imports; never `make clean` / `make test-sanitizers`):

```
make interop-test                         # aggregate; was green except the catalog gate before its revert
make interop-host-window-map-test interop-host-wayland-windows-test interop-host-wayland-windows-coverage
make interop-keyboard-lanes-test interop-keyboard-lanes-coverage av-test
make interop-open-e2e-test interop-session-local-e2e-test
make interop-window-state-adapter-test interop-window-state-adapter-coverage
make interop-guest-launch-test interop-guest-launch-coverage
```

Last measured module gates (line / branch): host_window_map 100 / 94.5; host_popup_positioner 100 / 100;
host_wayland_windows 100 / 96.2; keyboard_lock 100 / 96.6; keyboard_layouts 100 / 96.3;
keyboard_inhibit 100 / 97.4; open_request 97.8 / 96.9; activation_store 99.1 / 100;
window_state_adapter 100 / 100 (portable logic); window_state_abi 97.2 / 100; guest_launch 97.9 / 90.2;
window_coordinator 97.2 / 90.1 (merged version); session_control_host 96.5 / 90.2.
C suites ran under ASan/LSan/UBSan with `detect_leaks=1`; Zig used `std.testing.allocator` and
allocation-failure sweeps (no Zig sanitizer claim). Coverage runs print their evidence directory
under `build/coverage/`.

Real Windows runs (`windows-2022`, workflow `interop_45_windows.yml`, run 40247820926 at `27ce716c`):
window-state adapter smoke (incl. cross-thread window), guest input smoke, guest launch smoke
(console exit 0, notepad pid/job, NotFound, msiexec command line) all passed.

## 5. CI (exact revisions)

Final-head workflow results are recorded in the `## CI at the final revision` section appended below
by the closing docs commit.

## 6. Remaining work, limitations, blockers

Order of work for the next session is in [resume_work_orders.json](resume_work_orders.json); the
pending reviews are in [REVIEW_TODO.md](REVIEW_TODO.md). In short: (1) run the three second-pass reviews and
adjust credit; (2) finish the coordinator and catalog lane gates and merge them; (3) register the
windows/input/catalog handlers in `session_handlers_builtin.c` once their dependencies exist (Wayland
display ownership, rooted store dirs from the device config); (4) frames/buffer layer (host FrameReady
import) which no lane has built; (5) native qualification of everything above.
Known spec questions recorded in the contracts: compositor-held last buffer vs IMPL 5.3 retire timing;
Pause key via `VK_PAUSE` (no E1 bytes via SendInput); AV pointer width 4/5 requires upgrading host and guest
together. Libxkbcommon is not a dependency, so keymap names and modifier indices come from a bounded text
parser with a documented fallback.

## 7. Native device / process state

No QEMU, swtpm or VM was started by this work; none is running. The only native execution was the
GitHub `windows-2022` runners. The original Win11 disk, TPM, firmware and private preparation assets were
not touched. No credentials or answer-media contents were read or written.

## 8. Main checkout and assets

The main checkout `/home/dev/Waddle-LSW` is on `feat/seamless-interop` at `f5178d3` with a clean working
tree and was not modified (the isolated worktrees add refs to the shared repository only).
`build/` there (VM disks, retained evidence) was not touched.

## 9. Ownership

All file ownership held by this session's workers (lanes keyboard, manifest, window_map, launch, session,
peer, coordinator, catalog) is released back to Codex. No worker is running; the stopped second-pass
review agents produced no changes.
