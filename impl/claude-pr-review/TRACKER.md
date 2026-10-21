# Feature Tracker: Owner-authorized Claude PR review

- **Contributors / Agents**: Codex
- **Time Started**: 2026-10-21T16:13:37Z
- **Time Ended**: 2026-10-21T16:23:23Z
- **Feature Branch**: feature/claude-pr-review
- **Target Merge Branch**: origin
- **Current Overall Status**: Review

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Specify authorization, static inspection, and operator instructions | Done | 20% | 100% | Verified repository/account IDs and CLI capabilities |
| #2 | Implement workflow, controller, and project review prompt | Done | 50% | 100% | Owner-only requests; immutable source; isolated credentials; validated verdict |
| #3 | Verify authorization, stale requests, failures, and output | Done | 30% | 100% | 47 offline cases, repeated three times; syntax, ShellCheck, actionlint pass |

**Total Feature Completion**: `100.0%`

## Commit History & Progress Log

- **Commit `4821ea86824535e4a4593bb1cac9f99bff494252`**: `docs(ci): specify owner-authorized Claude PR reviews`
  - **Task Impact**: +100% to TODO #1 (+20% overall).
  - **Summary**: Define event gates, credential boundaries, immutable source,
    verdict validation, error handling, and activation procedure.

- **Commit `8d26b7a6788424f39283ee1e888ab01294a69089`**: `feat(ci): add owner-authorized static Claude PR reviews`
  - **Task Impact**: +100% to TODO #2 (+50% overall).
  - **Summary**: Add the identity-gated workflow, immutable regular-blob snapshot,
    static project prompt, separate credential phases, and checked verdict posting.
  - **Verification**: Bash syntax, ShellCheck, workflow structure checks, and 38
    offline cases pass before commit; the test suite is added in the next commit.

- **Commit `HEAD`**: `test(ci): verify Claude review authorization and failure paths`
  - **Task Impact**: +100% to TODO #3 (+30% overall).
  - **Summary**: Add 47 offline cases using real Git histories, mocked credentials,
    and mocked API/model processes; add a credential-free CI verification job.
    Exercise snapshot/diff limits, source omissions, rejected actors/events,
    stale requests, invalid output, token scrubbing, API failures, and one-shot posting.

## Verification receipts

- Bash syntax and ShellCheck pass for controller and test script.
- actionlint 1.7.12 validates both new workflows with no diagnostics; its official
  release archive checksum was verified before use.
- All 47 offline cases pass; three additional full repetitions pass (141 cases).
- Workflow structure checks confirm event types, default-branch checkout, owner
  gate, job-level concurrency, minimal permissions, and secret separation.
- No host/guest application, README, dependency pin, or submodule changed.
- No Anthropic request or live GitHub comment was made during verification.
  The first paid integration run remains an operator action after merge, outside
  the completed local-verification scope. CI results must be checked on the PR.

The newest ledger entry uses the Git revision `HEAD` to identify its own commit;
embedding that commit's hash in its own contents is self-referential. The next
commit resolves the previous entry to its concrete hash. `git log` supplies the
newest entry's exact hash without creating an endless series of ledger commits.
