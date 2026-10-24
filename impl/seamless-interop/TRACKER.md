# Feature Tracker: Seamless Interop

- **Contributors / Agents**: Claude Code (cloud session), Codex (/root), Luna implementation/review (luna_foundations, luna_integration, luna_acceptance, luna_layout, luna_mounts, luna_qemu_build)
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
| #2  | Verify audio passthrough on the existing pipeline: evidence script + manual run recorded in VERIFICATION_M0.md | In Progress | 3% | 60% | Script, fixture and template done; **native pipeline readiness and hardware run pending** (`scripts/interop_verify_m0.sh audio`) |
| #3  | Verify a guest window appears like a native window on GNOME + KDE; fix defects found (title updates, CSD negotiation) | In Progress | 3% | 60% | Configure listener, staged modes and server-decoration fallback fixed; **GNOME and KDE native hardware runs pending** |
| #4  | Interop protocol v2 header + Zig codec, Hello/capabilities, chunked transfers | Done | 4% | 100% | 23 codec/imported parser tests; 97.13% branch / 96.56% line; bounded SemVer2 and app-key helpers; native Windows/ASan/LSan gates pass |
| #5  | IVSHMEM layout v3 (slot heap, 64 windows, 8 audio rings) | In Progress | 3% | 60% | Snapshot, heap, C ABI, native CPU atomics and pure slot pins/growth/drain accepted; live mapping/session integration and qualification remain open |
| #6  | Guest session agent skeleton, guest-local code signing + UIAccess install, logon autostart, versioned deploy/update | In Progress | 4% | 25% | Shared framing/owned queue/handshake/liveness qualified on Linux and Windows; Win32 listener/mapping, signing/deploy and native acceptance remain open |
| #7  | `waddle-session` host client, module extraction from av_wayland.c, singleton, reconnect, autostart | In Progress | 4% | 25% | Shared framing/owned queue/handshake/liveness qualified on Linux and Windows; host transport, singleton, module extraction and native acceptance remain open |
| #8  | VM devices: explicit network + CUPS guestfwd, qemu-xhci, PipeWire audiodev, QEMU configure flags, new config keys | In Progress | 3% | 65% | Config, CLI, persistence, device argv and pinned source build accepted; VM lifecycle/native gates and audio routing remain open |
| #9  | Export all virtiofs mounts + root path fallback fix | In Progress | 1% | 60% | Root fix, QEMU mount arrays, owned supervisor lifecycle and canonical/symlink translation accepted; guest binding and native qualification pending |
| #10 | Session-wide window tracking, classification, ownership graph, generation ids | In Progress | 4% | 35% | Pure graph25% + C ABI10% accepted after reviewed73bc833 import, full main gates and actual Windows CI40126247004; O5/native fact findings block observer credit; coordinator and installed session remain open |
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
| #34 | End-to-end acceptance matrix (GNOME/KDE/Sway), CI workflow, coverage/sanitizer gates | In Progress | 4% | 0% | Foundation CI extended; native CPU units pass; full integrated acceptance waits on all |

**Total Feature Completion**: `17.80%`

## Active Foundation Subtasks and Ownership

Subtask percentages sum to 100% of each parent task, not overall feature weight.
Design assignments earn no credit; only accepted implementation is recorded.

| Task | Acceptance allocation | Current accepted subtasks |
|---|---|---|
| #5 | Snapshot validator/geometry/audio bounds 20%; bitmap allocator 20%; Pure C ABI wrappers 5%; native atomic adapters/proof 5%; pure pin/growth/drain bookkeeping 10%; live runtime integration 15%; coverage/sanitizer/stress/native proof 25% | Snapshot20%; heap20%; C ABI5%; native CPU atomic adapters5%; pure pin/growth/drain10%; Linux/Windows coverage and tests pass |
| #6 | Shared ingress framing5%; owned send queue5%; handshake10%; liveness5%; Win32 listener/mapping loop25%; signing/deploy/update/autostart30%; native qualification20% | Shared framing5%; owned queue5%; handshake10%; liveness5%; native CPU units accepted; integrated agent remains open |
| #7 | Shared ingress framing5%; owned send queue5%; handshake10%; liveness5%; host singleton/daemon/VSOCK/local control25%; AV/Wayland module boundary20%; reconnect/autostart15%; native qualification15% | Shared framing5%; owned queue5%; handshake10%; liveness5%; native CPU units accepted; integrated host client remains open |
| #8 | Config parse/validate/edit-reset 25%; typed CLI get/set/reset 5%; export/import/clone persistence 5%; QEMU argv 15%; QEMU build features 15%; VM lifecycle/device integration 15%; regression/native qualification 20% | Config25%; typed CLI5%; persistence5%; QEMU argv15%; pinned source build15%; focused startup gates pass |
| #9 | QEMU many-mount API 10%; all-mount process lifecycle 30%; guest drive/tag integration 20%; root fallback removal 10%; canonical/symlink translation 10%; fault/restart/native qualification 20% | Root fallback10%; QEMU API10%; all-mount supervisor30%; canonical/symlink10%; whole-module100% line/branch, leak-enabled sanitizers and full host tests pass |
| #10 | Pure bounded graph25%; opaque C ABI10%; portable observer15%; native adapter15%; coordinator/capture/slot/wire integration20%; native installed-session qualification15% | Pure graph25%; C ABI10%; O25 pruning corrected and Linux/Windows qualified. O5 delivered-destroy loss and native fact consistency remain open; no observer/native/session credit |
| #33 | Bounded schema codec 15%; journal/lock/recovery engine 30%; artifact ownership/backup API 20%; integrated commands/all producers 20%; crash/native/full-removal qualification 15% | Bounded schema codec 15%; allocator-failure tests and 92.75% branch / 100% line |

Execution resumed at the user's request from checkpoint4079560. Current
exclusive Luna/high work orders and phase allocation are recorded in
[resume_work_orders.json](resume_work_orders.json). Lead retains shared ABI,
build/workflow, docs/tracker, Git and runner ownership. Shared builds are serial.
Resumed at the user's request from [the saved checkpoint](checkpoint_2026_10_24.md).
Current exclusive assignments are in resume_work_orders.json; previous runner
state must be rechecked before native actions.
The historical [checkpoint](checkpoint_2026_10_23.md) remains valid evidence;
its uncompiled draft earns no credit until current acceptance passes.

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

- **Commit `9a6f367`**: `chore(interop): format the configuration extension with Zig`
  - **Task Impact**: +0% to TODO #8 (+0% overall; total 12.10%).
  - **Summary**: Applies Zig's formatter to the feature-owned config extension;
    no behavior change. Restricted-executor formatting returned Unexpected;
    host-access formatting succeeds and all nineteen config tests pass again.

