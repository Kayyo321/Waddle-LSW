# Feature Tracker: Seamless Interop

- **Contributors / Agents**: Claude Code (cloud session), Codex (/root), Luna audits (luna_foundations, luna_integration, luna_acceptance)
- **Time Started**: 2026-10-22T00:59:49Z
- **Time Ended**: TBD
- **Feature Branch**: feat/seamless-interop
- **Target Merge Branch**: origin
- **Current Overall Status**: In Progress

Original base: `feature/software-vgpu-slicing` at `8a6e25e`; synchronized with
`origin` at `89671de` by merge `e4f9685` on 2026-10-23. Design and frozen contracts:
[IMPL_DESC.md](IMPL_DESC.md). Manual evidence: [VERIFICATION_M0.md](VERIFICATION_M0.md)
(created by TODO #2).

Original review and its follow-up resolutions:
[status_2026_10_23.md](status_2026_10_23.md). Contract corrections supersede
the original protocol v1/layout v2 freeze with protocol v2/layout v3. Completion
is tracked in the task table below; hardware evidence and runtime integration
remain pending.

Execution plan: [completion_plan.md](completion_plan.md), reviewed with three
parallel Luna audits on 2026-10-23. It defines exclusive implementation lanes,
per-task acceptance and the PR path into `origin`; planning earns no progress.
The user selected ViGEmBus inclusion for #25; implementation and driver/native
acceptance remain pending.

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1  | IMPL_DESC.md + TRACKER.md, contract freeze (protocol v2, layout v3, manifest schema, config keys) | Done | 2% | 100% | Versioned amendment 2026-10-23; CLI stop/force, serial mapping and minima explicit |
| #2  | Verify audio passthrough on the existing pipeline: evidence script + manual run recorded in VERIFICATION_M0.md | In Progress | 3% | 60% | Script, fixture and template done; **hardware run by user pending** (`scripts/interop_verify_m0.sh audio`) |
| #3  | Verify a guest window appears like a native window on GNOME + KDE; fix defects found (title updates, CSD negotiation) | In Progress | 3% | 60% | Configure listener, staged modes and server-decoration fallback fixed; **GNOME and KDE hardware runs by user pending** |
| #4  | Interop protocol v2 header + Zig codec, Hello/capabilities, chunked transfers | Done | 4% | 100% | 19 tests; 97.23% branch / 96.65% line; sanitizers clean; Windows ABI link; bounded app-key helper |
| #5  | IVSHMEM layout v3 (slot heap, 64 windows, 8 audio rings) | In Progress | 3% | 40% | Snapshot primitives and first-fit reservations accepted; native ABI/atomics and slot pins/runtime remain open |
| #6  | Guest session agent skeleton, guest-local code signing + UIAccess install, logon autostart, versioned deploy/update | Pending | 4% | 0% | Waits on #4 |
| #7  | `waddle-session` host client, module extraction from av_wayland.c, singleton, reconnect, autostart | Pending | 4% | 0% | Waits on #4, #5 |
| #8  | VM devices: explicit network + CUPS guestfwd, qemu-xhci, PipeWire audiodev, QEMU configure flags, new config keys | In Progress | 3% | 25% | Config defaults/parse/validate/edit-reset accepted; QEMU/devices/CLI persistence/native gates remain open |
| #9  | Export all virtiofs mounts + root path fallback fix | In Progress | 1% | 10% | Phantom root mapping removed; canonical/symlink checks and all-mount lifecycle/guest binding pending |
| #10 | Session-wide window tracking, classification, ownership graph, generation ids | Pending | 4% | 0% | Waits on #6 |
| #11 | Host toplevel/popup/parent mapping, positioner, title/app_id updates, decoration negotiation | Pending | 4% | 0% | Waits on #7, #10 |
| #12 | Move/resize via WM_NCHITTEST interception, caption double-click maximize | Pending | 3% | 0% | Waits on #11 |
| #13 | Minimize/maximize/fullscreen/restore/close state machine | Pending | 3% | 0% | Waits on #11 |
| #14 | Guest display sizing, fractional scale/DPI, multi-output | Pending | 3% | 0% | Waits on #11 |
| #15 | Keyboard completeness (full scancode table, host repeat, lock sync, layout sync, shortcut inhibit) | Pending | 3% | 0% | Waits on #7 |
| #16 | Pointer completeness (XBUTTONs, value120, activating click) + cursor shape/image sync | Pending | 3% | 0% | Waits on #7, #10 |
| #17 | Relative pointer & pointer lock | Pending | 2% | 0% | Waits on #16 |
| #18 | Touch + pen/tablet via synthetic pointer devices | Pending | 3% | 0% | Waits on #16 |
| #19 | Pinch gesture mapping | Pending | 1% | 0% | Waits on #16 |
| #20 | IME (guest default, host text-input-v3 mode with guest-drawn preedit) | Pending | 3% | 0% | Waits on #15 |
| #21 | Clipboard both directions (text, HTML, RTF, images, files incl. cut) | Pending | 4% | 0% | Waits on #7, #9 |
| #22 | Drag and drop both directions | Pending | 3% | 0% | Waits on #21 |
| #23 | Microphone (PipeWire audiodev + discard sink) + per-app audio streams | Pending | 3% | 0% | Waits on #5, #8 |
| #24 | USB redirection (CLI, daemon QMP, polkit ACL grant) for webcams, wired gamepads, other USB | Pending | 3% | 0% | Waits on #8 |
| #25 | Bluetooth/evdev gamepads via virtual XInput | Pending | 2% | 0% | User selected ViGEmBus inclusion 2026-10-23; source/installer pins, implementation and native acceptance pending; IMPL_DESC §3.18 |
| #26 | Printing via IPP guestfwd to host CUPS | Pending | 2% | 0% | Waits on #8 |
| #27 | Guest app catalog, icon extraction (PNG), change notifications | Pending | 4% | 0% | Waits on #6 |
| #28 | Host desktop-entry/icon/menu materialization, app_id matching, toplevel icon, desktop-shortcut mirroring | Pending | 4% | 0% | Waits on #27 and #33 journal/artifact API milestone; full #33 closure follows all artifact producers |
| #29 | `waddle open` launch path, activation tokens, cold-boot feedback, File Explorer seed | Pending | 3% | 0% | Waits on #7, #27 |
| #30 | Installer flow: UAC modes window/auto/native, secure-desktop safety net, elevated windows, auto entries on install/uninstall | Pending | 3% | 0% | Waits on #6, #28, #29 |
| #31 | .exe/.msi/.lnk MIME handler, PE classifier, `exe_handler` setting | Pending | 2% | 0% | Waits on #29 and #33 journal/artifact API milestone; full #33 closure follows all artifact producers |
| #32 | binfmt_misc system artifacts, `install --system`, status/conflict reporting | Pending | 1% | 0% | Waits on #31 |
| #33 | Integration manifest + install/remove/repair/status/migrate, TryExec self-hiding | In Progress | 3% | 15% | Owned bounded schema codec accepted; journal/artifact API, commands and removal qualification remain pending |
| #34 | End-to-end acceptance matrix (GNOME/KDE/Sway), CI workflow, coverage/sanitizer gates | Pending | 4% | 0% | Waits on all |

**Total Feature Completion**: `12.10%`

## Active Foundation Subtasks and Ownership

Subtask percentages sum to 100% of each parent task, not overall feature weight.
Design assignments earn no credit; only accepted implementation is recorded.

| Task | Acceptance allocation | Current accepted subtasks |
|---|---|---|
| #5 | Snapshot validator/geometry/audio bounds 20%; bitmap allocator 20%; C ABI+native atomic adapters 10%; pinned slot/growth/runtime integration 25%; coverage/sanitizer/stress/native proof 25% | Snapshot primitives 20%; first-fit reservations 20%; focused coverage gates pass |
| #8 | Config parse/validate/edit-reset 25%; config CLI serialization/get 10%; QEMU argv/build features 30%; VM lifecycle/device integration 15%; regression/native qualification 20% | Config parse/validate/edit-reset 25%; 19 Zig tests and C ABI/sanitizer fixture |
| #9 | All-mount process lifecycle/QEMU 40%; guest drive/tag integration 20%; root fallback removal 10%; canonical/symlink translation 10%; fault/restart/native qualification 20% | Root fallback removal 10%; export-boundary fixtures pass |
| #33 | Bounded schema codec 15%; journal/lock/recovery engine 30%; artifact ownership/backup API 20%; integrated commands/all producers 20%; crash/native/full-removal qualification 15% | Bounded schema codec 15%; allocator-failure tests and 92.75% branch / 100% line |

Luna `luna_foundations` owns new layout Zig source/tests; Luna
`luna_integration` owns config source/header/focused config tests; Luna
`luna_acceptance` owns new manifest Zig source/tests. Lead owns shared ABI,
build/workflow wiring, docs/tracker, Git and final review. No Windows/GPU
runner is granted by these assignments. Shared build targets run serially.

## Commit History & Progress Log

- **Commit `a2f99c9`**: `docs(seamless-interop): freeze interop contracts and design`
  - **Task Impact**: +80% to TODO: #1 (+1.6% overall feature completion)
  - **Summary**: Adds IMPL_DESC.md with scope, recorded user decisions, frozen
    interop protocol v1, IVSHMEM layout v2, daemon/config/manifest contracts,
    peripheral and desktop-integration mechanisms, execution sequences,
    threading, failure modes and verification criteria.

- **Commit `14183f8`**: `docs(seamless-interop): add feature tracker`
  - **Task Impact**: +20% to TODO: #1 (+0.4% overall feature completion)
  - **Summary**: Adds this tracker with 34 weighted tasks (sum 100%),
    dependencies and blockers.

- **Commit `d5dcc25`**: `fix(av): request client-side decorations for Windows-framed windows`
  - **Task Impact**: +20% to TODO: #3 (+0.6% overall feature completion)
  - **Summary**: Generates and links xdg-decoration client code, binds the
    optional manager, requests client-side mode per toplevel and destroys the
    decoration before its toplevel. The Wayland request double records proxy
    interfaces, destructor order and set_mode arguments; `make av-test` and the
    ASan/LSan/UBSan build of `av_wayland_state_test` pass.

- **Commit `23dd01f`**: `fix(av): re-issue window titles when the guest title changes`
  - **Task Impact**: +10% to TODO: #3 (+0.3% overall feature completion)
  - **Summary**: Calls `xdg_toplevel_set_title` only when an accepted geometry
    message carries a different title; asserted for unchanged, changed and
    repeated titles.

- **Commit `03505b9`**: `test(seamless-interop): add milestone 0 hardware evidence script`
  - **Task Impact**: +50% to TODO: #2 (+1.5% overall), +20% to TODO: #3 (+0.6% overall)
  - **Summary**: Adds `scripts/interop_verify_m0.sh` (audio and window modes,
    automatic PipeWire/guest-counter checks, operator questions, Markdown
    summary), its fake-tool fixture `tests/interop/verify_m0.sh`, and
    `make interop-test` via `scripts/interop.mk`. No hardware evidence yet.

- **Commit `d4ccabd`**: `docs(seamless-interop): add milestone 0 verification record`
  - **Task Impact**: +10% to TODO: #2 (+0.3% overall), +10% to TODO: #3 (+0.3% overall)
  - **Summary**: Adds VERIFICATION_M0.md with prerequisites, procedure, pass
    rules, the fixes made before the run and empty result tables to be filled
    from real GNOME/KDE runs.

- **Commit `d3cc78d`**: `feat(interop): add interop protocol v1 public header`
  - **Task Impact**: +20% to TODO: #4 (+0.8% overall feature completion)
  - **Summary**: Public constants, structs, enumerations and the documented
    allocation-free codec C ABI.

- **Commit `1b22b42`**: `feat(interop): implement the Zig interop codec with tests and gates`
  - **Task Impact**: +70% to TODO: #4 (+2.8% overall feature completion)
  - **Summary**: Registry, message table, cross-field rules, transactional
    writer and SHA-256 transfer reassembly; 16 Zig tests including a
    20000-case fuzz, C ABI round trip, ASan/LSan/UBSan clean, coverage gate
    96.97% branch / 96.35% line, Windows static library and ABI link.

- **Commit `5fdb8a9`**: `ci(interop): verify seamless-interop gates on the feature branch`
  - **Task Impact**: +10% to TODO: #4 (+0.4% overall feature completion)
  - **Summary**: New interop workflow (Linux gates + native Windows ABI run);
    AV workflow also runs on this branch. Workflow results are recorded when
    observed.

- **Commit `f450b3f`**: `docs(seamless-interop): record protocol codec completion`
  - **Task Impact**: +0% (tracker bookkeeping).
  - **Summary**: Marks TODO #4 Done and resolves earlier HEAD entries.

- **Commit `e4f9685`**: `chore(seamless-interop): merge updated origin into public feature branch`
  - **Task Impact**: +0% to TODO #1-#34 (+0% overall); upstream synchronization.
  - **Summary**: Merges the 61 missing upstream commits through `89671de`
    without rewriting the 10 published feature commits. No conflicts; recursive
    submodules synchronized. Interop/AV tests, both sanitizer targets, codec
    coverage and Windows ABI cross-link pass. Upstream commits retain their
    existing software-vGPU tracker attribution; they do not advance this feature.

- **Commit `e99c103`**: `docs(seamless-interop): record synchronized branch review and next steps`
  - **Task Impact**: +0% to TODO #1-#34 (+0% overall); research and bookkeeping.
  - **Summary**: Adds the current-source status report, distinguishes codec
    completion from pending runtime work, records verification and contract
    gaps, and orders the next tasks. No hardware acceptance
    or additional feature implementation is claimed.

- **Commit `2450d22`**: `fix(av): handle decoration configure events before presenting frames`
  - **Task Impact**: +0% to TODO #3 (+0% overall); corrects already credited
    negotiation work, with real compositor acceptance still pending.
  - **Summary**: Registers and delivers the configure listener, stages modes
    until the surface acknowledgement, handles forced server-side decoration
    through the protocol destructor, and tests invalid/missing/stale events,
    listener/allocation failures and teardown. IMPL_DESC section 2.6 specifies
    ownership, ordering and fallback. Normal and ASan/LSan/UBSan callback tests pass.

- **Commit `f2b4d32`**: `fix(interop): version corrected memory and session contracts`
  - **Task Impact**: +0% to TODO #1/#4/#5/#7/#11/#12/#27/#33 (+0% overall);
    repairs existing contracts and adds ABI declarations without claiming the
    pending allocator, session, catalog or integration runtime.
  - **Summary**: Protocol v2/layout v3 replace the inconsistent freeze; assert
    every memory-field offset, sizes and alignment on Linux/Windows. Add bounded
    AUMID keys, independent window minima, menu trigger references and local-only
    stop frames. Specify serial translation, stop lifecycle and repair --force.
    Nineteen Zig tests and C ABI tests pass; coverage 97.23% branch/96.65% line,
    sanitizers clean and Windows ABI cross-link passes.

- **Commit `9b3efcb`**: `refactor(interop): enforce codec naming and public interface contracts`
  - **Task Impact**: +0% to TODO #1/#4 (+0% overall); audit corrections to
    already credited code, no new runtime milestone completion.
  - **Summary**: Rename owned Zig types/constants and fixtures, preserve vendor
    member spellings and public ABI, and document every parameter, ownership,
    nullability, error and threading rule on all 18 C/Zig exports. Correct the
    misleading unchanged-on-failure claim for sticky writer errors. All code
    and test literals are preserved. IMPL_DESC section 3.12.6 records the audit;
    final interop-test, rebuilt av-test, both sanitizer suites and the Windows
    ABI cross-link pass. Coverage stays 97.23% branch/96.65% line; formatting and
    declaration documentation checks pass.

- **Commit `288cd9e`**: `docs(seamless-interop): plan parallel Luna implementation and merge gates`
  - **Task Impact**: +0% to TODO #1-#34 (+0% overall); planning and review only.
  - **Summary**: Records three read-only Luna audits, a dependency-ordered plan
    for all 34 tasks, exclusive worker/lead ownership, per-module and native
    acceptance gates, exact completion arithmetic and PR merge into `origin`.
    Splits #33's early journal/artifact API prerequisite from final lifecycle
    closure to remove the coarse #28/#31 dependency cycle without changing scope.
    Records the user's ViGEmBus inclusion decision without claiming dependency
    installation or #25 progress. Corrects the stale session-start v2-header
    reference to the already-frozen layout v3/SMW3. Forced normal interop rebuild
    passes 19 Zig tests, C ABI and M0 fixture; initial reused sanitizer binary
    failed LSan thread inspection. Documentation checks verify all 34 tasks,
    unchanged weights/current progress, wave ceilings, links and existing
    feature-commit attribution; `git diff --check` passes. No fresh hardware,
    sanitizer or coverage result is claimed.

- **Commit `9b4078c`**: `docs(interop): specify foundation ownership and acceptance boundaries`
  - **Task Impact**: +0% to TODO #5/#8/#9/#33 (+0% overall); implementation
    contracts and acceptance allocation, no accepted runtime work yet.
  - **Summary**: Specifies bounded layout snapshots, mapping initialization
    authority, config editing compatibility, export semantics, manifest parser
    ownership, transaction/lock order and transfer reservation budgets. Assigns
    exclusive Luna implementation lanes and weighted subtasks before code.
    Existing feature completion remains 9.6%; native/hardware gates stay open.

- **Commit `87dc328`**: `feat(interop): add transactional seamless configuration settings`
  - **Task Impact**: +25% to TODO #8 (+0.75% overall; total 10.35%).
  - **Summary**: Luna implements all frozen config keys/defaults, bounded unique
    USB selectors and qualified edit/reset while retaining unqualified legacy
    names. Lead review fixes qualified AV path-length handling and exact empty
    values; tests cover alias collisions, ten-edit limit, rollback and repeated
    sections. Nineteen Zig tests and the C config fixture pass; focused C
    ASan/LSan/UBSan runs pass with leak detection enabled. Config data only:
    QEMU/device behavior, CLI persistence and native qualification stay pending.

- **Commit `d9def23`**: `fix(interop): restrict automatic path mapping to configured exports`
  - **Task Impact**: +10% to TODO #9 (+0.10% overall; total 10.45%).
  - **Summary**: Removes CLI/daemon phantom root mappings, refuses invalid config
    and partial allocation/capacity failures, validates bounded mount strings
    before publishing rules and preserves explicit root and guest-native paths.
    C export tests and CLI option fixtures pass; ASan/LSan/UBSan C regressions
    pass with leak detection enabled. CLI socket error distinction requires
    local UNIX-socket access and passes with it. Generic legacy path helper
    behavior is unchanged; realpath/symlink containment, all-mount processes
    and guest drive binding remain open. Independent Luna review found no
    additional substantive path-slice defects.

- **Commit `2baf303`**: `feat(interop): validate detached layout v3 memory snapshots`
  - **Task Impact**: +20% to TODO #5 (+0.60% overall; total 11.05%).
  - **Summary**: Luna adds allocation-free little-endian header/Ready-descriptor
    snapshots, checked capture geometry/clamping and eight audio-ring bounds.
    Lead rejects initial 81.25% branch coverage; revised dynamic malformed-input
    tests pass 98.44% branch (63/64), 98.57% line (69/70), eight Zig tests and a
    20,000-case mutation corpus with the testing allocator. Aggregate interop
    test/coverage targets now include layout. No C ABI/native atomic transition,
    allocator, mapping initialization or live slot ownership is claimed.

- **Commit `e368647`**: `feat(interop): reserve heap pages with epoch-bound ownership tickets`
  - **Task Impact**: +20% to TODO #5 (+0.60% overall; total 11.65%).
  - **Summary**: Luna adds a fixed-size first-fit page map/reservation ledger
    with exact epoch/ticket/run matching, no-wrap tickets and transactional
    failures. Stale, forged, interior and duplicate releases cannot reclaim a
    current reservation. Lead rejects 87.50% initial branch coverage; expanded
    exhaustion/corruption tests pass 97.92% branch (47/48), 100% line (44/44),
    nine allocator tests plus eight imported layout tests and 4,000 independent
    page-oracle operations. Capturer/compositor pins and growth/reconnect
    ownership remain runtime subtasks.

- **Commit `d942bb0`**: `feat(interop): parse owned bounded integration manifests`
  - **Task Impact**: +15% to TODO #33 (+0.45% overall; total 12.10%).
  - **Summary**: Luna adds bounded strict schema-1 parsing, known-kind/prior
    validation, duplicate ID/key rejection and one explicit document arena.
    Lead review corrects nested-arena lifetime, enum naming and preservation
    of empty prior registry strings. After rejecting initial 77.95% branch
    coverage, twelve tests including valid/invalid allocation-failure sweeps
    pass 92.75% branch (179/193), 100% production line (124/124). The codec
    neither authorizes paths nor writes artifacts; journal/locks/commands and
    full lifecycle remain pending.

- **Commit `HEAD`**: `chore(interop): format the configuration extension with Zig`
  - **Task Impact**: +0% to TODO #8 (+0% overall; total 12.10%).
  - **Summary**: Applies Zig's formatter to the feature-owned config extension;
    no behavior change. Restricted-executor formatting returned Unexpected;
    host-access formatting succeeds and all nineteen config tests pass again.
    `HEAD` identifies this containing formatting commit.
