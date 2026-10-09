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

- Current integration commit: Task #2 +70% (+24.5% overall), Task #3 +80%
  (+20% overall), Task #4 +60% (+9% overall). Connects immutable identities to
  Windows/guest/Wayland, fixes terminal queue/EOF behavior and tests both real
  historical/new transport directions. Exact hash is recorded in the next receipt.

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
  exact-commit sign-off follows the atomic integration commit.
