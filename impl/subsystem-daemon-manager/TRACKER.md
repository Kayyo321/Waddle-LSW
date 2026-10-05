# Feature Tracker: Subsystem Daemon Manager

- **Contributors / Agents**: Antigravity Agent, Koyak, Codex (/root)
- **Time Started**: 2026-10-04T19:40:00Z
- **Time Ended**: 2026-10-05T02:05:00Z
- **Feature Branch**: feature/subsystem-daemon-manager
- **Target Merge Branch**: origin
- **Current Overall Status**: Completed

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1      | Define shared C daemon control wire protocol (`include/waddle/daemon_protocol.h`) and status data structures | Done | 10% | 100% | Packed structs, Little-Endian, PascalCase constants, socket framing verified |
| #2      | Implement daemon configuration subsystem in Zig (`src/daemon_config.zig` / `src/daemon_config.h`) for parsing VM specs, exports, and paths | Done | 10% | 100% | Bounds checked, multi-export & INI parsing verified in Zig & C |
| #3      | Implement QEMU process manager and QMP (QEMU Machine Protocol) JSON-RPC client in C (`src/daemon_qemu.c`, `src/daemon_qmp.c`) | Done | 10% | 100% | CLI builder, mock QMP server/client, and process lifecycle verified |
| #4      | Implement VirtIO-FS (`virtiofsd`) lifecycle manager and multi-export filesystem bridge (`src/daemon_fs.c`, `src/daemon_fs.h`) | Done | 10% | 100% | vhost-user socket setup, sandbox/cache flags, and path mapping verified |
| #5      | Implement Waddle background daemon supervisor core (`src/daemon_main.c`, `src/daemon_state.c`) with non-blocking event loop and readiness prober | Done | 10% | 100% | Lockfile, state machine, child reaper & multiplexed server verified |
| #6      | Implement host CLI lifecycle commands (`--start`, `--stop`, `--restart`, `--status`, `--kill`, `--logs`, `fs`) and daemon client library in C | Done | 10% | 100% | Client IPC, socket resolution, lifecycle commands, and unit tests verified |
| #7      | Implement zero-flag default interactive terminal launcher with transparent auto-start and working directory export mapping | Done | 10% | 100% | Interactive launcher, auto-start, export mount auto-loading & root fallback verified |
| #8      | Build mock subsystem test harness, unit tests, integration test suite, and VirtIO-FS filesystem integration demonstration | Done | 10% | 100% | Live VirtIO-FS directory/file creation and instant host visibility verified |
| #9      | Vendor QEMU upstream Git repository as submodule (`submodules/qemu`) pinned to stable release commit | Done | 10% | 100% | Tracked via public HTTPS, pinned to v9.2.4 (b2995afe), shallow clone configured |
| #10     | Integrate QEMU build target into build system and refactor daemon binary discovery for bundled vendor binaries | Done | 10% | 100% | Minimal softmmu build target in GNUmakefile and relative binary resolution verified |

**Total Feature Completion**: `100.0%`

## Commit History & Progress Log