- **Commit `e6e40fa`**: `feat(interop): expose seamless settings through device config commands`
  - **Task Impact**: +5% to TODO #8 (+0.15% overall; total 12.25%).
  - **Summary**: Luna exposes frozen qualified settings as typed JSON/text,
    documents command keys and separates legacy AV edit aliases. Lead review
    fixes USB list output capacity to include all fifteen separators. Isolated
    CLI integration passes defaults, every enum alternative, qualified lookup,
    set/get/reset, 16 selectors and 17-selector rejection with exact rollback.
    Path-option regressions pass again. Export/import/clone persistence remains
    a separate 5% subtask; no QEMU/native device behavior is credited.

- **Commit `bbccef0`**: `test(interop): qualify foundation units on Linux and native Windows`
  - **Task Impact**: +0% to TODO #5/#33/#34 (+0% overall; total 12.25%).
  - **Summary**: Extends aggregate sanitizer/testing-allocator and Windows CI
    execution to layout, heap and manifest; all Linux interop gates and Windows
    cross-builds pass. Records hashed source/binary/log evidence of actual
    Windows 11 build 26300 unit runs (8 layout, 17 heap including layout,
    12 manifest tests), with verified transfers and task-temporary cleanup.
    The host/guest runner is available; missing guest runtime/drivers still
    prevent integrated M0/session acceptance. No live mapping/GUI/audio/input,
    native atomic proof or hosted CI result is claimed. Independent Luna
    read-only review finds no additional primitive correctness defects.

- **Commit `b0d6e96`**: `docs(interop): define layout C boundary and settings persistence`
  - **Task Impact**: +0% to TODO #5/#8 (#5 ABI/atomic allocation split 5%/5%;
    total remains 12.25%).
  - **Summary**: Publishes the fixed C boundary before consumer implementation,
    with private snapshots, transactional outputs, alias/alignment/range checks
    and exact ABI layouts. Defines additive backup settings compatibility and
    clone/import host-mount boundaries. Clarifies QEMU silent HDA, default input
    source and validated named sink identity; routing and native gates remain
    pending. No implementation acceptance is claimed by this contract commit.

- **Commit `9a6b429`**: `feat(interop): construct explicit named QEMU network USB and audio devices`
  - **Task Impact**: +15% to TODO #8 (+0.45% overall; total 12.70%).
  - **Summary**: Adds explicit none/user networking, conditional CUPS forwarding,
    optional xHCI and one HDA endpoint with PipeWire/silent backend. Named
    builder/spawn APIs validate device IDs and daemon state passes canonical
    runtime identity. Lead removes empty input-target property and adds
    deterministic absent/regular/symlink/socket CUPS fixtures. Bounded argument
    construction, cleanup/capacity, alpha/beta routing names, AV endpoint
    uniqueness and QMP/process fixtures pass normally and under ASan/LSan/UBSan.
    The 30% argv/build allocation is refined into two 15% subtasks; only argv
    earns credit. PipeWire-disabled source-build configuration and unresolved
    discard-target routing are concrete open build/#23 requirements; no native
    audio or complete VM lifecycle acceptance is claimed.

- **Commit `91afd85`**: `fix(interop): preserve seamless settings in device backups`
  - **Task Impact**: +5% to TODO #8 (+0.15% overall; total 12.85%).
  - **Summary**: Fixes reproduced export/import loss of all new settings by
    adding typed optional schema-1 keys with legacy defaults, exact enum/range
    validation and bounded unique USB selectors. Clone/export/import round trips
    preserve every setting and all sixteen selectors; clone retains host mounts
    and import suppresses them. Seven Zig storage tests include malformed input
    and allocation-failure sweeps. Full make test passes, including 24 transport
    scenarios, shutdown/stress, daemon state/QMP, CLI persistence, 360 storage
    crash/failure boundaries and path regressions. Focused CLI C-consumer
    ASan/LSan/UBSan integration passes with leak detection enabled; Zig objects
    retain ReleaseSafe bounds checks and testing-allocator tests. No complete
    source-QEMU build or guest device/audio qualification is claimed.

- **Commit `4079560`**: `docs(interop): save a verified recoverable implementation checkpoint`
  - **Task Impact**: +0% to TODO #1-#34 (+0% overall; total 12.85%).
  - **Summary**: Stops workers at user request, preserves the uncompiled C ABI
    draft as an unapplied patch, tightens mapping-overlap boundary documentation
    and records complete resume/runner/build instructions. Preserves hashed
    host test logs, native Windows evidence and explicit QEMU build/audio gaps.
    Header ABI assertions compile on Linux/Windows; draft apply-check passes.
    No C wrapper/runtime implementation, source-QEMU build or additional native
    acceptance is credited. README is unchanged and the running Windows VM is preserved.

- **Commit `6cbc90e`**: `docs(interop): resume bounded parallel implementation from checkpoint`
  - **Task Impact**: +0% to TODO #5/#8/#9/#34 (+0% overall; total12.85%).
  - **Summary**: Resumes three exclusive Luna/high lanes for pure C ABI, pinned
    source-build features and many-mount QEMU APIs. Specifies build provenance,
    invalidation and socket/tag ownership before implementation. Refines #9's
    existing40% allocation into API10% and lifecycle30% without credit changes.
    Remote origin/origin remains89671de; checkpoint4079560's hosted interop and
    Native AV workflows both succeeded (runs40020339744 and40020339384).
    Current native runner/provisioning is rechecked before any VM changes.

- **Commit `ef39989`**: `feat(interop): expose validated layout snapshots through the C ABI`
  - **Task Impact**: +0% to TODO #5 pending native Windows execution
    (+0% overall; total12.85%).
  - **Summary**: Implements all four frozen private-snapshot wrappers with
    bounded pointer/alignment/range/alias checks and staged outputs. Lead
    rejects initial79.17% branch coverage; revised tests pass100% (48/48),
    97.37% production line (74/76), eleven Zig tests including imported layout
    tests, C ABI consumers and ASan/LSan/UBSan. Windows C consumer cross-links;
    workflow now runs it natively. The pure C ABI5% remains uncredited until
    the native Windows gate passes; no live atomic/pin/runtime claim is made.
    Source commit ef39989 remains pending native execution.

