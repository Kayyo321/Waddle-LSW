# Feature Tracker: High-Performance Audio/Video Passthrough for Gaming

- **Contributors / Agents**: Antigravity, Codex
- **Time Started**: 2026-10-05T14:15:27-04:00
- **Time Ended**: TBD
- **Feature Branch**: feature/high-perf-av-passthrough
- **Target Merge Branch**: origin
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| **1**   | **Memory & Protocol Definitions** | | | | |
| #1.1 | Define C structs for IVSHMEM window slot header (64-byte aligned, atomic flags) | Done | 1.6% | 100% | Define C11 lock-free ownership states, stable offsets and bounded fixed-format audio metadata; verify layout on Linux and cross-build Windows. |
| #1.2 | Define C structs for IVSHMEM audio ring buffer (lockless SPSC, 48kHz 16-bit) | Done | 1.6% | 100% | Define C11 lock-free ownership states, stable offsets and bounded fixed-format audio metadata; verify layout on Linux and cross-build Windows. |
| #1.3 | Update VSOCK IPC control messages to include full geometries & lifecycle events | Done | 2.4% | 100% | Add isolated AV control IDs and a fixed little-endian Zig codec validating titles, geometry, state flags and damage rectangles before exposing data to C. |
| **2**   | **Guest Window Tracking Agent** | | | | |
| #2.1 | Register `SetWinEventHook` for CREATE, DESTROY, LOCATIONCHANGE, MINIMIZE | In Progress | 4% | 80% | Register bounded WinEvent hooks, enumerate initial visible application windows, filter shell noise, extract DWM bounds and translate tracked host resize/minimize/close requests; Windows cross-compilation passes. |
| #2.2 | Implement HWND filtering (exclude non-app windows, tooltips, hidden elements) | In Progress | 3.2% | 80% | Register bounded WinEvent hooks, enumerate initial visible application windows, filter shell noise, extract DWM bounds and translate tracked host resize/minimize/close requests; Windows cross-compilation passes. |
| #2.3 | Extract bounds via `DwmGetWindowAttribute` and send `MsgWindowGeometry` | In Progress | 3.2% | 75% | Register bounded WinEvent hooks, enumerate initial visible application windows, filter shell noise, extract DWM bounds and translate tracked host resize/minimize/close requests; Windows cross-compilation passes. |
| **3**   | **Video Capture & Transport (Guest)** | | | | |
| #3.1 | Initialize DXGI Desktop Duplication / Windows.Graphics.Capture pipeline | In Progress | 4% | 60% | Own and release D3D11 duplication resources, crop DWM bounds through reusable staging textures and publish only free video slots; Zig validates row copies and tests padding/short-buffer failures. Native fidelity remains a hardware verification gate. |
| #3.2 | Implement rendering/copying DXGI surface to IVSHMEM double/triple buffer slots | In Progress | 6.4% | 85% | Enumerate the selected device, own its handle and cached mapping, validate fixed-layout metadata before exposure, and unmap/close after workers join. Keep SDK ABI headers isolated to one boundary translation unit; Windows compilation passes. |
| #3.3 | Implement `memory_order_acquire/release` fences for buffer synchronization | Done | 4% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #3.4 | Dispatch `MsgFrameReady` over VSOCK with buffer index and damage rects | In Progress | 2.4% | 35% | Add isolated AV control IDs and a fixed little-endian Zig codec validating titles, geometry, state flags and damage rectangles before exposing data to C. |
| **4**   | **Audio Capture & Transport (Guest)** | | | | |
| #4.1 | Initialize WASAPI `AUDIOCLIENT_ACTIVATION_PARAMS` for per-process loopback | In Progress | 4% | 70% | Activate isolated process loopback using an agile ref-counted C completion handler, request fixed-format PCM, register MMCSS and drain packets into the bounded ring. Preserve callback and activation-blob lifetime after timeout and release every packet/resource; Windows cross-build passes. |
| #4.2 | Implement lockless SPSC write loop from WASAPI buffers to IVSHMEM audio ring | Done | 6.4% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #4.3 | Implement audio buffer overrun recovery and latency logging | In Progress | 3.2% | 60% | Activate isolated process loopback using an agile ref-counted C completion handler, request fixed-format PCM, register MMCSS and drain packets into the bounded ring. Preserve callback and activation-blob lifetime after timeout and release every packet/resource; Windows cross-build passes. |
| **5**   | **Host Wayland Client (Video)** | | | | |
| #5.1 | Create `xdg_toplevel` upon receiving `MsgWindowCreate` from guest | In Progress | 4% | 70% | Bind xdg-shell and create captured-window toplevels with an explicit wl_shm fallback. Retain slots through compositor release, apply bounded damage and geometry requests, and keep retired buffer contexts alive until release; host compilation passes. |
| #5.2 | Convert IVSHMEM slots to DMA-BUFs using KVMFR / memfds | In Progress | 4.8% | 70% | Validate page-aligned regions against driver-reported size, export CLOEXEC DMA-BUFs using pinned ioctls and import advertised linear ARGB slots. Close local export FDs after submission and retain compositor ownership; ordinary files select wl_shm. |
| #5.3 | Handle `MsgFrameReady` by attaching DMA-BUF to `wl_surface` and committing | In Progress | 4% | 65% | Validate page-aligned regions against driver-reported size, export CLOEXEC DMA-BUFs using pinned ioctls and import advertised linear ARGB slots. Close local export FDs after submission and retain compositor ownership; ordinary files select wl_shm. |
| #5.4 | Process `MsgWindowGeometry` to move/resize/minimize Wayland surfaces | In Progress | 4% | 50% | Bind xdg-shell and create captured-window toplevels with an explicit wl_shm fallback. Retain slots through compositor release, apply bounded damage and geometry requests, and keep retired buffer contexts alive until release; host compilation passes. |
| **6**   | **Host PipeWire Client (Audio)** | | | | |
| #6.1 | Initialize PipeWire stream matching the hardcoded IVSHMEM format | In Progress | 4% | 75% | Create a fixed-format host stream with a realtime dequeue/read/queue callback, silence underruns and count diagnostics without allocation or logging. Stop the loop before releasing stream and borrowed mapping references; host compilation passes. |
| #6.2 | Implement lockless SPSC read loop from IVSHMEM audio ring buffer | Done | 4.8% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #6.3 | Feed audio frames into PipeWire playback buffer callback | In Progress | 3.2% | 75% | Create a fixed-format host stream with a realtime dequeue/read/queue callback, silence underruns and count diagnostics without allocation or logging. Stop the loop before releasing stream and borrowed mapping references; host compilation passes. |
| **7**   | **Testing & CI Verification** | | | | |
| #7.1 | Write unit tests for lockless IVSHMEM queue mechanisms (video and audio) | In Progress | 1.6% | 75% | Run 100000 ordered video and PCM frames across concurrent producer/consumer threads, cross uint32 cursor wrap and verify sparse mapping bounds and regular-file DMA-BUF rejection. ASan/LSan/UBSan and Zig allocator tests pass with zero reported leaks. |
| #7.2 | Run LeakSanitizer/AddressSanitizer and verify zero bytes leaked | In Progress | 1.6% | 60% | Run 100000 ordered video and PCM frames across concurrent producer/consumer threads, cross uint32 cursor wrap and verify sparse mapping bounds and regular-file DMA-BUF rejection. ASan/LSan/UBSan and Zig allocator tests pass with zero reported leaks. |

