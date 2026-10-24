# Feature Tracker: Guest window graph delegated slice

- **Contributors / Agents**: Codex coordinator; Claude Code (session coordinator with Sonnet 5.5 and Haiku 5.5 implementers, Opus 5.5 reviewer sub-agents)
- **Time Started**: 2026-10-24T15:13:35.810932Z
- **Time Ended**: TBD
- **Feature Branch**: feat/seamless-window-graph
- **Target Merge Branch**: feat/seamless-interop
- **Current Overall Status**: Review

Task-local progress only. Global TODO #10 credit is awarded by Codex after
review, integration and required native verification. The global seamless
tracker was not edited in this worktree. File ownership is released to Codex
(see HANDOFF_RETURN.md).

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Bounded graph/classification/generation engine | Review | 35% | 95% | 136 Zig tests, 210000 random-trace steps; coverage 99.87% line / 98.25% branch |
| #2 | Opaque C ABI and lifecycle | Done | 10% | 100% | 21 exports checked against the header; 14 ABI tests; 99.30% / 96.55% |
| #3 | Session-wide Win32 observation adapter | Review | 25% | 85% | Portable core tested (63 tests, 97.0% / 93.6%); native `_WIN32` section cross-compiled only, never executed |
| #4 | Portable, ABI, failure, stress, coverage and sanitizer qualification | Review | 20% | 80% | Gates green on Linux; no Windows execution; a few evidence items not provided (see IMPL_DESC 7.5) |
| #5 | Return document and integration/ownership handback | Done | 10% | 100% | HANDOFF_RETURN.md written, ownership released |

**Total Feature Completion**: `90.50%`

## Commit History & Progress Log

Task Impact percentages are task-local; global TODO #10 credit is +0 for every commit.

- **Commit `3221815`**: `docs(interop): bootstrap isolated window graph handoff` - +0%. Codex supplies the contract.
- **Commit `da898fc`**: `feat(interop): define the window graph C ABI header` - +40% to #2 (+4.0% overall).
- **Commit `fe1e5c4`**: `feat(interop): define the Win32 window observer header` - +10% to #3 (+2.5% overall).
- **Commit `df749f8`**: `feat(interop): add pure window graph validation and classification layer` - +20% to #1 (+7.0% overall).
- **Commit `ec5ffef`**: `docs(interop): specify the window graph and Win32 observer in detail` - +0% (specification baseline).
- **Commit `b583c90`**: `feat(interop): implement the Win32 window observer adapter` - +30% to #3 (+7.5% overall).
- **Commit `059d02c`**: `feat(interop): add window graph state, event ring and quarantine queries` - +20% to #1 (+7.0% overall).
- **Commit `30620cb`**: `docs(interop): log window graph commits and task progress in the tracker` - +0%.
- **Commit `2831282`**: `feat(interop): amend the window graph ABI after design review` - +10% to #2 (+1.0% overall).
- **Commit `467bbba`**: `feat(interop): amend the Win32 observer API after design review` - +5% to #3 (+1.25% overall).
- **Commit `29fbfb7`**: `fix(interop): apply design-review findings to the Win32 observer` - +15% to #3 (+3.75% overall).
- **Commit `1aab2e2`**: `feat(interop): add the window graph planning engine` - +30% to #1 (+10.5% overall).
- **Commit `5d0c7ce`**: `docs(interop): align the window graph design with the reviewed headers` - +20% to #5 (+2.0% overall).
- **Commit `c496c2b`**: `feat(interop): add refresh, keep/unseen and invalidate_all to the window graph` - +20% to #1 (+7.0% overall).
- **Commit `6dd8aa3`**: `feat(interop): export the window graph C ABI from Zig` - +50% to #2 (+5.0% overall).
- **Commit `02ee43c`**: `test(interop): add the window graph C ABI test` - +25% to #4 (+5.0% overall).
- **Commit `65b2032`**: `fix(interop): make a new refresh latch event due once even with a frozen clock` - +5% to #3 (+1.25% overall).
- **Commit `0ecc2e6`**: `test(interop): add the Win32 observer API double and lifecycle tests` - +20% to #4 (+4.0% overall).
- **Commit `d267b49`**: `chore(build): add standalone window graph make targets` - +10% to #4 (+2.0% overall).
- **Commit `f7eb737`**: `test(interop): raise Win32 observer branch coverage above the 90 percent gate` - +10% to #4 (+2.0% overall).
- **Commit `8af4d5c`**: `fix(interop): close a deferred cycle through leftovers and repair ticks after silence` - +10% to #1 (+3.5% overall).
- **Commit `9a9ff8a`**: `docs(interop): describe the graph-clock tick estimate in the window graph header` - +0%.
- **Commit `09bc75e`**: `fix(interop): stop child-window hint floods from churning every exported window` - +20% to #3 (+5.0% overall).
- **Commit `00a84e4`**: `test(interop): add observer flood, fuzz, allocation-failure and in-flight tests` - +10% to #4 (+2.0% overall).
- **Commit `221fb74`**: `chore(build): add ReleaseSafe runs, symbol check and calloc wrap to window graph targets` - +5% to #4 (+1.0% overall).
- **Commit `0c71275`**: `docs(interop): align the window graph design with the code review results` - +20% to #5 (+2.0% overall).
- **Commit `5c02dcc`**: `docs(interop): record the window graph handoff return` - +40% to #5 (+4.0% overall).
- **Commit `3714cf2`**: `docs(interop): finalize the window graph tracker` - +20% to #5 (+2.0% overall); this tracker.

