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
| #1      | Define shared C wire protocol header (`include/waddle/cli_protocol.h`) | Done | 10% | 100% | Packed structs, PascalCase constants, type_t naming, Doxygen |
| #2      | Implement Win32 `CommandLineToArgvW` inverse escaping generator and unit tests | Done | 10% | 100% | Deterministic escaping for spaces, quotes, backslashes; 91.9% coverage |
| #3      | Implement host CLI argument parser, `termios` raw mode manager, and signal trap | Done | 15% | 100% | Host CLI and terminal behavior verified by integration/PTY tests |
| #4      | Implement host non-blocking multiplexer event loop (`poll`) and stream handlers | Done | 15% | 100% | Bounded queue_t stream loop, decoder_t, zero leak verification |
| #5      | Implement path translation engine (Linux host paths to guest VirtIO-FS drives) | Done | 10% | 100% | Zig path rules and C mapping regressions pass |
| #6      | Implement Windows guest console execution agent with ConPTY and pipe support | In Progress | 20% | 85% | Raw/ConPTY process lifecycle implemented; native Windows verification pending |
| #7      | Implement guest asynchronous I/O pump and VSOCK listener/dispatcher | In Progress | 10% | 85% | Viosock/AF_UNIX listener and session pumps implemented; native Windows/VSOCK verification pending |
| #8      | Build loopback mock test harness, unit tests, and integration test suite | Done | 10% | 100% | 16 C integration scenarios, 17 Zig native tests, >90% coverage, ASan/LSan clean |

**Total Feature Completion**: `95.5%`

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
  - **Summary**: Define wire validation, bounded queues, CLI options, mock process ownership, verification, and explicit Windows limitations.

- **Commit `79cd5b7`**: `feat(cli): implement framing and Windows argument serialization`
  - **Task Impact**: +100% to #1 (+10% overall), +100% to #2 (+10% overall), +40% to #5 (+4% overall); +24% overall.
  - **Summary**: Explicit LE codec, CRC, bounded incremental decoder/queues, packed shared ABI, quoting and canonical mock decoder, opt-in root mapping.

- **Commit `129a36a`**: `feat(cli): add terminal restoration and signal wakeups`
  - **Task Impact**: +40% to #3 (+6% overall); +6% overall.
  - **Summary**: Isolate raw terminal ownership, standard descriptor flag restoration, and pending signal/self-pipe handling.

- **Commit `482ce5c`**: `feat(cli): add the bounded host stream multiplexer`
  - **Task Impact**: +60% to #4 (+9% overall); +9% overall.
  - **Summary**: Implement poll-driven socket/stdin/output queues, response and stream state validation, EOF ordering, output drain, signals, and monotonic session deadlines.

- **Commit `f593228`**: `feat(cli): expose host execution over UNIX sockets and VSOCK`
  - **Task Impact**: +40% to #3 (+6% overall); +6% overall.
  - **Summary**: Add option parsing, explicit environment overrides, cwd and path mapping, framed spawn construction, nonblocking connection setup, and integration of terminal/session modules.

- **Commit `fc2265d`**: `feat(mock): launch isolated pipe and PTY subprocesses`
  - **Task Impact**: +20% to #8 (+2% overall); +2% overall.
  - **Summary**: Validate spawn payload strings/environment/flags, decode canonical arguments, execute Linux children with cwd and explicit environment, and report exec errors.

- **Commit `ee88c1b`**: `feat(mock): relay framed I/O through a private UNIX listener`
  - **Task Impact**: +20% to #8 (+2% overall); +2% overall.
  - **Summary**: Add single-session private UNIX listener, bounded nonblocking pipe/PTY relay, stdin EOF, resize/signals, output EOF and exit ordering, and disconnect cleanup.

- **Commit `3a6de9b`**: `test(cli): verify duplex streams and terminal lifecycle end to end`
  - **Task Impact**: +20% to #3 (+3% overall), +40% to #4 (+6% overall), +40% to #8 (+4% overall); +13% overall.
  - **Summary**: All 15 C integration scenarios pass: outputs/exit status, exact arguments, cwd/environment, missing executable, 16 MiB duplex, signals, timeout, and termios restoration.

- **Commit `6eb81df`**: `test(cli): make fixture path bounds explicit`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Add an explicit socket-path capacity assertion before concatenation.

