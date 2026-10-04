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
| #1      | Define shared C daemon control wire protocol (`include/waddle/daemon_protocol.h`) and status data structures | Pending | 10% | 0% | Packed structs, Little-Endian, PascalCase constants, type_name_t naming |
| #2      | Implement daemon configuration subsystem in Zig (`src/daemon_config.zig` / `src/daemon_config.h`) for parsing VM specs, exports, and paths | Pending | 10% | 0% | Memory safe, bounds checked, no leaks |
| #3      | Implement QEMU process manager and QMP (QEMU Machine Protocol) JSON-RPC client in C (`src/daemon_qemu.c`, `src/daemon_qmp.c`) | Pending | 15% | 0% | Command-line builder, QMP capabilities handshake, ACPI shutdown |
| #4      | Implement VirtIO-FS (`virtiofsd`) lifecycle manager and multi-export filesystem bridge (`src/daemon_fs.c`, `src/daemon_fs.h`) | Pending | 15% | 0% | vhost-user socket setup, sandbox/cache flags, multi-export mapping |
| #5      | Implement Waddle background daemon supervisor core (`src/daemon_main.c`, `src/daemon_state.c`) with non-blocking event loop and readiness prober | Pending | 15% | 0% | Single-instance lockfile, state machine, child reaper |
| #6      | Implement host CLI lifecycle commands (`--start`, `--stop`, `--restart`, `--status`, `--kill`, `--logs`, `fs`) and daemon client library in C | Pending | 15% | 0% | Seamless CLI user experience, JSON output support |
| #7      | Implement zero-flag default interactive terminal launcher with transparent auto-start and working directory export mapping | Pending | 10% | 0% | waddle without flags drops directly into PowerShell/CMD ConPTY session |
| #8      | Build mock subsystem test harness, unit tests, integration test suite, and VirtIO-FS filesystem integration demonstration | Pending | 10% | 0% | Mock QEMU/virtiofsd, ASan/LSan clean, live cross-filesystem test script |

**Total Feature Completion**: `0.0%`

## Commit History & Progress Log

- **Commit `01adf3e`**: `docs(impl): add detailed implementation description and tracker for subsystem daemon manager`
  - **Task Impact**: 0% progress impact (specification and tracking baseline established for tasks #1 through #8)
  - **Summary**: Created comprehensive implementation description covering QEMU lifecycle, VirtIO-FS integration, daemon control protocol, CLI commands, state machines, and testing plan.
