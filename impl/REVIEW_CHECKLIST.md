# Return-home review checklist

**Current pending-status index · 2026-10-09 UTC · `feature/software-vgpu-slicing`**

Start here. This is the single prioritized list of open review/acceptance work;
linked feature receipts are historical evidence and the detailed handoff supplies
commands. A checked box requires an exact tested commit, binary hashes and logs in
[one result receipt](HARDWARE_TEST_RESULT_TEMPLATE.md). Never inherit a pass after
relevant source, toolchain, driver or VM changes.

Recorded prior backup: [`51583f9`](https://github.com/Kayyo321/Waddle-LSW/commit/51583f927a0ba82adecaa6d48d2814ffda97bc26).
The source links below include the follow-on lifecycle and Windows oracle changes;
record the exact containing commit you actually test.
No hardware/Adobe completion is claimed. Review in this order:

- [ ] **P0 · Source and native CI.** Fresh checkout; capture source/toolchain/binary
  versions; rerun automated gates. **Upgrade host and guest together:** old guests
  are view-only on new hosts; old hosts reject the new first-window capability.
  Linux cloud input/WSI suites and 394-case ICD Debug/ReleaseSafe runs passed;
  whole-ICD coverage passed 90.32% branches after six regression additions.
  At `f04e358`, both AV workflows and the native Windows guest passed, including
  new lifecycle/quiescence fixtures and genuine leak-enabled Linux AV tests
  ([run evidence](https://github.com/Kayyo321/Waddle-LSW/actions/runs/38002124467/job/114063342283)).
  Windows vGPU reaches 391/393 tests: two synthetic WSI capability fixtures need
  repair; their independently reviewed fix passes 394/394 in both Linux modes
  and full Windows cross-links, with native execution pending. The prior
  environment-helper and extension-oracle failures are cleared.
  Linux vGPU confirmed the worker link repair, then failed real presented-worker
  binding at mode 0; runtime diagnosis is open. Native CPU-model reproduction
  explains the recorder overflow. The published all-sites-preserving repair passes
  the exact 76,512-site replay (90.30% branches) and current 394-test aggregate
  (90.32% branches, 98.53% lines); strict sanitizer CI is pending at `51583f9`. Bounded device-wire/native, WSI and ICD sanitizer
  stages passed leak-enabled CI at `c11ce21`; local LSan remains ptrace-blocked.
  The broader 32-module ICD-owned/full-seam instrumentation migration remains a
  separate unresolved gate. Rerun affected checks on the next published source.
  [Build/log commands](HARDWARE_TEST_HANDOFF.md#4-rebuild-and-rerun-hardware-independent-gates-first)
  · [coverage source](https://github.com/Kayyo321/Waddle-LSW/commit/2d97603e3275d0c329ae81f3fe56413b641bd3d4)
  · [coverage receipt](wsi-window-lifetime/TRACKER.md)
  · [Windows helper source](https://github.com/Kayyo321/Waddle-LSW/commit/37970ae95d630c447c2fb108b1cef97535b9a059)
  · [Windows helper receipt](windows-icd-test-environment/TRACKER.md)
  · [extension oracle repair](https://github.com/Kayyo321/Waddle-LSW/commit/b317c0577cafb20aa16463544491449cd1c61715)
  · [extension oracle receipt](native-icd-extension-oracle/TRACKER.md)
  · [sanitizer repair/remaining limits](owned-sanitizer-inventory/TRACKER.md)
  · [worker link repair](https://github.com/Kayyo321/Waddle-LSW/commit/37df9499f197c51443e1aca892139cf6a41a59c6)
  · [worker execution limits](presented-worker-link/TRACKER.md)
  · [native WSI fixture repair](native-wsi-test-capabilities/TRACKER.md)
  · [all-sites recorder evidence](coverage-recorder-integrity/TRACKER.md).
- [ ] **P1 · Physical input and cleanup.** Native `--input` fixture, then ordinary
  app as a Wayland window: keyboard/mouse/wheel, focus transfer, held keys/buttons,
  disconnect, foreground denial, scaling and repeated recovery. Watch stuck input
  and unintended-window delivery. Guest layout only; IME/clipboard/tablet remain
  outside this milestone. [Exact/manual cases](HARDWARE_TEST_HANDOFF.md#5-real-windows-and-wayland-input-acceptance)
  · [input integration](https://github.com/Kayyo321/Waddle-LSW/commit/388b7b35064fcd529cf5c011562ed01ac65ff80a)
  · [input receipts](desktop-input/EVIDENCE.md).
- [ ] **P1 · Native window/swapchain lifetime.** Resize, minimize/zero area,
  replacement failure, destruction, stale frames, repeated recovery and memory
  ownership under error/pressure. Run new native lifecycle and guest-quiescence
  fixtures; test observed same-HWND readmission, delayed close/resize/destroy,
  old busy buffers and compatibility diagnostics. Unobserved reuse remains open.
  [AV generation cases](HARDWARE_TEST_HANDOFF.md#generation-safe-lifecycle-upgrade-and-regression-checks)
  · [generation implementation](https://github.com/Kayyo321/Waddle-LSW/commit/1337fe5a3a02695b047209b24f8474b60acfeb9c)
  · [generation receipts](av-window-generation/TRACKER.md)
  · [synthetic platform callback fix](https://github.com/Kayyo321/Waddle-LSW/commit/f6e5695dd4c6c365f18baeb18ffa30003c7424fc).
  [Exact/manual cases](HARDWARE_TEST_HANDOFF.md#6-real-wsi-lifecycle-acceptance)
  · [WSI implementation](https://github.com/Kayyo321/Waddle-LSW/commit/51ee17bca208db3241fefaf65fddcbc8577616dd)
  · [WSI receipts](wsi-window-lifetime/TRACKER.md).
- [ ] **P1 · Real vGPU/DXVK and native heap.** Prove real cross-VM mapping and GPU
  backend; execute native loader/direct oracles and pinned DXVK workload; complete
  heap/TLS qualification. Portable real-Windows supervisor automation is still
  missing. Mocks, cross-links and software Vulkan do not close this gate.
  [Commands/blockers](HARDWARE_TEST_HANDOFF.md#7-physical-vulkan-patched-dxvk-and-native-heap-gates).
- [ ] **P2 · Integrated desktop and simultaneous shared GPU.** Verify HWND ↔
  swapchain ↔ Wayland lifetime together, app window families, reconnect/pressure,
  color/scaling/audio, and an overlapping Linux+Windows workload on the same
  physical RTX 5080 with Linux retaining ownership. Capture adapter identity,
  outputs and agreed budgets. [Acceptance cases](HARDWARE_TEST_HANDOFF.md#8-integrated-app-shared-gpu-and-remaining-roadmap-gates).
- [ ] **P2 · Actual Adobe feasibility.** Pin app/project/reference versions;
  separately prove the Photoshop operation and required D3D12/OpenCL path, then
  Premiere effects, hardware decode and hardware encode. These are still open
  implementation/qualification gaps; launch success or CPU fallback is not enough.
  [Remaining roadmap](SHARED_GPU_DESKTOP_ROADMAP.md#5-milestone-work-packages-and-passfail-gates).

Stop on corruption, wrong-window injection, unexplained loss/leaks or a mismatched
backend. Preserve logs and [follow the triage rules](HARDWARE_TEST_HANDOFF.md#9-failure-triage-and-stopping-rules).
Use a fresh isolated test checkout and the machine owner's session/GPU lease;
do not disturb preserved acceptance artifacts. After each future checkpoint,
update affected rows here and link its exact source/evidence, rather than creating
another competing pending list. Detailed cases remain in the handoff.
