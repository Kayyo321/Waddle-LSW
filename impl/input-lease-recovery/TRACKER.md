# Feature Tracker: Revocable input leases and focus recovery

- **Contributors / Agents**: AV implementation and integration contributors
- **Time Started**: 2026-10-10T00:37:13Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: main
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Freeze repository-native protocol and safety specification | Done | 10% | 100% | Minimal epoch/serial/incarnation contract |
| #2 | Implement codec, compatibility fixtures and portable lease/input core | Done | 25% | 100% | Codec 100% lines / 95.89% branches; lease 99.19% / 92.47%; input 100% / 97.32% |
| #3 | Integrate guest native observations, export and ordered authorization | In Progress | 25% | 0% | V2 entry preserved; V3 startup not enabled yet |
| #4 | Integrate host barrier, suppression and pointer synchronization | In Progress | 20% | 0% | Separate callback and no focus-result machinery |
| #5 | Automated aggregates, coverage and independent review | Pending | 15% | 0% | Required before publication |
| #6 | Authorized physical Windows/Wayland acceptance | Pending | 5% | 0% | Hardware and isolated interactive desktop unavailable here |

**Total Feature Completion**: `35.0%`

## Commit History & Progress Log

- Commit `8b5232c6c56028a5a003df9e37faddae900a4846`: `feat(av): add canonical epoch protocol and portable lease authority`: #1 +100% (+10% overall), #2 +100% (+25% overall). Nine Zig codec tests, existing input/identity/lifecycle fixtures, new portable lease fixtures, frozen V2 parser checks, and unchanged >=90% production coverage gates pass. Guest opt-in and production adapter changes are deliberately excluded from this checkpoint. Native acceptance remains pending.

- Core test-wiring checkpoint (`test(av): wire lease coverage and frozen V2 compatibility gate`, exact receipt follows): #2 unchanged at 100%. Adds the production lease/frozen V2 fixture to both AV aggregates and coverage without runtime V3 opt-in; adds fragmented Ack, ordered focus/data, cross-direction stale FIFO, partial EOF, queue exhaustion and fresh-session regression evidence.