- **Commit `6f1ce16`**: `feat(interop): add native descriptor ownership and heartbeat atomics`
  - **Task Impact**: +0% to TODO #5 pending native Windows execution
    (+0% overall; total12.85%).
  - **Summary**: Adds checked x86_64 C11 state transitions and single-writer
    heartbeat operations without changing shared layouts. The Linux CPU fixture
    passes 100,000 exact-sequence publications, cancellation handoff and pointer/
    transition rejection; ASan/LSan/UBSan passes with leak detection enabled.
    Production coverage is93.46% branch (100/107),96.81% line (91/94), using
    atomic gcov counters. Windows cross-link passes; hosted execution remains
    blocked by the separate wide-switch recorder defect. Native atomic5% earns
    no credit yet; mapping initialization, pins/growth and runtime stay open.
    Source commit6f1ce16 awaits the Windows execution gate.

- **Commit `84fc54b`**: `feat(interop): add bounded drive-tagged QEMU mount arrays`
  - **Task Impact**: +10% to TODO #9 (+0.10% overall; total12.95%).
  - **Summary**: Adds zero-to-eight mount builder/spawn APIs with exact ordered
    drive tags, duplicate/path validation and transactional argv cleanup.
    Legacy single-mount identity remains compatible. Lead review hardens
    kill/reap/cleanup so transient failures preserve process, QMP and descriptor
    ownership for retry. Full-capacity argument, QMP, EPERM/EIO/EINTR fixtures
    pass normally and under ASan/LSan/UBSan with leak detection enabled.
    Supervisor ownership, guest tag binding and native qualification stay open.
    Source commit84fc54b provides the accepted QEMU API subtask.

- **Commit `ffd08d4`**: `fix(interop): retain filesystem backend ownership through teardown errors`
  - **Task Impact**: +0% to TODO #9 (+0% overall; total12.95%).
  - **Summary**: Adds explicit read-only spawn/builder policy and bounded
    preflight, refuses unowned pre-existing listeners, and preserves resources
    across kill/reap/unlink errors for retry. Real task-owned child fixtures
    cover EPERM/EIO/EINTR, timeout cleanup, occupied paths and invalid policies;
    normal and ASan/LSan/UBSan runs pass with leak detection enabled. These
    primitives support the still-open all-mount supervisor milestone; native
    guest binding and write-denial are not credited. Source commitffd08d4.

- **Commit `56ceca5`**: `feat(interop): build pinned QEMU with required host backends`
  - **Task Impact**: +15% to TODO #8 (+0.45% overall; total13.40%).
  - **Summary**: Adds an out-of-tree pinned build with dependency/tool/config/
    binary provenance, exclusive locking and invalidation on failed attempts.
    Eighteen isolated tests cover stale/tampered/missing artifacts, tool failure
    and interruption, including a TERM-ignoring descendant after leader exit.
    Actual QEMU9.2.4 at b2995af builds with PipeWire/Slirp/libusb. Diskless paused
    QMP startup passes for silent and PipeWire backend configurations, HDA,
    xHCI and user+socket-guestfwd configuration, with assertions disabled too.
    Hashed receipt: evidence/qemu_source_build_2026_10_24.json. No guest OS,
    audio stream/audibility, USB realization or network/printing traffic proof
    is claimed. Source commit56ceca5 provides the accepted build subtask.

- **Commit `51f3923`**: `test(interop): preserve wide compiler switches in coverage recording`
  - **Task Impact**: +0% to TODO #34 (+0% overall; total13.40%).
  - **Summary**: Fixes hosted run40055774547 failing before Windows execution
    because compiler-generated switch arity exceeded32. Schema2 retains every
    site and supports1..255 edges using sparse extra masks; the512KiB ceiling
    and76,512-site fixture remain unchanged (382,560bytes). Ten recorder tests
    pass with ASan/LSan/UBSan, including full-width concurrent union, corrupted
    metadata rejection, budget errors and nonrecoverable sanitizer violation.
    Luna's five production coverage modes pass; lead aggregate and hosted
    Linux/Windows reruns remain pending. No native or overall acceptance credit.
    Source commit51f3923 contains this recorder correction.

- **Commit `571c884`**: `fix(interop): bind vendor packages to verified executable provenance`
  - **Task Impact**: +0% to TODO #8 (+0% overall; total13.40%).
  - **Summary**: Prevents stale source-build attribution after a system-QEMU
    copy. Bounded probes, source/stamp/hash validation, exclusive identity lock,
    owned temporary publication and post-copy verification cover both package
    origins. virtiofsd requires advertised --readonly support. Fourteen isolated
    tests and actual pinned-QEMU9.2.4/virtiofsd1.13.2 packaging/verification pass.
    Publication interruption is detectable by hash verification; no atomic-pair
    or native guest behavior claim. Source commit571c884.

- **Commit `eea6671`**: `feat(interop): supervise all configured filesystem backends`
  - **Task Impact**: +30% to TODO #9 (+0.30% overall; total13.70%).
  - **Summary**: Owns all eight ordered backends with per-mount policy, reverse
    rollback and retained teardown failures. QEMU-first stop/reap, exact live
    retry identities and all log paths are covered. Lead review fixes dead-VM
    log/QMP descriptor leaks and unconditional successful KillResp; IPC fault
    injection and actual descriptor closure regressions pass. Full make test
    passes before the final focused fixes; updated server/lifecycle tests pass
    afterward, including ASan/LSan/UBSan with leak detection. Native smoke proves
    two backend startup and QMP mount IDs after temporary vhost-vsock access,
    but the harness then failed on its assumption that CLI kill retains the
    supervisor. The ACL is restored and all task-owned processes are stopped;
    no completed native qualification or guest drive/write-denial credit.
    Source commiteea6671 provides the accepted supervisor milestone.

- **Commit `3a9a40a`**: `docs(interop): checkpoint reviewed foundations for the next session`
  - **Task Impact**: +10% to TODO #5 (+0.30% overall; total14.00%).
  - **Summary**: Accepts the pending C ABI5% (ef39989) and atomic adapter5%
    (6f1ce16) after hosted run40057377210 passes both Linux and actual native
    Windows execution at51f3923. Native AV run40057377234 also succeeds. Saves
    hashed logs/CI receipt, all three unapproved next-slice proposals, incomplete
    native smoke and exact resume instructions. User requested stopping here;
    workers are complete, all task-owned native processes are stopped and the
    temporary vhost-vsock ACL is restored. Packaging/supervisor later commits
    have focused host proof; hosted rerun on the checkpoint remains pending.
    No integrated native feature acceptance, completion timestamp or merge.
    Source commit3a9a40a contains the checkpoint and qualification receipt.

