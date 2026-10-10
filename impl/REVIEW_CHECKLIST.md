# Return-home review checklist

**Current pending-status index · 2026-10-10 UTC · `feature/software-vgpu-slicing`**

Start here. This is the single prioritized list of open review/acceptance work;
linked feature receipts are historical evidence and the detailed handoff supplies
commands. A checked box requires an exact tested commit, binary hashes and logs in
[one result receipt](HARDWARE_TEST_RESULT_TEMPLATE.md). Never inherit a pass after
relevant source, toolchain, driver or VM changes.

Recorded green backup: [`879d0c3`](https://github.com/Kayyo321/Waddle-LSW/commit/879d0c38c29e22b400c29fd79236b36df6f015f3).
The source links below include the follow-on lifecycle and Windows oracle changes;
record the exact containing commit you actually test.
No hardware/Adobe completion is claimed. Review in this order:

- [ ] **P0 · Source and native CI.** Fresh checkout; capture source/toolchain/binary
  versions; rerun automated gates. **Upgrade host and guest together for V3
  lease recovery:** V1 guests are view-only; V2 guests keep prior fail-closed
  input behavior; V2 hosts reject a V3 guest at its first window.
  **All six native CI workflow runs passed at `879d0c3`.** Windows ICD passes
  394 Debug + 394 ReleaseSafe tests and native loader cycles. Linux passes all
  eight strict recorder sanitizer probes, 90.30% whole-ICD branch coverage,
  direct/loader production-worker normal and leak-enabled sanitizer targets,
  and subsequent export/surface/Wayland/runtime/context gates. The prior Windows
  fixture, query-oracle and recorder failures are closed at this exact source.
  [Exact green receipt and job links](CI_GREEN_CHECKPOINT_2026-10-10.md).
  A later documentation-only `414d7e4` run exposed an intermittent CLI terminal
  shutdown race (success output followed by Broken pipe). A deterministic
  production-session regression reproduces it; the reviewed correction passes
  15 cases and bounded stress checks. Fresh native CI remains required
  ([shutdown receipt](auto-terminal-race/TRACKER.md)).
  Local LSan remains ptrace-blocked; supported CI supplies the leak evidence.
  The broader 32-module ICD-owned/full-seam instrumentation migration remains a
  separate unresolved gate. Rerun affected checks on subsequent sources and on
  the hardware checkout; no physical GPU or application acceptance is implied.
  [Build/log commands](HARDWARE_TEST_HANDOFF.md#4-rebuild-and-rerun-hardware-independent-gates-first)
  · [exact source mapping](CLOUD_PUBLICATION_CHECKPOINT.md)
  · [broader sanitizer gate](owned-sanitizer-inventory/TRACKER.md).
- [ ] **P1 · Physical input and cleanup.** Use an isolated interactive guest
  desktop: `SendInput` is global, and foreground checks/hooks cannot atomically
  guarantee an HWND destination or detect every unobserved transition.
  Native `--input` fixture, then ordinary
  app as a Wayland window: keyboard/mouse/wheel, focus transfer, held keys/buttons,
  disconnect, foreground denial, scaling and repeated recovery. Watch stuck input
  and unintended-window delivery. Input-lease protocol and integrated host/guest source are
  independently reviewed at local `bcc1a67`; functional/coverage gates and
  cross-links pass. Supported sanitizer CI and physical acceptance remain pending
  ([lease scope and progress](input-lease-recovery/TRACKER.md)).
  Guest layout only; IME/clipboard/tablet remain
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
