# Seamless interop completion plan

Prepared 2026-10-23 by Codex (`/root`), with parallel read-only Luna audits
`luna_foundations`, `luna_integration`, and `luna_acceptance`.

## Objective and baseline

Complete [IMPL_DESC.md](IMPL_DESC.md), reach 100% in
[TRACKER.md](TRACKER.md), then merge `feat/seamless-interop` through a PR
whose base is the **branch `origin`** in `Kayyo321/Waddle-LSW`. The remote
is also named `origin`; `origin/origin` is its local tracking ref. GitHub
repository metadata confirms this default branch; the planning-time search
returned no open seamless-interop PR. No PR or merge is performed by this plan.

Audited code: `9b3efcb`. Locally recorded upstream: `89671de`, already an
ancestor of the feature head. Refresh remote refs before implementation and
merge; these observations do not freeze a moving remote.

- Completion is **9.6%**: #1/#4 Done, #2/#3 at 60%, 30 tasks at 0%.
  **90.4 percentage points remain.** Planning earns no additional credit.
- Only `src/interop/interop_codec.zig` implements interop code today. The
  memory header freezes ABI, but provides no allocator or mapping validator.
  Session binaries, live negotiation/transfer admission, window graph, extended
  input, desktop artifacts and peripheral bridges remain to develop.
- Current contracts: protocol **v2**, layout **v3/SMW3**, manifest schema **1**.
  Reject superseded mappings/frames; preserve existing per-PID AV compatibility.
- User decision, 2026-10-23: **“Include ViGEmBus implementation.”** Deliver
  #25; do not use the declined-capability completion path. This selects the
  plan; no driver installation or guest modification occurs now.
- Leave README untouched. Keep work and atomic commits on the feature branch.

## Critical findings and prerequisites

| Source evidence | Consequence for implementation |
|---|---|
| `scripts/interop.mk` only runs codec/ABI and M0 fake-tool fixtures | Extend tests/coverage/sanitizers with every runtime module. Codec coverage cannot qualify missing code. |
| `src/av/av_peer.h` uses 128 AV frames; interop requires 512 and different messages | Reuse bounded I/O patterns through reviewed adapters/new peer, never AV frame aliases. |
| `src/av/av_windows.c` tracks one process subtree and 16 windows | Build a session-wide 64-window graph. Reuse identity/observation patterns after adversarial delayed-callback/HWND-reuse review. |
| `daemon_state.c` starts `mounts[0]`; `daemon_qemu.c` accepts one virtiofs socket | #9 needs arrays of owned processes/sockets/logs, deterministic device IDs, partial-start rollback, restart and full teardown. |
| `src/cli/host.c` invents `/` → `Z:\\`; `daemon_fs_translate_path` also has a no-mount fallback | Translate canonical paths against the longest matching configured export. No mounts means no mapped host paths. Explicit root exports remain valid. Specify compatibility consequences before #9 code. |
| `QemuMaxArgs` is 80 | Derive/test the bound for all supported mounts/devices and terminating NULL. Fail atomically, never omit devices on overflow. |
| IMPL_DESC §4.2 used “v2 header” | Correct stale text to layout v3/SMW3; no contract/version change. |
| Install §4.1 holds the manifest lock while starting the session; session §4.2 recovers/migrates that manifest | Define durable transaction phases and a lock order for session.lock/manifest.lock. Release the manifest lock before waiting on child work that needs it. Test concurrent install/start/stop and crashes without deadlock. |
| Logon session agent and legacy AV share IVSHMEM | Guest must not zero/reformat an active AV mapping at logon. Mode exclusion covers initialization, writes, draining and reinitialization. |
| `av_lease` and `av_input` use AV messages | Preserve semantics through explicit adapters/core extraction, with old AV tests retained. Source reuse alone is not interop integration. |
| Keyboard layout table, PNG helpers and platform mechanisms are design only | Assign real code and native probes; unsupported required behavior stays open rather than being simulated and credited. |