- **Commit `2cd516e`**: `docs(interop): preserve checkpoint logs excluded by ignore rules`
  - **Task Impact**: +0% to TODO #5/#8/#9/#34 (+0% overall; total14.00%).
  - **Summary**: Explicitly adds only the task-owned checkpoint evidence logs,
    which the repository-wide log ignore rule omitted from3a9a40a. Verifies
    every receipt-listed artifact is tracked and SHA256-matched. No code or
    acceptance change. Source commit2cd516e preserves the evidence logs.

- **Commit `e86cf8e`**: `docs(interop): specify resumed lifetime recovery and path work`
  - **Task Impact**: +0% to TODO #5/#9/#33/#34 (+0% overall; total14.00%).
  - **Summary**: Records lead decisions on generation/sequence, delayed frame
    release pins, growth/drain, canonical path behavior and durable journal
    boundaries before three exclusive Luna implementation lanes resume. Both
    checkpoint-head workflows succeed (40058148467 interop,40058148519 Native
    AV). Planning and prior CI add no feature credit. Source commite86cf8e.

- **Commit `4710cc7`**: `docs(interop): reserve an isolated Claude window graph handoff`
  - **Task Impact**: +0% to TODO #10/#34 (+0% overall; total14.00%).
  - **Summary**: At the user's request, prepares a manual Claude Code handoff
    and mandatory return template, with a dedicated feature worktree pinned to
    e86cf8e and exclusive window-graph/Win32-observer paths. The Claude session
    is not launched; no subagent skill runner is used. Local commits and tests
    stay isolated; shared source/build/CI/runner/Git integration remain with
    Codex. Existing Luna lanes have no overlapping files. Planning earns no
    implementation credit. Source commit4710cc7 contains this handoff.

- **Commit `7d66882`**: `fix(interop): canonicalize exported host paths before translation`
  - **Task Impact**: +10% to TODO #9 (+0.10% overall; total14.10%).
  - **Summary**: Adds a distinct C/POSIX filesystem boundary while retaining the
    pure lexical API. Validates all existing export roots before startup, rejects
    symlink root components and duplicate normalized roots, resolves input links
    and missing-output ancestors, preserves file/ ENOTDIR behavior, and rejects
    paths outside every export with EXDEV. CLI and daemon use this boundary.
    Whole-module coverage is143/143 lines and170/170 branches; fault fixtures,
    leak-enabled ASan/LSan/UBSan and the full host make test pass. Evidence:
    [qualification receipt](canonical_qualification_2026_10_24.json). Roots must
    remain stable; guest binding and native qualification remain open. Source commit7d66882 contains this implementation.

- **Commit `d547f5b`**: `docs(interop): bound parallel runtime and recovery follow-ups`
  - **Task Impact**: +0% to TODO #5/#9/#33/#34 (+0% overall; total14.10%).
  - **Summary**: Records the opaque scalar slot-runtime C API, explicit epoch
    and proof-gated destruction, and resumes bounded independent Luna tasks
    after the manifest worker hit an output limit. Reserves all Claude-owned
    paths unchanged. Clarifies menu roots and writer epoch requirements.
    Planning and pending unaccepted source add no credit. Source commitd547f5b contains this design/ownership update.

- **Commit `c6ed0b6`**: `feat(interop): track slot pins growth and proof-gated drain`
  - **Task Impact**: +0% to TODO #5/#34 (+0% overall; total14.10%).
  - **Summary**: Adds fixed, allocation-free window/triple ownership, transactional
    growth, exact release pins, captured-epoch writer operations, retirement
    quarantine and explicit producer/host drain proofs. Forty tests pass and
    production coverage is202/204 lines and149/164 branches. Adds this suite to
    Linux gates and Windows artifact execution. Windows cross-build passes;
    actual Windows execution remains pending, so the10% pure bookkeeping
    milestone receives no credit yet. The previous25% runtime allocation is
    split into10% pure bookkeeping and15% live integration; weights unchanged.
    [Qualification](slot_runtime_qualification_2026_10_24.json). Source commitc6ed0b6 contains this implementation.

- **Commit `cb4379c`**: `docs(interop): qualify slot bookkeeping on Linux and Windows`
  - **Task Impact**: +10% to TODO #5 (+0.30% overall; total14.40%).
  - **Summary**: Accepts the pure pin/growth/drain bookkeeping milestone from
    c6ed0b6 after run40109278457 passes Linux gates and actual Windows execution
    on that exact commit. Production coverage and tests remain as recorded in
    [the qualification receipt](slot_runtime_qualification_2026_10_24.json).
    Live mapping/capture/session integration and the separate15% allocation
    remain open. This is CPU module qualification, not compositor acceptance.
    Source commitcb4379c contains this qualification.

- **Commit `dd0ca3d`**: `test(interop): preserve disposable device lifecycle evidence`
  - **Task Impact**: +0% to TODO #8/#9/#34 (+0% overall; total14.40%).
  - **Summary**: Adds the native private-profile/blank-disk smoke fixture and
    passing0/1/8-mount evidence, including QMP identity, four readonly backend
    arguments, process reap and exact temporary ACL restoration. Removes unsafe
    PID fallback signaling; failed supervisor/ACL cleanup retains private state
    and a failed receipt, with interrupts deferred through final cleanup. Four
    fault-only cleanup tests join hardware-free CI. This proves startup/backend
    configuration only; guest drive binding/write denial and other native
    devices remain open. Existing Windows disk untouched. [Evidence receipt](device_smoke_qualification_2026_10_24.json).
    Source commitdd0ca3d contains this fixture/evidence.

- **Commit `f33de21`**: `docs(interop): define quiescent guest metadata initialization`
  - **Task Impact**: +0% to TODO #5/#6/#34 (+0% overall; total14.40%).
  - **Summary**: Specifies exact mapping argument/proof validation, fixed
    header/descriptors, atomic initialization and untouched audio/heap regions
    for the guest pre-Hello primitive. Reserves separate Luna files while the
    slot C adapter receives error-path tests; Claude paths stay exclusive.
    Manifest storage remains unaccepted with an unresolved leaf race and
    insufficient branch coverage. Source commitf33de21 contains this design/ownership.

- **Commit `279a3c4`**: `feat(interop): expose proof-gated slot lifetime C API`
  - **Task Impact**: +0% to TODO #5/#6/#34 (+0% overall; total14.40%).
  - **Summary**: Adds an opaque C-allocator-owned runtime, exhaustive stable
    statuses and scalar views. Every operation checks a captured epoch; outputs
    remain unchanged on failure, aliases are rejected and destroy requires
    successful producer/host drain. Independent review added missing height and
    stride ABI offset guards. Forty-seven Zig tests, C fixtures, leak-enabled
    sanitizers and90.18% line/98.33% branch coverage pass. Windows C and Zig
    executables cross-build; actual execution is pending the next CI run.
    [Qualification](slot_runtime_abi_qualification_2026_10_24.json). No live
    integration credit. Source commit279a3c4 contains this implementation.