| #8.1 | Pin and verify Looking Glass driver ABI headers as a submodule | Done | 1.6% | 100% | Add public HTTPS Looking Glass submodule pinned to B7 commit 27fe47c; audit GPL-2.0-or-later headers for GPL-3.0 compatibility and isolate usage to KVMFR/IVSHMEM driver ABI declarations. |

| **10** | **All-in-one AV environment provisioning** | | | | |
| #10.1 | Probe host/guest prerequisites and report actionable capability failures | Pending | 5% | 0% | Added by user: program must set up its AV environment. |
| #10.2 | Build/install pinned KVMFR module and provision shared-memory access | Pending | 5% | 0% | Running-kernel headers/signing and privilege boundaries required. |
| #10.3 | Configure managed QEMU IVSHMEM, silent audio endpoint and GPU capture | Pending | 5% | 0% | Must preserve CLI/device-management behavior. |
| #10.4 | Deploy guest AV agent and signed drivers with readiness verification | Pending | 3% | 0% | No manually prepared external AV environment assumed. |
| #10.5 | Exercise provisioned real AV session and verify latency/fidelity | Pending | 2% | 0% | Real capture/playback measurements; no mock substitution. |

**Total Feature Completion**: `62.28%`

## Commit History & Progress Log

- **Commit `3923dd5`**: `docs(tracker): initialize implementation plan and tracker`
  - **Task Impact**: 0% overall; planning baseline.
  - **Summary**: Defined the requested scope and task breakdown before implementation.

