# Feature Tracker: Guest window graph delegated slice

- **Contributors / Agents**: Codex coordinator; Claude Code (session coordinator with Sonnet 5.5 and Haiku 5.5 implementers, Opus 5.5 reviewer sub-agents)
- **Time Started**: 2026-10-24T15:13:35.810932Z
- **Time Ended**: TBD
- **Feature Branch**: feat/seamless-window-graph
- **Target Merge Branch**: feat/seamless-interop
- **Current Overall Status**: In Progress

Task-local progress only. Global TODO #10 credit is awarded by Codex after
review, integration and required native verification. Do not edit the global
seamless tracker in this worktree.

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Bounded graph/classification/generation engine | In Progress | 35% | 40% | Pure layer and state/ring/quarantine done; planning, refresh and review amendments pending |
| #2 | Opaque C ABI and lifecycle | In Progress | 10% | 40% | Header committed; Zig exports and C ABI test pending; header amended after Opus review |
| #3 | Session-wide Win32 observation adapter | In Progress | 25% | 40% | Header and implementation committed, not yet linked or tested; review amendments pending |
| #4 | Portable, ABI, failure, stress, coverage and sanitizer qualification | Pending | 20% | 0% | Native runner remains Codex-owned; no gate has run yet |
| #5 | Return document and integration/ownership handback | Pending | 10% | 0% | Return template supplied; IMPL_DESC committed |

**Total Feature Completion**: `28.0%`

## Commit History & Progress Log

- **Commit `3221815`**: `docs(interop): bootstrap isolated window graph handoff`
  - **Task Impact**: +0% to local #1-#5; +0% global TODO #10.
  - **Summary**: Codex supplies the task/return contract and exclusive ownership
    before Claude starts. No production implementation or qualification.

- **Commit `da898fc`**: `feat(interop): define the window graph C ABI header`
  - **Task Impact**: +40% to TODO: #2 (+4.0% overall); +0 global.
  - **Summary**: Public opaque C ABI: fixed-layout facts/event/stats structs with
    static asserts, status/disposition/reason enums, entry-point prototypes.

- **Commit `fe1e5c4`**: `feat(interop): define the Win32 window observer header`
  - **Task Impact**: +10% to TODO: #3 (+2.5% overall); +0 global.
  - **Summary**: Observer API table, config, stats, status and lifecycle contract.

- **Commit `df749f8`**: `feat(interop): add pure window graph validation and classification layer`
  - **Task Impact**: +20% to TODO: #1 (+7.0% overall); +0 global.
  - **Summary**: Facts validation, section 4.4 classifier, identity and arithmetic
    helpers with 18 tests.

- **Commit `ec5ffef`**: `docs(interop): specify the window graph and Win32 observer in detail`
  - **Task Impact**: +0% to local tasks (specification baseline); +0 global.
  - **Summary**: Task-local IMPL_DESC covering all required sections.

- **Commit `b583c90`**: `feat(interop): implement the Win32 window observer adapter`
  - **Task Impact**: +30% to TODO: #3 (+7.5% overall); +0 global.
  - **Summary**: Portable core and `_WIN32` section; compiled with gcc, zig cc and
    mingw only; not linked against the graph or run.

- **Commit `059d02c`**: `feat(interop): add window graph state, event ring and quarantine queries`
  - **Task Impact**: +20% to TODO: #1 (+7.0% overall); +0 global.
  - **Summary**: graph_t, create/destroy, guards, event ring with coalescing, drain,
    complete_retire, stats and capacity diagnostic with 29 tests.
