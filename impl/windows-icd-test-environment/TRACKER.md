# Feature Tracker: Portable Windows ICD test environment

- **Contributors / Agents**: Windows ICD test environment worker; independent reviewer
- **Time Started**: 2026-10-09T21:51:46Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: origin
- **Current Overall Status**: Review

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Diagnose native read/mutation contract and reproduce Windows link failure | Done | 20% | 100% | PEB reads require SetEnvironmentVariableW; full-unit baseline fails on setenv/unsetenv |
| #2 | Implement test-only guard and lossless restoration regression | Done | 30% | 100% | No production, README, threshold, or CI-inclusion changes |
| #3 | Pass Linux full/targeted suites, whole-ICD coverage and Windows full cross-links | Done | 25% | 100% | Final source passes all local gates; cross-link is not execution |
| #4 | Complete independent review | Done | 10% | 100% | Final helper approved after absent-delete correction; no remaining code blockers |
| #5 | Execute complete native Windows CI suites after publication | Pending | 15% | 0% | Parent owns authorized publication and CI monitoring |

**Total Feature Completion**: `85.0%`

## Verification receipts

- Baseline source: `e827fde` (concurrent documentation-only `99e1701` does not change the source). Full Windows Debug unit cross-link reproduced undefined `setenv`/`unsetenv` with exit 1. Log: `build/windows-icd-environment-baseline.log`.
- Toolchain: official Zig 0.13.0; local SDK loaded from `/workspace/shared/waddle-tools/env.sh`; Windows commands clear `CPATH` and `LIBRARY_PATH` so Linux SDK include/library paths cannot contaminate the target.
- Final-source complete Linux command: `source /workspace/shared/waddle-tools/env.sh; make vgpu-icd-units > build/windows-icd-test-environment/linux-units-final.log 2>&1`. Exit **0**; **393 Debug and 393 ReleaseSafe tests** pass, with the 128-cycle native instance-dispatch fixture in each mode. Both passes use the frozen source checksum below; the earlier `linux-units.log` predates the final idempotent-deletion assertions and is not the final receipt.
- Final-source complete Windows Debug and ReleaseSafe cross-links both exit **0**, producing PE32+ x86-64 executables. They use the exact `Verify native Windows device requests and extension codecs` workflow ICD commands and all 23 oracle objects, ICD/values fixtures, and six codec libraries; the only execution adaptations are `--test-no-exec` and dedicated `-femit-bin=build/windows-icd-test-environment/icd-{Debug,ReleaseSafe}.exe` paths. C oracle preparation also uses the workflow's exact include paths, `-UNDEBUG`, `-Dmain=venus_icd_native_fixture`, and target. Expanded command logs: `windows-debug-final.log` and `windows-release-safe-final.log` in the same evidence directory. Native execution is **not** implied.
- `llvm-readobj --coff-imports` of both final Windows executables reports `KERNEL32.dll!SetEnvironmentVariableW`; neither imports `setenv` nor `unsetenv`. Receipt: `build/windows-icd-test-environment/windows-imports-final.txt`.
- Targeted final-source Linux Debug and ReleaseSafe runs each pass all three selected tests: the environment mutation/restoration regression and both affected diagnostic tests. Selection appended to each unchanged `make -n vgpu-icd-units` command: `--test-filter "test environment" --test-filter "opt in diagnostics" --test-filter "gap empty diagnostics"`. Log: `build/windows-icd-test-environment/linux-targeted.log`; aggregate exit 0.
- Independent review approved the final helper after verifying absent deletion, non-null empty values, WTF-8/UTF-16 round trips, and allocation cleanup. Final source SHA-256: `9e99d20b033bff32fda8b7057d84b5081f177cbae0b24f0dd9a360b9faddd5a8`.
- Production bytes before the preexisting test-fixture delimiter are identical to `e827fde` (SHA-256 `5c1cb4f65483c0a007a2027344d3950a96bcec2ac8e768081438b59b7bb948d8`). The six appended queue regressions remain byte-identical (SHA-256 `acba4eca0460e33625d52694449c79e3bea2f673456c0c2c830cde68ca14f6f9`).
- Final-source unchanged full coverage command: `source /workspace/shared/waddle-tools/env.sh; make vgpu-icd-coverage > build/windows-icd-test-environment/coverage.log 2>&1`. Exit **0**; all prerequisite gates and **393 instrumented ICD tests** pass. Production branches remain **90.32% (4369/4837)** and lines remain **98.53% (4694/4764)**, with **2406** compiler panic/stack-canary guards excluded by the unchanged harness. This exactly preserves the prior `c5ea2c7`/`e827fde` coverage receipt, denominators, and 90% threshold; the new environment helper is test-only. Detailed artifacts: `build/coverage/vgpu/venus_icd/run-20261009T220045336738Z-574/`. Final source checksum verified again after the command completed, at 2026-10-09T22:04:56Z.
- Native Windows execution has not been performed by this Linux executor. No runtime or hardware acceptance is inferred from linking.

## Commit History & Progress Log

- **Commit `cb0844a238570b3aa136a9c9ce7e72be52aaaf85`**: `test(vgpu): make ICD environment fixtures portable on Windows`
  - **Task Impact**: #1 +100% (+20% overall), #2 +100% (+30%), #3 +100% (+25%), #4 +100% (+10%); bounded repair is **85%** complete.
  - **Summary**: Test-only native process-environment guard, lossless restoration regression, complete unchanged Linux/coverage gates, Windows cross-link receipts, and independent review. #5 remains 0%; parent-owned native CI execution must complete before claiming runtime acceptance.

This documentation-only attribution receipt records the verified implementation commit above. Task impact: **+0%**; total remains **85%**, with native Windows CI execution still pending.