- **Commit `7992bb5`**: `docs(tracker): update implementation with resolved design decisions`
  - **Task Impact**: 0% overall; planning baseline.
  - **Summary**: Defined the requested scope and task breakdown before implementation.

- **Commit `81fd201`**: `docs(tracker): detail implementation tasks for gaming passthrough`
  - **Task Impact**: 0% overall; planning baseline.
  - **Summary**: Defined the requested scope and task breakdown before implementation.

- **Commit `f6504ce62f66`**: `docs(av): specify buffer ownership and platform constraints`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Resolve cursor ownership, compositor placement, native audio requirements and honest hardware verification boundaries.

- **Commit `d529d65e2d2e`**: `feat(av): define cache-line aligned video and audio ABI`
  - **Task Impact**: +100% to #1.1 (+2.00% overall); +100% to #1.2 (+2.00% overall)
  - **Summary**: Define C11 lock-free ownership states, stable offsets and bounded fixed-format audio metadata; verify layout on Linux and cross-build Windows.

- **Commit `683a73829fbf`**: `feat(av): implement bounded lockless video and PCM transport`
  - **Task Impact**: +100% to #3.3 (+5.00% overall); +100% to #4.2 (+8.00% overall); +40% to #4.3 (+1.60% overall); +100% to #6.2 (+6.00% overall); +35% to #7.1 (+0.70% overall)
  - **Summary**: Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass.

- **Commit `aeb6813a4a11`**: `feat(av): add bounded lifecycle and frame control codec`
  - **Task Impact**: +100% to #1.3 (+3.00% overall); +35% to #3.4 (+1.05% overall)
  - **Summary**: Add isolated AV control IDs and a fixed little-endian Zig codec validating titles, geometry, state flags and damage rectangles before exposing data to C.

- **Commit `0b2d3219fd34`**: `feat(av): track target-process Windows window lifecycle`
  - **Task Impact**: +80% to #2.1 (+4.00% overall); +80% to #2.2 (+3.20% overall); +75% to #2.3 (+3.00% overall)
  - **Summary**: Register bounded WinEvent hooks, enumerate initial visible application windows, filter shell noise, extract DWM bounds and translate tracked host resize/minimize/close requests; Windows cross-compilation passes.

- **Commit `f6accb42adb4`**: `feat(av): capture bounded visible-window crops with DXGI`
  - **Task Impact**: +60% to #3.1 (+3.00% overall); +65% to #3.2 (+5.20% overall)
  - **Summary**: Own and release D3D11 duplication resources, crop DWM bounds through reusable staging textures and publish only free video slots; Zig validates row copies and tests padding/short-buffer failures. Native fidelity remains a hardware verification gate.

