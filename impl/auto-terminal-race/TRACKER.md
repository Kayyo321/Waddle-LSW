# Feature Tracker: CLI terminal-delivery race

- **Contributors / Agents**: diagnose_auto_terminal_race; independent review by review_cli_shutdown_ordering
- **Time Started**: 2026-10-10T00:43:18Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: main
- **Current Overall Status**: Review

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Identify and deterministically reproduce terminal delivery race | Done | 20% | 100% | Actual unmodified production session emits marker then EPIPE125 with complete ProcessExit(0) already queued |
| #2 | Implement bounded directional shutdown handling | Done | 35% | 100% | Fixed 2s absolute cap, earlier explicit deadline, no write retry or protocol relaxation |
| #3 | Add deterministic regression and exact-status bounded stress | Done | 25% | 100% | 15 socketpair cases and 512 stress sessions; eight full CLI cases plus 32 strict real CLI/mock sessions |
| #4 | Independent review and final local verification | Done | 10% | 100% | Independent normal/UBSan 15-case and 512-stress passes; final named-fixture addendum reviewed before commit |
| #5 | Native named-socket and leak-enabled sanitizer qualification | Pending | 10% | 0% | Locally blocked by socket EPERM and LSan ptrace limitation; parent owns exact-revision CI |

**Total Feature Completion**: `90.0%`

## Commit History & Progress Log

- Implementation commit: pending atomic commit. Tasks #1 through #4 account for
  +90 percentage points. The follow-up verification receipt will record its exact
  returned commit hash and final review result.

## Verification receipts

- Original production session blob: `a52787c37600b23129aab22a5338c5bc9ff04303`.
  Anonymous socketpair reproduction returned 125 with EPIPE despite complete valid
  ProcessExit(0) buffered; fixed production session returned zero.
- Final plain and separately UBSan-only runs: all 15 cases and 512 stress cases
  passed, including the ACK scheduling correction. The independent reviewer rebuilt
  and passed those same final normal/UBSan commands without diagnostics.
- Eight concurrent workers completed 4096 exact-status production sessions with
  zero failures before the two added local-output backpressure cases; every worker
  exit was checked. This was production-session socketpair stress, not a claim of
  full real CLI/mock execution.
- Full CLI, integration fixture, and final auto-terminal fixture compile under
  strict warning-as-error flags. The 32-session named-socket loop has not run
  successfully in this executor and remains a native CI requirement.
- Actual ASan+LSan+UBSan binary with `detect_leaks=1`, abort/halt enabled: blocked
  at child shutdown by LeakSanitizer's ptrace runtime error. No suppressed pass.
- Original named-socket auto-terminal fixture: startup blocked by EPERM on socket
  creation, both before and after requesting ordinary execution escalation. No
  further escalations or security setting changes; zero such failures are counted
  as race evidence. Real CLI fixture execution remains a native CI requirement.
