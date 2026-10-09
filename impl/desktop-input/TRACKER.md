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
| #5 | Boundary fixtures, sanitizers and build verification | Pending | 15% | 0% | Record actual commands/results |
| #6 | Real compositor/Windows workflow qualification | Blocked | 10% | 0% | No Windows, GPU or KVM in cloud |

**Total Feature Completion**: `75.0%`

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

- **Commit `HEAD`**: `feat(av): connect Wayland seat input to tracked guest injection`
  - **Task Impact**: +100% to #3 and #4 (+40% overall).
  - **Summary**: Adds keyboard/pointer seat lifecycle and callbacks, focus-aware
    coordinates, held-enter suppression, bounded queue failure and teardown
    behavior. Guest validates observable HWND incarnation/process/foreground,
    applies SendInput, releases held state on removal/foreground loss/disconnect,
    and retains/restores per-monitor-v2 thread DPI context. Socket and callback
    fixtures pass; Linux host and Windows guest/fixture build with warnings as
    errors. Native `--input` fixture is supplied but not executed in cloud.