- **Commit `96b7aca88c97`**: `feat(av): capture process-tree audio through WASAPI loopback`
  - **Task Impact**: +70% to #4.1 (+3.50% overall); +20% to #4.3 (+0.80% overall)
  - **Summary**: Activate isolated process loopback using an agile ref-counted C completion handler, request fixed-format PCM, register MMCSS and drain packets into the bounded ring. Preserve callback and activation-blob lifetime after timeout and release every packet/resource; Windows cross-build passes.

- **Commit `834656a81308`**: `feat(av): feed bounded PCM into realtime PipeWire playback`
  - **Task Impact**: +75% to #6.1 (+3.75% overall); +75% to #6.3 (+3.00% overall)
  - **Summary**: Create a fixed-format host stream with a realtime dequeue/read/queue callback, silence underruns and count diagnostics without allocation or logging. Stop the loop before releasing stream and borrowed mapping references; host compilation passes.

- **Commit `906bedc118f5`**: `chore(deps): pin Looking Glass B7 driver ABI headers`
  - **Task Impact**: +100% to #8.1 (+2.00% overall)
  - **Summary**: Add public HTTPS Looking Glass submodule pinned to B7 commit 27fe47c; audit GPL-2.0-or-later headers for GPL-3.0 compatibility and isolate usage to KVMFR/IVSHMEM driver ABI declarations.

- **Commit `22e96114e057`**: `feat(av): present owned video slots through Wayland toplevels`
  - **Task Impact**: +70% to #5.1 (+3.50% overall); +40% to #5.3 (+2.00% overall); +50% to #5.4 (+2.50% overall)
  - **Summary**: Bind xdg-shell and create captured-window toplevels with an explicit wl_shm fallback. Retain slots through compositor release, apply bounded damage and geometry requests, and keep retired buffer contexts alive until release; host compilation passes.

- **Commit `012793649a25`**: `feat(av): export KVMFR slots for linear Wayland DMA-BUF import`
  - **Task Impact**: +70% to #5.2 (+4.20% overall); +25% to #5.3 (+1.25% overall)
  - **Summary**: Validate page-aligned regions against driver-reported size, export CLOEXEC DMA-BUFs using pinned ioctls and import advertised linear ARGB slots. Close local export FDs after submission and retain compositor ownership; ordinary files select wl_shm.

- **Commit `0d79d35dae82`**: `fix(av): retain DMA-BUF global while binding xdg-shell`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Remove an unintended destroy in the registry callback so global announcement order cannot leave the DMA-BUF client pointer dangling. DMA-BUF is destroyed only during full client teardown.

- **Commit `c564adb6259f`**: `feat(av): define validated fixed shared-memory window pools`
  - **Task Impact**: +10% to #3.2 (+0.80% overall)
  - **Summary**: Specify a fixed page-aligned 16-window triple-buffer layout and bounded Zig initialization/lookup functions. Keep ring and headers in the 64 KiB control region and prohibit live reset or pool reuse before compositor release.

- **Commit `be6aa8d385f3`**: `feat(av): map signed Windows IVSHMEM driver memory`
  - **Task Impact**: +10% to #3.2 (+0.80% overall)
  - **Summary**: Enumerate the selected device, own its handle and cached mapping, validate fixed-layout metadata before exposure, and unmap/close after workers join. Keep SDK ABI headers isolated to one boundary translation unit; Windows compilation passes.

- **Commit `f6fc0336fc0d`**: `test(av): stress shared-memory ownership under sanitizers`
  - **Task Impact**: +40% to #7.1 (+0.80% overall); +60% to #7.2 (+1.20% overall)
  - **Summary**: Run 100000 ordered video and PCM frames across concurrent producer/consumer threads, cross uint32 cursor wrap and verify sparse mapping bounds and regular-file DMA-BUF rejection. ASan/LSan/UBSan and Zig allocator tests pass with zero reported leaks.

- **Commit `HEAD`**: `docs(av): include all-in-one environment provisioning in feature scope`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Add the user-required host module, managed VM devices, signed guest driver deployment, readiness probing and provisioned-session verification tasks. Rescale prior tasks to 80 percent and reserve 20 percent for provisioning without assuming an external AV test environment.