- **Commit `4ae2c21`**: `docs(cli): provide a runnable mock passthrough demonstration`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Add make demo and documented manual/interactive commands.

- **Commit `7b8ddaf`**: `fix(cli): default unknown terminal dimensions independently`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Use 24 rows/80 columns when terminal ioctl reports zero, on both spawn and resize.

- **Commit `1096703`**: `docs(tracker): audit final proof-of-concept verification`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Recorded initial PoC audit state.

- **Commit `2e9ad2e`**: `docs(proto): enforce PascalCase constants, type_t naming, and Doxygen in cli_protocol.h`
  - **Task Impact**: 0% across #1–#8 (retained 100% completion on #1 with full documentation).
  - **Summary**: Document all wire header structs, enums, constants, and functions using structured Doxygen blocks. Transition constant macros and enum members to strict PascalCase (WaddleCliMagic, WaddleMsgSpawnReq) and ensure enum types end in _t.

- **Commit `b716450`**: `feat(proto): modernize protocol codecs with queue_t, decoder_t, and defensive memory safety`
  - **Task Impact**: 0% across #1–#8 (retained 100% completion on #1 and #4).
  - **Summary**: Modernize queue and decoder structs into queue_t and decoder_t, format with 4-space indent, add Doxygen comments, and enforce defensive pointer nulling on queue_free, wire_consume, and wire_destroy.

- **Commit `b274a0e`**: `feat(cli): enhance arguments.c with defensive pointer nulling and PascalCase constants`
  - **Task Impact**: 0% across #1–#8 (retained 100% completion on #2).
  - **Summary**: Format CommandLineToArgvW inverse quoting and parsing with clean 4-space indent, defensively null pointers when freeing argument lists, and use PascalCase WaddleMaxPayloadSize.

- **Commit `9547743`**: `feat(cli): apply PascalCase constants, queue_t, and Doxygen to terminal subsystem`
  - **Task Impact**: 0% across #1–#8 (retained 100% completion on #3).
  - **Summary**: Update terminal function signatures to take queue_t, use PascalCase constants WaddleMsgTerminalResize and WaddleMsgSignalEvent, and add Doxygen documentation.

- **Commit `9479cfa`**: `feat(cli): refactor session event loop with queue_t and PascalCase constants`
  - **Task Impact**: 0% across #1–#8 (retained 100% completion on #4).
  - **Summary**: Transition session multiplexer queues to queue_t and decoder_t, adopt PascalCase constants, format with 4-space indent, and defensively free intermediate queues on all branches.

- **Commit `1b3ca16`**: `feat(mock): align mock guest and process management with coding standards`
  - **Task Impact**: 0% across #1–#8 (retained completion on #8).
  - **Summary**: Rename mock_process to mock_process_t with full Doxygen documentation, adopt PascalCase constants, format mock launcher and relay, and defensively manage file descriptors.

- **Commit `8ad9fe7`**: `feat(cli): modernize host CLI launcher with queue_t and PascalCase constants`
  - **Task Impact**: 0% across #1–#8 (retained 100% completion on #3).
  - **Summary**: Use queue_t for environment and transmission queues in host launcher, adopt PascalCase constants, defensively free and null pointers on all exit branches, and document static routines.

- **Commit `62a308f`**: `test(cli): align C unit and integration tests with naming and safety standards`
  - **Task Impact**: 0% across #1–#8 (retained completion on #8).
  - **Summary**: Update unit and integration tests to use queue_t, decoder_t, test_case_t, and PascalCase identifiers.

- **Commit `6a2b1db`**: `test(unit): expand unit coverage above 90% across protocol codecs and arguments`
  - **Task Impact**: +10% to #8 (+1% overall); +1% overall.
  - **Summary**: Add queue compaction, bounds checking, flush socket tests, stream helpers, empty payloads, and clean EOF handling. Protocol codec statement coverage reaches 92.5% and arguments reaches 91.9%.

- **Commit `b6d70bd`**: `test(zig): add native Zig test specification and integrate into build system`
  - **Task Impact**: +10% to #8 (+1% overall); +1% overall.
  - **Summary**: Add tests/test_cli.zig implementing 17 automated tests using native Zig test specs, checking wire layouts, endian codecs, CRC32, queues, quoting roundtrips, and std.testing.allocator zero-leak verification. Integrate zig-test into make test.

