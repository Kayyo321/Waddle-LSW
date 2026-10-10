# Feature Tracker: Ordinary-window input milestone

- **Contributors / Agents**: dot / implement_desktop_input
- **Time Started**: 2026-10-09T19:58:33Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: origin
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:--|:--|:--|:--|:--|:--|
| #1 | Define input contract and limitations | Done | 10% | 100% | Scope intentionally narrower than roadmap B |
| #2 | Canonical codec and portable input ownership | Done | 25% | 100% | Core line/branch coverage 100%; codec lines 100%, branches 96.64% |
| #3 | Bind Wayland seat and host callbacks | Done | 20% | 100% | Headless callback/focus/coordinate fixtures pass; real compositor separate |
| #4 | Connect guest tracked-window injection and cleanup | Done | 20% | 100% | Guest and native fixture cross-compile; SendInput execution unverified |
| #5 | Boundary fixtures, sanitizers and build verification | Done | 15% | 100% | Required leak-enabled Linux CI passed at published 1de3fdd; native Windows component CI and bundle assembly also passed |
| #6 | Real compositor/Windows workflow qualification | Blocked | 10% | 0% | No Windows, GPU or KVM in cloud |

**Total Feature Completion**: `90.0%`

## Commit History & Progress Log

- **Commit `0df71a8`**: `docs(av): specify ordinary-window input contract`
  - **Task Impact**: +100% to #1 (+10% overall).
  - **Summary**: Defines canonical wire fields, window incarnations, bounded held
    state, cleanup ownership and unqualified physical/desktop behavior. HEAD
    denotes the containing commit until resolved by the next atomic commit.

- **Commit `1585dac`**: `feat(av): validate bounded input and track held ownership`
  - **Task Impact**: +100% to #2 (+25% overall).
  - **Summary**: Adds canonical input IDs, per-window incarnations/session serials,
    physical scan mapping, allocation-free held state, stale-focus no-ops and
    fallible release retry. Portable and Zig boundary/stress tests pass. Initial
    measured core coverage 100% lines/branches; codec 100% lines, 96.64% branches.
    Adds repeatable coverage gate and overridable platform protocol directory.

- **Commit `7894608`**: `feat(av): connect Wayland seat input to tracked guest injection`
  - **Task Impact**: +100% to #3 and #4 (+40% overall).
  - **Summary**: Adds keyboard/pointer seat lifecycle and callbacks, focus-aware
    coordinates, held-enter suppression, bounded queue failure and teardown
    behavior. Guest validates observable HWND incarnation/process/foreground,
    applies SendInput, releases held state on removal/foreground loss/disconnect,
    and retains/restores per-monitor-v2 thread DPI context. Socket and callback
    fixtures pass; Linux host and Windows guest/fixture build with warnings as
    errors. Native `--input` fixture is supplied but not executed in cloud.

- **Commit `55d24fb`**: `ci(av): verify desktop input on feature branch`
  - **Task Impact**: +0%; required CI is configured, not executed here.
  - **Summary**: Parent-owned workflow wiring adds the input core to Windows
    sources/fixtures and this feature branch to AV checks. No native evidence.

- **Commit `7ac4306`**: `test(av): record input verification and native DPI assertions`
  - **Task Impact**: +80% to #5 (+12% overall).
  - **Summary**: Four of five equally weighted cloud verification parts passed:
    normal boundary/regression tests, Linux/Windows builds, source coverage,
    supplemental ASan/UBSan. The fifth, required LSan, is blocked by runtime
    ptrace restrictions and remains required. Native input and full desktop
    qualification remain blocked. Exact commands/results are in EVIDENCE.md.

- **Commit `HEAD`**: `docs(av): correct measured cloud compiler version`
  - **Task Impact**: +0%; verification status unchanged.
  - **Summary**: Corrects the evidence compiler version to the actual `gcc
    --version` result, Debian GCC 14.2.0. No source behavior changes.

- **Containing receipt commit; CI source `1de3fddf13067238ca4e428c223338ca79e37287`**:
  - **Task Impact**: +20% to #5 (+3% overall, now 90%). No #6 credit.
  - **Summary**: Both AV workflows completed successfully. Linux jobs executed
    `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 make -B av-test`
    with `-fsanitize=address,leak,undefined`, including input ownership/replay/
    failure/release and socket framing/full-queue disconnect/fresh-session tests.
    Input coverage remained 78/78 lines and 104/104 branches (100%). Native
    Windows component jobs and complete distribution assembly passed.
  - **Evidence**: [PR Linux job](https://github.com/Kayyo321/Waddle-LSW/actions/runs/37995902338/job/114043280601),
    [push Linux job](https://github.com/Kayyo321/Waddle-LSW/actions/runs/37995897492/job/114043284899).
    This supplies the previously missing AV LSan evidence at that exact source;
    the cloud executor's local ptrace restriction remains true. Neither native
    component CI nor mocks establish manual `--input`, live Wayland integration,
    full desktop behavior, or physical RTX sharing. Those remain pending.