Before first edits, the lead extends IMPL_DESC with manifest lock/transaction
phases, mapping initialization authority and capture-writer lifetime, root
translation semantics, bounded transfer memory budgets and module interfaces.
Incompatible frozen wire/layout/schema changes require a version bump and
attributed tracker entry before consumers. A platform limitation permits only
the fallback explicitly defined by the specification; otherwise keep the task
open and report the concrete conflict instead of silently reducing scope.

## Parallel team and ownership

The user requests **Luna implementation workers**, overriding the workforce
skill's usual Sol worker default. Use at most three children, each
`gpt-6-luna`, `fork_turns=none`, explicit `reasoning_effort=high`. High effort
is selected for lifecycle, ABI and cancellation reasoning. Reassess on failed
invariants, contradictory platform evidence or changed scope. No recursive
delegation or automatic model substitution.

The current lead remains the critical reviewer; no parent-session model setting
is changed or claimed by this plan. Luna/high support was checked with the
workforce model-support script and the collaboration interface; all three
audit agents were explicitly spawned as Luna/high. No Sol implementation
worker or advisor was spawned.

**Lead exclusively owns:** IMPL_DESC, TRACKER, this plan; shared protocol/ABI
headers; `GNUmakefile`, `scripts/interop.mk`, `scripts/av.mk`; workflows;
`.gitmodules`/gitlinks; shared CLI and daemon dispatch integration. Workers
return precise integration patches/requests for these files. Only the lead
stages, commits, synchronizes branches, updates submodules and manages the PR.

**Each worker exclusively owns:** the source/test files named in its work
order. Separate host/guest/keyboard/pointer/clipboard command modules; never
assign two workers the same umbrella session/seat/dispatch file. Record
ownership handoff before reassignment. No broad formatting or unrelated edits.

**Runner ownership:** existing make targets share `build/` and run serially
under the lead. Parallel worker tests need separate output/cache directories.
`test-sanitizers` invokes `clean`: use an isolated qualification checkout and
never remove another owner's evidence/build. One explicitly handed-over
Windows/GPU runner at a time; free agent capacity does not grant hardware access.
Workers stop only their own fixtures, never unrelated processes or runs.

Each work order includes baseline SHA, TODO/subtask, model/effort, exclusive and
prohibited files, interface version, runner owner, exact acceptance commands,
expected observable results and escalation conditions. Each receipt returns
files touched, command/cwd/environment/exit, logs/artifact hashes, limitations
and ownership handoff. The lead inspects the actual diff and critical tests;
agent confidence is not acceptance.

## Dependency waves and progress ceilings

These are dependencies, not time estimates. Arrows within a cell mean serial
work. Providers must pass interface/behavior review before dependent integration.
Independent implementation can proceed if M0 hardware is unavailable, with
#2/#3 remaining open. Qualification of the first integrated session waits for
M0 evidence and resolution of its defects. The lead owns M0 scheduling and
records the named hardware supervisor's handoff for an authorized Windows VM,
Linux Wayland GNOME/KDE session, matching AV bundle and PipeWire/audio devices.
No lease or missing prerequisite means no hardware run. Do not keep three
workers busy when only one or two assignments are safe.

| Wave | Luna A | Luna B | Luna C | Lead acceptance / cumulative ceiling |
|---|---|---|---|---|
| 0 | Mapping/init/slot lifetime audit | Windows UIAccess/display/printing/UAC probes | Compositor capability/evidence inventory | Resolve prerequisites; real #2/#3 M0 evidence: **12%**. |
| 1 | #5 layout validator/allocator | #8 VM/config → #9 shares | #33 manifest/journal/parser primitives | #5/#8/#9 accepted: **19%**. #33 remains partial. |
| 2 | #7 host peer/session and AV extraction | #6 guest agent/deploy | #33 concurrency/recovery and mock session | Stable #33 journal/artifact API accepted; #6/#7, including native deployment, accepted: **27%**. |
| 3 | #10 guest window graph | #11 fake-boundary preparation; integrate after #10 acceptance | #27 guest catalog/icons | #11 depends on accepted #10 identity/parent behavior; live endpoints and all three tasks accepted: **39%**. |
| 4 | #12 move/resize → #13 state → #14 display | #15 keyboard → #16 pointer/cursor | #28 desktop artifacts → #29 open/activation | These seven tasks accepted: **61%**. Shared dispatch remains lead-owned. |
| 5 | #17 lock → #18 touch/pen → #19 pinch → #20 IME | #21 clipboard | #23 microphone/per-app audio | These six tasks accepted: **77%**. |
| 6 | #22 DnD → #30 installer/UAC | #24 USB → #26 printing → #25 ViGEmBus | #31 MIME → #32 binfmt → finish #33 after other lanes' producers | These eight tasks accepted: **96%**. #30 also waits on #28/#29. |
| 7 | Assigned guest/native defects | Assigned host/compositor defects | Assigned integration/lifecycle defects | #34 qualification and final audit: **100%**, then PR merge into `origin`. Any required blocked/untested native case prevents this gate. |

