# Feature Tracker: Guest WSI resize and retirement lifetime

- **Contributors / Agents**: Codex WSI lifetime worker; independent WSI reviewer; queue coverage regression worker and reviewer
- **Time Started**: 2026-10-09T19:59:10Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: origin
- **Current Overall Status**: Review

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Specify bounded lifetime and native thread contracts | Done | 15% | 100% | IMPL_DESC.md; unobserved HWND reuse and full window-event integration remain separate |
| #2 | Implement replacement retirement, resize/loss and queue ownership gates | Done | 35% | 100% | Retire on failed replacement; exact Win32 capabilities; acquire/present gates; correct acquisition release; queue result precedence and conservative loss after ambiguous failure |
| #3 | Add exact-size native presentation with balanced DC release attempts | Done | 20% | 100% | Preserves scaled compatibility API; exact path rejects extent changes before drawing |
| #4 | Run functional, coverage, cross-build, integrated and sanitizer gates | In Progress | 30% | 80% | Whole-ICD branch gate restored to 90.32%; 392 Debug/ReleaseSafe tests pass; full LSan gate remains blocked by executor ptrace |

**Total Feature Completion**: `94.0%`

Task #4 has five equally weighted gates: portable functional tests, production coverage, Windows cross-builds, complete ICD Debug/ReleaseSafe aggregate, prescribed ASan/LSan. Supplemental leak-disabled runs do not close the final gate. This bounded tracker does not increase software-vGPU TODO #3, close roadmap D, or permit feature merge.

## Verification receipts

Source base: `6eea43c`; owned implementation frozen in `5be8152610619c49578f89581370d65ba0146181`. Subsequent concurrent commits do not alter these owned production paths. Toolchain: official Zig 0.13.0, GCC 14.2.0, LLVM 19.1.7. Existing pinned protocol headers generated successfully; no dependency pins or runtime dependencies changed.

- `make vgpu-wsi-lifecycle-test`: passed 12 Zig cases in Debug and ReleaseSafe plus production C native API-double and unsupported-platform tests. Zig's testing allocator reported no outstanding allocations. Final log: `build/wsi-lifecycle/committed-test.log`.
- `python3 tests/av/coverage.py venus_wsi`: passed production line coverage 99.33% (148/149) and branch coverage 92.95% (145/156), excluding 16 compiler panic/stack-canary guards under the established repository harness. Final log: `build/wsi-lifecycle/committed-zig-coverage.log`.
- `python3 tests/vgpu/wsi_lifecycle_coverage.py`: passed production native C line coverage 100% (47/47), branch coverage 98.61% (71/72). This executes the real C source against local test API declarations, not a Windows runtime. Log: `build/wsi-lifecycle/committed-native-coverage.log`.
- `make build/vgpu_win32_present_test.exe`: passed real Windows SDK cross-link with user32/gdi32/dwmapi. The fixture adds exact-size presentation alongside 128 scaled BGRA/FIFO/IMMEDIATE/GDI/USER cycles and checks resize rejection. Native execution is untested here. Log: `build/wsi-lifecycle/windows.log`.
- `zig test src/vgpu/venus_wsi.zig -Iinclude -Isubmodules/venus_protocol/include -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=build/wsi-lifecycle/wsi-test-windows.exe`: passed Windows state-fixture cross-link; unexecuted here.
- `make vgpu-icd-units`: passed all 386 Debug and 386 ReleaseSafe tests, plus the 128-cycle native instance-dispatch fixture in each mode; process exit zero verified. Includes acquired-old retirement behavior, loss-before-replacement entry point, every supported result-priority pair, mixed rejection order, and pre-/post-enqueue OOM behavior with and without shared waits. Log: `build/wsi-lifecycle/icd-units-final.log`.
- `make vgpu-wsi-lifecycle-sanitizers`: C fixture reaches LeakSanitizer's fatal ptrace restriction; the required combined gate does not pass here. `python3 tests/vgpu/state_sanitizers.py venus_wsi` separately ran all 12 current tests under owned ASan instrumentation and then hit the same LSan restriction. Zero native leaks are not verified. Logs: `build/wsi-lifecycle/sanitizers.log` and `build/wsi-lifecycle/committed-zig-sanitizers.log`.
- Supplemental current binaries rerun with `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1`: C ASan/UBSan fixture and owned Zig ASan/allocator suite passed. These do not substitute for LSan. Logs: `build/wsi-lifecycle/c-asan-final.log`, `build/wsi-lifecycle/committed-zig-asan.log`. Exact final Zig runner: `build/sanitizers/vgpu/venus_wsi/run-20261009T202038782277Z-5/runner`.
- Independent review reproduced the 12 Debug/ReleaseSafe cases and native coverage. It identified and the implementation corrected exact Win32 capability extents, ICD liveness bypass of failed-replacement retirement, image acquisition release on rejection, aggregate error priority, and unsafe retryable OOM after queue effects. Final independent sign-off for `5be8152` found no remaining correctness issues after reviewing both 386-test aggregate passes; recorded 2026-10-09T20:20:46Z.

### Whole-ICD coverage regression checkpoint, 2026-10-09T21:49:11Z

