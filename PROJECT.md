# Project Specification: Waddle-LSW (Waddle Linux Subsystem for Windows)

## 1. Executive Summary & Vision

**Waddle-LSW** (Waddle Linux Subsystem for Windows) is a free, open-source compatibility layer and subsystem architecture designed to deliver seamless, performant integration of Microsoft Windows applications into a native Linux desktop environment.

Unlike traditional virtual machine viewers that trap Windows within a single monolithic hypervisor window, Waddle-LSW achieves seamless, rootless/borderless application integration. Individual Windows application windows are tracked inside a lightweight guest VM, their individual surface buffers are captured at high framerates with low latency, transmitted across domain boundaries via high-throughput shared memory, and composed directly onto the host Linux desktop as native Wayland surfaces and subsurfaces.

### Core Objectives
- **Zero-Friction Host Integration**: Windows applications appear alongside native Linux windows in standard Wayland desktop environments (GNOME, KDE Plasma, Sway, Hyprland, etc.), complete with window management, positioning, stacking, and focus.
- **Near-Native Graphics & Compute Performance**: Utilize GPU partitioning / paravirtualization (GPU-PV/vGPU slicing) so Windows workloads access direct hardware acceleration.
- **Unified Filesystem Access**: High-performance shared filesystem bridge bridging Linux paths and Windows guest drives via VirtIO-FS and WinFsp.
- **Low-Latency Compositing**: Sub-millisecond frame blitting using Looking Glass-inspired shared memory techniques and DMA-BUF imports directly into Wayland subsurfaces.
- **Bidirectional CLI & Process Passthrough**: Execute Windows binaries, CLI utilities, and shells transparently from the Linux terminal with high-performance I/O streaming, ConPTY terminal handling, signal forwarding, and exit code propagation.

---

## 2. Core Architectural Pillars & Language Policy

### 2.1 Language Strategy & Selection Hierarchy
- **C as Default Language ("C Code Usually")**:
  - The primary, default implementation language for Waddle-LSW is **C (C11/C17/C23)**.
  - C is used across the host compositor client, guest tracking agent, IPC serialization, shared memory layouts, and CLI passthrough utilities.
  - Ensures a minimal footprint, zero runtime overhead, deterministic low-latency execution, immediate portability across Linux and Windows NT, and a universal C ABI baseline.
- **Zig Where Memory Safety is Needed**:
  - **Zig (0.13+)** is explicitly mandated and utilized for components requiring high assurance, spatial and temporal memory safety, robust slice/buffer bounds checking, compile-time validation (`comptime`), allocators, and untrusted input parsing.
  - Delivers complete manual memory control and safety guarantees without garbage collection, large runtime baggage, or unexpected compiler overhead.
  - Interoperates directly with C headers and C ABI structures with zero abstraction cost.
- **C++ Strictly Where Compatibility is Required**:
  - **C++ (C++20/C++23)** represents the absolute upper ceiling of abstraction permitted in Waddle-LSW.
  - Its usage is strictly restricted to scenarios where external vendor SDKs or specific Windows/guest frameworks (such as direct DirectX 11/12, Windows Runtime / WinRT, or COM APIs) expose exclusively C++ interfaces without viable C or Zig bindings.
  - Higher-level managed or garbage-collected runtimes (and other heavy language ecosystems such as Rust or Go) are strictly disallowed.

### 2.2 Code Beauty & Naming Convention Specification
To ensure strict consistency, aesthetic elegance, and seamless cross-language interoperability (C, Zig, and C++), all code must strictly follow these naming standards:
- **Default-Case: `snake_case`**:
  - All standard identifiers—including variables, function names, method names, struct/union/class members, function parameters, and source file names—must use `snake_case`.
  - Examples: `window_count`, `parse_message()`, `slot_index`, `session_id`.
- **Constants: `PascalCase`**:
  - All constants—including compile-time constants, global read-only variables, enum members, and macro definitions representing constant values—must use `PascalCase`.
  - Examples: `MaxBufferSize`, `DefaultVsockPort`, `MagicHeaderValue`, `IpcTimeoutMs`.
- **Prohibition: Never `camelCase`**:
  - `camelCase` (e.g., `windowCount`, `parseMessage`, `slotIndex`) is **strictly and unconditionally forbidden** across all languages, files, and subsystems in Waddle-LSW.
- **Types: `type_name_t`**:
  - All types—including structs, unions, enums, typedefs, class names, and type aliases—must use `snake_case` with a mandatory trailing `_t` suffix.
  - Examples: `window_slot_header_t`, `cli_message_header_t`, `surface_descriptor_t`, `ipc_status_t`.

