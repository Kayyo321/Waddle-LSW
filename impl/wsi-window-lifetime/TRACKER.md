# Feature Tracker: Guest WSI resize and retirement lifetime

- **Contributors / Agents**: Codex WSI lifetime worker; independent WSI reviewer
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
| #4 | Run functional, coverage, cross-build, integrated and sanitizer gates | In Progress | 30% | 60% | Functional/coverage/cross-build verified; integrated Debug386 passed, ReleaseSafe pending; full LSan gate blocked by executor ptrace |

**Total Feature Completion**: `88.0%`

Task #4 has five equally weighted gates: portable functional tests, production coverage, Windows cross-builds, complete ICD Debug/ReleaseSafe aggregate, prescribed ASan/LSan. Supplemental leak-disabled runs do not close the final gate. This bounded tracker does not increase software-vGPU TODO #3, close roadmap D, or permit feature merge.

## Verification receipts

Source base: `6eea43c`, with this owned working change. Toolchain: official Zig 0.13.0, GCC 14.2.0, LLVM 19.1.7. Existing pinned protocol headers generated successfully; no dependency pins or runtime dependencies changed.

- `make vgpu-wsi-lifecycle-test`: passed 12 Zig cases in Debug and ReleaseSafe plus production C native API-double and unsupported-platform tests. Zig's testing allocator reported no outstanding allocations. Final log: `build/wsi-lifecycle/test-final.log`.
- `python3 tests/av/coverage.py venus_wsi`: passed production line coverage 99.33% (148/149) and branch coverage 92.95% (145/156), excluding 16 compiler panic/stack-canary guards under the established repository harness. Final log: `build/wsi-lifecycle/zig-coverage-final.log`.
- `python3 tests/vgpu/wsi_lifecycle_coverage.py`: passed production native C line coverage 100% (47/47), branch coverage 98.61% (71/72). This executes the real C source against local test API declarations, not a Windows runtime. Log: `build/wsi-lifecycle/native-coverage-final.log`.
- `make build/vgpu_win32_present_test.exe`: passed real Windows SDK cross-link with user32/gdi32/dwmapi. The fixture adds exact-size presentation alongside 128 scaled BGRA/FIFO/IMMEDIATE/GDI/USER cycles and checks resize rejection. Native execution is untested here. Log: `build/wsi-lifecycle/windows.log`.
- `zig test src/vgpu/venus_wsi.zig -Iinclude -Isubmodules/venus_protocol/include -lc -target x86_64-windows-gnu --test-no-exec -femit-bin=build/wsi-lifecycle/wsi-test-windows.exe`: passed Windows state-fixture cross-link; unexecuted here.
- `make vgpu-icd-units`: final run currently passed all 386 Debug tests, with ReleaseSafe pending. Includes acquired-old retirement behavior, loss-before-replacement entry point, every supported result-priority pair, mixed rejection order, and pre-/post-enqueue OOM behavior with and without shared waits. Log: `build/wsi-lifecycle/icd-units-final.log`.
- `make vgpu-wsi-lifecycle-sanitizers`: C fixture reaches LeakSanitizer's fatal ptrace restriction; the required combined gate does not pass here. `python3 tests/vgpu/state_sanitizers.py venus_wsi` separately ran all 12 current tests under owned ASan instrumentation and then hit the same LSan restriction. Zero native leaks are not verified. Logs: `build/wsi-lifecycle/sanitizers.log` and `build/wsi-lifecycle/zig-sanitizers-final.log`.
- Supplemental current binaries rerun with `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:halt_on_error=1`: C ASan/UBSan fixture and owned Zig ASan/allocator suite passed. These do not substitute for LSan. Logs: `build/wsi-lifecycle/c-asan-final.log`, `build/wsi-lifecycle/zig-asan-final.log`. Exact final Zig runner: `build/sanitizers/vgpu/venus_wsi/run-20261009T201455356147Z-127/runner`.
- Independent review reproduced the 12 Debug/ReleaseSafe cases and native coverage. It identified and the implementation corrected exact Win32 capability extents, ICD liveness bypass of failed-replacement retirement, image acquisition release on rejection, aggregate error priority, and unsafe retryable OOM after queue effects. Final integrated sign-off remains pending ReleaseSafe aggregate.

## Commit History & Progress Log

The initial implementation changeset carries #1 +100%, #2 +100%, #3 +100%, and #4 +60%, totaling +88% of this bounded milestone. Its immutable commit ID and subsequent verification attribution are recorded in the next documentation checkpoint; a commit cannot contain its own hash.

## Still required

1. Complete the current ReleaseSafe ICD aggregate and independent final review.
2. Run the unchanged prescribed ASan/LSan targets in a non-ptrace environment or CI. No leak suppression or disabled detection counts as acceptance.
3. Execute native Windows C/state fixtures and affected real Windows Vulkan/DXVK resize/minimize/replacement/loss acceptance on the final source, under the existing owner's GPU/VM lease.
4. Complete the separate AV-window generation to WSI/Wayland integration, including destruction events and unobserved numeric HWND reuse. No property/subclass workaround is claimed here.
