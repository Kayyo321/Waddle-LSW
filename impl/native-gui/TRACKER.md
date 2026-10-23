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
| #2 | Launch GUI by default and retain explicit REPL | Pending | 10% | 0% | Preserve explicit CLI commands |
| #3 | Native manager, creation, settings and complete CLI access | Pending | 40% | 0% | Async argv adapter |
| #4 | Terminal, launchers, AV and desktop capability handling | Pending | 25% | 0% | Existing QEMU socket |
| #5 | Automated integration, sanitizer checks and usage docs | Pending | 15% | 0% | Xvfb and real QEMU |

**Total Feature Completion**: `10.0%`

## Commit History & Progress Log

The planning commit is identified by `docs(gui): specify native manager integration`.
Hash: `453be2a`. The user supplied the expanded plan after this commit; the
branch and documentation now follow the requested names. Task impact: +100% to #1 (+10% overall).