### 2.3 Documentation Enforcement Policy
- **Comprehensive Documentation Mandatory**: Documentation is strictly enforced across every file and interface in Waddle-LSW.
- **Structured Symbol Comments**: Every public function, struct, union, enum, typedef, constant, and macro must be documented using structured documentation comments (e.g., Doxygen-style `/** ... */` in C/C++, `///` in Zig).
- **Required Contract Details**:
  - Function documentation must clearly specify parameter directions (`in`, `out`, `inout`), valid value ranges, nullability, memory ownership/allocation lifetimes, error codes, and thread-safety invariants.
  - Data structure documentation must detail field meanings, byte packing, alignment constraints (e.g., 64-byte cache line alignment), and concurrency access rules.
- **Zero Ambiguity Rule**: Undocumented public symbols or ambiguous interface boundaries are prohibited and will be rejected in review.

### 2.4 Testing & Code Coverage Specification
- **Rigorous Automated Testing**: Automated unit, integration, and stress tests are required for all modules, codecs, protocols, and state machines.
- **Enforced Code Coverage**: Code coverage metrics must be tracked and enforced (via `gcov`/`lcov` or sanitizer instrumentation). Boundary conditions, error branches, corrupted payload handling, and timeout recovery paths must be thoroughly exercised.
- **Zig Built-in Test Specification**:
  - Tests may be authored in Zig using its native, built-in test specifications (`test "..." { ... }`).
  - Leverages Zig's native test runner (`zig test`), standard testing assertions (`std.testing.expect`, `std.testing.expectEqual`), and zero-overhead C interop (`@cImport`) to test C ABI libraries, Zig components, and system interfaces natively without external test framework dependencies.

### 2.5 Zero-Leak Memory Safety Policy & Enforcement
Waddle-LSW enforces an uncompromising, zero-tolerance policy against memory leaks, use-after-free conditions, double-free bugs, and unverified buffer overflows across all components:
- **Mandatory Zero-Leak Invariant**:
  - No code may leak dynamic memory under any normal execution path, error unwinding branch, cancellation signal, or protocol failure condition.
  - Every single allocated byte must have a deterministically proven, documented deallocation site. Memory leaks are classified as critical severity bugs and will cause automated build rejections.
- **Strict Ownership & Resource Lifecycle Rules**:
  - **Explicit Ownership Transfers in C**: Every function allocating or transferring memory must explicitly state ownership transfer in its API documentation. Systems must provide symmetric allocator/deallocator pairs (e.g., `*_create()` and `*_destroy()`, or `*_init()` and `*_free()`).
  - **Memory Safety via Zig**: Components handling untrusted inputs, variable-length serialization, and network parsing are implemented in Zig, leveraging bounds-checked slices and explicit allocators without runtime garbage collection.
  - **RAII in C++ Interop**: Where C++ compatibility boundaries are required, raw pointer ownership (`new`/`delete`) is strictly prohibited in favor of standard RAII containers (`std::unique_ptr`, `std::vector`, `std::span`).
- **Enforced Leak Detection via Sanitizers**:
  - All test suites must execute clean runs under **LLVM LeakSanitizer (`-fsanitize=leak` / `-fsanitize=address`)** with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`.
  - In Zig test harnesses, tests must utilize `std.testing.allocator`, which automatically detects and fails on un-freed memory upon test completion.
  - Test suites with even a single leaked byte are considered broken and cannot be merged.

### 2.6 External Dependency & Git Submodule Policy
- **Mandatory Git Submodule Architecture**:
  - All external third-party libraries, vendor SDKs, protocols, and header collections not provided by standard host/guest base system packages must be vendored as **Git submodules** residing under the dedicated `submodules/` directory (e.g., `submodules/<library-name>`).
  - Loose source vendoring directly into the repository tree without submodule tracking is strictly prohibited.
  - Dynamic dependency fetching during the build phase (such as unpinned runtime network downloads, remote script execution, or unverified CMake `FetchContent` network calls at configure time) is prohibited; all builds must remain fully deterministic, offline-capable, and hermetic.
- **Commit Pinning & Release Hygiene**:
  - Every submodule must be pinned to an explicit, immutable commit SHA or verified release tag. Tracking dynamic branch heads (e.g., `HEAD`, `main`, or `master`) in production commits is strictly forbidden.
  - Submodules must be licensed compatibly with Waddle-LSW (GPL/LGPL/MIT/Apache/BSD compatibility) and audited prior to inclusion.
  - All submodule remote URLs must use public HTTPS (`https://github.com/...`) to ensure unauthenticated clones in CI environments and developer setups.
