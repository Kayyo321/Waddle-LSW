# Feature Tracker: High-Performance Audio/Video Passthrough for Gaming

- **Contributors / Agents**: Antigravity, Codex, Codex, Codex, Codex, Codex, Codex, Codex
- **Time Started**: 2026-10-05T14:15:27-04:00
- **Time Ended**: TBD
- **Feature Branch**: feature/high-perf-av-passthrough
- **Target Merge Branch**: origin
- **Current Overall Status**: In Progress

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| **1**   | **Memory & Protocol Definitions** | | | | |
| #1.1 | Define C structs for IVSHMEM window slot header (64-byte aligned, atomic flags) | Done | 2% | 100% | Define C11 lock-free ownership states, stable offsets and bounded fixed-format audio metadata; verify layout on Linux and cross-build Windows. |
| #1.2 | Define C structs for IVSHMEM audio ring buffer (lockless SPSC, 48kHz 16-bit) | Done | 2% | 100% | Define C11 lock-free ownership states, stable offsets and bounded fixed-format audio metadata; verify layout on Linux and cross-build Windows. |
| #1.3 | Update VSOCK IPC control messages to include full geometries & lifecycle events | Done | 3% | 100% | Add isolated AV control IDs and a fixed little-endian Zig codec validating titles, geometry, state flags and damage rectangles before exposing data to C. |
| **2**   | **Guest Window Tracking Agent** | | | | |
| #2.1 | Register `SetWinEventHook` for CREATE, DESTROY, LOCATIONCHANGE, MINIMIZE | In Progress | 5% | 80% | Register bounded WinEvent hooks, enumerate initial visible application windows, filter shell noise, extract DWM bounds and translate tracked host resize/minimize/close requests; Windows cross-compilation passes. |
| #2.2 | Implement HWND filtering (exclude non-app windows, tooltips, hidden elements) | In Progress | 4% | 80% | Register bounded WinEvent hooks, enumerate initial visible application windows, filter shell noise, extract DWM bounds and translate tracked host resize/minimize/close requests; Windows cross-compilation passes. |
| #2.3 | Extract bounds via `DwmGetWindowAttribute` and send `MsgWindowGeometry` | In Progress | 4% | 75% | Register bounded WinEvent hooks, enumerate initial visible application windows, filter shell noise, extract DWM bounds and translate tracked host resize/minimize/close requests; Windows cross-compilation passes. |
| **3**   | **Video Capture & Transport (Guest)** | | | | |
| #3.1 | Initialize DXGI Desktop Duplication / Windows.Graphics.Capture pipeline | In Progress | 5% | 60% | Own and release D3D11 duplication resources, crop DWM bounds through reusable staging textures and publish only free video slots; Zig validates row copies and tests padding/short-buffer failures. Native fidelity remains a hardware verification gate. |
| #3.2 | Implement rendering/copying DXGI surface to IVSHMEM double/triple buffer slots | In Progress | 8% | 65% | Own and release D3D11 duplication resources, crop DWM bounds through reusable staging textures and publish only free video slots; Zig validates row copies and tests padding/short-buffer failures. Native fidelity remains a hardware verification gate. |
| #3.3 | Implement `memory_order_acquire/release` fences for buffer synchronization | Done | 5% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #3.4 | Dispatch `MsgFrameReady` over VSOCK with buffer index and damage rects | In Progress | 3% | 35% | Add isolated AV control IDs and a fixed little-endian Zig codec validating titles, geometry, state flags and damage rectangles before exposing data to C. |
| **4**   | **Audio Capture & Transport (Guest)** | | | | |
| #4.1 | Initialize WASAPI `AUDIOCLIENT_ACTIVATION_PARAMS` for per-process loopback | In Progress | 5% | 70% | Activate isolated process loopback using an agile ref-counted C completion handler, request fixed-format PCM, register MMCSS and drain packets into the bounded ring. Preserve callback and activation-blob lifetime after timeout and release every packet/resource; Windows cross-build passes. |
| #4.2 | Implement lockless SPSC write loop from WASAPI buffers to IVSHMEM audio ring | Done | 8% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #4.3 | Implement audio buffer overrun recovery and latency logging | In Progress | 4% | 60% | Activate isolated process loopback using an agile ref-counted C completion handler, request fixed-format PCM, register MMCSS and drain packets into the bounded ring. Preserve callback and activation-blob lifetime after timeout and release every packet/resource; Windows cross-build passes. |
| **5**   | **Host Wayland Client (Video)** | | | | |
| #5.1    | Create `xdg_toplevel` upon receiving `MsgWindowCreate` from guest | Pending | 5% | 0% | - |
| #5.2    | Convert IVSHMEM slots to DMA-BUFs using KVMFR / memfds | Pending | 8% | 0% | - |
| #5.3    | Handle `MsgFrameReady` by attaching DMA-BUF to `wl_surface` and committing | Pending | 5% | 0% | - |
| #5.4    | Process `MsgWindowGeometry` to move/resize/minimize Wayland surfaces | Pending | 5% | 0% | - |
| **6**   | **Host PipeWire Client (Audio)** | | | | |
| #6.1    | Initialize PipeWire stream matching the hardcoded IVSHMEM format | Pending | 5% | 0% | - |
| #6.2 | Implement lockless SPSC read loop from IVSHMEM audio ring buffer | Done | 6% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #6.3    | Feed audio frames into PipeWire playback buffer callback | Pending | 4% | 0% | - |
| **7**   | **Testing & CI Verification** | | | | |
| #7.1 | Write unit tests for lockless IVSHMEM queue mechanisms (video and audio) | In Progress | 2% | 35% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #7.2    | Run LeakSanitizer/AddressSanitizer and verify zero bytes leaked | Pending | 2% | 0% | - |

**Total Feature Completion**: `52.05%`

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

- **Commit `HEAD`**: `feat(av): capture process-tree audio through WASAPI loopback`
  - **Task Impact**: +70% to #4.1 (+3.50% overall); +20% to #4.3 (+0.80% overall)
  - **Summary**: Activate isolated process loopback using an agile ref-counted C completion handler, request fixed-format PCM, register MMCSS and drain packets into the bounded ring. Preserve callback and activation-blob lifetime after timeout and release every packet/resource; Windows cross-build passes.
