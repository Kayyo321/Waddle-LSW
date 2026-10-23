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
| #4 | Terminal, launchers, AV and desktop capability handling | In Progress | 25% | 85% | VTE/AV integration implemented; hardware checks unavailable |
| #5 | Automated integration, sanitizer checks and usage docs | Pending | 15% | 0% | Xvfb and real QEMU |

**Total Feature Completion**: `75.25%`

## Commit History & Progress Log

The planning commit is identified by `docs(gui): specify native manager integration`.
Hash: `453be2a`. The user supplied the expanded plan after this commit; the
branch and documentation now follow the requested names. Task impact: +100% to #1 (+10% overall).

- **Commit `910924b`**: `feat(gui): add native application foundation and explicit repl dispatch`
  - **Task Impact**: +100% to #2 (+10% overall); +10% to #3 (+4% overall).
  - **Summary**: Display-independent CLI dispatch, async adapter, preference store and build foundation.

- **Commit `4033ec6`**: `feat(gui): implement native device management workflows`
  - **Task Impact**: +75% to #3 (+30% overall).
  - **Summary**: Dashboard, lifecycle, native creation/review and backup forms, configuration,
    logs, diagnostics, scoped palette hooks, batch queue and capability explanations.

- **Commit `feat(gui): add VTE sessions and saved application workflows`** (hash recorded next commit)
  - **Task Impact**: +85% to #4 (+21.25% overall).
  - **Summary**: Integrated PTY sessions and cleanup, saved command/graphical launchers,
    AV attachment, single-instance selection, palette, private preferences, activity
    retention, reviewed exports and graceful cancellation. System-library build passes.
