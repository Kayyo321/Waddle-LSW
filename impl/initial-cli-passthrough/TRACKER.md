# Feature Tracker: Initial CLI Passthrough

- **Contributors / Agents**: Antigravity Agent, Codex
- **Time Started**: 2026-10-04T18:15:38Z
- **Time Ended**: TBD
- **Feature Branch**: feature/initial-cli-passthrough
- **Target Merge Branch**: origin
- **Current Overall Status**: Review

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1      | Define shared C wire protocol header (`include/waddle/cli_protocol.h`) | Done | 10% | 100% | Packed structs, PascalCase constants, type_t naming, Doxygen |
| #2      | Implement Win32 `CommandLineToArgvW` inverse escaping generator and unit tests | Done | 10% | 100% | Deterministic escaping for spaces, quotes, backslashes; 91.9% coverage |
| #3      | Implement host CLI argument parser, `termios` raw mode manager, and signal trap | Done | 15% | 100% | Host CLI and terminal behavior verified by integration/PTY tests |
| #4      | Implement host non-blocking multiplexer event loop (`poll`) and stream handlers | Done | 15% | 100% | Bounded queue_t stream loop, decoder_t, zero leak verification |
| #5      | Implement path translation engine (Linux host paths to guest VirtIO-FS drives) | Done | 10% | 100% | Zig path rules and C mapping regressions pass |
| #6      | Implement Windows guest console execution agent with ConPTY and pipe support | In Progress | 20% | 95% | 47 native Windows scenarios pass; real VM interactive acceptance and Windows heap instrumentation remain |
| #7      | Implement guest asynchronous I/O pump and VSOCK listener/dispatcher | In Progress | 10% | 95% | Native duplex/cancellation/handle stress pass; real Linux-to-Windows Viosock/export acceptance remains |
| #8      | Build loopback mock test harness, unit tests, and integration test suite | Done | 10% | 100% | 16 C integration scenarios, 17 Zig native tests, >90% coverage, ASan/LSan clean |

**Total Feature Completion**: `98.5%`

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

- **Commit `25893ed`**: `fix(guest): preserve a completed child exit code of 259`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Use process-handle signaled state to distinguish running children instead of treating STILL_ACTIVE as a forbidden exit code; completed Windows exit 259 remains valid.

- **Commit `7f64ef8`**: `chore(build): add the standalone Windows guest target`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Build the complete x86_64 Windows guest from C native modules and a ReleaseSafe Zig codec with strict warnings and system Winsock imports. Include guest parser tests in make zig-test. Full Windows executable cross-link passes.

- **Commit `1e7f12d`**: `fix(guest): resolve bare executable names before process creation`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Resolve bare argv[0] through SearchPathW before passing a nonnull application name to CreateProcessW; preserve explicit application selection and release the owned resolution buffer on errors.

- **Commit `36d0312`**: `test(guest): add native Windows pipe regressions`
  - **Task Impact**: 0% across #1–#8 (0% overall; runtime verification pending).
  - **Summary**: Add a native Windows AF_UNIX fixture validating separate streams, full exit codes, canonical Unicode argv, Unicode cwd, last-wins environment, missing programs and executable search. The fixture cross-builds; runtime execution remains pending.

- **Commit `2ca555e`**: `test(guest): stress Windows binary duplex backpressure`
  - **Task Impact**: 0% across #1–#8 (0% overall; runtime verification pending).
  - **Summary**: Add a concurrent 5 MiB stdin writer while the child emits 6 MiB before consuming input; verify all 16 MiB of raw binary streams and EOF ordering. Fixture cross-build passes.

- **Commit `b03db3e`**: `test(guest): verify raw signals and disconnect cleanup`
  - **Task Impact**: 0% across #1–#8 (0% overall; runtime verification pending).
  - **Summary**: Exercise CTRL_BREAK interrupt handling, whole-job SIGTERM and disconnect while a child is running; observe child process handles to require actual termination before repeat sessions.

