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
| #3 | Native manager, creation, settings and complete CLI access | In Progress | 40% | 95% | Native workflow and CLI tests passed; final gate pending |
| #4 | Terminal, launchers, AV and desktop capability handling | In Progress | 25% | 95% | PTY/AV adapter tests pass; backend dependencies documented |
| #5 | Automated integration, sanitizer checks and usage docs | Pending | 15% | 0% | Xvfb and real QEMU |

**Total Feature Completion**: `81.75%`

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

- **Commit `42e460b`**: `feat(gui): add VTE sessions and saved application workflows`
  - **Task Impact**: +85% to #4 (+21.25% overall).
  - **Summary**: Integrated PTY sessions and cleanup, saved command/graphical launchers,
    AV attachment, single-instance selection, palette, private preferences, activity
    retention, reviewed exports and graceful cancellation. System-library build passes.

- **Commit `fix(gui): preserve device scope and session lifecycle`** (hash recorded next commit)
  - **Task Impact**: +10% to #3 (+4% overall); +10% to #4 (+2.5% overall).
  - **Summary**: Keep logs scoped during selection changes, protect closing-window
    dispatch, restore landing preferences, use literal terminal search, validate
    blank sizes and save reviewed output asynchronously. Native and CLI tests pass;
    library leak roots are isolated by sanitizer evidence.