- **Commit `165ff2e`**: `fix(daemon): widen path candidate buffers to prevent format truncation warnings`
  - **Task Impact**: 0% progress impact (bugfix to TODO: #10)
  - **Summary**: Increased candidate buffer sizes in qemu_find_binary and daemon_fs_find_binary to WaddleMaxPathLen + 64 to eliminate GCC format truncation errors under AddressSanitizer.

- **Commit `176afbf`**: `refactor(daemon): discover QEMU and VirtIO-FS binaries relative to executable`
  - **Task Impact**: +50% to TODO: #10 (+5.0% overall feature completion)
  - **Summary**: Implemented relative binary discovery for QEMU and VirtIO-FS via /proc/self/exe searching <exe_dir>/vendor before falling back to system paths.

- **Commit `d1afd2f`**: `feat(build): add QEMU submodule build target and vendor binary directory`
  - **Task Impact**: +50% to TODO: #10 (+5.0% overall feature completion)
  - **Summary**: Defined build/vendor target in GNUmakefile compiling QEMU x86_64-softmmu to build/vendor/qemu-system-x86_64 and populating build/vendor/virtiofsd.

- **Commit `1cd611d`**: `chore(deps): add qemu as git submodule pinned to v9.2.4 (b2995afe)`
  - **Task Impact**: +100% to TODO: #9 (+10.0% overall feature completion)
  - **Summary**: Added upstream QEMU repository as Git submodule under submodules/qemu pinned to stable release tag v9.2.4 (b2995afe) with shallow clone configuration and audited GPLv2 license compatibility.

- **Commit `dd94f70`**: `docs(impl): document bundled QEMU architecture and register submodule tasks`
  - **Task Impact**: 0% progress impact (specification baseline for TODO: #9 and #10)
  - **Summary**: Documented Section 8 in IMPL_DESC.md detailing rationale, IPC boundary, licensing, build target, and relative binary discovery; registered tasks #9 and #10 in TRACKER.md.

- **Commit `e6e4695`**: `feat(test): implement mock subsystem acceptance harness and VirtIO-FS verification`
  - **Task Impact**: +100% to TODO: #8 (+10.0% overall feature completion)
  - **Summary**: Implemented live VirtIO-FS acceptance test (tests/demo_fs.sh) validating instant host appearance of guest directory/file and bidirectional sync; enhanced cmd_fs_test in src/host.c and mock_process.c.

- **Commit `432b404`**: `docs(tracker): record completion of Task #7 in tracker`
  - **Task Impact**: Synchronize tracker metadata with Task #7 commit.
  - **Summary**: Updated progress log and tracker table for zero-flag interactive launcher and auto-start.

- **Commit `c805c19`**: `feat(cli): implement zero-flag default interactive launcher and auto-start`
  - **Task Impact**: +100% to TODO: #7 (+10.0% overall feature completion)
  - **Summary**: Implemented zero-flag interactive terminal session with auto-start of daemon/hypervisor, mount rule auto-loading from config.ini, root fallback translation, and unit tests in tests/test_auto_terminal.c.

- **Commit `a6808d9`**: `docs(tracker): record completion of Task #6 in tracker`
  - **Task Impact**: Synchronize tracker metadata with Task #6 commit.
  - **Summary**: Updated progress log and tracker table for daemon client library and host lifecycle commands.

- **Commit `e6bc98e`**: `feat(cli): implement daemon client library and host lifecycle commands`
  - **Task Impact**: +100% to TODO: #6 (+15.0% overall feature completion)
  - **Summary**: Implemented C client library (src/daemon_client.c/h), host CLI lifecycle subcommands (start, stop, restart, status with JSON, kill, fs, logs), and unit/integration tests with 0 sanitizer leaks.

- **Commit `40cf9b6`**: `docs(tracker): record completion of Task #5 in tracker`
  - **Task Impact**: Synchronize tracker metadata with Task #5 commit.
  - **Summary**: Updated progress log and tracker table for supervisor core and event loop.

- **Commit `4ec280e`**: `feat(daemon): implement background supervisor core, lockfile, and event loop`
  - **Task Impact**: +100% to TODO: #5 (+15.0% overall feature completion)
  - **Summary**: Implemented supervisor state machine, single-instance fcntl lockfile, waddled binary entry point, non-blocking UNIX socket event loop, and IPC dispatcher.

- **Commit `a36e2ca`**: `feat(fs): implement VirtIO-FS daemon lifecycle manager and translation bridge`
  - **Task Impact**: +100% to TODO: #4 (+15.0% overall feature completion)
  - **Summary**: Implemented VirtIO-FS binary discovery, argument builder, process lifecycle supervisor, socket readiness probing, and path translation integration.

- **Commit `a25feae`**: `feat(qemu): implement QEMU process manager and QMP JSON-RPC client in C`
  - **Task Impact**: +100% to TODO: #3 (+15.0% overall feature completion)
  - **Summary**: Implemented QEMU command-line builder, stdout/stderr logging, process spawning/polling, and lightweight QMP JSON-RPC client with capabilities handshake, status query, and ACPI powerdown.

- **Commit `a1cf0ca`**: `feat(config): implement memory-safe INI daemon configuration in Zig`
  - **Task Impact**: +100% to TODO: #2 (+10.0% overall feature completion)
  - **Summary**: Implemented daemon configuration loader in Zig with C ABI exports for parsing INI specs, memory/vCPU bounds, tilde paths, and filesystem export mappings.

- **Commit `effab6e`**: `feat(daemon): define daemon control wire protocol and data structures`
  - **Task Impact**: +100% to TODO: #1 (+10.0% overall feature completion)
  - **Summary**: Implemented daemon wire header, message types, states, start/stop/status/fs payloads, validation, socket framing, and unit test suite in C.

- **Commit `01adf3e`**: `docs(impl): add detailed implementation description and tracker for subsystem daemon manager`
  - **Task Impact**: 0% progress impact (specification and tracking baseline established for tasks #1 through #8)
  - **Summary**: Created comprehensive implementation description covering QEMU lifecycle, VirtIO-FS integration, daemon control protocol, CLI commands, state machines, and testing plan.

- **Commit `033fe1b`**: `docs(tracker): record initial documentation commit SHA in tracker`
  - **Task Impact**: 0% progress impact
  - **Summary**: Synced initial commit SHA into feature tracker.

- **Commit `aa3bd74`**: `fix(daemon): correctly wait for QEMU and VSOCK guest agent readiness`
  - **Task Impact**: 0% progress impact (bugfix to Tasks #5 and #6)
  - **Summary**: Fixed state machine ignoring `WaitGuest` flags. Added QEMU exit poll check to prevent hanging. Exported CLI errors upon subsystem start failure. Avoided `connect()` consuming mock socket.

- **Commit `b6f0a33`**: `test(mock): rewrite desktop write test using mock guest socket architecture`
  - **Task Impact**: 0% progress impact (bugfix/improvement to Task #8)
  - **Summary**: Fixed `tests/desktop_write_test.sh` string expansion issues and integrated `WADDLE_MOCK_GUEST_SOCK` for full end-to-end testing on CI servers without KVM access.

- **Commit `6619533`**: `fix(cli): restore terminal before printing socket errors`
  - **Task Impact**: 0% progress impact (bugfix to Task #7)
  - **Summary**: Explicitly called `waddle_terminal_close()` in `host.c` before printing `connect_peer` connection errors to `stderr` to prevent horizontal carriage return drift caused by raw terminal modes.

- **Commit `fd657b9`**: `refactor(src): organize source files into categorized subdirectories`
  - **Task Impact**: 0% progress impact (tree hygiene & organization)
  - **Summary**: Reorganized `src/` into `common/`, `cli/`, `daemon/`, `guest/`, and `mock/` subdirectories and updated makefile and test scripts.

- **Commit `5109a58`**: `test(desktop): add --do-not-delete and --content options to desktop write test`
  - **Task Impact**: 0% progress impact (test tooling enhancement)
  - **Summary**: Added `--do-not-delete` flag and custom `--content` option parsing to `tests/desktop_write_test.sh`.

- **Commit `344050c`**: `refactor(tests): organize test files into categorized subdirectories`
  - **Task Impact**: 0% progress impact (tree hygiene & organization)
  - **Summary**: Reorganized `tests/` into `unit/`, `integration/`, `daemon/`, `acceptance/`, and `windows/` subdirectories and updated build recipes and test scripts.


- **Commit `0142b89`**: `chore(ci): repair native Windows verification paths for daemon PR`
  - **Task Impact**: +0% to TODO: #8 (maintenance; feature task remains 100%).
  - **Summary**: Fix parser, source, include, native fixture, and heap-audit paths after src/test reorganization. Preserve native regression and zero-allocation heap gates. CI rerun required at the PR head.

Current-commit audit entries use Git's unique message revision selector (`:/...`),
resolvable with `git rev-parse`, because a commit cannot embed its own hash.

- **Commit `:/avoid persisted checkout credentials in contribution CI`**: `chore(ci): avoid persisted checkout credentials in contribution CI`
  - **Task Impact**: +0% to TODO: #8 (CI hardening; no implementation progress change).
  - **Summary**: Set persist-credentials false in both read-only pull_request workflows so tested contribution code cannot read an authentication token from Git config. Retain SHA-pinned checkout, hosted runners, and existing gates; no pull_request_target execution is introduced.