- **Commit `99049ce`**: `test(guest): exercise native ConPTY resize and merged output`
  - **Task Impact**: 0% across #1–#8 (0% overall; runtime verification pending).
  - **Summary**: Launch a real ConPTY child, send terminal resize and stdin EOF, require merged stdout and stderr EOF before the exit frame. Strict native fixture cross-build passes.

- **Commit `614031d`**: `test(guest): audit repeated session handle stability`
  - **Task Impact**: 0% across #1–#8 (0% overall; runtime verification pending).
  - **Summary**: Run 32 additional sessions against the same listener and require native process handle counts to remain stable after teardown; strict fixture cross-build passes.

- **Commit `3e33cfa`**: `chore(test): expose the native Windows regression target`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Add make windows-test for native execution of the cross-buildable AF_UNIX regression suite, with strict test compilation and explicit dependencies.

- **Commit `f63f27d`**: `ci(guest): run regressions on a native Windows runner`
  - **Task Impact**: 0% across #1–#8 (0% overall; first CI run pending).
  - **Summary**: Add Windows 2022 CI with a checksum-pinned Zig 0.13 toolchain, strict guest/fixture builds, native parser allocator tests and AF_UNIX regressions; initialize any repository submodules recursively.

- **Commit `ed016d5`**: `fix(build): link Windows compiler runtime at the executable boundary`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Native Windows CI exposed Zig 0.13 COFF build-obj inability to merge compiler-runtime objects. Disable object-level runtime emission and let zig cc provide it at final link; complete Linux-to-Windows cross-build passes.

- **Commit `962dd9e`**: `fix(guest): detect disconnect while stdin is backpressured`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Monitor exceptional socket conditions in the main child wait loop without consuming input frames, so pipe writes cannot hide peer disconnection; session failure cancels blocked stdin and kills the child tree.

- **Commit `28e970f`**: `fix(build): package the Windows Zig codec as a static library`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Zig 0.13 native COFF still emits multiple objects with libc despite disabling compiler runtime; package the parser using build-lib -static, which supports the native archive boundary. Cross-linked guest and fixture pass.

- **Commit `6a2bb8b`**: `test(guest): disconnect while the child refuses stdin`
  - **Task Impact**: 0% across #1–#8 (0% overall; native CI pending).
  - **Summary**: Fill a sleeping child stdin pipe with a complete 16 KiB frame, then close transport and require the child wait handle to signal within 15 seconds; verifies main socket monitoring and synchronous-I/O cancellation.

- **Commit `69f1664`**: `test(cli): verify path option grammar and cleanup`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Exercise missing/invalid mappings, duplicate normalized sources, 65-rule rejection, unmatched argv/cwd and valid mapping through connection setup. Run in make test, including sanitizer builds; all cases pass.

- **Commit `02ece7d`**: `fix(build): keep libc out of the pure Zig Windows archive`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: The allocation-free Windows parser uses no C library symbols; defer libc entirely to final C executable linking, preventing native Zig from adding imported libraries to its parser archive. Strict cross-build passes; native rerun pending.

- **Commit `2bca738`**: `test(coverage): enforce implementation coverage for C and Zig parsers`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Add make coverage using gcov and base-package kcov; reset counters, exclude native test bodies and fail below 90%. Measured protocol 92.50%, arguments 93.18%, guest parsing 98.02% and path parsing 100%.

- **Commit `96cc34e`**: `fix(ci): use native Windows paths for Zig archive emission`
  - **Task Impact**: 0% across #1–#8 (0% overall; native rerun pending).
  - **Summary**: Use quoted native separator paths at Windows compiler output/input boundaries and enable linker diagnostics to resolve native archive emission errors; Linux cross-builds remain passing.

