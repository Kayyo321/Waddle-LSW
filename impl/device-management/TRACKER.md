# Feature Tracker: Device Management & Multi-Device CLI Lifecycle

- **Contributors / Agents**: Antigravity Agent, Codex (/root)
- **Time Started**: 2026-10-04T22:11:00Z
- **Time Ended**: TBD
- **Feature Branch**: feature/device-management
- **Target Merge Branch**: origin
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Fix VirtIO-FS readiness probing and stabilize startup | Done | 10% | 100% | Existing stat-based fix; final VM acceptance remains #10 |
| #2 | Implement registry discovery, storage paths, and device metadata | Done | 10% | 100% | Existing baseline; hardening/recovery tracked in #7 |
| #3 | Implement init and aliases with valid QCOW2 creation | In Progress | 10% | 50% | CLI exists; transactional publication and removal of placeholder-disk fallback remain |
| #4 | Resolve targets across lifecycle, shell, exec, logs, and filesystem commands | In Progress | 10% | 50% | CLI wired; default precedence, explicit batch behavior, authoritative status, and supervisor exit remain |
| #5 | Unit/integration tests for init and multi-device baseline | Pending | 10% | 0% | Existing device smoke tests do not establish complete CLI/error-path verification |
| #6 | Specify complete device-management CLI and completion criteria | Done | 10% | 100% | IMPL_DESC.md defines command grammar, output, ownership, transactions, and acceptance |
| #7 | Implement offline rename/remove/retention, leases, journal recovery, and registry hardening | Pending | 10% | 0% | Explicit operands, quiescence, safe traversal, default updates, and interruption recovery |
| #8 | Implement show/default/config get/set/reset and diagnostic doctor | Pending | 10% | 0% | Typed validation, preservation of exports, no live changes, bounded repairs |
| #9 | Implement independent clone and local export/import | Pending | 10% | 0% | Flattened disks, validated portable manifest/hash, fresh CIDs, import without host exports |
| #10 | Complete expanded unit/integration/stress, sanitizer, coverage, and real Windows VM gates | Pending | 5% | 0% | Planned cases in IMPL_DESC.md section 7; no unsupported completion claim |
| #11 | Verify specification revision and PR CI at final head | Done | 5% | 100% | Linux and Windows checks passed at af829ec; final tracker-only audit recheck required before handoff |

**Total Feature Completion**: `45.0%`

## Commit History & Progress Log

- **Commit `fe34450`**: `docs(device): initialize implementation description and progress tracker`
  - **Task Impact**: Scaffolding baseline (0% progress)
  - **Summary**: Created initial IMPL_DESC.md and TRACKER.md defining architecture, data structures, and task breakdown.

- **Commit `76a8c15`**: `fix(daemon): use non-destructive stat check for virtiofsd socket probing`
  - **Task Impact**: +100% to TODO: #1 (+20.0% overall feature completion)
  - **Summary**: Replaced connect() call in probe_socket_ready with stat() inode verification to prevent virtiofsd from shutting down prematurely on client disconnect.

- **Commit `df7e601`**: `feat(device): implement device registry, storage manager, and discovery in C`
  - **Task Impact**: +100% to TODO: #2 (+20.0% overall feature completion)
  - **Summary**: Implemented daemon_device.h and daemon_device.c providing device name validation, config/state path resolution, QCOW2 overlay disk creation, dynamic CID allocation, and registry enumeration.

- **Commit `d701801`**: `feat(daemon): wire device runtime directory isolation and configuration auto-loading`
  - **Task Impact**: +50% to TODO: #3, +50% to TODO: #4 (+20.0% overall feature completion)
  - **Summary**: Updated daemon_client.c to extract device runtime paths when spawning waddled, and updated daemon_state.c to dynamically load device-specific INI profiles or auto-load single-device profiles.

- **Commit `d21c43c`**: `feat(cli): add multi-device target resolution and command line support for device management`
  - **Task Impact**: +0% to TODO: #3/#4 in this audit; retain their 50% pending expanded safety and selection requirements.
  - **Summary**: Added init/list and lifecycle target dispatch, device tests in the build, QEMU log capture, and device-aware client routing. This existing commit was absent from the prior log.

- **Commit `926fbff`**: `docs(device): specify complete device management CLI`
  - **Task Impact**: +100% to TODO: #6 (+10.0% under revised weights). Scope expansion reweights existing #1–#5 from 20% each to 10% each: prior 60.0% becomes 30.0%, then specification completion brings it to 40.0%.
  - **Summary**: Define remove/rename, retention, show/default/config/doctor, clone/export/import, batch lifecycle, grammar, JSON, locks, crash recovery, error behavior, and all required verification. Expanded implementation tasks remain pending.

