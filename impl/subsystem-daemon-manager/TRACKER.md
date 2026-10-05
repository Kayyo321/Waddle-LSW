# Feature Tracker: Subsystem Daemon Manager

- **Contributors / Agents**: Antigravity Agent, Koyak
- **Time Started**: 2026-10-04T19:40:00Z
- **Time Ended**: TBD
- **Feature Branch**: feature/subsystem-daemon-manager
- **Target Merge Branch**: origin
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1      | Define shared C daemon control wire protocol (`include/waddle/daemon_protocol.h`) and status data structures | Done | 10% | 100% | Packed structs, Little-Endian, PascalCase constants, socket framing verified |
| #2      | Implement daemon configuration subsystem in Zig (`src/daemon_config.zig` / `src/daemon_config.h`) for parsing VM specs, exports, and paths | Done | 10% | 100% | Bounds checked, multi-export & INI parsing verified in Zig & C |
| #3      | Implement QEMU process manager and QMP (QEMU Machine Protocol) JSON-RPC client in C (`src/daemon_qemu.c`, `src/daemon_qmp.c`) | Done | 15% | 100% | CLI builder, mock QMP server/client, and process lifecycle verified |
| #4      | Implement VirtIO-FS (`virtiofsd`) lifecycle manager and multi-export filesystem bridge (`src/daemon_fs.c`, `src/daemon_fs.h`) | Done | 15% | 100% | vhost-user socket setup, sandbox/cache flags, and path mapping verified |
| #5      | Implement Waddle background daemon supervisor core (`src/daemon_main.c`, `src/daemon_state.c`) with non-blocking event loop and readiness prober | Pending | 15% | 0% | Single-instance lockfile, state machine, child reaper |
| #6      | Implement host CLI lifecycle commands (`--start`, `--stop`, `--restart`, `--status`, `--kill`, `--logs`, `fs`) and daemon client library in C | Pending | 15% | 0% | Seamless CLI user experience, JSON output support |
| #7      | Implement zero-flag default interactive terminal launcher with transparent auto-start and working directory export mapping | Pending | 10% | 0% | Primary goal: 'waddle' drops into ConPTY, resolves Linux CWD -> Z:\... |
| #8      | Build mock subsystem test harness, unit tests, integration test suite, and VirtIO-FS filesystem integration demonstration | Pending | 10% | 0% | Primary goal: live dir/mkdir test verifying instant folder appearance on Linux desktop |

**Total Feature Completion**: `50.0%`

## Commit History & Progress Log

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

