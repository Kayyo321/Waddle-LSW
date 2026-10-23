# Feature Tracker: Native GTK4/libadwaita GUI

- **Contributors / Agents**: Codex (/root)
- **Time Started**: 2026-10-23T04:20:00Z
- **Time Ended**: TBD
- **Feature Branch**: feature/native-gtk-gui
- **Target Merge Branch**: feature/software-vgpu-slicing
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Specify GUI boundary, ownership and verification | Done | 10% | 100% | Initial design |
| #2 | Launch GUI by default and retain explicit REPL | Pending | 10% | 0% | Preserve explicit CLI commands |
| #3 | Native manager, creation, settings and complete CLI access | Pending | 40% | 0% | Async argv adapter |
| #4 | Desktop viewer, QMP lifecycle and input | Pending | 25% | 0% | Existing QEMU socket |
| #5 | Automated integration, sanitizer checks and usage docs | Pending | 15% | 0% | Xvfb and real QEMU |

**Total Feature Completion**: `10.0%`

## Commit History & Progress Log

The planning commit is identified by `docs(gui): specify native manager integration`.
Its immutable hash is recorded in the next implementation commit to avoid a
self-referential commit hash. Task impact: +100% to #1 (+10% overall).