- **Commit `664a224`**: `chore(ci): repair device branch verification workflows`
  - **Task Impact**: +0% to TODO: #11 pending successful checks; +0% implementation completion.
  - **Summary**: Follow reorganized guest/common/test paths on Windows and install Linux vendor-build prerequisites and real qemu-img. Preserve native heap, allocator, sanitizer, coverage, and cross-build gates.

- **Commit `bf14c42`**: `chore(ci): enable long paths for native guest checkout`
  - **Task Impact**: +0% to TODO: #11 pending rerun.
  - **Summary**: Initial native CI failed while checking out deep QEMU/EDK2 cryptography submodule paths. Enable Git long-path support before recursive checkout without skipping dependencies.

- **Commit `6292d09`**: `fix(build): isolate vendor configuration from application flags`
  - **Task Impact**: +0% to TODO: #11 pending rerun; no new device implementation progress.
  - **Summary**: Fresh Linux CI exposed application pedantic/Werror flags leaking into QEMU/DTC configuration. Clear application CFLAGS/CPPFLAGS/LDFLAGS only at vendor configure; retain strict flags and sanitizers for every Waddle target.

- **Commit `af829ec`**: `docs(device): record local specification verification and journal ordering`
  - **Task Impact**: +50% to TODO: #11 (+2.5% overall); +0% to completed TODO: #6.
  - **Summary**: Record isolated ASan/LSan/UBSan and Zig allocator suite success and Windows guest/fixture cross-build. Specify durable journal intent before allocating staged disk copies and safe recovery of not-yet-created paths. Native CI remains pending.

- **Commit `:/record passing device specification CI evidence`**: `docs(tracker): record passing device specification CI evidence`
  - **Task Impact**: +50% to TODO: #11 (+2.5% overall); overall completion 45.0%. All remaining implementation tasks are unchanged.
  - **Summary**: Record successful Linux sanitizer/allocator/coverage/cross-build and native Windows regression/heap gates at af829ec. The final audit changes only this tracker; verify its PR-head rerun before reporting completion of this request.

## Audit conventions and specification review

Historical progress entries above retain the weights in effect when their commits
were authored. The revised table is authoritative for current overall completion.
Task #6 records specification completion only. Task #11 verifies this revision and
existing CI, not the unimplemented acceptance matrix. Time Ended stays TBD until
the entire feature is implemented and verified; the specification PR is a draft.

A current commit cannot embed its own hash without changing that hash. Its log
entry uses Git's exact-subject revision selector, resolvable with `git rev-parse`;
a subsequent audit replaces it with the resulting hash. No implementation progress
is awarded merely for a documentation or CI maintenance commit.

## Verification evidence for the specification revision

- `git diff --check` passed; README has no diff against target branch.
- Command inventory contains all 13 device subcommands; seven required sections
  are present; tracker weights sum to 100 and the weighted total is 45.0%.
- `make test-sanitizers` passed with isolated temporary XDG config/state/runtime
  roots and explicit `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1`
  and `LSAN_OPTIONS=abort_on_error=1`. Existing C, Zig allocator, daemon, CLI,
  and filesystem mock suites passed. Planned new device acceptance is not claimed.
- `make windows build/windows_guest_test.exe` passed (cross-build only).
- Draft PR: https://github.com/Kayyo321/Waddle-LSW/pull/3 (target `origin`).
- Initial remote attempts exposed Windows long-path checkout and fresh vendor
  compiler-flag inheritance failures; dedicated maintenance commits address them.
  Both rerun workflows passed at `af829ec`; the final tracker-only commit receives
  another CI run before handoff.

### Successful remote checks at `af829ec`

- Linux CLI verification: [run 37256632667](https://github.com/Kayyo321/Waddle-LSW/actions/runs/37256632667), success.
  Sanitizer and allocator suites, coverage gate, Windows cross-build all pass.
  Measured existing implementation line coverage: protocol.c 92.50%, arguments.c
  93.18%, guest_codec.zig 98.56%, path_rules.zig 100.00%. This does not establish
  branch coverage or coverage for the planned registry transaction implementation.
- Native Windows guest: [run 37256632581](https://github.com/Kayyo321/Waddle-LSW/actions/runs/37256632581), success.
  Parser allocator tests, native build, AF_UNIX regressions, and MSVC debug heap
  audit pass. All 47 heap checkpoints report zero allocations, zero bytes, intact
  heap; negative controls detect leak, overrun, and freed-write defects.
- No real Windows VM acceptance for expanded device-management commands is claimed.
  Feature Time Ended remains TBD and PR remains draft pending #3–#5 and #7–#10.
