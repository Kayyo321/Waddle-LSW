# Feature Tracker: Owner-authorized Claude PR review

- **Contributors / Agents**: Codex
- **Time Started**: 2026-10-21T16:13:37Z
- **Time Ended**: TBD
- **Feature Branch**: feature/claude-pr-review
- **Target Merge Branch**: origin
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Specify authorization, static inspection, and operator instructions | Done | 20% | 100% | Verified repository/account IDs and CLI capabilities |
| #2 | Implement workflow, controller, and project review prompt | Done | 50% | 100% | Owner-only requests; immutable source; isolated credentials; validated verdict |
| #3 | Verify authorization, stale requests, failures, and output | Pending | 30% | 0% | Local mocks; first live paid run after merge |

**Total Feature Completion**: `70.0%`

## Commit History & Progress Log

- **Commit `4821ea86824535e4a4593bb1cac9f99bff494252`**: `docs(ci): specify owner-authorized Claude PR reviews`
  - **Task Impact**: +100% to TODO #1 (+20% overall).
  - **Summary**: Define event gates, credential boundaries, immutable source,
    verdict validation, error handling, and activation procedure.

- **Commit `HEAD`**: `feat(ci): add owner-authorized static Claude PR reviews`
  - **Task Impact**: +100% to TODO #2 (+50% overall).
  - **Summary**: Add the identity-gated workflow, immutable regular-blob snapshot,
    static project prompt, separate credential phases, and checked verdict posting.
  - **Verification**: Bash syntax, ShellCheck, workflow structure checks, and 38
    offline cases pass before commit; the test suite is added in the next commit.

The newest ledger entry uses the Git revision `HEAD` to identify its own commit;
embedding that commit's hash in its own contents is self-referential. The next
commit resolves the previous entry to its concrete hash. `git log` supplies the
newest entry's exact hash without creating an endless series of ledger commits.
