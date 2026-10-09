# Feature Tracker: Native ICD extension projection oracle

- **Contributors / Agents**: Native extension oracle repair worker; independent reviewer
- **Time Started**: 2026-10-09T22:13:29Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: origin
- **Current Overall Status**: Review

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Diagnose platform projection and audit related stale assumptions | Done | 20% | 100% | Exact guest swapchain spec70 on Windows; two affected test locations |
| #2 | Repair exact C ABI and negotiated Zig projection expectations | Done | 30% | 100% | Production behavior and all other Zig tests unchanged |
| #3 | Verify Linux full units/C ABI/coverage and Windows full-unit links | Done | 25% | 100% | 393 Debug + 393 ReleaseSafe; three C variants; full coverage; both Windows links |
| #4 | Complete independent source and evidence review | Done | 10% | 100% | No source blockers; reviewed source hashes match final test evidence |
| #5 | Observe exact-commit native Windows and leak-enabled Linux CI | Pending | 15% | 0% | No Windows runtime here; local LSan fails under tracing, including approved retry |

**Total Feature Completion**: `85.0%`

## Commit History & Progress Log

- **Commit `1535600d5c5b3c72960efff6b2d3b441d8968a7d`**: `test(vgpu): verify exact native extension projections`
  - **Task Impact**: #1 +100% (+20% overall), #2 +100% (+30%), #3 +100% (+25%), #4 +100% (+10%); bounded repair is **85%** complete.
  - **Summary**: Exact platform-aware C and negotiated Zig extension oracles, scoped source preservation, full Linux/C ABI evidence, complete Windows links, and independent source/evidence approval. #5 remains 0% pending native Windows runtime and leak-capable Linux CI.

This documentation-only attribution/coverage receipt changes no source and adds **+0%** completion. Native runtime and sanitizer limits remain explicit below.

## Verification receipts

All final verification uses these unchanged source SHA-256 values:

- `src/vgpu/venus_icd.zig`: `177a558f27d07d01c7760ac682e78db8dd382b97d8065f5e9a6d975ad6eef907`
- `tests/vgpu/icd.c`: `cc82349ba37c730a5945b3c289173faae14b4809cc93162733a7ab127ce74879`

Toolchain: official Zig 0.13.0 and `/workspace/shared/waddle-tools/env.sh`. Evidence directory: `build/native-icd-extension-oracle/`. These are local test receipts, not committed build artifacts.

- `source /workspace/shared/waddle-tools/env.sh; make vgpu-icd-units`: exit **0**, **393 Debug + 393 ReleaseSafe** tests and the 128-cycle C fixture in both modes. `linux-units.log` SHA-256: `3b3d6838295f5385d2fbaf328e12b923e2003117fc96f9d527824e74ce330c87`.
- `make build/vgpu_icd_test build/vgpu_icd_loader_test build/vgpu_icd_mapping_fault_test`, followed by executing all three binaries: exit **0**. Each C fixture passes 128 lifecycle cycles; the loader variant also passes manifest discovery and eight instance/device/queue/fence lifecycles. `linux-c-abi.log` SHA-256: `157e9cd5754503a916cca25eae709aa1b344d0847fb5ddb535b9957845894ffb`.
- `bash build/native-icd-extension-oracle/cross-link.sh`: exit **0**, both full Debug and ReleaseSafe Windows ICD unit executables link as PE32+ x86-64. All 23 workflow C oracle objects and the renamed-main C fixture were rebuilt with the exact Windows target, strict warning flags, assertions enabled, and workflow include paths. The existing Windows values-oracle object and six codec libraries are unchanged dependencies. The test commands add only `--test-no-exec` and dedicated output filenames. `CPATH` and `LIBRARY_PATH` are cleared for every Windows command. `windows-cross-links.log` SHA-256: `ae3b8ad9fe3e807cbf9ba50bbc02116161e551afbc520cad360c6fe818a49a73`.
- The source boundary audit compares both the prefix and suffix outside the one approved Zig test block against `84bcb08`. Both are byte-identical: prefix SHA-256 `b26ec5c5acf7c8ef1d458809a5e909c685b9023bb5f6ce6a6dc03246efc61187`, suffix `81ca505987da2224418b11f47a80a1b27f896f22eadd0ecabffa1c992800f411`. Thus production logic, existing environment fixtures, all other tests, and prior WSI regressions are preserved. Receipt: `source-boundary-audit.txt`.
- Independent reviewer approved the exact source hashes above, including the complete CI oracle list, unchanged native Windows runtime commands, exact guest/host projection, zero/short/exact/oversized outputs, poisoned tail/error preservation, protocol filtering, and transport/cache counts. No test filters, skips, capability inflation, README edits, dependency changes, or threshold changes were introduced.

- Final unchanged whole-ICD coverage: `source /workspace/shared/waddle-tools/env.sh; make vgpu-icd-coverage` exits **0**. All prerequisite gates and **393 instrumented ICD tests** pass. Production branch coverage is **90.32% (4369/4837)**; line coverage is **98.53% (4694/4764)**; **2406** compiler panic/stack-canary guards are excluded by the unchanged harness. These values exactly match the previous receipt, including denominators and the 90% threshold. Detailed artifacts: `build/coverage/vgpu/venus_icd/run-20261009T222512848584Z-550/`. Final log: `coverage.log`, SHA-256 `f154828a96b174c46d47d12ef6b37426c0f2c8dbb7f5b39d1c978fbe014f8eb1`. Both reviewed source hashes were verified again after completion at 2026-10-09T22:27:47Z. Concurrent AV commits do not modify these sources.

### Explicit sanitizer/runtime limitations

The ordinary C sanitizer aggregate aborted in its allocation-fault prerequisite before reaching the main recipe: `LeakSanitizer has encountered a fatal error`, with the runtime's ptrace warning. Original log: `linux-c-sanitizers.log`; unchanged in-sandbox retry: `linux-c-mapping-sanitizer-retry.log`. An approved escalated execution of that same binary with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1` still exited **134** with the same tracing-related LSan failure; see `linux-c-mapping-sanitizer-escalated.log`. The separately extracted, unchanged main C sanitizer compile/run recipe also built successfully and exited **134** with that same runtime failure; see `linux-c-abi-sanitizer-escalated.log` and its exact command script `c-abi-sanitizer.sh`. The loader sanitizer binary was compiled successfully, but not executed after the runtime limitation was confirmed (`linux-loader-sanitizer-build.log`). No tracing/security settings, sanitizer settings, suppressions, or instrumentation thresholds were relaxed.

These failures are **not** sanitizer passes or proof of zero leaks. The local source/ABI/link checks above are complete; a leak-capable native Linux CI run and actual native Windows execution remain separate pending qualification. Parent owns publication and monitoring. No native Windows runtime or real hardware acceptance is claimed.