- **Commit `acd2011`**: `feat(interop): initialize quiescent guest mapping metadata`
  - **Task Impact**: +0% to TODO #5/#6/#34 (+0% overall; total14.40%).
  - **Summary**: Adds checked layout-v3 header/descriptor initialization before
    Hello, with explicit ownership proof precondition and no writes beyond the
    metadata prefix. Independent review finds no actionable defect. Sparse guard
    fixture, leak-enabled sanitizers and96.88% line/94.44% branch coverage pass;
    Windows fixture cross-builds and joins native CI. Signed-driver access and
    session integration remain open. [Qualification](mapping_init_qualification_2026_10_24.json).
    Source commitacd2011 contains this implementation.

- **Commit `ef56078`**: `docs(interop): specify bounded session stream framing`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Defines fixed-buffer codec-backed incremental framing,
    coalesced-frame consumption, alias/output lifetimes, error/EOF behavior and
    exact tests before session transport implementation. Reserves an independent
    Luna parser lane and read-only handshake contract investigation. Frozen
    codec/wire and Claude graph ownership remain unchanged. Source commitef56078 contains this design/ownership.

- **Commit `0d870e7`**: `docs(interop): preserve rejected manifest store draft and review`
  - **Task Impact**: +0% to TODO #33/#34 (+0% overall; total14.40%).
  - **Summary**: Archives the33-test journal proposal outside production source
    with exact test evidence and unresolved leaf mutation/cleanup races, actual
    syscall-fault requirements and failed earlier branch coverage. Preserves
    partial review fixes and an unapproved atomic-retention redesign proposal;
    no build/command consumer or engine credit is claimed. Resume instructions
    are in[the review](resume_proposals/manifest_store_review.md). Source commit0d870e7 contains this draft/evidence.

- **Commit `0f341a5`**: `docs(interop): define owned queue and handshake acceptance`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Specifies FIFO ownership,512-frame/eight-slot admission bounds,
    partial writes and allocation-failure invariants; freezes bounded typed
    handshake roles, validation, complete-send readiness, exact deadline and
    external mapping obligations. Defines endpoint subtask allocations before
    qualification; isolated common modules earn no native endpoint credit.
    Reserves independent Luna files. Source commit0f341a5 contains this design/ownership.

- **Commit `11fb4c1`**: `docs(interop): specify bounded control liveness and adapter maps`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Defines Ready-only receive deadlines, Ping cadence and bounded
    per-request expiry under continuous other traffic, plus caller-owned request
    IDs and lifecycle-only disconnect actions. Saves read-only host/guest API
    maps for actual OS adapters; legacy driver validation and daemon Running
    are not new-session readiness. No live endpoint or qualification credit.
    Source commit11fb4c1 contains this specification/evidence.

- **Commit `2407048`**: `docs(interop): record native Windows session foundation qualification`
  - **Task Impact**: +0% to TODO #5/#6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Records exact-head interop40110811887 Linux/Windows and Native
    AV40110811855 success atacd2011. Windows executes both C/Zig opaque-runtime
    suites and the metadata initializer. Qualification receipts retain hashed
    run/job evidence and remove pending-CPU-execution limitations. Live endpoint,
    signed-driver and compositor acceptance remain open; no extra task credit.
    Source commit2407048 contains this qualification.

- **Commit `9f921d9`**: `feat(interop): frame bounded fragmented session input`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Adds a fixed64KiB codec-backed reader with exact coalesced-frame
    consumption, sticky header failure, explicit payload-validation results and
    borrowed-view/EOF rules. Independent review finds no actionable defect.
    Lead adds payload-only fragmentation and failed/closed consumer regressions;
    twelve tests,100% production lines and94.44% branches pass. Normal codec
    object links without a symbol workaround. Windows cross-build joins native
    CI; framing5% per endpoint remains uncredited pending that run.
    [Qualification](stream_reader_qualification_2026_10_24.json). Source commit9f921d9 contains this implementation.

- **Commit `7840c82`**: `docs(interop): clarify terminal callbacks and request ID retirement`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Freezes Closed as terminal for delayed handshake callbacks and
    requires explicit expiry release before reusing a tracked Ping ID. This
    prevents a returned retirement action from naming a newly registered request.
    Assigns both review fixes and boundary regressions to independent Luna workers
    while the lead integrates queue/CI outside Claude's reserved window files.
    Source commit7840c82 contains this specification.

- **Commit `ab676a8`**: `feat(interop): own bounded queued frames across partial writes`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Adds a codec-validated owning512-record FIFO, eight reserved
    control admission slots, exact partial-write accounting and deterministic
    freeing on success, OOM and cancellation. Independent review finds no defect;
    lead adds a full64KiB optional-extension copy/last-byte regression. Ten tests
    pass with testing allocator and exhaustive allocation failure,100% production
    lines/90.24% branches. Gates and Windows cross-build are wired; queue5% per
    endpoint remains uncredited until native Windows execution.
    [Qualification](stream_send_queue_qualification_2026_10_24.json). Source commitab676a8 contains this implementation.

- **Commit `6593765`**: `docs(interop): specify shared semantic version parsing`
  - **Task Impact**: +0% to TODO #4/#6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Records a discovered codec contract defect: digits-only parsing
    rejects prerelease/build labels and permits leading zeroes. Specifies one
    bounded allocation-free SemVer2 parser for codec and peer consumers, checked
    uint32 major conversion and full grammar/Windows/coverage verification.
    Saves the C/OS peer-boundary investigation as a proposal; unresolved policy
    choices are not implementation acceptance. Source commit6593765 contains this specification.

- **Commit `31fbea9`**: `feat(interop): track bounded Ready-session liveness`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Implements exact2s Ping cadence and6s receive/per-request
    deadlines with four bounded records, caller-owned request IDs and copied
    retirement actions. Review closes expired-ID reuse/retirement collision;
    tests cover out-of-order/unknown/late Pongs, continuous other traffic and
    exact deadline/clock/token boundaries. Thirteen tests pass,97.09% production
    lines/90% branches; no socket or mapping-drain proof is claimed. Windows
    cross-build and CI gates added; liveness5% per endpoint awaits native CI.
    [Qualification](session_liveness_qualification_2026_10_24.json). Source commit31fbea9 contains this implementation.

