# Feature Tracker: Presented Worker Native WSI Link Input

- **Contributors / Agents**: Presented-worker link repair worker; independent presented-worker-input reviewer
- **Time Started**: 2026-10-09T22:49:08Z
- **Time Ended**: TBD
- **Feature Branch**: feature/software-vgpu-slicing
- **Target Merge Branch**: origin
- **Current Overall Status**: Review

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| #1 | Inspect native sink contract and add its existing shared link input | Done | 30% | 100% | No fixture stub or assertion change |
| #2 | Reproduce old link failure and build actual corrected direct/loader executables | Done | 30% | 100% | Four executables linked; native symbols defined exactly once |
| #3 | Independent review and exact-file atomic commit receipts | Done | 20% | 100% | Minimal source correction approved; exact implementation commit recorded below |
| #4 | Run unchanged independent production-worker and sanitizer CI gates | Blocked | 20% | 0% | Local renderer/toolchain absent; native LSan cannot inspect process |

**Total Feature Completion**: `80.0%`

## Verification Receipts

- Base reviewed source includes `df5675b`; concurrent documentation-only parent
  commit `485f966` does not affect this correction. Toolchain: Debian GCC
  14.2.0-19 and Zig 0.13.0, with the existing local SDK environment sourced.
- Exact CI command attempted before and after the change:
  `make vgpu-presented-worker-test vgpu-presented-worker-sanitizers`.
  Both attempts exit **2** while configuring the absent production renderer:
  `/bin/sh: 1: meson: not found`. No production worker or render-server executable
  exists here. The Linux development pkg-config entries for epoxy, libdrm and
  Vulkan, plus Vulkan ICD manifests, are also absent. This is a prerequisite
  blocker, not an accepted runtime result.
- Original link failure independently reproduced with the real make recipe and
  original object list: derive `old_objects` by removing only
  `build/venus_win32_present.o` from `VgpuPresentedWorkerObjects`, then run
  `make -o build/waddle_vgpu_worker "VgpuPresentedWorkerObjects=$old_objects"
  build/vgpu_presented_worker_test`. Exit **2**, undefined
  `venus_win32_present_extent` and `venus_win32_present_pixels_exact`, including
  `venus_wsi.zig:306` and `scripts/vgpu.mk:593`. The `-o` flag excludes only the
  absent production-worker build for this link-only experiment; it does not
  execute or replace that worker.
- Corrected actual executable links exit **0**:
  `make -o build/waddle_vgpu_worker -o build/waddle_vgpu_worker_sanitized
  build/vgpu_presented_worker_test build/vgpu_presented_worker_sanitized
  build/vgpu_loader_worker_test build/vgpu_loader_worker_sanitized`.
  These are the real make target recipes, including their normal sanitizer
  flags. `nm --defined-only` finds exactly one definition each of
  `venus_win32_present_extent`, `venus_win32_present_pixels`, and
  `venus_win32_present_pixels_exact` in every executable; `nm -u` finds none of
  these symbols unresolved. No native sink is mocked.
- Forced-rebuild dry-run recipe audit finds exactly one native sink object in
  each direct and loader executable recipe, both phony sanitizer rebuild
  recipes, and the regular/sanitized remote-image recipes. The remote-image
  dry-run excludes absent renderer/worker prerequisites and the hardcoded system
  Wayland XML prerequisite; it is **not** a successful remote-image build.
  The image, compute, compute-push and triangle selectors reuse the four linked
  worker executables, without new link recipes.
- `make vgpu-wsi-lifecycle-test` passes: all **12 Debug and 12 ReleaseSafe Zig
  tests**, the real C sink compiled against Win32 API doubles, and the production
  unsupported-platform boundary. This verifies portable exact-size/minimize/
  resize/loss behavior only, not Windows runtime or hardware presentation.
- `make vgpu-wsi-lifecycle-sanitizers` builds the genuine native-boundary test
  with ASan/LSan/UBSan and runs with
  `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1`. It exits **2**
  after LeakSanitizer's fatal process-inspection/ptrace error (child status 134).
  The same exact verification under approved elevated execution gives the same
  blocker. The following Zig state-sanitizer recipe does not run after that
  failure. Leak detection is never disabled and no sanitizer pass is claimed.
- A direct invocation of the corrected sanitized acceptance executable with
  the real expected worker/render-server paths reports
  `Presented worker binding failed: mode=0`, then the same LSan runtime blocker
  (status 134). No independent subprocess execution proof is available locally.
- `make vgpu-wsi-lifecycle-coverage` exits **0** with all **12 instrumented
  WSI tests** passing. Zig WSI production branches: **92.95% (145/156)**;
  lines: **99.33% (148/149)**; the existing harness excludes **16** compiler
  panic/stack-canary guards. Production C native boundary branches:
  **98.61% (71/72)**; lines: **100.00% (47/47)**. Existing denominators and
  90% thresholds remain unchanged. Artifacts:
  `build/coverage/vgpu/venus_wsi/run-20261009T225202325685Z-12/` and
  `build/coverage/vgpu/wsi_native/run-20261009T225238855418Z-70/`.
- Independent source/symbol review approves the one-line correction after
  verifying the production definition, existing source/header dependencies,
  every inherited consumer and all four actual executable symbol tables. The
  review requires the blocked runtime, LSan and remote-image evidence to remain
  explicitly separate from successful links; these receipts preserve that limit.
- `git diff --check` passes. Only the shared object-list line and this bounded
  repair's implementation/tracker documents change. Ownership assertions,
  independent exec, deadlines, watchdogs, sanitizer flags and CI gates are
  unchanged.

Local command logs and executable SHA-256 receipts are preserved in
`build/presented-worker-link/`. In particular `presented-worker-old-link.log`,
`presented-worker-fixed-link.log`, `presented-worker-exact-gate.log`,
`presented-worker-native-sink.log`, `presented-worker-native-sanitizers.log`, and
`presented-worker-runtime-blocked.log` keep successful link evidence separate
from the failed or blocked runtime checks.

## Commit History & Progress Log

- **Commit `18e9d57ed79775779ad4d7416b77e516adbf26fd`**:
  `fix(vgpu): link native WSI sink into worker acceptance binaries`
  - **Task Impact**: #1 +100% (+30% overall), #2 +100% (+30%), #3 +100%
    (+20%); bounded repair is **80%** complete.
  - **Summary**: One production native-sink object added to the shared worker
    link list, four actual executable links and unique-symbol checks, unchanged
    portable WSI/coverage verification, explicit runtime/sanitizer blockers, and
    independent approval. TODO #4 remains 0% pending unchanged Linux CI gates.

This documentation-only attribution receipt records the exact implementation
commit above. Task impact: **+0%**; total remains **80%**. Independent production
worker execution and leak acceptance are still pending on the published
revision; the local link evidence cannot advance TODO #4.
