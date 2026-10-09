# Feature Tracker: Exact device sanitizer source inventory

- **Contributors / Agents**: Owned sanitizer inventory worker; independent boundary reviewer
- **Time Started**: 2026-10-09T22:03:49Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: origin
- **Current Overall Status**: Review

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Identify exact recursive closures and genuine fixture boundaries | Done | 20% | 100% | Four/five modules; all eleven helper production declarations retained |
| #2 | Repair boundaries/manifests and wire fail-closed regression | Done | 25% | 100% | Both device sanitizer make targets require the four invariant tests |
| #3 | Verify Debug/ReleaseSafe, owned emission/hooks/runtime and coverage | Done | 20% | 100% | Both unchanged unit/coverage suites and owned emission/hooks pass; expected cloud LSan limitation verified, native zero-leak acceptance remains #5 |
| #4 | Complete independent boundary and reachability review | Done | 15% | 100% | Reviewer verified source ownership, all emitted symbols, real final hooks, regression wiring and honest scope |
| #5 | Verify full zero-leak device gates in native Linux CI | Pending | 20% | 0% | Parent owns publication and CI monitoring; no cloud LSan success claimed |

**Total Feature Completion**: `80.0%`

## Verification receipts

- Both production source files compare byte-for-byte with the committed baseline after removing only the newly inserted comments. No executable Zig code changes.
- The four read-only invariant regressions pass with `python3 -B tests/vgpu/owned_sanitizer_inventory.py`.
- The exact workflow wire aggregate passes 32 Debug and 32 ReleaseSafe tests and all owned instrumentation/final-access checks. Its instrumented runner passes all 32 tests, then fails the unchanged LSan exit scan with the explicit ptrace-not-supported diagnostic. This is not a successful zero-leak receipt; `report.json` is correctly absent.
- Wire partial evidence: `build/device_wire_sanitizers/run-20261009T220533731789Z-56/`. Its `sanitized.json` records 31 runtime declarations, 41 emitted owned definitions, 1,091 preserved guards, no uncovered functions, and successful entire-module reverse proof. Per-module ASan relocation counts are device wire 49, render wire 279, pipeline helper 72, and image-view native 159. Pipeline helper has eight instantiated definitions covering all five source declarations; image-view has all six definitions.
- Native-device partial evidence: `build/device_native_owned_safety/run-20261009T220612750096Z-56/`. All 45 Debug, 45 ReleaseSafe and 45 instrumented units pass, followed by the same explicit LSan ptrace failure. Inventory/emission records show 41 production declarations, 73 emitted definitions, 1,244 preserved guards, no uncovered functions, and exact reverse proof. Native-device ASan relocation/final-hook counts are 525/525.
- Independent final-access rechecks on both retained binaries pass. Clearly labeled `final_access_partial.json` files record the successful subset only; no `report.json` completion record exists. Final hooks are wire 49, render 279, pipeline helper 70, and image-view native 153 in each aggregate.
- The independent reviewer verified every wire/native symbol occurs exactly once in final symbols and disassembly, checked current source hashes and the 14 helper emitted definitions, and approved all code, invariant wiring and scope documentation with no blockers.
- Exact workflow aggregates are rerunning after final make wiring in `build/owned-sanitizer-inventory/wire-final.log` and `native-final.log`. Separate unchanged coverage gates pass: device wire branches **100.00% (88/88)** and lines **98.15% (53/54)** with 12 unchanged compiler guards excluded; native device branches **95.26% (181/190)** and lines **99.23% (129/130)** with 32 unchanged compiler guards excluded. Evidence: `build/coverage/vgpu/venus_device_wire/run-20261009T220736641508Z-11/` and `build/coverage/vgpu/venus_device_native/run-20261009T220756928444Z-60/`. No downstream target skipped by make's early LSan failure is treated as passed.
- The broader 32-module ICD-owned/full-seam migration remains explicitly deferred as documented in IMPL_DESC.md. It is not a dependency of the standard Linux workflow's ICD sanitizer target.

## Commit History & Progress Log

The implementation commit contains the bounded source-boundary/manifests repair, enforced regression, unchanged local verification receipts, and independent review. Task impact: #1 +100% (+20% overall), #2 +100% (+25%), #3 +100% (+20%), #4 +100% (+15%), totaling **80%**. Its exact hash is recorded in the following attribution receipt. Native CI acceptance (#5) remains 0%; this is not a complete zero-leak or hardware acceptance claim.