- **Commit `ce9178d`**: `feat(interop): enforce staged peer handshake readiness`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Implements both typed handshake roles, overall5s deadline,
    mapping validation, capability subset/intersection, exact Hello correlation
    and full-send readiness. Review fixes delayed callbacks reopening Closed;
    independent follow-up finds no remaining semantic defect. Sixteen tests
    pass,100% production lines/90.60% branches, including each callback's stale
    token/backward clock, exact deadline, role/phase/version and terminal guards.
    Windows cross-build/CI added; handshake10% per endpoint awaits native CI.
    [Qualification](session_handshake_qualification_2026_10_24.json). Source commitce9178d contains this implementation.

- **Commit `18292b0`**: `docs(interop): define common bounded request identifier ownership`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Specifies a common per-direction sequence,4096-record local
    admission bound, wrap skipping only live IDs, explicit release and unchanged
    failures. Separates IDs from feature resource ownership and prevents a second
    private Ping sequence when composing endpoint cores. Source commit18292b0 contains this specification.

- **Commit `0035379`**: `fix(interop): validate complete bounded semantic agent versions`
  - **Task Impact**: +0% to TODO #4/#6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Replaces digits-only codec validation with shared bounded
    SemVer2 grammar and checked uint32 major extraction. Accepts valid metadata
    and long bounded minor/patch; rejects forbidden leading zeroes. Independent
    review finds no defect. Direct parser and Hello/Ack writer/receive regressions
    pass; parser100% lines/94.12% branches, whole codec96.56% lines/97.13% branches.
    Windows cross-build and native CI added; existing #4 completion earns no
    extra credit for this correction.
    [Qualification](agent_version_qualification_2026_10_24.json). Source commit0035379 contains this implementation.

- **Commit `317d307`**: `refactor(interop): conform session namespace aliases to type naming`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Applies only mechanical std/c namespace alias normalization
    to the four new session modules to honor the type/module naming invariant.
    Transformation equality against prior source is checked; all51 focused
    tests pass. Earlier coverage receipts retain their exact original hashes;
    combined native CI requalifies the normalized sources before task credit.
    [Qualification](session_namespace_qualification_2026_10_24.json). Source commit317d307 contains this conformance change.

- **Commit `e5014ac`**: `feat(interop): bound shared request identifier occupancy`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Adds one fixed4096-record request namespace per direction,
    sequential nonzero wrap skipping live IDs, explicit release and retirement
    without cursor reset. Independent review finds no defect; lead strengthens
    stress to64 retained IDs with varied interior churn across wrap and adds
    runtime zero-sentinel guards. Seven tests pass,96.43% production lines/90%
    branches; native Windows CI is wired after successful cross-build.
    [Qualification](request_ids_qualification_2026_10_24.json). Source commite5014ac contains this implementation.

- **Commit `6f6b445`**: `docs(interop): retain next adapter review boundaries`
  - **Task Impact**: +0% to TODO #5/#6/#7/#34 (+0% overall; total14.40%).
  - **Summary**: Saves independent raw Win32 mapping/lifecycle and complete
    message/capability/request-correlation proposals for the next connected
    endpoint layer. Both are explicitly unaccepted; common controls do not
    authorize unnegotiated facilities and transport closure is not mapping drain.
    Records released source ownership and running combined CI after all local
    regression, leak-enabled sanitizer and production coverage gates passed.
    Source commit6f6b445 contains this research/coordination.

- **Commit `8493582`**: `docs(interop): freeze retryable Windows mapping owner API`
  - **Task Impact**: +0% to TODO #5/#6/#34 (+0% overall; total14.40%).
  - **Summary**: Defines an opaque raw SDK lease owner with transactional outputs,
    retained primary/cleanup diagnostics, explicit map-release and close retries,
    publication/quiescence preconditions and guarded pinned driver ABI. Specifies
    portable leak-enabled API doubles and native SDK fixtures; neither can stand
    in for live driver or compositor proof. Source commit8493582 contains this interface/specification.

- **Commit `4dc8df7`**: `docs(interop): qualify shared session milestones on Linux and Windows`
  - **Task Impact**: +25% to TODO #6 and +25% to TODO #7 (+2.00% overall; total16.40%).
  - **Summary**: Accepts framing5% (9f921d9), owned queue5% (ab676a8),
    handshake10% (ce9178d) and liveness5% (31fbea9) for each endpoint after
    combined exact-source CI40114904773 passes Linux and actual Windows at
    e5014ac, including namespace normalization, SemVer correction and common
    request IDs. All local interop, leak-enabled C sanitizers and per-module
    production line/branch gates pass. No OS loop, signed driver, live surface
    or integrated endpoint/native acceptance is credited. Native AV regression
    state and future disposable Windows runner plan are preserved separately.
    [Qualification](session_core_qualification_2026_10_24.json). Source commit4dc8df7 contains this milestone qualification.

- **Commit `894142b`**: `docs(interop): clarify published-map cleanup retry proof`
  - **Task Impact**: +0% to TODO #5/#6/#34 (+0% overall; total16.40%).
  - **Summary**: Review requires proof1 on every retry while a published driver
    map remains owned, but permits proof0 for retained handle/set after successful
    release. Clarifies malformed SDK data and cleanup-only diagnostic results,
    and requires fake SDK resource accounting independent of owner flags. Assigns
    source latch correction and failure regressions to separate Luna owners.
    Source commit894142b contains this review contract.

- **Commit `3230877`**: `feat(interop): retain raw Windows mapping leases across cleanup failures`
  - **Task Impact**: +0% to TODO #5/#6/#34 (+0% overall; total16.40%).
  - **Summary**: Implements the opaque SDK lease owner with bounded SetupAPI
    path acquisition, exact pinned IOCTL ABI, output preservation and retained
    map/device/set ownership on rollback/close failures. Review fixes lost
    publication history; independent review finds no remaining concrete defect.
    Same source runs with independent fake-resource counters, combined cleanup
    faults and retry regressions. Required lead ASan/LSan/UBSan passes with
    detect_leaks1; production98.06% lines/90.58% branches. Actual SDK object and
    Windows fixture cross-build pass; native CPU CI is wired, live driver/session
    integration remains unaccepted and earns no progress by itself.
    [Qualification](mapping_windows_qualification_2026_10_24.json). Source commit3230877 contains this implementation.

