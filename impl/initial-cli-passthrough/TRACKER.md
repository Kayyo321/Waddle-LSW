# Feature Tracker: Initial CLI Passthrough

- **Contributors / Agents**: Antigravity Agent, Codex
- **Time Started**: 2026-10-04T18:15:38Z
- **Time Ended**: TBD
- **Feature Branch**: feature/initial-cli-passthrough
- **Target Merge Branch**: origin
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1      | Define shared C wire protocol header (`include/waddle/cli_protocol.h`) | Done | 10% | 100% | Packed structs, magic 'WDLC', 32-byte header |
| #2      | Implement Win32 `CommandLineToArgvW` inverse escaping generator and unit tests | Done | 10% | 100% | Deterministic escaping for spaces, quotes, backslashes |
| #3      | Implement host CLI argument parser, `termios` raw mode manager, and signal trap | Pending | 15% | 0% | SIGWINCH and SIGINT via self-pipe trick |
| #4      | Implement host non-blocking multiplexer event loop (`poll`) and stream handlers | Pending | 15% | 0% | Multiplex STDIN, demux STDOUT and STDERR |
| #5      | Implement path translation engine (Linux host paths to guest VirtIO-FS drives) | In Progress | 10% | 40% | Single root-export mapping implemented; configurable rules pending. Rule-based POSIX to Windows path mapping |
| #6      | Implement Windows guest console execution agent with ConPTY and pipe support | Pending | 20% | 0% | `CreatePseudoConsole` & raw `CreatePipe` modes |
| #7      | Implement guest asynchronous I/O pump and VSOCK listener/dispatcher | Pending | 10% | 0% | Multi-threaded I/O forwarding and exit code retrieval |
| #8      | Build loopback mock test harness, unit tests, and integration test suite | Pending | 10% | 0% | End-to-end verification via UNIX domain socket mock |

**Total Feature Completion**: `24.0%`

## Commit History & Progress Log

- **Commit `2b7f20f`**: `docs(impl): add detailed implementation description for initial CLI passthrough`
  - **Task Impact**: 0% progress impact (specification foundation established for tasks #1 through #8)
  - **Summary**: Created comprehensive implementation description covering scope, architecture, ConPTY, C ABI protocol structures, concurrency model, and testing criteria.

- **Commit `43ed06b`**: `docs(impl): initialize feature tracker for initial CLI passthrough`
  - **Task Impact**: 0% progress impact (initialized task tracking framework)
  - **Summary**: Defined 8 discrete implementation tasks totaling 100% weight, baseline status, and attribution schema.

- **Commit `1f331b0`**: `docs(tracker): update rebased commit hashes in feature tracker`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Existing tracker hash audit, recorded during PoC planning.

- **Commit `a289e29`**: `docs(cli): specify the executable Linux proof of concept`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Define wire validation, bounded queues, CLI options, mock process ownership, verification, and explicit Windows limitations. HEAD resolves this commit until the next tracker update records its hash.

- **Commit `HEAD`**: `feat(cli): implement framing and Windows argument serialization`
  - **Task Impact**: +100% to #1 (+10% overall), +100% to #2 (+10% overall), +40% to #5 (+4% overall); +24% overall.
  - **Summary**: Explicit LE codec, CRC, bounded incremental decoder/queues, packed shared ABI, quoting and canonical mock decoder, opt-in root mapping. Unit tests pass with 7,776 quote round trips and fragmented/corrupt frame cases. Configurable export mapping remains pending.