Ceilings assume every previous wave passed and #25 ships. They count #33 only
at final closure; reviewed partial credit can occur earlier within its existing
3% weight. Arithmetic: 9.6 + 2.4 + 7 + 8 + 12 + 22 + 16 + 19 + 4 = 100.

Task #33 has two explicit milestones: **journal/artifact API accepted** in
waves 1/2 (bounded schema, locking, recovery, record/backup/restore operations
and concurrency/fault tests), then **full lifecycle accepted** in wave 6.
#28 and #31 consume the first milestone after lead acceptance; they do not
wait on final #33 completion. Final #33 waits on #28, #30, #31, #32, all guest
and peripheral artifact producers and their removal/restore tests. This
refines the tracker's former coarse dependency labels without changing scope,
weights or completed work. Within wave 6, C cannot close #33 while A/B still
have unfinished artifact-producing integration.

Lane C retains `mimeapps.zig` ownership from #33 primitives through #31;
other workers submit API requests. Before #24/#26/#25, the lead records a
handoff of #8/#9 daemon/QEMU/device interfaces and assigns any QMP/config/state
edits to exactly one owner. New guest peripheral modules remain separate from
the agent dispatcher. Wave labels alone never authorize shared-file edits.

Critical path: layout/VM → host+guest session → window graph/mapping →
launch/input → clipboard/DnD/UAC/artifact lifecycle → native Windows and
three-compositor qualification. Begin manifest primitives and uncertain platform
probes early to avoid late integration blockers.

## First implementation work orders

For every allocation, document one owner and deterministic release site. C is
the platform/control default, Zig owns untrusted parsing and memory-safe bounds
handling, C++ is limited to unavoidable SDK/COM/WinRT interfaces. New functions,
files and variables use snake_case, types end `_t`, constants use PascalCase;
structured public docs cover parameter directions, nullability, ownership,
lifetimes, return/errors and threading. Preserve third-party boundary spellings.

1. **A / #5:** Own new `src/interop/interop_layout.zig` and
   `tests/interop/layout_*`. Return an API patch for lead-owned
   `src/interop/interop_layout.h`; lead also owns memory-header and target
   changes. Validate range/alignment/epoch/reserved fields; implement
   1022-page first-fit allocation, three-slot admission, checked stride/size
   math, maximum capture clamp, growth with old-run pins, stale/duplicate
   release rejection, exhaustion/retry and eight audio-ring bounds. Prove
   aligned lock-free atomics on supported Linux/Windows ABIs and release/acquire
   publication. Test malformed mappings, capacity, maximum frames, delayed
   compositor releases, every allocation failure, random allocate/free stress
   and disconnect during capture. Proposed acceptance:
   `make interop-layout-test`, `make interop-layout-sanitizers`,
   `make interop-layout-coverage` (≥90% line and branch).
