# Return-home review checklist

**Current pending-status index · 2026-10-09 UTC · `feature/software-vgpu-slicing`**

Start here. This is the single prioritized list of open review/acceptance work;
linked feature receipts are historical evidence and the detailed handoff supplies
commands. A checked box requires an exact tested commit, binary hashes and logs in
[one result receipt](HARDWARE_TEST_RESULT_TEMPLATE.md). Never inherit a pass after
relevant source, toolchain, driver or VM changes.

Latest verified backup: [`1de3fdd`](https://github.com/Kayyo321/Waddle-LSW/commit/1de3fddf13067238ca4e428c223338ca79e37287).
No hardware/Adobe completion is claimed. Review in this order:

- [ ] **P0 · Source and native CI.** Fresh checkout; capture source/toolchain/binary
  versions; rerun automated gates. Linux cloud input/WSI suites and 392-case ICD
  Debug/ReleaseSafe runs passed. Whole-ICD coverage passed 90.32% branches after
  six regression additions. Windows AV/guest CI passed the earlier backup;
  Windows ICD units exposed POSIX environment-helper linkage and are being fixed.
  Latest-SHA CI is pending. Required local LSan was blocked by ptrace, not passed.
  [Build/log commands](HARDWARE_TEST_HANDOFF.md#4-rebuild-and-rerun-hardware-independent-gates-first)
  · [coverage source](https://github.com/Kayyo321/Waddle-LSW/commit/2d97603e3275d0c329ae81f3fe56413b641bd3d4)
  · [coverage receipt](wsi-window-lifetime/TRACKER.md).
- [ ] **P1 · Physical input and cleanup.** Native `--input` fixture, then ordinary
  app as a Wayland window: keyboard/mouse/wheel, focus transfer, held keys/buttons,
  disconnect, foreground denial, scaling and repeated recovery. Watch stuck input
  and unintended-window delivery. Guest layout only; IME/clipboard/tablet remain
  outside this milestone. [Exact/manual cases](HARDWARE_TEST_HANDOFF.md#5-real-windows-and-wayland-input-acceptance)
  · [input integration](https://github.com/Kayyo321/Waddle-LSW/commit/388b7b35064fcd529cf5c011562ed01ac65ff80a)
  · [input receipts](desktop-input/EVIDENCE.md).
- [ ] **P1 · Native window/swapchain lifetime.** Resize, minimize/zero area,
  replacement failure, destruction, stale frames, repeated recovery and memory
  ownership under error/pressure. Unobserved same-process HWND reuse remains open.
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