- **Commit `c488c7f`**: `docs(agents): enforce README maintenance and anti-vibe-coding policy`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Add explicit rule prohibiting README modification without direct user request, enforcing short and sweet format, and forbidding vibe-coded marketing prose.

- **Commit `f15c561`**: `docs(readme): add logo and streamline initial CLI passthrough PoC section`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Add logo.png to top right of title and trim the verbose CLI PoC section into a concise 3-command overview linking to feature implementation documents.

- **Commit `e8ee173`**: `docs(readme): enlarge logo and wrap intro text below heading line`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Increase logo dimensions to 180x180 (1.5x), place below H1 heading to eliminate line cutoff, and wrap introductory text cleanly with float clearing.

- **Commit `ccda5f1`**: `docs(cli): specify Windows guest and configurable export completion`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Define Win32 ownership, bounded Zig parsing, device ABI, path rules, and native verification gates.

- **Commit `b46475c`**: `feat(paths): add allocation-free Zig export validator`
  - **Task Impact**: +20% to #5 (+2% overall).
  - **Summary**: Define the borrowed rule ABI and bounds-checked UTF-8, component, drive-root and capacity validation. Native tests exercise parser failures and Unicode normalization. HEAD refers to the commit containing this entry and is resolved in the next commit.

- **Commit `cc62e84`**: `feat(paths): select exports through the host mapping API`
  - **Task Impact**: +20% to #5 (+2% overall).
  - **Summary**: Select longest component-boundary prefix, validate public rule inputs, preserve relative arguments, allocate and free output on failure, and link the Zig object into host targets. C unit suite passes; integration output failure is under investigation.

- **Commit `15ea478`**: `fix(build): link the Zig path object against libc`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Declare libc ownership to Zig so its runtime does not override the C process environment; all 16 Linux integration scenarios pass again.

- **Commit `901ed2e`**: `feat(cli): accept repeatable export mapping options`
  - **Task Impact**: +10% to #5 (+1% overall).
  - **Summary**: Accept at most 64 validated source=drive rules; reject normalized duplicate sources; map argv and cwd and free every owned source on all exits.

- **Commit `dd53ebe`**: `test(paths): verify export selection and invalid API inputs`
  - **Task Impact**: +10% to #5 (+1% overall).
  - **Summary**: Verify longest-prefix selection, component boundaries, root fallback, missing exports, NULL pointers, invalid syntax, rule limits and relative-path preservation.

- **Commit `34edd4d`**: `feat(guest): validate bounded wire headers in Zig`
  - **Task Impact**: +15% to #7 (+1.5% overall).
  - **Summary**: Add allocation-free header validation before payload allocation, enforce direction/session/sequence/size, and provide CRC verification; native adversarial header tests pass.

- **Commit `062a91f`**: `feat(guest): validate spawn views and executable selection`
  - **Task Impact**: +20% to #6 (+4% overall).
  - **Summary**: Add allocation-free UTF-8 spawn validation, canonical quoted argument grammar, explicit argv[0] extraction, environment entry checks and ConPTY dimension bounds; truncation and malformed-input tests pass.

- **Commit `dcfd880`**: `feat(guest): build owned Unicode child environments`
  - **Task Impact**: +15% to #6 (+3% overall).
  - **Summary**: Convert UTF-8 to owned UTF-16 and copy inherited environment into a sorted child-only block with case-insensitive last-wins overrides. Strict Windows cross-compilation passes; native execution pending.

- **Commit `f8e5a05`**: `feat(guest): launch raw pipe children with restricted inheritance`
  - **Task Impact**: +25% to #6 (+5% overall).
  - **Summary**: Create separate child streams with an explicit handle list, launch suspended, assign a kill-on-close job before resume, and release all handles and allocations on failure. Strict Windows cross-compilation passes.

- **Commit `748871e`**: `feat(guest): attach interactive children to ConPTY`
  - **Task Impact**: +25% to #6 (+5% overall).
  - **Summary**: Add noninheritable ConPTY communication pipes and pseudoconsole startup attribute; retain merged stdout, disable raw handle inheritance, and close pseudoconsole resources on failed launch. Strict Windows object build passes.

- **Commit `e4e1a1d`**: `feat(guest): validate stream controls and stdin EOF state`
  - **Task Impact**: +10% to #7 (+1% overall).
  - **Summary**: Validate reserved bytes, exact payload lengths, stream chunk bounds, EOF ordering, supported signals and signed terminal resize dimensions without allocation; native state tests pass.