- **Commit `6f52f6d`**: `docs(cli): document implemented guest ownership and verification boundaries`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Specify actual module ownership, native ABI sizes, cancellation and EOF semantics, executable lookup, environment bounds, reproducible test/coverage commands and real VSOCK VM acceptance steps.

- **Commit `1ac04f1`**: `docs(guest): document entry-point ownership and include guards`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Complete structured entry-point argument/lifetime/status documentation and document new public include-guard macros; strict Windows build passes.

- **Commit `70b5e6d`**: `chore(style): format the new bounded Zig parsers`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Apply Zig 0.13 canonical formatting to the feature parser sources; guest and path native suites remain passing.

- **Commit `37382c0`**: `chore(ci): separate native compilation from runtime verification`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Report Windows compiler and runtime phases as distinct CI steps and bound the regression execution to three minutes; keep the existing whole-job build deadline.

- **Commit `11b7fac`**: `fix(cli): report rejected path mappings on stderr`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Route argument/cwd translation errors through the existing local-error diagnostic and cleanup instead of silently returning 125; path option regression statuses remain passing.

- **Commit `7951443`**: `docs(impl): synchronize IMPL_DESC and TRACKER with coding standards and test coverage`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: - Update protocol data structures and constants in IMPL_DESC.md to PascalCase. - Add section 8.6 to IMPL_DESC.md covering documentation, Zig tests, and zero-leak gating. - Update TRACKER.md task #8 to 100% completion (advancing overall feature completion to 64.0%). - Record all atomic refactoring and testing commits in TRACKER.md commit history log. - Document verified 92.5% protocol line coverage, 17 Zig native tests, and zero memory leaks.

- **Commit `da1c447`**: `Added "logo.png"`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Historical documentation/asset change predating this completion pass; no implementation task progress.

- **Commit `871951c`**: `docs(tracker): log README rule and documentation refinement commits`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: - Record commit c488c7f (AGENTS.md README maintenance and anti-vibe-coding rule). - Record commit f15c561 (README.md logo and PoC section streamlining).

- **Commit `c8ab9a4`**: `docs(tracker): record README logo layout and wrap enhancement commit`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Historical documentation/asset change predating this completion pass; no implementation task progress.

- **Commit `d5ff661`**: `Updated "README.md"`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Historical documentation/asset change predating this completion pass; no implementation task progress.

- **Commit `b8dcd69`**: `docs(tracker): audit omitted historical feature commits`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Record five prior documentation/asset/tracker commits missing from the branch history log, with explicit zero implementation progress impact; no README changes made in this completion pass.

- **Commit `c4f7f26`**: `test(guest): reject corrupt handshakes on native transport`
  - **Task Impact**: 0% across #1–#8 (0% overall; runtime verification pending).
  - **Summary**: Send bad magic, a payload above 1 MiB and a corrupt CRC to the real listener; require connection closure before subsequent valid sessions. Native fixture cross-build passes.

- **Commit `7bf2138`**: `test(guest): report native regression stages as they execute`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Native compilation passes but runtime stalls; emit unbuffered spawn/completion stages and include scenario number in errors so the blocking boundary is visible in CI logs.

- **Commit `41bf2ce`**: `test(guest): bound native regression hangs independently of sockets`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Add a 60-second watchdog event/thread with explicit join and handle cleanup on success; timeout terminates listener and fixture even if a Windows AF_UNIX operation does not honor socket timeout.

- **Commit `6b108e3`**: `fix(guest): make receive cancellation independent of socket shutdown`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: After spawn, wait for read readiness in 100 ms slices and check the atomic stopping flag before recv. Initial handshake keeps its 30-second receive timeout; orderly input join no longer depends on AF_UNIX shutdown waking an existing blocked recv.

- **Commit `4d9b330`**: `chore(ci): enforce Linux sanitizer and parser coverage gates`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Add Linux CI with checksum-verified Zig, base-package kcov, ASan/LSan/UBSan and native allocator tests, 90% implementation coverage enforcement and complete Windows cross-builds.