2. **B / #8/#9:** Own `src/daemon/daemon_config.*`, `daemon_qemu.*`,
   `daemon_state.*`, `daemon_fs.*` and matching config/QEMU/fs/state tests.
   Include every frozen config key, serialization and device edit/reset hooks
   through a lead integration patch. Export all mounts with deterministic
   identities and complete partial-start rollback/restart/reaping. Test zero,
   explicit-root, overlapping and Unicode mounts, outside-share refusal, full
   argv capacity and network/CUPS/USB/audio permutations. Lead owns shared CLI
   and path-rule integration. Validate selected QEMU build features on a real
   VM; argv fixtures alone are insufficient. Existing targets build/run the
   current config/QEMU/fs checks: `build/test_daemon_config`,
   `build/test_daemon_qemu`, `build/test_daemon_fs`, and aggregate `make test`.
   New state rollback/restart fixtures are proposed and must be wired before
   these commands can establish the new #8/#9 acceptance criteria.
3. **C / #33 primitives:** Own new `src/interop/manifest.zig`,
   `manifest.h`, `mimeapps.zig`, `tests/interop/manifest_*` and `mimeapps_*`.
   Bounded schema-1/ID/path/scope parsing, locking, durable journal/recovery,
   atomic artifact writes, prior-state backups and no-follow handling. Crash
   at every durable boundary; cover disk-full, fsync/rename/backup failure,
   user-modified files, symlinks, foreign IDs, newer schema, repeated removal
   and concurrent install/session-start/session-stop with an explicit lock
   order and crash injection. Isolated temporary XDG roots;
   no real user/system/guest changes. Proposed acceptance:
   `make interop-manifest-test`, `make interop-manifest-sanitizers`,
   `make interop-manifest-coverage` (≥90% line and branch).

Proposed targets become requirements when wired by the lead, never evidence of
already existing coverage. Each small runtime behavior lands with its tests,
docs and reviewed tracker impact. Do not accept an entire wave as one commit.

## Definition of done for every tracker task

