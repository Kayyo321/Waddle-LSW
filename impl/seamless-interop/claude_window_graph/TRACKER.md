# Feature Tracker: Guest window graph delegated slice

- **Contributors / Agents**: Codex coordinator; Claude Code (session coordinator with Sonnet 5.5 and Haiku 5.5 implementers, Opus 5.5 reviewer sub-agents)
- **Time Started**: 2026-10-24T15:13:35.810932Z
- **Time Ended**: 2026-10-24T18:25:00Z
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
| #1 | Bounded graph/classification/generation engine | Done | 35% | 100% | 136 Zig tests, 210000 random-trace steps; coverage 99.87% line / 98.25% branch |
| #2 | Opaque C ABI and lifecycle | Done | 10% | 100% | 21 exports checked against the header; 14 ABI tests; 99.30% / 96.55% |
| #3 | Session-wide Win32 observation adapter | Review | 25% | 85% | Portable core tested (63 tests, 97.0% / 93.6%); native `_WIN32` section cross-compiled only, never executed |
| #4 | Portable, ABI, failure, stress, coverage and sanitizer qualification | Review | 20% | 80% | Gates green on Linux; no Windows execution; a few evidence items not provided (see IMPL_DESC 7.5) |
| #5 | Return document and integration/ownership handback | Done | 10% | 100% | HANDOFF_RETURN.md written, ownership released |

**Total Feature Completion**: `92.25%`

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
- **Tip commit**: `docs(interop): finalize the window graph tracker` - +20% to #5 (+2.0% overall); this tracker.