- **Subsystem & Boundary Isolation**:
  - External submodules are integrated into the build systems (CMake / Zig `build.zig`) via strictly scoped targets or headers, ensuring external headers do not pollute global include namespaces or violate C ABI stability invariants.
  - Feature branches that introduce new external dependencies must document them in the feature's `IMPL_DESC.md` and track integration progress in `TRACKER.md`.

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

### Requirement 4: Interactive CLI Passthrough & Process Launching
- **Transparent Terminal Command Execution**: Allow launching Windows CLI tools (`cmd.exe`, `powershell.exe`, compilers, CLI dev tools) directly from the Linux shell via `waddle run <cmd>` or `waddle exec <cmd>`.
- **ConPTY & VT100 Terminal Streaming**: Provide complete ANSI escape code passthrough, terminal window resizing (`SIGWINCH`), and raw mode TTY management.
- **Bidirectional I/O Streaming**: Stream `stdin`, `stdout`, and `stderr` over multiplexed `AF_VSOCK` channels with sub-millisecond latency.
- **Lifecycle & Exit Codes**: Accurate process exit code forwarding, signal translation (Linux `SIGINT`/`SIGTERM` to Windows console control events), and clean resource teardown.

---

## 4. Subsystems to Build

The engineering effort for Waddle-LSW is split into four primary software components:

### Component 1: Guest-Side Window Tracking & Capture Agent
**Target Platform**: Windows 10/11 64-bit (Guest VM)  
**Language / Tech**: C (C11/C23) / Zig (0.13+) (preferred), with C++20 strictly as an upper ceiling only where direct WinRT/DirectX C++ abstractions are unavoidable; Win32 API, DirectX (D3D11 / D3D12), DXGI, DWM APIs.

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
**Language / Tech**: C (C11/C23) / Zig (0.13+), KVM/QEMU IVSHMEM (`/dev/kvmfr*` or `/dev/uio*` / Win32 IVSHMEM driver), Linux `AF_VSOCK` / VirtIO-Serial; strict C ABI header definitions.

#### Responsibilities:
1. **Control Channel (Bidirectional IPC)**:
   - Ultra-low latency, deterministic message passing between guest agent and host client via `AF_VSOCK` or VirtIO-Serial.
   - Serialization protocol: High-performance binary protocol with explicit C ABI structs or lightweight schema-enforced messaging.
   - Message categories:
     - `WINDOW_ANNOUNCE`, `WINDOW_DESTROY`, `WINDOW_METADATA_UPDATE` (position, size, title, state).
     - `FRAME_AVAILABLE` (window ID, slot index, sequence number, damage rects).
     - `INPUT_EVENT` (pointer movement, button states, keyboard scancodes, focus request).
     - `CLIPBOARD_SYNC` (data offer, MIME types, payload transfer).
     - `PROCESS_SPAWN`, `PROCESS_STDIN`, `PROCESS_STDOUT`, `PROCESS_STDERR`, `PROCESS_SIGNAL`, `PROCESS_EXIT`.
2. **Data Channel (Shared Memory Frame Buffer)**:
   - Partitioned IVSHMEM ring buffer layout supporting multiple concurrent active windows.
   - Memory layout architecture:
     - **Header Area**: Magic identifier, version, memory layout directory, active window slot tables, heartbeat.
     - **Window Stream Buffers**: Dedicated double/triple buffer slots per window ID with cache-line-aligned status headers and atomic fence flags (lockless multi-producer / multi-consumer queue).
   - Host zero-copy import: Convert IVSHMEM regions into Linux DMA-BUFs using KVMFR / memory file descriptors for direct GPU consumption without CPU memcpy.

---

### Component 3: Host-Side Wayland Compositor Client
**Target Platform**: Linux Host (Wayland)  
**Language / Tech**: C (C11/C23) / Zig (0.13+) (preferred), with C++20 as upper ceiling only if strictly mandated by external dependencies; `libwayland-client`, `wayland-protocols`, `libxkbcommon`, `libepoxy` / OpenGL / Vulkan, DMA-BUF extensions.

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

