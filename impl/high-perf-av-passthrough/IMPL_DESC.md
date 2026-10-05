# Implementation Description: High-Performance Audio/Video Passthrough for Gaming

## 1. Title & High-Level Scope
- **Title**: High-Performance Audio/Video Passthrough for Gaming
- **Scope**: Implements near-bare-metal latency and FPS for Windows gaming by tracking game window lifecycle (move, resize, minimize, fullscreen, close), capturing the entire window exactly as it appears (including native Windows title bars and menu bars) using DXGI Desktop Duplication / Windows Graphics Capture, capturing game audio using WASAPI Application Loopback, and transporting both video and audio to the Linux host via IVSHMEM lockless ring buffers.
- **Out of Scope**: General full-desktop capturing. Virtual audio devices (we rely on WASAPI loopback combined with a QEMU `-audiodev none` configuration to prevent double audio without requiring custom drivers). Server-side Wayland decorations (we capture the native Windows decorations for 100% fidelity).

## 2. Architecture & Inter-Component Interactions
- **Guest Agent**: Uses `SetWinEventHook` to track window state (move, resize, minimize, fullscreen, close). Hooks into DXGI for video. Audio is captured via **per-process WASAPI Application Loopback** to isolate game audio from system sounds.
- **IPC Layer**:
  - Control channel (VSOCK): Transmits window state changes, geometry updates, and input events.
  - Video channel (IVSHMEM): Zero-copy DXGI surface blitting.
  - Audio channel (IVSHMEM): Low-latency audio stream using a lockless circular buffer for PCM data. The format is hardcoded to a standard gaming format (48kHz, 16-bit, Stereo) to avoid dynamic negotiation overhead.
- **Host Client**: Reads video frames via DMA-BUF and presents using Wayland `xdg_toplevel` (displaying the captured Windows decorations). Reads audio from IVSHMEM and plays back via PipeWire/PulseAudio.

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

## 8. Binding implementation refinements

The sketches above are conceptual, not compilable ABI definitions. The C ABI is
x86-64 little-endian only, with lock-free 32-bit atomics and explicit padding.
C owns platform handles; Zig owns control decoding, length/offset validation and
bounded PCM/pixel copies. AV uses a separate control connection on port 5001;
CLI port and CLI message IDs remain unchanged. No audio format negotiation occurs.

Video ownership is Free -> Writing -> Ready -> Consuming -> Free. Only a Free
slot can be claimed. Publication is release; claim is acquire; compositor release
is the only event permitting reuse after presentation. Triple buffering bounds
queued video. Dropped notifications must release their Ready slot. Headers are
64 bytes; the state is at offset 0, buffer index at 4, frame sequence at 8,
timestamp at 16, width/height/stride/format at 24/28/32/36 and reserved bytes at 40.
Each pixel region is page aligned and bounded by the negotiated mapping size.
No shared-memory pointer is transmitted on the wire.

Audio uses monotonically increasing uint32 cursors with modular subtraction,
capacity below 2^31 frames, four bytes per stereo S16LE frame, and 48,000 Hz.
Only producer writes write_head; only consumer writes read_head. Overrun drops
new incoming frames, counts them and leaves unread frames intact. Consumer may
skip old frames before copying to bound latency; producer never overwrites data
being read. Underrun writes silence. Initialization/reset requires both peers
quiescent. Runtime callbacks allocate no memory, take no locks, and do no logging.

Wayland placement is compositor-owned: x/y are retained as guest metadata;
standard xdg-shell provides resize/fullscreen/minimize requests but no global
position setter or unminimize request. Host configure dimensions are sent back
to SetWindowPos; close is translated to WM_CLOSE. No synthetic claim of moving
a toplevel to arbitrary screen coordinates is permitted.

A regular shared-memory file/memfd supports wl_shm only. Real DMA-BUF export
requires a KVMFR device and its ioctl ABI plus compositor linux-dmabuf support.
Import failure must select the explicit wl_shm fallback. DMA-BUF is never
claimed for a regular file. All exported FDs have a single documented owner.

Per-process WASAPI loopback requires Windows build 20348 or later; unsupported
systems return an explicit error rather than capturing the whole system. See
[Microsoft's process loopback contract](https://learn.microsoft.com/en-us/windows/win32/api/audioclientactivationparams/ns-audioclientactivationparams-audioclient_process_loopback_params).
48kHz stereo S16LE is requested with shared-mode conversion flags. A disabled
QEMU audio backend still requires a guest render endpoint; an absent endpoint
is an error. Desktop Duplication crops a visible window from a monitor and cannot
promise unoccluded content or original window alpha. Full feature verification
therefore requires per-window Windows Graphics Capture and real GPU/VM tests;
mock frame tests are not evidence of that fidelity or of end-to-end latency.

Tracker entries use HEAD for the commit containing the entry (self hashes cannot
be embedded in their own content). The next atomic commit resolves that reference
to the preceding immutable hash. Historical planning commits have zero progress.

### Control encoding

Each independent AV frame is exactly 328 bytes: LE uint16 magic 0x574c at 0,
LE uint16 type at 2, LE uint32 payload length 320 at 4. Payload offsets are
window_id 8 (u64), x/y 16/20 (i32), width/height 24/28 (u32), flags 32, dpi 36,
process_id 40, buffer_index 44, sequence 48 (u64), damage_x/y 56/60 (i32),
damage_width/height 64/68 (u32), NUL-terminated UTF-8 title 72..327. Message IDs
are create 1, destroy 2, geometry 3, frame 4, close 5. Window ID is nonzero.
Non-destroy/close dimensions are 1..8192, DPI 48..768, flags only bits 0/1.
Frame index is 0..2, sequence nonzero, damage nonempty within dimensions.
Encoders zero title padding. Decoders never cast wire buffers to native structs
and leave the output unchanged on error. No payload allocation is necessary.

### Driver ABI dependency

Looking Glass B7 is pinned at 27fe47cbe2a3a8da986d310ab866f0b646ed68f5 under
submodules/looking_glass, public HTTPS upstream https://github.com/gnif/LookingGlass.git.
Only module/kvmfr.h and vendor/ivshmem/ivshmem.h define driver ABI boundaries;
no upstream application implementation is linked or loosely copied. The KVMFR header carries a GPL-2.0-or-later notice; the IVSHMEM header
is distributed within the same GPL-2.0-or-later project, compatible with this repository's GPL-3.0.
Kernel driver installation remains an operator task, not an automatic build step.
Host uses KVMFR_DMABUF_GETSIZE/CREATE; guest uses the signed Red Hat IVSHMEM
interface GUID and map/unmap ioctls. No nested vendor sources are used.

### DMA-BUF export ownership

The host binds linux-dmabuf v3 only if present and requires advertised linear
ARGB8888. Each slot exports its page-aligned payload offset/capacity using the
pinned KVMFR ioctl ABI. GETSIZE bounds-checks the region before CREATE. Returned
CLOEXEC FD is closed immediately after the protocol duplicates it. A regular
file fails GETSIZE and selects wl_shm. The compositor owns the buffer import
until wl_buffer.release. Immediate DMA-BUF import rejection is a display error;
the session must reconnect with the shm path rather than reuse consumed pixels.
