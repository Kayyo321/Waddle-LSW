# Project Specification: Waddle-LSW (Waddle Linux Subsystem for Windows)

## 1. Executive Summary & Vision

**Waddle-LSW** (Waddle Linux Subsystem for Windows) is a free, open-source compatibility layer and subsystem architecture designed to deliver seamless, performant integration of Microsoft Windows applications into a native Linux desktop environment.

Unlike traditional virtual machine viewers that trap Windows within a single monolithic hypervisor window, Waddle-LSW achieves seamless, rootless/borderless application integration. Individual Windows application windows are tracked inside a lightweight guest VM, their individual surface buffers are captured at high framerates with low latency, transmitted across domain boundaries via high-throughput shared memory, and composed directly onto the host Linux desktop as native Wayland surfaces and subsurfaces.

### Core Objectives
- **Zero-Friction Host Integration**: Windows applications appear alongside native Linux windows in standard Wayland desktop environments (GNOME, KDE Plasma, Sway, Hyprland, etc.), complete with window management, positioning, stacking, and focus.
- **Near-Native Graphics & Compute Performance**: Utilize GPU partitioning / paravirtualization (GPU-PV/vGPU slicing) so Windows workloads access direct hardware acceleration.
- **Unified Filesystem Access**: High-performance shared filesystem bridge bridging Linux paths and Windows guest drives via VirtIO-FS and WinFsp.
- **Low-Latency Compositing**: Sub-millisecond frame blitting using Looking Glass-inspired shared memory techniques and DMA-BUF imports directly into Wayland subsurfaces.

---

## 2. Core Architectural Pillars

```
+---------------------------------------------------------------------------------+
|                                 LINUX HOST                                      |
|                                                                                 |
|  +---------------------------------------------------------------------------+  |
|  |                         Wayland Compositor                                |  |
|  |     (GNOME Mutter / KDE KWin / wlroots / Hyprland / Sway)                 |  |
|  +-------------------------------------+-------------------------------------+  |
|                                        ^                                        |
|                     wl_surface / wl_subsurface / dmabuf                         |
|                                        |                                        |
|  +-------------------------------------+-------------------------------------+  |
|  |             Waddle Host Wayland Compositor Client                         |  |
|  |  - Wayland protocol event loop (xdg_shell, linux_dmabuf, viewporter)      |  |
|  |  - Shm / DMA-BUF frame importer                                           |  |
|  |  - Host input event capture & forwarder (keyboard, mouse, tablet)         |  |
|  |  - Window geometry, z-order, and life-cycle manager                       |  |
|  +------------------+-----------------------------------+--------------------+  |
|                     ^                                   ^                       |
|        Control IPC  |                      Frame Sync   |                       |
|         (vsock)     |                      (IVSHMEM)    |                       |
|=====================v===================================v=======================|
|  HYPERVISOR / KVM / QEMU BUS BOUNDARY                                            |
|  - IVSHMEM / KVMFR (Inter-VM Shared Memory Device)                              |
|  - VirtIO-Serial / VSOCK (Low-latency bidirectional control IPC)                |
|  - VirtIO-FS Host Daemon (virtiofsd)                                            |
|  - GPU Partitioning / vGPU (SR-IOV / GPU-PV / dxgkrnl mediated slicing)         |
|=====================^===================================^=======================|
|                     |                                   |                       |
|  +------------------v-----------------------------------+--------------------+  |
|  |             Waddle Guest Window Tracking & Capture Agent                  |  |
|  |  - Win32 Shell & Event Hooks (SetWinEventHook, CBT, DWM API)              |  |
|  |  - Window boundary & property extractor (DPI, alpha, state, title)        |  |
|  |  - Per-window DXGI / Direct3D 11/12 surface capture pipeline              |  |
|  |  - High-performance memory copy to IVSHMEM / zero-copy staging buffers    |  |
|  |  - Synthetic input injector (SendInput / Windows Raw Input)              |  |
|  +---------------------------------------------------------------------------+  |
|                                                                                 |
|  +---------------------------+       +---------------------------------------+  |
|  |   VirtIO-FS Windows Driver|       |  vGPU / GPU-PV Display & Compute      |  |
|  |   + WinFsp Userland Mount |       |  DirectX 11/12, Vulkan Driver         |  |
|  +---------------------------+       +---------------------------------------+  |
|                                                                                 |
|                           WINDOWS GUEST SYSTEM                                  |
+---------------------------------------------------------------------------------+
```

