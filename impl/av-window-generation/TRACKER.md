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
| #2 | Implement codec, portable gate and native/host wiring | Pending | 35% | 0% | No frame incarnation claim |
| #3 | Test identity, transport and callback races | Pending | 25% | 0% | Both legacy directions required |
| #4 | Qualify cloud builds, coverage, sanitizers and review | Pending | 15% | 0% | Latest CI required |
| #5 | Qualify physical Windows, compositor and RTX | Pending | 10% | 0% | Manual hardware gate |

**Total Feature Completion**: `15.0%`

## Commit History & Progress Log

- Initial design commit (this file's first commit): define bounded modern identity
  and legacy display-only contract. Task #1 +100% (+15% overall). Exact commit hash
  will be recorded in the following implementation commit.
