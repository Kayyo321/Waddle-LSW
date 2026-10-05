# Feature Tracker: High-Performance Audio/Video Passthrough for Gaming

- **Contributors / Agents**: Antigravity
- **Time Started**: 2026-10-05T14:15:27-04:00
- **Time Ended**: TBD
- **Feature Branch**: feature/high-perf-av-passthrough
- **Target Merge Branch**: origin
- **Current Overall Status**: Planning

## Tasks & Progress

| TODO ID | Task Description | Status | Weight (%) | Progress (%) | Notes / Blockers |
|:-------:|:-----------------|:------:|:----------:|:------------:|:-----------------|
| **1**   | **Memory & Protocol Definitions** | | | | |
| #1.1    | Define C structs for IVSHMEM window slot header (64-byte aligned, atomic flags) | Pending | 2% | 0% | - |
| #1.2    | Define C structs for IVSHMEM audio ring buffer (lockless SPSC, 48kHz 16-bit) | Pending | 2% | 0% | - |
| #1.3    | Update VSOCK IPC control messages to include full geometries & lifecycle events | Pending | 3% | 0% | - |
| **2**   | **Guest Window Tracking Agent** | | | | |
| #2.1    | Register `SetWinEventHook` for CREATE, DESTROY, LOCATIONCHANGE, MINIMIZE | Pending | 5% | 0% | - |
| #2.2    | Implement HWND filtering (exclude non-app windows, tooltips, hidden elements) | Pending | 4% | 0% | - |
| #2.3    | Extract bounds via `DwmGetWindowAttribute` and send `MsgWindowGeometry` | Pending | 4% | 0% | - |
| **3**   | **Video Capture & Transport (Guest)** | | | | |
| #3.1    | Initialize DXGI Desktop Duplication / Windows.Graphics.Capture pipeline | Pending | 5% | 0% | - |
| #3.2    | Implement rendering/copying DXGI surface to IVSHMEM double/triple buffer slots | Pending | 8% | 0% | - |
| #3.3    | Implement `memory_order_acquire/release` fences for buffer synchronization | Pending | 5% | 0% | - |
| #3.4    | Dispatch `MsgFrameReady` over VSOCK with buffer index and damage rects | Pending | 3% | 0% | - |
| **4**   | **Audio Capture & Transport (Guest)** | | | | |
| #4.1    | Initialize WASAPI `AUDIOCLIENT_ACTIVATION_PARAMS` for per-process loopback | Pending | 5% | 0% | - |
| #4.2    | Implement lockless SPSC write loop from WASAPI buffers to IVSHMEM audio ring | Pending | 8% | 0% | - |
| #4.3    | Implement audio buffer overrun recovery and latency logging | Pending | 4% | 0% | - |
| **5**   | **Host Wayland Client (Video)** | | | | |
| #5.1    | Create `xdg_toplevel` upon receiving `MsgWindowCreate` from guest | Pending | 5% | 0% | - |
| #5.2    | Convert IVSHMEM slots to DMA-BUFs using KVMFR / memfds | Pending | 8% | 0% | - |
| #5.3    | Handle `MsgFrameReady` by attaching DMA-BUF to `wl_surface` and committing | Pending | 5% | 0% | - |
| #5.4    | Process `MsgWindowGeometry` to move/resize/minimize Wayland surfaces | Pending | 5% | 0% | - |
| **6**   | **Host PipeWire Client (Audio)** | | | | |
| #6.1    | Initialize PipeWire stream matching the hardcoded IVSHMEM format | Pending | 5% | 0% | - |
| #6.2    | Implement lockless SPSC read loop from IVSHMEM audio ring buffer | Pending | 6% | 0% | - |
| #6.3    | Feed audio frames into PipeWire playback buffer callback | Pending | 4% | 0% | - |
| **7**   | **Testing & CI Verification** | | | | |
| #7.1    | Write unit tests for lockless IVSHMEM queue mechanisms (video and audio) | Pending | 2% | 0% | - |
| #7.2    | Run LeakSanitizer/AddressSanitizer and verify zero bytes leaked | Pending | 2% | 0% | - |

**Total Feature Completion**: `0.0%`

## Commit History & Progress Log
