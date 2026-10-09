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
| #1 | Specify capability, identity, compatibility and ownership | Done | 15% | 100% | Independent review pending |
| #2 | Implement codec, portable gate and native/host wiring | In Progress | 35% | 30% | No frame incarnation claim |
| #3 | Test identity, transport and callback races | In Progress | 25% | 20% | Both legacy directions required |
| #4 | Qualify cloud builds, coverage, sanitizers and review | Pending | 15% | 0% | Latest CI required |
| #5 | Qualify physical Windows, compositor and RTX | Pending | 10% | 0% | Manual hardware gate |

**Total Feature Completion**: `30.5%`

## Evidence

- Portable identity: 100% production lines (25/25), branches (58/58), no exclusions.
- Zig codec: all eight tests pass, including exact 328-byte CreateV2 golden vector
  and malformed/truncated output-preservation tests.

## Commit History & Progress Log

- Commit `413c2c4`: define bounded modern identity
  and legacy display-only contract. Task #1 +100% (+15% overall).

- Current codec/identity commit: Task #2 +30% (+10.5% overall), Task #3 +20%
  (+5% overall). Implements explicit CreateV2 and the production native effect
  gate; proves stale controls invoke zero callbacks.