- **Commit `14c0954`**: `feat(guest): serialize framed Winsock transport`
  - **Task Impact**: +15% to #7 (+1.5% overall).
  - **Summary**: Add bounded exact receives with preallocation header validation and CRC, serialized header/body sends with wraparound sequence assignment, and a failure event that shuts down blocked socket operations. Strict Windows object build passes.

- **Commit `8edc3bc`**: `feat(guest): preserve output while cancelling input receives`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Introduce atomic orderly-input cancellation and receive-half shutdown so normal child teardown can wake input without marking transport failure or interrupting output drains.

- **Commit `edceab5`**: `feat(guest): forward stdin and process controls`
  - **Task Impact**: +15% to #7 (+1.5% overall).
  - **Summary**: Implement a fixed-buffer input worker for stdin EOF, signed ConPTY resize, ETX/CTRL_BREAK interrupts, job termination and heartbeat replies. Treat child stdin closure and orderly cancellation separately from failures. Strict Windows object build passes.

- **Commit `ff19f3e`**: `feat(guest): drain bounded output streams before EOF`
  - **Task Impact**: +10% to #7 (+1% overall).
  - **Summary**: Add one fixed-buffer reader per output pipe, separate raw streams and immediate merged-mode stderr EOF. Continue draining after transport failure so ConPTY shutdown cannot deadlock on its final output. Strict Windows object build passes.

- **Commit `f2831d8`**: `feat(guest): order spawn, worker teardown and exit notifications`
  - **Task Impact**: +10% to #7 (+1% overall).
  - **Summary**: Dispatch one validated spawn, send success before workers, monitor child/failure, terminate descendants, cancel and join input, close ConPTY while output drains, join readers, then send exit. Release session resources on every path; strict cross-build passes.

- **Commit `cff37e5`**: `feat(guest): parse listener ports with bounds-checked Zig`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Validate decimal listener ports without C string-to-integer parsing; reject zero, signs, whitespace and overflow. Native tests cover the full unsigned boundary.

- **Commit `3f37428`**: `feat(guest): listen on native Viosock and Windows UNIX sockets`
  - **Task Impact**: +10% to #7 (+1% overall).
  - **Summary**: Query the installed Viosock family through its device IOCTL, bind the independently declared 12-byte native ABI, provide AF_UNIX local testing, and dispatch sequential sessions with noninheritable sockets and handshake deadlines. Strict Windows object build passes.

- **Commit `HEAD`**: `fix(guest): preserve a completed child exit code of 259`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Use process-handle signaled state to distinguish running children instead of treating STILL_ACTIVE as a forbidden exit code; completed Windows exit 259 remains valid.

## Verification snapshot

- **Strict Naming Invariants**: All constants in `PascalCase` (`WaddleCliMagic`, `WaddleMsgSpawnReq`, `WaddleMaxPayloadSize`), types end in `_t` (`waddle_cli_msg_header_t`, `queue_t`, `decoder_t`, `mock_process_t`), default identifiers in `snake_case`, and zero `camelCase`.
- **Mandatory Documentation**: Complete structured Doxygen documentation (`/** ... */`) across all headers, structs, enums, macros, and public functions with parameter directions (`in`/`out`), nullability, memory ownership annotations, and thread safety invariants.
- **Zig Native Test Specifications**: `tests/test_cli.zig` implements 17 automated tests using native `test "..."` specifications, `@cImport`, and `std.testing` assertions. Tests pass cleanly with zero external framework dependencies.
- **Enforced Code Coverage**: Line coverage for protocol codecs and arguments exceeds the mandatory 90% threshold: `protocol.c` achieves 92.50% line coverage and 93.10% branch coverage; `arguments.c` achieves 91.87% line coverage and 100.00% branch coverage.
- **Zero Memory Leaks**: Symmetric alloc/free lifecycle functions with defensive pointer nulling (`free(p); p = NULL;`). Verified with zero bytes leaked under `std.testing.allocator` and under full test runs with LLVM AddressSanitizer and LeakSanitizer (`-fsanitize=address,leak,undefined`).
- **Integration Test Suite**: All 16 C integration scenarios pass, including 16 MiB duplex streaming, terminal resize, signal traps, error recovery, adversarial peers, and termios restoration.
- **Runnable Demo**: `make demo` runs the mock passthrough demonstration cleanly.