- **Commit `5d67f6f`**: `docs(interop): record exact Windows mapper CI qualification`
  - **Task Impact**: +0% to TODO #5/#6/#34 (+0% overall; total16.40%).
  - **Summary**: Archives successful Linux coverage/leak-enabled sanitizer and
    native Windows CPU fixture CI40117078089 plus Native AV40117078160 at
    exact source3230877. Fake driver calls remain explicit; no installed driver,
    live mapping or endpoint acceptance is credited. Source commit5d67f6f contains this receipt.

- **Commit `47c5eee`**: `docs(interop): define retained host frame ownership and safe stop boundaries`
  - **Task Impact**: +0% to TODO #1/#5/#6/#7/#34 (+0% overall; total16.40%).
  - **Summary**: Corrects GuestReady to the configured guest-exec service and
    defines epoch per authorized mapping initialization. Removes unsafe reset
    and pin-retirement claims on EOF/restart. Preserves independent drain reviews
    as unaccepted research; freezes a bounded pure registry with exact callback
    tickets, explicit never-attached completion, notification backpressure and
    retained quarantine. Assigns separate Luna implementation/tests/review lanes.
    No protocol-version or runtime behavior changes; source commit47c5eee contains this contract.

- **Commit `379a953`**: `docs(interop): freeze connected control peer composition`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total16.40%).
  - **Summary**: Defines the bounded protocolv2 composition before implementation:
    full-send readiness, scoped peer errors, deterministic terminal codec policy,
    one request namespace, transactional local errors and copy-out feature events.
    Keeps facility/native side effects and reconnect authorization outside this
    core. Preserves host-coordinated drain as unaccepted protocolv3 research and
    the completed registry review. Source commit379a953 contains this specification.

- **Commit `ee7e1bf`**: `docs(interop): reconcile peer deadlines with bytes already sent`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total16.40%).
  - **Summary**: Independent review catches an expired send acknowledgment that
    could expose already-sent bytes again. Requires exact queue/metadata progress
    while suppressing Ready, preflights short output/local callbacks before time
    effects and makes the peer terminal phase authoritative for untimed errors.
    Distinguishes invalid local callbacks from failed host native proof. Source
    commit ee7e1bf contains this reviewed clarification.

- **Commit `e72422c`**: `docs(interop): abort unfinished negotiation writes on local drain`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total16.40%).
  - **Summary**: Freezes a transport-abort latch for unfinished Hello/Ack when
    local drain closes admission; actual sent bytes still advance bookkeeping,
    and terminal handshake hooks are suppressed. Explicitly permits remote
    transient Ready from bytes already in flight without treating it as local
    admission or native proof. Saves the independent draft-source review so its
    deadline/terminal regressions are mandatory before acceptance. Source commit
    e72422c contains this clarification.

- **Commit `6dd76a2`**: `feat(interop): retain exact host frame lifetimes through notification`
  - **Task Impact**: +0% to TODO #5/#7/#34 (+0% overall; total16.40%).
  - **Summary**: Adds a fixed192-record Zig ledger with exact epoch/frame keys,
    non-reused tickets, explicit compositor versus never-attached completion,
    slot-free/queue ownership ordering and retained drain/quarantine. Independent
    source/test reviews find no internal defect. Eight new builtins plus eight
    independent adversarial tests pass; production98.85% lines/94.44% branches.
    Full interop and leak-enabled C sanitizer suites pass, both Windows suites
    cross-build and native CI is wired. No live compositor or unmap proof is
    implied. [Qualification](host_buffer_registry_qualification_2026_10_24.json).
    Source commit6dd76a2 contains this implementation.

- **Commit `e0e27ca`**: `docs(interop): freeze copied control peer C boundary`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total16.40%).
  - **Summary**: Defines an opaque C-allocator owner, exact64-bit struct layouts,
    copied RX/TX buffers, output/clock preservation, complete allocation alias
    checks and stable phase/event/status values for C OS adapters. Specifies
    runtime guard, leak-enabled C and native Windows tests before acceptance.
    Pure control implementation and its wrapper remain unaccepted while failure
    coverage is expanded; no live endpoint or mapping proof is claimed. Source commit e0e27ca contains this interface/specification.

- **Commit `9cf3ccd`**: `docs(interop): record registry CI and returned graph ownership`
  - **Task Impact**: +0% to TODO #5/#6/#7/#10/#34 (+0% overall; total16.40%).
  - **Summary**: Archives successful Linux/native Windows CI40120219965 and
    Native AV40120220111 at source6dd76a2. Records human release of Claude's
    clean isolated3714cf2 and starts independent graph/observer review and fresh
    standalone gates before import. Preserves peer draft-source findings; its
    37-case line gate passes but75.58% branches still fails90%, so no peer/core
    acceptance or native progress is awarded. HEAD is this evidence/ownership commit.

- **Commit `45c981c`**: `docs(interop): record returned graph review fixes and active peer lanes`
  - **Task Impact**: +0% to TODO #6/#7/#10/#34 (+0% overall; total16.40%).
  - **Summary**: Archives the independent observer findings against3714cf2:
    premature singleton release and discarded failed unhook tokens. Records
    isolated fixes ac4c422/28748bc and mechanical conformance1a179b0. Final
    leak-enabled gates remain in progress; no source imported or native result
    claimed. Updates exclusive Luna core/ABI coverage lanes and graph review.
    Peer94.46% lines/80.81% branches is still below the unchanged90% branch gate.
    HEAD identifies this containing review/coordination commit.

- **Commit `564d589`**: `docs(interop): freeze window tracking acceptance and review disposition`
  - **Task Impact**: +0% to TODO #10/#34 (+0% overall; total16.40%).
  - **Summary**: Freezes parent #10 subtask allocation before any credit. Records
    exact1a179b0 full Linux/leak/coverage/cross-build success, further cached-child
    correctness requirements, and dedicated real-Win32 fixture/CI40123985933
    on isolated5aff081. Source remains isolated; native results pending, O5
    delivered-destroy loss unaccepted, no parent/native credit awarded.
    HEAD identifies this containing allocation/review commit.

- **Commit `bccad0b`**: `docs(interop): require contextual C peer error classification`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total16.40%).
  - **Summary**: Replaces broad tag-only C status translation with read-only
    caller preflights and InternalInvariant for unexpected post-preflight helper
    disagreement. Keeps every legitimate caller status, short-output atomicity
    and ACK accounting. This closes retry/abort ambiguity independently of
    coverage; source corrections and exact qualification remain pending.
    HEAD identifies this containing contract commit.