The subsequent complete ICD run exposed a real branch-gate failure despite the bounded WSI module gates above passing: **89.99% (4353/4837)** production branches, **98.51% (4693/4764)** lines. Preserve this failed baseline; it is not a pass rounded to 90%. Original log: `/workspace/shared/waddle-tools/final-icd-coverage.log`. Original detailed artifacts: `build/coverage/vgpu/venus_icd/run-20261009T202725706476Z-577/`.

Test/design commit: `c5ea2c72aeddfd2e4a7d5897b8542f3a1254f914`. Final `src/vgpu/venus_icd.zig` SHA-256: `31fc346c79646dc79d21ca8d115d097c2fd4394d7d0a6198e058fbaebe26831d`. The source checksum was verified after final unit execution and before committing. Production code, coverage scripts, instrumentation rules, exclusions, the 90% threshold, dependency pins and README are unchanged.

- Exact final unit command after loading the verified local SDK: `source /workspace/shared/waddle-tools/env.sh; make vgpu-icd-units > build/wsi-lifecycle/icd-coverage-regression-units-final.log 2>&1`. Exit **0**; all **392 Debug and 392 ReleaseSafe tests** pass, with the **128-cycle native instance-dispatch fixture in each mode**. Source checksum receipt: `build/wsi-lifecycle/icd-coverage-regression-final.sha256`. The earlier `icd-coverage-regression-units.log` predates strengthened assertions and is not the final receipt.
- Exact whole-target command: `source /workspace/shared/waddle-tools/env.sh; make vgpu-icd-coverage > build/wsi-lifecycle/icd-coverage-regression.log 2>&1`. Exit **0**; all prerequisite coverage gates and **392 instrumented ICD tests** pass. Final production branches: **90.32% (4369/4837)**. Final production lines: **98.53% (4694/4764)**. This exercises **16 additional production branch edges**, preserves the exact branch/line denominators, and exceeds the minimum passing branch count by **15 edges**. Instrumentation reports **14235/16384 sites**, maximum **10/32 edges**, and **2406 compiler panic/stack-canary guards excluded** under the unchanged harness.
- Detailed current coverage artifacts: `build/coverage/vgpu/venus_icd/run-20261009T214636681486Z-574/`, including `branches.json`, `branch_missed.json`, `line_missed.json`, instrumented IR, edge hits, and the executable runner. The complete target log preserves the prerequisite results as well as the final ICD totals.
- Six new regressions exercise shared-wait GPU-fence/poll failure, sticky loss without republishing results, repeated first-item allocation failure without a result array, sparse/invalid/active queue-owner handling, missing acquire queues, unknown/foreign/mismatched image retirement ownership, and invalid-wait readback rollback. The readback test positively requires acknowledged staging creation, successful command recording, then ordered command/pool/buffer/memory cleanup before asserting no GPU submission. Independent static review signed off the strengthened tests at 2026-10-09T21:44:33Z.

These are portable fixtures, not native Windows execution or real GPU proof. The prescribed LSan limitation and native Windows/Wayland/shared-GPU acceptance remain unchanged. This correction restores the full coverage gate without increasing the bounded tracker beyond 94%.

## Commit History & Progress Log

- **Commit `5be8152610619c49578f89581370d65ba0146181`**: `fix(vgpu): harden Win32 swapchain retirement and resize lifetimes`
  - **Task Impact**: #1 +100% (+15% overall), #2 +100% (+35%), #3 +100% (+20%), #4 +60% (+18%): initial checkpoint 88%.
  - **Summary**: Production WSI/ICD/native fixes, deterministic lifecycle/ownership fixtures, build gates, design and initial receipts. Scope remains the bounded compatibility path.
- **Verification checkpoint for `5be8152`**, 2026-10-09T20:20:46Z:
  - **Task Impact**: #4 +20% (+6% overall): both integrated modes and independent final review verified; total 94%.
  - **Summary**: Repeated committed-source functional/coverage checks and owned ASan instrumentation. Full prescribed LSan remains blocked, without changing targets or suppressing leak acceptance. This documentation checkpoint changes no production source.
- **Commit `c5ea2c72aeddfd2e4a7d5897b8542f3a1254f914`**: `test(vgpu): cover queue-side WSI failure transactions`
  - **Task Impact**: #4 +0%; overall remains 94%. Restores the newly exposed whole-ICD branch gate and strengthens transaction/ownership evidence without claiming another acceptance gate.
  - **Summary**: Adds six independently reviewed portable regressions and their detailed design. Final 392-test Debug/ReleaseSafe aggregate and unchanged whole-ICD coverage target pass with the exact totals and source checksum above. No production implementation or coverage-policy changes.

## Still required

1. Run the unchanged prescribed ASan/LSan targets in a non-ptrace environment or CI. No leak suppression or disabled detection counts as acceptance.
2. Execute native Windows C/state fixtures and affected real Windows Vulkan/DXVK resize/minimize/replacement/loss acceptance on the final source, under the existing owner's GPU/VM lease.
3. Complete the separate AV-window generation to WSI/Wayland integration, including destruction events and unobserved numeric HWND reuse. No property/subclass workaround is claimed here.
