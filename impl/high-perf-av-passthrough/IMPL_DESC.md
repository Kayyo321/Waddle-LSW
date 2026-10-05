# Implementation Description: High-Performance Audio/Video Passthrough for Gaming

## 1. Title & High-Level Scope
- **Title**: High-Performance Audio/Video Passthrough for Gaming
- **Scope**: Implements near-bare-metal latency and FPS for Windows gaming by tracking game window lifecycle (move, resize, minimize, fullscreen, close), capturing the window surface using DXGI Desktop Duplication / Windows Graphics Capture, capturing game audio using WASAPI loopback, and transporting both video and audio to the Linux host via IVSHMEM and VirtIO. Host client handles server-side decorations (File, Edit menus natively rendered by Wayland if desired) and PipeWire audio injection.
- **Out of Scope**: General full-desktop capturing. Virtual audio devices (uses loopback on existing devices).

## 2. Architecture & Inter-Component Interactions
- **Guest Agent**: Uses `SetWinEventHook` to track window state (move, resize, minimize, fullscreen, close). Hooks into DXGI for video and WASAPI for audio. Audio is captured per-process or system-wide loopback with session filtering.
- **IPC Layer**:
  - Control channel (VSOCK): Transmits window state changes, geometry updates, and input events.
  - Video channel (IVSHMEM): Zero-copy DXGI surface blitting.
  - Audio channel (IVSHMEM or VSOCK): Low-latency audio stream, using a circular buffer in IVSHMEM for PCM data.
- **Host Client**: Reads video frames via DMA-BUF and presents using Wayland `xdg_toplevel` (supporting decorations like File, Edit menus). Reads audio from IVSHMEM and plays back via PipeWire/PulseAudio.

## 3. Data Structures, Protocols & Memory Layouts
- **Window Slot Header** (IVSHMEM):
  ```c
  struct alignas(64) window_slot_header_t {
      std::atomic<uint32_t> slot_state; // SlotReady, SlotWriting, SlotConsuming
      uint32_t buffer_index;
      uint64_t frame_sequence;
      uint64_t timestamp_ns;
      uint32_t width;
      uint32_t height;
      uint32_t stride;
      uint32_t format;
      uint8_t reserved[24];
  };
  ```
- **Audio Ring Buffer** (IVSHMEM):
  ```c
  struct alignas(64) audio_ring_header_t {
      std::atomic<uint32_t> write_head;
      std::atomic<uint32_t> read_head;
      uint32_t sample_rate;
      uint32_t channels;
      uint32_t format; // PCM 16-bit, 24-bit, Float32
      uint8_t reserved[44];
  };
  ```
- **Control Messages** (VSOCK):
  `MsgWindowGeometry` updated to include fullscreen and minimize states.

## 4. Step-by-Step Execution Sequence
1. **Initialization**: Guest agent starts, connects to VSOCK, maps IVSHMEM. Host client initializes Wayland and PipeWire.
2. **Window Open**: Guest detects `EVENT_OBJECT_CREATE`. If it's a game/target window, allocates IVSHMEM slots for video and audio.
3. **Capture Loop**: Guest captures DXGI frame -> writes to IVSHMEM -> fences -> sends `MsgFrameReady` via VSOCK. WASAPI captures audio -> writes to IVSHMEM audio ring buffer -> updates `write_head`.
4. **Presentation**: Host receives `MsgFrameReady`, imports DMA-BUF, calls `wl_surface_damage_buffer`, commits. Host audio thread polls or waits on eventfd for audio data, writes to PipeWire.
5. **Window Management**: Host user moves Wayland window -> sends `MsgWindowGeometry` -> Guest calls `SetWindowPos`. Same for resizing, minimizing, and closing (`WM_CLOSE`).

## 5. Concurrency, Threading & Synchronization
- Video ring buffer uses acquire-release atomic fences (`std::memory_order_release` after writing, `std::memory_order_acquire` before reading).
- Lock-free SPSC (Single Producer Single Consumer) queue for audio PCM data.
- Guest capture loop runs on a dedicated high-priority multimedia thread (`AvSetMmThreadCharacteristics`).

## 6. Error Handling & Failure Modes
- Disconnect: If VSOCK disconnects, guest agent releases WASAPI and DXGI resources and attempts reconnect. Host destroys Wayland surfaces.
- Buffer Overrun: If audio `write_head` catches up to `read_head`, guest drops oldest frames and increments an overrun counter.
- DXGI Lost: If surface is lost, guest re-initializes DXGI capture pipeline seamlessly.

## 7. Verification & Testing Criteria
- **Latency**: End-to-end video latency from DXGI present to Wayland commit must be < 7ms at 144Hz. Audio latency < 10ms.
- **LeakSanitizer**: Zero leaked bytes across all tests.
- **Coverage**: 90% coverage for audio and video ring buffer logic.