- **Commit `661bef6`**: `fix(ci): use Debian base coverage packages in Linux verification`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Ubuntu 24.04 has no kcov package in its configured archive. Run the Linux job in Debian trixie with base build/coverage packages and ptrace permission; this matches the verified local kcov toolchain.

- **Commit `bc43aee`**: `feat(guest): adapt cmd.exe switches with a bounded Zig serializer`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Specify and implement allocation-free unquoting of recognized leading cmd.exe switches, stop at /c or /k and preserve all later shell bytes. Native tests cover switch cases, quoting, unknown options and capacity.

- **Commit `9184c70`**: `fix(guest): launch cmd.exe with recognized native switches`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Use the bounded compatibility serializer only when the resolved executable basename is cmd.exe; convert and free temporary UTF-8 storage before process launch, preserving explicit application selection. Strict cross-build passes.

- **Commit `3da51da`**: `fix(guest): relay duplex streams with nonblocking socket readiness`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Windows native large-duplex testing exposed blocking socket stalls. Enable nonblocking established transport, preserve partial frame offsets, handle would-block through bounded select waits and wake failed sends without growing queues; cross-build passes.

- **Commit `cf8f0dd`**: `test(guest): use a nonblocking full-duplex native client`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Make the native fixture socket nonblocking too; handle partial transfers with readiness waits and explicit deadlines, and wait for expected closure without racing would-block. Prevent client-side blocking calls from masking server duplex behavior.

- **Commit `0bcc6d6`**: `fix(guest): observe graceful EOF without consuming input frames`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Poll read readiness as well as exceptional socket state and use a bounded nonblocking peek to distinguish graceful EOF from queued data; tolerate a concurrent receive worker consuming readiness before the peek.

- **Commit `29f459f`**: `test(guest): locate duplex stalls with bounded progress output`
  - **Task Impact**: 0% across #1–#8 (0% overall).
  - **Summary**: Report the first stream/input chunk and one message per MiB during the native duplex fixture so provider stalls are attributed to a direction and byte boundary.

- **Commit `b7cb8c7`**: `fix(guest): bound native socket transfer chunks`
  - **Task Impact**: 0% across TODO #6 and #7; verification pending
  - **Summary**: Limit individual send requests to 4096 bytes while retaining frame serialization and partial-transfer offsets; exercise the same limit in the native peer.

- **Commit `b1fc46d`**: `fix(cli): order late resize signals before stdin frames`
  - **Task Impact**: 0% across TODO #3 and #8; existing behavior corrected
  - **Summary**: Collect pending terminal events after reading ready stdin and before framing its bytes, closing the signal-arrival race after poll. All Linux sanitizer, allocator and integration tests pass.

- **Commit `4c45f17`**: `fix(guest): shorten native socket readiness retry intervals`
  - **Task Impact**: 0% across TODO #6 and #7; verification pending
  - **Summary**: The native AF_UNIX duplex transfer advances only about 100 KiB/s with 100 ms readiness intervals. Retry at 1 ms while preserving nonblocking I/O, bounded buffers, failure observation and cancellation.

- **Commit `015ce22`**: `docs(cli): record measured verification and remaining guest gates`
  - **Task Impact**: 0% across #1–#8; correct verification evidence
  - **Summary**: Replace stale verification counts with measured sanitizer and coverage results; specify bounded socket retries and distinguish native handle checks from unsupported Windows LeakSanitizer and untested real VM acceptance.

- **Commit `e96b3bb`**: `test(guest): allow bounded output-first duplex backpressure`
  - **Task Impact**: 0% across TODO #6 and #7; native verification pending
  - **Summary**: Native output progresses through 3 MiB stdout and into stderr; the deliberate 6 MiB output-before-input phase exceeds the original 15-second writer deadline. Use 60-second frame deadlines, a 180-second independent watchdog and a five-minute CI step without relaxing byte checks.