| TODO | Implementation and evidence needed for 100% |
|---|---|
| #1 | Already Done. Extend task details before code, fix stale references, version incompatible changes; no extra planning credit. |
| #2 | Real 300-second audio run: guest PID, PipeWire node, ≥95% running samples, ≤0.1% post-warm-up drops, operator audible/mixer answers, ring-backlog latency statistics/method and environment. Acoustic latency remains optional. |
| #3 | Separate GNOME/KDE Notepad runs for frame/title/focus/typing/taskbar/Alt+Tab/move/resize/fullscreen/close. M0 exceptions explicit; UNRECORDED is not a verified fullscreen result. |
| #4 | Already Done for codec scope; preserve ABI/tests. Live timers/negotiation, queue and 16-transfer admission belong to #6/#7. |
| #5 | Layout-v3 validator/allocator, descriptor/audio-ring lifetime, bounded arithmetic, release/growth/exhaustion/reconnect stress, native ABI/atomic proof, ≥90% line/branch. |
| #6 | Guest session main loop/listener, capabilities/queues, singleton/logon, signed UIAccess bundle install, versioned update/rollback and ordered teardown; native tests and actual installed signature/path/token behavior. |
| #7 | Host session, 512-frame peer, negotiated dispatch, connect/Hello/liveness/backoff timers, one-redeploy limit, 16 transfers/direction plus byte budgets, local credentials, singleton/AV exclusion, stop/drain/reconnect/autostart; incremental AV extraction with old fixtures unchanged; pinned Wayland protocols. |
| #8 | All config defaults/ranges/edit/reset, explicit none/user networking, conditional CUPS forwarding, xhci and PipeWire I/O, QEMU feature flags; worst-case argv/error cleanup and real VM smoke. |
| #9 | Every export reaches its guest drive; canonical longest-prefix rules, no phantom root; mount startup rollback/restart/stop; path boundary/symlink/Unicode/outside-share cases. |
| #10 | Session-wide 64-window roles/exclusions/graph, parent-first announcement, bounded deferrals, epoch/generation identity, delayed-event/HWND-reuse/capacity recovery; native fixture windows. |
| #11 | Host toplevel/dialog/popup/tooltip roles, positioners/parent lifetime, live-press popup grabs, title/app_id/minima, decoration negotiation, configure ack/buffer release; stale callbacks cannot use retired surfaces. |
| #12 | Caption move, eight resize edges, intercepted press without injection, release suppression, hung-hit-test fallback, double-click rules and stored valid Wayland serial; fake and real compositor cases. |
| #13 | Bidirectional min/max/fullscreen/restore/close state table without configure feedback loops; taskbar restore, guest close and delayed frames on all desktops. |
| #14 | Guest display/DPI API proof, physical/logical conversion and rounding, minima, multi-output add/remove; actual mixed-scale and pointer-alignment evidence. |
| #15 | Full scancode golden vectors, ≥80-layout KLID table/XKB parsing, fake-clock repeat, modifier/lock sync, shortcut-inhibit policy/fallback, full release on focus/lease loss. |
| #16 | XBUTTONs, high-resolution/horizontal axis, 300 ms activating-click replay, cursor shape/image/hotspot sync, coordinate conversion and stale-epoch cleanup. |
| #17 | Relative motion and lock grant/refusal/unlock/focus/disconnect lifecycle, actual locked application. |
| #18 | Touch IDs/frames, pen pressure/tilt/buttons and synthetic-device lifetime, all contact/tip cancellation; native WM_POINTER fixture and real touch/pen applications. |
| #19 | Pinch begin/update/end/cancel with bounded finite values and balanced input; compositor swipe behavior preserved. |
| #20 | Guest and host IME, text-input-v3, caret/preedit position/lifetime, Unicode/surrogates, cancellation and no duplicate committed keys; actual Japanese IME in both modes. |
| #21 | Both directions: text/HTML/RTF/PNG-DIB/files+cut; exact CF_HTML offsets, delayed render/epoch ownership, no loops, validated paths, caps/timeouts and FD/buffer cleanup. |
| #22 | Both Wayland/OLE DnD directions, copy/move action completion, timeouts/cancellation, bounded transfers, path mapping and drag-loop teardown; real Files↔Explorer. |
| #23 | Eight-ring lifetime, per-process WASAPI→PipeWire names/icons, mic via HDA, discard sink, combined fallback and no duplicate audio before/during/after session; drop/latency/overrun/stop tests. |
| #24 | USB list/attach/detach/all, disappearance/busy errors, QMP completion/partial failure, scoped polkit/ACL grant and exact prior ACL restore, hotplug/auto-attach/restart cleanup; webcam/wired gamepad. |
| #25 | ViGEmBus source and signed-artifact pins, reviewed client ABI, evdev focused routing, XInput reports/neutralization, unplug/cancel/teardown; native virtual controller and real Bluetooth/evdev gamepad. User selected inclusion. |
| #26 | CUPS deltas/URI encoding, reachable forwarding, verified Windows IPP creation/elevated helper, bounded retry, printer manifest/removal; dry-run fixtures and real printed page. |
| #27 | Catalog sources/filter/changes, bounded app keys/collision rejection, Explorer seed and icon PNG extraction/validation, thread queue and snapshot/remove/reconnect. |
| #28 | Escaped atomic desktop/icon/menu/shortcut files, stable app_id/window association and icon capability/fallback, TryExec, modified-file protection and manifest; actual desktop menu/dock/taskbar identities. |
| #29 | Frozen open grammar, app/path/installer/shortcut dispatch, bounded PE parser, quoting/shared-path checks, boot feedback/tokens, TTY console delegation and exact exit codes; mock and real cold/warm launch. |
| #30 | Installer/catalog lifecycle, all UAC modes/prior-policy restore/elevated windows, deny/timeout and secure-desktop input stop; actual install/launch/uninstall and native automation proof. |
| #31 | exe/msi/lnk handler/config, GUI/CUI classification, MIME default restore preserving unrelated content; file-manager launches. |
| #32 | System binfmt/helper artifacts, root-only scope, MZ rule conflicts/order, executable/argv handling and previous-file restore; isolated system tests. |
| #33 | Full install/remove/repair/status/migrate/shortcut user/system/guest lifecycle, every artifact producer registered, journal recovery/forced backups/prior settings restore; no orphans or startup deadlocks. |
| #34 | Expanded Linux/native Windows CI, mock full workflows, module coverage/sanitizers/stress, complete GNOME/KDE/Sway matrix, immutable evidence and final review; blocked/untested required cases remain incomplete. |

## Feasibility and dependency gates

