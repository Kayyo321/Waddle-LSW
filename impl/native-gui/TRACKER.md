# Feature Tracker: Native GTK4/libadwaita GUI

- **Contributors / Agents**: Codex (/root)
- **Time Started**: 2026-10-23T04:20:00Z
- **Time Ended**: TBD
- **Feature Branch**: feature/native-gui
- **Target Merge Branch**: feature/software-vgpu-slicing
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Specify GUI boundary, ownership and verification | Done | 10% | 100% | Initial design |
| #2 | Launch GUI by default and retain explicit REPL | Done | 10% | 100% | `910924b`; explicit commands preserved |
| #3 | Native manager, creation, settings and complete CLI access | In Progress | 40% | 85% | Native forms implemented; verification pending |
| #4 | Terminal, launchers, AV and desktop capability handling | Pending | 25% | 0% | Existing QEMU socket |
| #5 | Automated integration, sanitizer checks and usage docs | Pending | 15% | 0% | Xvfb and real QEMU |

**Total Feature Completion**: `54.0%`

## Commit History & Progress Log

The planning commit is identified by `docs(gui): specify native manager integration`.
Hash: `453be2a`. The user supplied the expanded plan after this commit; the
branch and documentation now follow the requested names. Task impact: +100% to #1 (+10% overall).

- **Commit `910924b`**: `feat(gui): add native application foundation and explicit repl dispatch`
  - **Task Impact**: +100% to #2 (+10% overall); +10% to #3 (+4% overall).
  - **Summary**: Display-independent CLI dispatch, async adapter, preference store and build foundation.

- **Commit `feat(gui): implement native device management workflows`** (hash recorded next commit)
  - **Task Impact**: +75% to #3 (+30% overall).
  - **Summary**: Dashboard, lifecycle, native creation/review and backup forms, configuration,
    logs, diagnostics, scoped palette hooks, batch queue and capability explanations.