- **Commit `ac4c422`**: `fix(interop): reject graph create aliases before clearing output`
  - **Task Impact**: +0% to local #2/#4; +0% global TODO #10 (local92.25%).
  - **Summary**: Codex review fixes config mutation before alias rejection and
    clarifies the NULL-on-failure exception. Adds runtime aligned/misaligned
    overlap regressions; Debug/ReleaseSafe150 Zig and27 C tests pass. HEAD is this
    containing review-fix commit; Windows execution and native acceptance remain open.

- **Commit `28748bc`**: `fix(interop): retain observer reservation and failed hook ownership`
  - **Task Impact**: +0% to local #3/#4; +0% global TODO #10 (local92.25%).
  - **Summary**: Keeps a separate lifecycle reservation through hook cleanup,
    immediately owns acquired tokens and retains failed unhooks for stop/destroy
    retry. Failed destroy preserves owner/slot. Adds deterministic second-start
    and partial-rollback retry regressions. Strict65 portable tests and Windows
    cross-build pass; lead leak-enabled qualification remains pending. HEAD is
    this containing observer review-fix commit.

- **Commit `1a179b0`**: `refactor(interop): conform graph namespace aliases to type naming`
  - **Task Impact**: +0% to local #1/#2/#4; +0% global TODO #10 (local92.25%).
  - **Summary**: Renames owned Zig namespace aliases to `_t` names without
    changing behavior. Token-stream proof preserves literals, comments, trivia,
    punctuation, C pointer sentinels and external module properties. The receipt
    records exact before/after hashes. Debug graph136/ABI150 tests pass; full
    post-review qualification and native Windows execution remain pending.
    HEAD identifies this containing conformance commit.

- **Commit `e7e7e6a`**: `docs(interop): qualify reviewed graph source before incarnation correction`
  - **Task Impact**: +0% to local #1/#2/#3/#4; +0% global TODO #10 (local92.25%).
  - **Summary**: Lead reproduces full gates at exact1a179b0 with detect_leaks1:
    graph136/ABI150 in Debug and ReleaseSafe, C27/observer65, exact21 exports,
    production line/branch gates and Windows cross-builds all pass. Archives
    hashes and log. O5 delivered-destroy loss and O25 stale deferred descendants
    remain open; this is provisional evidence, not final integration acceptance.
    HEAD identifies this containing evidence commit.

- **Commit `d626301`**: `docs(interop): require cached descendant invalidation on parent discontinuity`
  - **Task Impact**: +0% to local #1/#3/#4; +0% global TODO #10 (local92.25%).
  - **Summary**: Freezes the staged bounded deletion-mask correction across
    identity replacement, accepted destroy, exclusion and refresh. Roots include
    all Active cascade members; QueueFull remains transactional and same-object
    Reclassified keeps its flush. O5 remains unresolved. HEAD is this contract.