- **Commit `c194b3b`**: `refactor(interop): conform opaque peer struct tag to type naming`
  - **Task Impact**: +0% to TODO #6/#7 (+0% overall; total16.40%).
  - **Summary**: Names the incomplete C struct tag interop_peer_t consistently
    with its existing typedef and repository type rule. Pointer ABI, public
    function names and numeric layouts remain unchanged; strict C fixture syntax
    check passes. HEAD identifies this naming-only interface commit.

- **Commit `7aa4ce7`**: `feat(interop): compose bounded connected control peer state`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total16.40%).
  - **Summary**: Composes framing, owned queue, Hello/Ack mapping gates, common
    IDs and liveness with an authoritative terminal phase. Full-send readiness,
    exact expired-send accounting, incomplete-negotiation abort and copied feature
    dispatch preserve native ownership boundaries. Review fixes mandatory host
    Ack send-failure flag and documents nontransactional internal errors. Lead47
    tests and Windows cross-build pass; production96.88% lines90.18% branches.
    Wrapper/native execution and OS loops remain open; no extra parent credit.
    [Qualification](control_peer_core_qualification_2026_10_24.json).
    HEAD identifies this containing implementation commit.

- **Commit `a408163`**: `feat(interop): expose contextual copied control peer C boundary`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total16.40%).
  - **Summary**: Implements13 opaque C exports with bounded copied buffers,
    allocation alias preflights and caller-versus-internal recovery classification.
    Independent C client,84 Zig tests, complete interop regression suite,
    leak-enabled C sanitizer suite and all production coverage gates pass.
    ABI199/201 lines and140/150 branches; exact header/symbol match. Three
    Windows peer executables cross-build; actual execution awaits feature CI.
    OS owners and native integration remain open; no extra parent credit.
    [Qualification](control_peer_abi_qualification_2026_10_24.json).
    HEAD identifies this containing implementation commit.

- **Commit `57847b1`**: `feat(interop): integrate reviewed window graph and observer boundary`
  - **Task Impact**: +35% to TODO #10 (+1.40% overall; total17.80%).
  - **Summary**: Imports owned source/tests/docs at reviewed isolated73bc833,
    preserving original3714cf2 return and full isolated provenance. Main graph
    gates pass:145 graph/159 ABI in both modes, C27/doubles65, leak-enabled
    sanitizers, exact21 exports, graph97.95% branch99.88% line and ABI96.77%
    branch99.32% line. Same source passes actual Windows CI40126247004 including
    target-correlated native smoke. Awards only frozen pure graph25% and C ABI10%.
    O5 loss/startup ordering, native fact consistency, coordinator/capture/wire
    and installed-session acceptance remain open; observer/native credit stays0.
    [Qualification](claude_window_graph/main_import_qualification_2026_10_24.json).
    HEAD identifies this containing import commit.

- **Commit `e385e3c`**: `docs(interop): specify native window fact consistency guards`
  - **Task Impact**: +0% to TODO #10/#34 (+0% overall; total17.80%).
  - **Summary**: Freezes pre/post PID/TID checks before property reads and a
    three-result fullscreen helper so indeterminate monitor queries preserve
    prior graph facts. Requires separate deterministic native fault tests and
    unchanged real native smoke; no atomic snapshot or same-tuple reuse claim.
    Implementation/native CI and O5 acceptance remain open.
    HEAD identifies this contract commit.

- **Commit `1e49547`**: `docs(interop): freeze callback-free tracked window snapshot`
  - **Task Impact**: +0% to TODO #10/#34 (+0% overall; total17.80%).
  - **Summary**: Specifies a read-only sorted/deduplicated Active+Retiring HWND
    snapshot with exact Free-slot count, complete C span validation and unchanged
    outputs/counters on failure. Adds no wire or layout changes. This is only
    the bounded O5 capacity prerequisite; observer preservation stays open.
    HEAD identifies this contract commit.

- **Commit `5ed706d`**: `fix(interop): reject inconsistent native window fact snapshots`
  - **Task Impact**: +0% to TODO #10/#34 (+0% overall; total17.80%).
  - **Summary**: Captures identity before properties and rejects changed/zero
    final PID/TID; monitor-info failure is indeterminate instead of false. Adds
    a separate synthetic native fault fixture using real HWNDs and owner-thread
    Started observer cleanup. Lead strict Zig link and MinGW compile pass;
    six-executable Windows graph CI now includes it. Native execution is pending
    for this source; O5 and observer/native acceptance remain open.
    [Qualification](claude_window_graph/native_fact_guards_qualification_2026_10_24.json).
    HEAD identifies this native correction commit.

- **Commit `4f5a92d`**: `docs(interop): freeze bounded Linux connected socket owner`
  - **Task Impact**: +0% to TODO #7/#34 (+0% overall; total17.80%).
  - **Summary**: Freezes descriptor transfer, bounded copied RX/TX, exact positive
    send ACKs, single-use poll guidance, mapping-wait timers, finite nonextending
    drain and one-shot close. Guest initialization and Host failed-validation
    behavior preserve the accepted peer boundary. Refines #7 transport25% into
    byte owner5%/VSOCK-singleton10%/daemon-local-control10% before any credit.
    Source, socketpair fault/stress/coverage/leak qualification remain pending.
    HEAD identifies this contract commit.

- **Commit `dda77a2`**: `docs(interop): record integrated peer Linux and Windows CI`
  - **Task Impact**: +0% to TODO #6/#7/#34 (+0% overall; total17.80%).
  - **Summary**: Exact57847b1 passes interop CI40128006140 Linux/native Windows
    and Native AV40128006121. Windows executes47 composed-core and84 C ABI
    tests plus the independent C fixture; Linux full regression, leak-enabled
    sanitizer and coverage gates pass. No live endpoint/native memory acceptance
    is implied; later native guard/snapshot source still requires fresh CI.
    [Qualification](ci_qualification_57847b1.json). HEAD is this evidence commit.

- **Commit `HEAD`**: `feat(interop): snapshot tracked window keys and exact free slots`
  - **Task Impact**: +0% to TODO #10/#34 (+0% overall; total17.80%).
  - **Summary**: Adds read-only sorted unique Active/Retiring HWND snapshot with
    exact Free-slot count and complete staged C boundary validation. Lead keeps
    prior bad-pointer regression alongside new guards; C consumer independently
    tests duplicate generations and retirement completion. Full148/164 Zig,
    C28/doubles65, leak-enabled sanitizer, coverage and six Windows cross-build
    gates pass. Graph97.85% branches99.88% lines; ABI97.06% branches99.39% lines;
    exact22 exports. Native execution pending. O5 preservation still open.
    [Qualification](claude_window_graph/snapshot_qualification_2026_10_24.json).
    HEAD identifies this implementation commit.