### Component 4: CLI Passthrough & Process Execution Bridge
**Target Platform**: Linux Host CLI & Windows Guest Execution Agent  
**Language / Tech**: C (C11/C23) / Zig (0.13+) (preferred), POSIX termios, Win32 ConPTY (Pseudo Console) API, Linux `AF_VSOCK` / VirtIO-Serial.

#### Responsibilities:
1. **Host-Side CLI (`waddle` / `waddle-cli`)**:
   - Command line argument parsing and Windows-style argument quoting/escaping (`CommandLineToArgvW` semantics).
   - Raw terminal mode management (`termios`) and terminal window resize signal (`SIGWINCH`) forwarding.
   - Multiplexed standard I/O streaming (stdin, stdout, stderr) over low-latency VSOCK.
   - Accurate exit code propagation matching guest process termination.
2. **Guest-Side Console Agent (`waddle-guest-exec`)**:
   - Process spawning via `CreateProcessW` with standard Win32 pipes for non-interactive execution, or Windows Pseudo Console (`CreatePseudoConsole`) for full interactive terminal sessions (e.g. PowerShell, CMD, interactive development tools).
   - Asynchronous I/O multiplexing between Windows console pipes and VSOCK IPC.
   - Environment variable passing and path translation (e.g., mapping host Linux paths to guest VirtIO-FS drives).
   - Signal handling: mapping Linux `SIGINT`/`SIGTERM` to Windows `GenerateConsoleCtrlEvent` (`CTRL_C_EVENT`).

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
- `MsgProcessSpawn`: `{ uint64_t request_id, uint32_t flags (INTERACTIVE_CONPTY, PIPE_STREAMS), uint16_t rows, uint16_t cols, char cwd[1024], char cmdline[4096], char env[4096] }`
- `MsgProcessIo`: `{ uint64_t session_id, uint8_t stream_type (STDIN, STDOUT, STDERR), uint32_t length, uint8_t payload[] }`
- `MsgProcessSignal`: `{ uint64_t session_id, uint32_t signal_code (CTRL_C, CTRL_BREAK, TERMINATE), uint16_t rows, uint16_t cols }`
- `MsgProcessExit`: `{ uint64_t session_id, uint32_t exit_code, uint32_t error_status }`

---

## 6. Implementation Phasing & Milestones

1. **Phase 1: Transport & Prototype Compositor (Host Client)**
   - Setup IVSHMEM reader on Linux host in C/Zig.
   - Implement Wayland client rendering a test pattern from shared memory via `wl_shm` and `linux_dmabuf`.
   - Setup bidirectional VSOCK / serial control socket.
2. **Phase 2: CLI Passthrough & Process Launching Bridge**
   - Implement host CLI utility (`waddle` / `waddle-cli`) in C/Zig with termios raw mode and VT100/ANSI passthrough.
   - Implement guest console agent (`waddle-guest-exec`) in C/Zig with Win32 `CreateProcessW` and ConPTY support.
   - Multiplex stdin/stdout/stderr streaming and exit code propagation over VSOCK.
3. **Phase 3: Windows Window Tracking & DXGI Capture (Guest Agent)**
   - Implement Win32 window event hooks to detect application open/close/move.
   - Implement DXGI desktop / window capture pipeline capturing per-window pixel surfaces.
   - Blit captured surfaces into IVSHMEM memory regions with atomic locking.
4. **Phase 4: Multi-Window Composition & Wayland Subsurfaces**
   - Host client dynamically instantiates and tears down Wayland surfaces corresponding to guest windows.
   - Implement window movement, resizing, damage tracking, and DPI scaling.
5. **Phase 5: Input Injection, Mouse/Keyboard, & Focus Management**
   - Forward pointer and keyboard events from Wayland to guest agent.
   - Implement mouse cursor tracking, shape sync, and proper focus synchronization.
   - Bidirectional clipboard integration.
6. **Phase 6: Subsystem Integrations & Packaging**
   - VirtIO-FS configuration templates, WinFsp mounting automations.
   - vGPU / GPU-PV configuration documentation, setup scripts, and driver installation guides.
   - Daemon management, systemd user services, and complete error handling.

---

## 7. Quality & Performance Benchmarks
- **Latency Target**: Input-to-display latency < 16ms (within 1 frame at 60Hz), targeting < 7ms at 144Hz.
- **Framerate Target**: Steady 60 FPS for general desktop applications; support up to 144+ FPS for accelerated applications.
- **Resource Footprint**: Minimal CPU overhead (< 2-3% CPU utilization on idle/moderate desktop usage) by prioritizing hardware-assisted capture and DMA buffer sharing.