- **Commit `978278e`**: `test(interop): add real Windows observer adapter smoke fixture`
  - **Task Impact**: +0% to local #3/#4; +0% global TODO #10 (local92.25%).
  - **Summary**: Adds a Windows-only fixture using the actual native table,
    ownerless HWND query, real hook/message delivery, visibility/Gone and
    teardown. Bounded cleanup preserves failed owners for retries; unavailable
    desktop is a failure. Strict Windows cross-build passes. No Windows execution
    or foreign-process/UAC/capture result is claimed. HEAD is this fixture commit.

- **Commit `5aff081`**: `ci(interop): execute isolated graph and native observer Windows gates`
  - **Task Impact**: +0% to local #3/#4; +0% global TODO #10 (local92.25%).
  - **Summary**: Runs standalone Linux test/leak/coverage and Windows cross-build
    gates, then executes graph C ABI, observer doubles and real Win32 smoke on
    Windows2022. Branch triggers cover isolated review and parent feature. No PR
    or standard merge is opened; native results remain pending until CI finishes.
    HEAD identifies this workflow commit.

- **Commit `a2e72d3`**: `fix(interop): close standalone native query snapshot scopes`
  - **Task Impact**: +0% to local #3/#4; +0% global TODO #10 (local92.25%).
  - **Summary**: Direct calls through the native query table now own one scan
    scope across all return paths; pump/refresh keep their existing shared scope.
    This prevents a fallback Toolhelp handle waiting for a future observer call
    to close. Documents owner-thread serialization. Strict Windows fixture link
    and portable65-case lifecycle regression pass; native execution pending.
    HEAD identifies this containing ownership correction.

- **Commit `ae6d592`**: `docs(interop): reject deferred identity retention after replacement failure`
  - **Task Impact**: -5% to local #1 (-1.75% local total, now90.50%);
    +0% global TODO #10 (parent16.40%).
  - **Summary**: Critical review requires pruning a changed Deferred parent and
    its cached children even if replacement lookup/capacity/generation fails.
    Defines Ok/Excluded without fabricated Active retirement; ordinary errors
    and QueueFull remain unchanged. Revokes local graph completion while this
    correction is under review. Original handback92.25% remains historical.
    HEAD identifies this contract correction.

- **Commit `98cfc6f`**: `test(interop): correlate native hook delivery with fixture HWND`
  - **Task Impact**: +0% to local #3/#4; +0% global TODO #10 (local90.50%).
  - **Summary**: Independent review found aggregate hint counters could pass on
    unrelated desktop traffic. White-box inclusion now inspects the exact
    production dirty key and Create/Show kinds, without synthetic callbacks or
    a production seam. Bounds message dispatch per poll; preserves10s timeout.
    Strict Windows cross-link passes; native runtime evidence remains pending.
    HEAD identifies this containing smoke-test correction.

- **Commit `ff51808`**: `docs(interop): record isolated Linux and real Windows review qualification`
  - **Task Impact**: +0% to local #3/#4; +0% global TODO #10 (local90.50%).
  - **Summary**: CI40123985933 succeeds on Linux and Windows at exact5aff081:
    C ABI27, observer doubles65 and actual native smoke execute. Records the
    aggregate-hint limitation and excludes newer snapshot cleanup, target
    correlation and pending graph fixes from this result. No installed-session
    credit or standard merge. HEAD identifies this evidence commit.

- **Commit `HEAD`**: `fix(interop): invalidate cached descendants across parent discontinuity`
  - **Task Impact**: +0% to local #1/#4; +0% global TODO #10 (local90.50%).
  - **Summary**: Implements the bounded staged deletion mask for every Active
    cascade root, changed Deferred parent, accepted destroy, exclusion and doomed
    refresh entry. Masks cycle/flush scans, preserves QueueFull rollback and
    same-object Reclassified behavior. Failed Deferred-only replacement prunes
    stale cached facts and returns Ok/Excluded rather than reviving them later.
    Nine new builtins bring Debug and ReleaseSafe to145 passing each. Public
    comments describe exceptions; no ABI size/export change. Full leak/coverage/
    Windows qualification remains pending. HEAD is this correctness commit.
