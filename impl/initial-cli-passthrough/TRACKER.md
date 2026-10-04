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
| #3      | Implement host CLI argument parser, `termios` raw mode manager, and signal trap | Done | 15% | 100% | Host CLI and terminal behavior verified by integration/PTY tests. SIGWINCH and SIGINT via self-pipe trick |
| #4      | Implement host non-blocking multiplexer event loop (`poll`) and stream handlers | Done | 15% | 100% | Bounded stream loop validated under binary duplex and slow output readers. Multiplex STDIN, demux STDOUT and STDERR |
| #5      | Implement path translation engine (Linux host paths to guest VirtIO-FS drives) | In Progress | 10% | 40% | Single root-export mapping implemented; configurable rules pending. Rule-based POSIX to Windows path mapping |
| #6      | Implement Windows guest console execution agent with ConPTY and pipe support | Pending | 20% | 0% | `CreatePseudoConsole` & raw `CreatePipe` modes |
| #7      | Implement guest asynchronous I/O pump and VSOCK listener/dispatcher | Pending | 10% | 0% | Multi-threaded I/O forwarding and exit code retrieval |
| #8      | Build loopback mock test harness, unit tests, and integration test suite | In Progress | 10% | 80% | 15 Linux integration scenarios pass; Windows and benchmark verification pending. End-to-end verification via UNIX domain socket mock |

**Total Feature Completion**: `62.0%`

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

- **Commit `79cd5b7`**: `feat(cli): implement framing and Windows argument serialization`
  - **Task Impact**: +100% to #1 (+10% overall), +100% to #2 (+10% overall), +40% to #5 (+4% overall); +24% overall.
  - **Summary**: Explicit LE codec, CRC, bounded incremental decoder/queues, packed shared ABI, quoting and canonical mock decoder, opt-in root mapping. Unit tests pass with 7,776 quote round trips and fragmented/corrupt frame cases. Configurable export mapping remains pending.

- **Commit `129a36a`**: `feat(cli): add terminal restoration and signal wakeups`
  - **Task Impact**: +40% to #3 (+6% overall); +6% overall.
  - **Summary**: Isolate raw terminal ownership, standard descriptor flag restoration, and pending signal/self-pipe handling. Module compiles with warnings as errors; CLI integration and behavioral checks remain pending.

- **Commit `482ce5c`**: `feat(cli): add the bounded host stream multiplexer`
  - **Task Impact**: +60% to #4 (+9% overall); +9% overall.
  - **Summary**: Implement poll-driven socket/stdin/output queues, response and stream state validation, EOF ordering, output drain, signals, and monotonic session deadlines. Module compiles with warnings as errors; real transport integration remains pending.

- **Commit `f593228`**: `feat(cli): expose host execution over UNIX sockets and VSOCK`
  - **Task Impact**: +40% to #3 (+6% overall); +6% overall.
  - **Summary**: Add option parsing, explicit environment overrides, cwd and path mapping, framed spawn construction, nonblocking connection setup, and integration of terminal/session modules. Host builds, help works, missing socket returns 125. Guest end-to-end and PTY verification remain pending.

- **Commit `fc2265d`**: `feat(mock): launch isolated pipe and PTY subprocesses`
  - **Task Impact**: +20% to #8 (+2% overall); +2% overall.
  - **Summary**: Validate spawn payload strings/environment/flags, decode canonical arguments, execute Linux children with cwd and explicit environment, report exec errors through a close-on-exec pipe, and own process-group cleanup. Mock launcher compiles; Windows task #6 stays pending.

- **Commit `ee88c1b`**: `feat(mock): relay framed I/O through a private UNIX listener`
  - **Task Impact**: +20% to #8 (+2% overall); +2% overall.
  - **Summary**: Add single-session private UNIX listener, bounded nonblocking pipe/PTY relay, stdin EOF, resize/signals, output EOF and exit ordering, and disconnect cleanup. A real shell command produced separate stdout/stderr and exit 42 end to end; mock exited 0. Windows transport task #7 stays pending.

- **Commit `3a6de9b`**: `test(cli): verify duplex streams and terminal lifecycle end to end`
  - **Task Impact**: +20% to #3 (+3% overall), +40% to #4 (+6% overall), +40% to #8 (+4% overall); +13% overall.
  - **Summary**: All 15 C integration scenarios pass: outputs/exit status, exact arguments, cwd/environment, missing executable, 16 MiB duplex with slow readers, SIGINT/SIGTERM, timeout, adversarial peers, PTY resize/Ctrl-C, disconnect, and termios restoration. Host tasks complete for the PoC contract; Windows validation and performance targets remain unverified.

- **Commit `HEAD`**: `test(cli): make fixture path bounds explicit`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Add an explicit socket-path capacity assertion before concatenation, resolving the optimization-dependent format-truncation warning found by the sanitizer build.