- **Commit `cde9154`**: `docs(tracker): record passing native Windows and Linux verification`
  - **Task Impact**: +10% to #6 (+2% overall), +10% to #7 (+1% overall); total 98.5%
  - **Summary**: Record successful 47-scenario Windows execution with 77-to-77 handles and passing Linux sanitizer/coverage CI. Move to Review while retaining real VM/Viosock/export and Windows heap merge gates.

- **Commit `5cd1232`**: `test(guest): instrument native Windows session heap ownership`
  - **Task Impact**: 0% to #6 and #7 (0% overall; execution pending).
  - **Summary**: Add MSVC debug-heap client-allocation checkpoints across all native sessions and three detector negative controls; wire the native CI audit without claiming results before execution.

- **Commit `4b1342a`**: `fix(test): resolve forced heap header through MSVC include path`
  - **Task Impact**: 0% to #6 (0% overall).
  - **Summary**: The first native audit exposed MSVC forced-include lookup relative to the source unit; add the test header directory explicitly and request its basename.

- **Commit `29cdbba`**: `fix(test): link Zig safe-mode Windows runtime imports`
  - **Task Impact**: 0% to #6 (0% overall).
  - **Summary**: Link the system ntdll import library explicitly when MSVC links the safe-mode Zig codec, matching imports otherwise supplied by Zig cc.

- **Commit `90f3b3e`**: `test(guest): expose file and console fixtures for VM acceptance`
  - **Task Impact**: 0% to #6 and #7 (0% overall; VM execution pending).
  - **Summary**: Add deterministic guest child modes for actual exported-file read/write and interactive console input, dimensions, merged stderr, and Ctrl-C across Viosock.

- **Commit `c255d59`**: `test(cli): automate real Viosock and export acceptance`
  - **Task Impact**: 0% to #6 and #7 (0% overall; VM execution pending).
  - **Summary**: Add explicit real-VM acceptance for host/guest streams, mapped file access, 16 MiB duplex, reconnects, and bounded C PTY input/resize/interrupt/restoration checks.

- **Commit `HEAD`**: `test(guest): retain heap failure diagnostics in native CI`
  - **Task Impact**: 0% to #6 (0% overall).
  - **Summary**: All detector controls fire; the first instrumented session needs a diagnostic report. Preserve client counts and allocation dump output before fatal termination and print it when the fixture fails.

## Verification snapshot

- Linux `make test-sanitizers` passes: C unit cases, 16 integration scenarios, nine path-option cases and 25 Zig tests. ASan/LSan/UBSan report no errors; Zig allocator tests report no leaks.
- `make coverage` passes implementation line coverage gates: protocol.c 92.50% (185/200), arguments.c 93.18% (123/132), guest_codec.zig 98.56% (137/139), path_rules.zig 100% (50/50).
- `make windows build/windows_guest_test.exe` passes strict Windows x86_64 cross-compilation. [Native Windows CI run 37239865276](https://github.com/Kayyo321/Waddle-LSW/actions/runs/37239865276) passes all 47 scenarios, including 16 MiB duplex, signals, malformed handshakes, blocked-input cancellation, ConPTY and 32 repeated sessions. Listener handle counts remain 77 → 77. [Linux CI run 37239865243](https://github.com/Kayyo321/Waddle-LSW/actions/runs/37239865243) passes sanitizer, allocator, coverage and cross-build gates.
- Real Linux-to-Windows Viosock, interactive VM operation and mounted export access remain unverified. See IMPL_DESC.md section 10.3 for concrete acceptance commands.
- Zig 0.13 x86_64-windows-gnu does not support LeakSanitizer. Native handle stress cannot replace Windows heap instrumentation; this remains a merge verification gate.
- New owned public interfaces have structured ownership/bounds/error/thread documentation. Existing repository-wide naming and documentation claims have not been extended into an unaudited universal assertion.
- No README change or dependency addition was made in this completion pass.