Probe uncertain platform mechanisms before broad implementation: guest-local
signing/UIAccess/elevated capture+input (#6/#30), display/DPI (#14), compositor
text-input/icon/activation support (#20/#28/#29), Windows IPP creation (#26),
and ViGEmBus driver/client compatibility (#25). Use current primary platform
documentation, record exact OS/compositor versions, and validate natively.
Cross-linking and fakes cannot establish platform behavior. Failed probes
require an explicit design correction or open blocker, never silent omission.

Lead adds `submodules/wayland_protocols` at #7 and `submodules/vigembus` at #25.
Each addition has a dedicated `chore(deps):` commit, public HTTPS upstream,
upstream-available immutable SHA, license/rationale/boundary in IMPL_DESC and a
dedicated weighted subtask inside the existing TODO. No invented SHA or loose
vendor headers. Audit whether ViGEm client APIs need another pinned dependency;
document it before use. Pin/check the signed installer provenance and SHA-256.
Verify supported Windows driver loading before the full bridge; do not replace
this gate with test-signing or weakened guest security.

Reflect base-system libxkbcommon/libcups/Wayland/PipeWire/D-Bus prerequisites
in build/CI. No opportunistic dependency/toolchain installation by workers.
The lead handles all dependency mutation; hardware/guest changes follow the
actual owner's authorization and runner handoff.

## Hypercritical acceptance process

Before committing each small behavior, the lead reviews its actual diff,
public docs/naming, success/failure/cancel/teardown, arithmetic overflow, stale
identity/epoch, wrong direction/capability, queue exhaustion and partial EOF.
Fault injection must account for every allocation/handle and partial init
state. Snapshot borrowed/mapped data safely; do not treat guest-authored
descriptors as immutable. Capture/audio writers must stop before runs/mapping
are freed. Realtime callbacks cannot allocate, block on manifests or call Wayland.

Existing aggregate commands, serially in an owned disposable qualification
checkout containing no needed build outputs or evidence:

```bash
export ZIG_GLOBAL_CACHE_DIR=/tmp/seamless_qualification_zig_cache
make -B interop-test
make interop-sanitizers
make interop-coverage
make build/interop_codec_abi_test.exe
make -B av-test
make av-sanitizers
make av-coverage
make test
make test-sanitizers
make coverage
```

`test-sanitizers` deletes `build/`; use an isolated checkout and rebuild final
binaries afterwards. Extend interop aggregates to include all new modules and
mock-session workflows. Focused `interop-<module>-test`, `-sanitizers` and
`-coverage` targets in this plan are **proposed**, not currently available.
Expand `guest_windows.yml` for native session tests and feature/PR triggering.
Retain `interop.yml`'s native codec ABI run. Separately extend the AV/guest
workflow's SDK-built WGC artifact flow for source-matched session binaries;
the present interop workflow neither builds nor receives that DLL.

Require ≥90% **line and branch per module** for codec/layout/manifest/desktop
entry/MIME/PE/clipboard inventory, not diluted aggregate coverage. State
machines need transition/integration/stress tests. Allocation-owning Zig
fixtures use `std.testing.allocator`; C consumers run ASan/LSan/UBSan with leak
detection on and zero leaked bytes. Restricted LSan execution is blocked until
run on a supported owned runner; disabling detection is not acceptance.
Windows native heap/handle receipts complement portable instrumented fixtures.

Existing M0 commands:

```bash
scripts/interop_verify_m0.sh audio --device <name> --duration 300
scripts/interop_verify_m0.sh window --device <name>
```

Run window mode separately on GNOME/KDE; save results in VERIFICATION_M0.
Resolve its documented GNOME fullscreen exception with a reproducible test or
explicit acceptance clarification; do not turn UNRECORDED into PASS.
For #2's required latency sample, record the script's ring-backlog mean/p95/max
and its counter-derived method; this measures queue backlog, not acoustic
end-to-end latency. The optional acoustic row remains optional and no new
latency threshold is invented.

Create `acceptance_matrix.md` for #34: one row per required case per desktop
from IMPL_DESC §7.5, plus pinch, ViGEm, capability-missing, reconnect/VM restart
and session-stop cases. Cover menu/icons/shortcuts, window actions/roles,
US/DE/FR/Japanese input, pointer/cursor/lock/touch/pen, clipboard/DnD, mic/audio,
USB/gamepads/printing, installer/UAC, MIME/console/binfmt and complete removal.
Record source SHA, recursive pins, tool/OS/compositor/guest versions,
binary/DLL hashes, commands, operator/runner lease, raw logs, outcome and durable
artifact location. Outcomes: verified/failed/blocked/untested/not claimed.
Only specification-permitted optional behavior may be not claimed.

Use one authorized hardware supervisor; preserve other owners' disks,
firmware, VMs, processes and receipts. Existing vGPU handoffs supply ownership
and evidence patterns; unrelated vGPU qualification is not automatically part
of this feature. Record actual capture/rendering backends and declared scope.
Unavailable devices/machines leave required rows open.

## Tracker and commit discipline

Keep the 34 task weights unchanged. Before a pending task begins, assign
acceptance subtasks totaling 100% of that task. Suggested starting split:
contract/API/test plan 10%, portable code+unit/fault tests 40%, live integration
and stress 25%, required native/manual evidence+review 25%. Adjust once to the
actual task before credit starts. Helper-only code cannot close a live behavior.
Preserve current #1/#4/#2/#3 credit and history; apply new subtask accounting to
remaining work and document any evidence-driven correction.

Overall is `sum(task_weight * task_progress / 100)`. Every feature commit
gets a tracker entry with hash, TODO delta, overall delta and evidence. Atomic
conventional commit messages include a detailed body and TODO impact; no
completed work stays uncommitted. A containing-commit `HEAD` entry is resolved
to its actual SHA at the next tracker update. The final documentation commit
may identify its own containing `HEAD`, which Git history resolves; a literal
self-referential commit hash is impossible. All earlier hashes must be exact.

## PR and merge into origin

1. Refresh remote feature/base refs, inspect divergence and synchronize the
   published feature through a reviewed upstream merge as appropriate. Do not
   overwrite the stale local `origin` branch or force-push by default.
2. Run `git submodule update --init --recursive`; inspect pins, HTTPS URLs and
   conflicts. Qualify integrated source, rerunning affected checks after any
   upstream, code, dependency, DLL or environment change.
3. Reconcile every TODO/subtask/commit. Only after all required evidence passes
   set every task and overall to 100%, concrete UTC Time Ended, status Completed.
   Keep failed-attempt evidence with its resolution.
4. Push and open/update a PR with base `origin`, links to IMPL_DESC/TRACKER/
   acceptance matrix, concrete behavior and validation. Attach it to this chat.
5. Require current-head Linux/native Windows CI, no unresolved findings and
   clean mergeability. Lead audits the full diff, naming/docs/dependencies and
   unchanged README. Reassess/rerun affected gates if head/base changes.
6. Merge the PR with a merge commit, preserving atomic commit attribution.
   GitHub currently allows merge commits and disables auto-merge; do not assume
   an auto-merge queue will finish it. Confirm merge result and remote `origin`
   SHA, then report PR/merge SHA/tracker/evidence and validation.

## Planning evidence

All Luna audits were read-only. The lead independently inspected the critical
source seams and GitHub default-branch/PR metadata. No runtime implementation,
hardware run or dependency installation is claimed.

Fresh baseline: initial non-forced `make interop-test` reused a sanitizer-built
C ABI executable and failed at LSan thread inspection under this executor;
all 19 Zig tests passed first. A separate forced normal rebuild
`ZIG_GLOBAL_CACHE_DIR=/tmp/seamless_plan_zig_cache make -B interop-test` passed
all 19 tests, C ABI and every M0 fake-tool fixture. Logs:
`/tmp/seamless_plan_interop_test.log` (failed attempt) and
`/tmp/seamless_plan_interop_rebuild.log` (passing rebuild). These are transient
local logs, not durable hardware evidence. No fresh sanitizer/coverage/native
result is inferred from historical receipts. Final documentation validation
and the planning commit are attributed in TRACKER with zero progress change.
