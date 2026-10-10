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

- **Commit `7a0dbd1d279fd5fe99bd3e520c42295162b3a38d`**:
  `fix(cli): drain terminal frames after peer write shutdown`
  - **Task Impact**: +100% to tasks #1, #2, #3, and #4 (+90% overall).
  - **Summary**: Fixed bounded remote-result draining; added deterministic,
    exact-status stress and full-CLI regressions; hardened natural peer lifecycle;
    documented diagnosis and explicit native-runtime qualification limits.
- **Verification receipt (this documentation-only update)**:
  - **Task Impact**: +0%; binds the reviewed implementation hash and final source
    manifest. Native qualification remains pending; no source or test behavior
    changes are included.

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

## Final reviewed source manifest

Independent reviewer `review_cli_shutdown_ordering` approved the session, protocol
ordering, lifetime/error paths, fixed remote-result deadlines, preserved local
output backpressure, and observation-only wrappers. Its final normal and UBSan
runs passed all 15 cases and 512 stress sessions. A source-only addendum approved
the final 32-session real CLI/mock fixture, with independent strict compilation.
The named-socket run and its 20-second aggregate budget remain native CI checks.

| File | SHA-256 |
|:-----|:--------|
| `GNUmakefile` | `cf9c9604dcbc71ca4a8bc8759cdcbb8e0b44cbfe8dddd62254f9a469343e5eea` |
| `src/cli/session.c` | `66ae63fbc94903cf55456822bcb0226d39a322c49770845628065effd1f3faa0` |
| `src/cli/session.h` | `11c43300e88f70832d053c798dd68513a73eca30bc6d1a5136e599149f658ad4` |
| `tests/cli/session_shutdown.c` | `87349148b8bd04f0fed46d88a87aed245be11088c60a28ad3c7738b1a2aeb86e` |
| `tests/integration/integration.c` | `5d3955c6f203a9b3526d052a5de18d4db6b882ec5f116d61ac8804f3391994bd` |
| `tests/daemon/test_auto_terminal.c` | `d3dd52803cd55ab8e2b099b53c4b64d52814f8173f28c06cfa2e171442c1b99a` |

Native CI receipt: pending parent publication and exact-revision qualification.
