# Feature Tracker: Generation-safe AV window lifecycle

- **Contributors / Agents**: AV lifecycle implementation and independent reviewers
- **Time Started**: 2026-10-09T22:07:44Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: main
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Specify capability, identity, compatibility and ownership | Done | 15% | 100% | Independent protocol review approved |
| #2 | Implement codec, portable gate and native/host wiring | Done | 35% | 100% | No frame incarnation claim |
| #3 | Test identity, transport and callback races | Done | 25% | 100% | Frozen old codec/peer and Wayland doubles pass |
| #4 | Qualify cloud builds, coverage, sanitizers and review | In Progress | 15% | 60% | Latest CI required |
| #5 | Qualify physical Windows, compositor and RTX | Pending | 10% | 0% | Manual hardware gate |

**Total Feature Completion**: `84.0%`

## Evidence

- Portable identity: 100% production lines (25/25), branches (58/58), no exclusions.
- Zig codec: all eight tests pass, including exact 328-byte CreateV2 golden vector
  and malformed/truncated output-preservation tests.

## Commit History & Progress Log

- Commit `413c2c4`: define bounded modern identity
  and legacy display-only contract. Task #1 +100% (+15% overall).

- Commit `84bcb08`: Task #2 +30% (+10.5% overall), Task #3 +20%
  (+5% overall). Implements explicit CreateV2 and the production native effect
  gate; proves stale controls invoke zero callbacks.

- Commit `56b48be`: Task #2 +70% (+24.5% overall), Task #3 +80%
  (+20% overall), Task #4 +60% (+9% overall). Connects immutable identities to
  Windows/guest/Wayland, fixes terminal queue/EOF behavior and tests both real
  historical/new transport directions.

## Integration verification, 2026-10-09

- Full `make av-test av-input-test` passed with production codec, portable gate,
  frozen old encoder/decoder/peer, socket FIFO races, Wayland protocol-call double,
  retained busy buffers and terminal callback/queue/EOF regressions.
- Full C AV suite passed with ASan/UBSan and leak instrumentation compiled in;
  local execution used `ASAN_OPTIONS=detect_leaks=0` only because the separate
  `detect_leaks=1` attempt failed with the runner's explicit ptrace limitation.
  This is not zero-leak evidence; latest-commit CI remains required.
- Codec production coverage: 100% lines (84/84), 96.10% branches (148/154);
  existing nine compiler panic/stack-canary exclusions unchanged. Identity:
  100% lines (25/25), 100% branches (58/58), no exclusions.
- Strict Linux host link passed. Windows guest and native lifecycle fixture,
  including a same-numeric-HWND hide/readmit control race, cross-linked. The new
  production-guest capture-double quiescence executable also cross-linked.
- Native Windows CI execution of the new lifecycle/quiescence fixtures is pending.
  No physical Windows/compositor/RTX or direct GPU/WSI integration credit is taken.
- Independent protocol/runtime reviewer approved the core and current integration;
  exact-commit sign-off confirmed `56b48be4d6fc1052a32b20eda5f9fbbfeb62928a`.

## Exact-source receipt

- Source: `56b48be4d6fc1052a32b20eda5f9fbbfeb62928a`; core:
  `84bcb08272a4512731d3bf34ac1cf017c9a07b2d`. Independent correctness review
  verified this exact owned source and reran transport/Wayland fixtures.
- Final forced Linux host and all three Windows links passed on this source.
- Preserved cloud receipt: `build/av-window-generation/run-20261009T222502Z`.
  `manifest.json` SHA-256: `cd7e821e292a4e5823d133fed3725f8dc3500ed1399e01ef7f7daf8b325b696d`.
  It inventories source/executable hashes, test logs and explicit pending gates.
- Commit `dec6602` (receipt only): no implementation progress change (0%). Latest CI,
  native Windows execution and physical/manual acceptance remain open.

## Synthetic native-platform callback correction

- Review found that the modern CreateV2 native-platform fixture could receive
  focus/input callbacks while its old Geometry/Close-only assertion rejected them.
- The fixture now passes the same allocation-free callback tested by the CPU
  regression. It validates the synthetic identity, canonical fields and increasing
  input serial, records controls/input separately and rejects unexpected types.
  It never forwards or injects input into a guest.
- Strict callback CPU test and strict real native-platform executable link passed.
  Full AV/input suites passed again; the focused callback regression also passed
  ASan/UBSan with `detect_leaks=0`. Existing ptrace-blocked LSan and pending CI/native
  execution qualifications remain unchanged. No physical execution credit is taken.
- Independent review compiled and ran the exact shared callback regression and
  confirmed there are no production-source changes. Current fixture-correction
  commit changes no feature progress percentage (0%).