---

## 3. High-Level Requirements

### Requirement 1: VirtIO-FS + WinFsp (Filesystem Interoperability)
- **Host Daemon (`virtiofsd`)**: Export host Linux directories (e.g., user home `/home/$USER`, project trees, media mounts) into the guest.
- **Guest Integration (WinFsp)**: Utilize Windows File System Proxy (WinFsp) alongside VirtIO-FS drivers to map Linux exports to Windows drive letters (e.g., `Z:\` or UNC paths `\\waddle-host\shared`).
- **File System Semantics**:
  - Transparent translation between POSIX permissions / paths and Windows NT ACLs / paths.
  - Proper handling of case sensitivity, symlinks, file locking semantics, and UTF-8 filename encoding.
  - Inotify / FileSystemWatcher synchronization for real-time file change notifications across guest and host.

### Requirement 2: GPU-PV / vGPU Slicing (Hardware Acceleration)
- **vGPU / GPU Partitioning**:
  - Support GPU paravirtualization (GPU-PV via Hyper-V/KVM dxgkrnl paravirtualization, Intel GVT-g, NVIDIA vGPU, or AMD/Intel SR-IOV slicing).
  - Enable full hardware-accelerated DirectX 11/12, Vulkan, DirectCompute, and OpenCL inside the Windows guest.
  - Direct hardware swapchain rendering to ensure zero CPU overhead during intensive 3D/2D rendering.
- **Resource Management**: Dynamic or statically sliced VRAM allocations and compute priority configurations.

### Requirement 3: Looking Glass-Inspired Borderless Capture & Compositing
- **Low-Latency Zero-Copy Transport**: High-speed inter-VM frame transfer modeled on Looking Glass (IVSHMEM / KVMFR) delivering 60-144+ FPS with sub-frame presentation latency.
- **Borderless Windowed Capture**: Rather than capturing the entire monolithic Windows desktop framebuffer, the guest agent identifies individual top-level Win32/UWP window surfaces, their bounding rectangles, transparent regions (DWM non-client rendering, alpha blending), and damage rects.
- **Wayland Subsurface Blitting**: Host compositor client blits each captured surface into a corresponding Wayland `wl_surface` / `wl_subsurface` or imports shared DMA-BUFs directly into the Wayland scene graph.

---

## 4. Subsystems to Build

The engineering effort for Waddle-LSW is split into three primary software components:

### Component 1: Guest-Side Window Tracking & Capture Agent
**Target Platform**: Windows 10/11 64-bit (Guest VM)  
**Language / Tech**: Modern C++20 / Rust, Win32 API, DirectX (D3D11 / D3D12), DXGI, DWM APIs.

#### Responsibilities:
1. **Window Lifecycle & Topology Tracking**:
   - Register global hooks using `SetWinEventHook` (listening for `EVENT_OBJECT_CREATE`, `EVENT_OBJECT_DESTROY`, `EVENT_OBJECT_SHOW`, `EVENT_OBJECT_HIDE`, `EVENT_OBJECT_LOCATIONCHANGE`, `EVENT_SYSTEM_FOREGROUND`, `EVENT_SYSTEM_MINIMIZESTART`, `EVENT_SYSTEM_MINIMIZEEND`).
   - Filter system noise (hidden tooltips, background utility windows, overlay panels) to isolate legitimate top-level application windows.
   - Extract window metadata: window handle (`HWND`), title text, class name, process ID, client rect, screen bounding rect, extended window styles (`WS_EX_LAYERED`, `WS_EX_TOPMOST`, `WS_EX_TRANSPARENT`), DPI scaling factors, and z-ordering.
2. **Per-Window Surface Capture**:
   - Leverage Windows Graphics Capture API (`Windows.Graphics.Capture`) and/or Desktop Duplication API with per-window occlusion and clipping.
   - Direct DWM / DirectX surface tapping to capture window framebuffers including alpha transparency channels (critical for curved corners, shadow effects, and non-rectangular UI).
   - Compute dirty rects/damage regions per frame to minimize memory bandwidth.
3. **Buffer Marshalling**:
   - Copy or direct-map captured DXGI surfaces into the IVSHMEM shared memory segment using circular ring buffers or multi-buffering (double/triple buffer per active window).
   - Write per-frame synchronization metadata (frame ID, timestamp, buffer slot index, damage rects).
4. **Input Injection**:
   - Receive normalized input events (mouse move, mouse clicks, mouse wheel, keyboard scancodes, modifiers) from the host IPC.
   - Inject input cleanly into target `HWND` using `SendInput` or Win32 raw input injection mechanisms to guarantee proper focus and interaction.

---

### Component 2: IPC & Buffer Transport Layer
**Target Platform**: Host Linux & Guest Windows Cross-Domain Transport  
**Language / Tech**: C++20 / C / Rust, KVM/QEMU IVSHMEM (`/dev/kvmfr*` or `/dev/uio*` / Win32 IVSHMEM driver), Linux `AF_VSOCK` / VirtIO-Serial.

#### Responsibilities:
1. **Control Channel (Bidirectional IPC)**:
   - Ultra-low latency, deterministic message passing between guest agent and host client via `AF_VSOCK` or VirtIO-Serial.
   - Serialization protocol: High-performance binary protocol (Cap'n Proto / FlatBuffers) or lightweight schema-enforced JSON-RPC.
   - Message categories:
     - `WINDOW_ANNOUNCE`, `WINDOW_DESTROY`, `WINDOW_METADATA_UPDATE` (position, size, title, state).
     - `FRAME_AVAILABLE` (window ID, slot index, sequence number, damage rects).
     - `INPUT_EVENT` (pointer movement, button states, keyboard scancodes, focus request).
     - `CLIPBOARD_SYNC` (data offer, MIME types, payload transfer).
2. **Data Channel (Shared Memory Frame Buffer)**:
   - Partitioned IVSHMEM ring buffer layout supporting multiple concurrent active windows.
   - Memory layout architecture:
     - **Header Area**: Magic identifier, version, memory layout directory, active window slot tables, heartbeat.
     - **Window Stream Buffers**: Dedicated double/triple buffer slots per window ID with cache-line-aligned status headers and atomic fence flags (lockless multi-producer / multi-consumer queue).
   - Host zero-copy import: Convert IVSHMEM regions into Linux DMA-BUFs using KVMFR / memory file descriptors for direct GPU consumption without CPU memcpy.

---

### Component 3: Host-Side Wayland Compositor Client
**Target Platform**: Linux Host (Wayland)  
**Language / Tech**: Modern C++20 / Rust, `libwayland-client`, `wayland-protocols`, `libxkbcommon`, `libepoxy` / OpenGL / Vulkan, DMA-BUF extensions.

#### Responsibilities:
1. **Wayland Shell Integration**:
   - Communicate with the native Linux Wayland compositor using standard protocols (`xdg_wm_base`, `xdg_surface`, `xdg_toplevel`, `wl_subcompositor`, `wl_subsurface`).
   - Create a corresponding `xdg_toplevel` for every top-level Windows application reported by the guest agent.
   - Mirror window decorations: Server-Side Decoration (`zxdg_decoration_manager_v1`) or client-side custom titlebars and controls matching the user's desktop theme.
   - Handle minimize, maximize, fullscreen, restore, and resize requests, synchronizing states bidirectionally with the guest.
2. **Frame Presentation & Subsurface Management**:
   - Import frame buffers from the transport layer into Wayland using `zwp_linux_dmabuf_v1` or shared memory `wl_shm` fallbacks.
   - Attach buffers to `wl_surface` and configure `wp_viewport` (viewporter protocol) for clean fractional scaling and DPI adaptation.
   - Commit frame damages precisely using `wl_surface_damage_buffer` to allow the host compositor to optimize refresh rates and power consumption.
3. **Input Handling & Translation**:
   - Capture Wayland seat events (`wl_pointer`, `wl_keyboard`, `wl_touch`).
   - Translate Wayland coordinate space to guest window coordinates.
   - Translate Linux evdev / XKB scancodes into Windows Virtual-Key (VK) codes and hardware scancodes.
   - Handle pointer entry, exit, relative pointer motion (for gaming/3D apps), and pointer constraints.
4. **Clipboard Integration**:
   - Implement `wl_data_device_manager` to synchronize clipboard contents (plain text, HTML, PNG images) seamlessly between host and guest.

---

## 5. Technical Specifications & Protocols

### 5.1 Shared Memory Protocol (IVSHMEM Layout)
```
+-------------------------------------------------------------------------------+
| WADDLE IVSHMEM MEMORY MAP                                                     |
+-------------------------------------------------------------------------------+
| [0x0000_0000 - 0x0000_0FFF] Global Header & Registry (4 KB)                   |
|   - uint32_t magic = 0x5741444C ('WADL')                                      |
|   - uint32_t protocol_version                                                 |
|   - uint32_t max_windows                                                      |
|   - uint32_t total_memory_size                                                |
|   - WindowSlotRegistry entries [Slot 0 .. Slot N-1]                            |
+-------------------------------------------------------------------------------+
| [0x0000_1000 - 0x0000_FFFF] Reserved Control Block & Diagnostics (60 KB)      |
+-------------------------------------------------------------------------------+
| [0x0001_0000 - End of Shm ] Window Buffer Pools                               |
|   Each active window is assigned dynamic buffer slots:                        |
|   - Slot Header: Atomic state flags (READY, CONSUMING, WRITING), Buffer ID    |
|   - Format descriptor: DRM fourcc code (e.g. DRM_FORMAT_ARGB8888), width,     |
|     height, stride, timestamp, damage rectangles                              |
|   - Raw Pixel Surface Buffer Data (linear RGB/RGBA/NV12)                      |
+-------------------------------------------------------------------------------+
```

### 5.2 Window State Synchronization Protocol (Control Channel)
Messages transferred over VirtIO-Serial / VSOCK:
- `MsgWindowCreate`: `{ uint64_t window_id, char title[256], int32_t x, int32_t y, uint32_t width, uint32_t height, uint32_t flags, uint32_t dpi }`
- `MsgWindowDestroy`: `{ uint64_t window_id }`
- `MsgWindowGeometry`: `{ uint64_t window_id, int32_t x, int32_t y, uint32_t width, uint32_t height }`
- `MsgWindowState`: `{ uint64_t window_id, uint32_t state_flags (MINIMIZED, MAXIMIZED, FOCUSED) }`
- `MsgFrameReady`: `{ uint64_t window_id, uint32_t buffer_slot, uint64_t frame_index, Rect damage }`
- `MsgInputPointer`: `{ uint64_t window_id, uint32_t event_type, int32_t x, int32_t y, uint32_t buttons, int32_t wheel_delta }`
- `MsgInputKeyboard`: `{ uint64_t window_id, uint32_t key_action, uint32_t vk_code, uint32_t scan_code, uint32_t modifiers }`

---

## 6. Implementation Phasing & Milestones

1. **Phase 1: Transport & Prototype Compositor (Host Client)**
   - Setup IVSHMEM reader on Linux host.
   - Implement Wayland client rendering a test pattern from shared memory via `wl_shm` and `linux_dmabuf`.
   - Setup bidirectional VSOCK / serial control socket.
2. **Phase 2: Windows Window Tracking & DXGI Capture (Guest Agent)**
   - Implement Win32 window event hooks to detect application open/close/move.
   - Implement DXGI desktop / window capture pipeline capturing per-window pixel surfaces.
   - Blit captured surfaces into IVSHMEM memory regions with atomic locking.
3. **Phase 3: Multi-Window Composition & Wayland Subsurfaces**
   - Host client dynamically instantiates and tears down Wayland surfaces corresponding to guest windows.
   - Implement window movement, resizing, damage tracking, and DPI scaling.
4. **Phase 4: Input Injection, Mouse/Keyboard, & Focus Management**
   - Forward pointer and keyboard events from Wayland to guest agent.
   - Implement mouse cursor tracking, shape sync, and proper focus synchronization.
   - Bidirectional clipboard integration.
5. **Phase 5: Subsystem Integrations & Packaging**
   - VirtIO-FS configuration templates, WinFsp mounting automations.
   - vGPU / GPU-PV configuration documentation, setup scripts, and driver installation guides.
   - Daemon management, systemd user services, and complete error handling.

---

## 7. Quality & Performance Benchmarks
- **Latency Target**: Input-to-display latency < 16ms (within 1 frame at 60Hz), targeting < 7ms at 144Hz.
- **Framerate Target**: Steady 60 FPS for general desktop applications; support up to 144+ FPS for accelerated applications.
- **Resource Footprint**: Minimal CPU overhead (< 2-3% CPU utilization on idle/moderate desktop usage) by prioritizing hardware-assisted capture and DMA buffer sharing.
