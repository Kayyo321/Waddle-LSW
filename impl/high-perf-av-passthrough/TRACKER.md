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
| #2.1 | Register `SetWinEventHook` for CREATE, DESTROY, LOCATIONCHANGE, MINIMIZE | In Progress | 4% | 90% | Run tracked DXGI crops and a joined WASAPI capture worker over a bounded nonblocking Viosock session. Assign triple-buffer pools, publish frame notifications, cancel unsent slots and retain pool leases until host release. Emit system SDK COM GUIDs and cross-build/link the complete guest executable. |
| #2.2 | Implement HWND filtering (exclude non-app windows, tooltips, hidden elements) | In Progress | 3.2% | 90% | Run tracked DXGI crops and a joined WASAPI capture worker over a bounded nonblocking Viosock session. Assign triple-buffer pools, publish frame notifications, cancel unsent slots and retain pool leases until host release. Emit system SDK COM GUIDs and cross-build/link the complete guest executable. |
| #2.3 | Extract bounds via `DwmGetWindowAttribute` and send `MsgWindowGeometry` | In Progress | 3.2% | 90% | Run tracked DXGI crops and a joined WASAPI capture worker over a bounded nonblocking Viosock session. Assign triple-buffer pools, publish frame notifications, cancel unsent slots and retain pool leases until host release. Emit system SDK COM GUIDs and cross-build/link the complete guest executable. |
| **3**   | **Video Capture & Transport (Guest)** | | | | |
| #3.1 | Initialize DXGI Desktop Duplication / Windows.Graphics.Capture pipeline | In Progress | 4% | 85% | Load the sibling first-party WGC DLL through restricted absolute search, retain its opaque contexts and publish mapped rows through Zig bounded copies. Report visible-window DXGI fallback explicitly and send actual captured dimensions after resize; complete C guest cross-link passes. |
| #3.2 | Implement rendering/copying DXGI surface to IVSHMEM double/triple buffer slots | In Progress | 6.4% | 90% | Load the sibling first-party WGC DLL through restricted absolute search, retain its opaque contexts and publish mapped rows through Zig bounded copies. Report visible-window DXGI fallback explicitly and send actual captured dimensions after resize; complete C guest cross-link passes. |
| #3.3 | Implement `memory_order_acquire/release` fences for buffer synchronization | Done | 4% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #3.4 | Dispatch `MsgFrameReady` over VSOCK with buffer index and damage rects | In Progress | 2.4% | 85% | Run tracked DXGI crops and a joined WASAPI capture worker over a bounded nonblocking Viosock session. Assign triple-buffer pools, publish frame notifications, cancel unsent slots and retain pool leases until host release. Emit system SDK COM GUIDs and cross-build/link the complete guest executable. |
| **4**   | **Audio Capture & Transport (Guest)** | | | | |
| #4.1 | Initialize WASAPI `AUDIOCLIENT_ACTIVATION_PARAMS` for per-process loopback | In Progress | 4% | 85% | Run tracked DXGI crops and a joined WASAPI capture worker over a bounded nonblocking Viosock session. Assign triple-buffer pools, publish frame notifications, cancel unsent slots and retain pool leases until host release. Emit system SDK COM GUIDs and cross-build/link the complete guest executable. |
| #4.2 | Implement lockless SPSC write loop from WASAPI buffers to IVSHMEM audio ring | Done | 6.4% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #4.3 | Implement audio buffer overrun recovery and latency logging | In Progress | 3.2% | 85% | Run tracked DXGI crops and a joined WASAPI capture worker over a bounded nonblocking Viosock session. Assign triple-buffer pools, publish frame notifications, cancel unsent slots and retain pool leases until host release. Emit system SDK COM GUIDs and cross-build/link the complete guest executable. |
| **5**   | **Host Wayland Client (Video)** | | | | |
| #5.1 | Create `xdg_toplevel` upon receiving `MsgWindowCreate` from guest | In Progress | 4% | 85% | Run the bounded AV peer alongside Wayland display polling and PipeWire playback, map validated daemon-provisioned memory and translate guest pool assignments into retained compositor buffers. Own shutdown ordering and build/link the complete native host executable. |
| #5.2 | Convert IVSHMEM slots to DMA-BUFs using KVMFR / memfds | In Progress | 4.8% | 80% | Run the bounded AV peer alongside Wayland display polling and PipeWire playback, map validated daemon-provisioned memory and translate guest pool assignments into retained compositor buffers. Own shutdown ordering and build/link the complete native host executable. |
| #5.3 | Handle `MsgFrameReady` by attaching DMA-BUF to `wl_surface` and committing | In Progress | 4% | 85% | Run the bounded AV peer alongside Wayland display polling and PipeWire playback, map validated daemon-provisioned memory and translate guest pool assignments into retained compositor buffers. Own shutdown ordering and build/link the complete native host executable. |
| #5.4 | Process `MsgWindowGeometry` to move/resize/minimize Wayland surfaces | In Progress | 4% | 70% | Run the bounded AV peer alongside Wayland display polling and PipeWire playback, map validated daemon-provisioned memory and translate guest pool assignments into retained compositor buffers. Own shutdown ordering and build/link the complete native host executable. |
| **6**   | **Host PipeWire Client (Audio)** | | | | |
| #6.1 | Initialize PipeWire stream matching the hardcoded IVSHMEM format | In Progress | 4% | 85% | Run the bounded AV peer alongside Wayland display polling and PipeWire playback, map validated daemon-provisioned memory and translate guest pool assignments into retained compositor buffers. Own shutdown ordering and build/link the complete native host executable. |
| #6.2 | Implement lockless SPSC read loop from IVSHMEM audio ring buffer | Done | 4.8% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #6.3 | Feed audio frames into PipeWire playback buffer callback | In Progress | 3.2% | 85% | Run the bounded AV peer alongside Wayland display polling and PipeWire playback, map validated daemon-provisioned memory and translate guest pool assignments into retained compositor buffers. Own shutdown ordering and build/link the complete native host executable. |
| **7**   | **Testing & CI Verification** | | | | |
| #7.1 | Write unit tests for lockless IVSHMEM queue mechanisms (video and audio) | In Progress | 1.6% | 75% | Run 100000 ordered video and PCM frames across concurrent producer/consumer threads, cross uint32 cursor wrap and verify sparse mapping bounds and regular-file DMA-BUF rejection. ASan/LSan/UBSan and Zig allocator tests pass with zero reported leaks. |
| #7.2 | Run LeakSanitizer/AddressSanitizer and verify zero bytes leaked | In Progress | 1.6% | 60% | Run 100000 ordered video and PCM frames across concurrent producer/consumer threads, cross uint32 cursor wrap and verify sparse mapping bounds and regular-file DMA-BUF rejection. ASan/LSan/UBSan and Zig allocator tests pass with zero reported leaks. |

| #8.1 | Pin and verify Looking Glass driver ABI headers as a submodule | Done | 1.6% | 100% | Add public HTTPS Looking Glass submodule pinned to B7 commit 27fe47c; audit GPL-2.0-or-later headers for GPL-3.0 compatibility and isolate usage to KVMFR/IVSHMEM driver ABI declarations. |

| **10** | **All-in-one AV environment provisioning** | | | | |
| #10.1 | Probe host/guest prerequisites and report actionable capability failures | In Progress | 5% | 40% | Prepare exact-size private IVSHMEM backing automatically, validate explicitly assigned VFIO GPU state and reject existing/symlink files without replacing user data. Own mapping/file cleanup and prevent resetting KVMFR slots still held by a compositor; native provisioning tests pass. |
| #10.2 | Build/install pinned KVMFR module and provision shared-memory access | In Progress | 5% | 65% | Build the pinned driver through an argv-only child process against running-kernel headers, expose an administrator-only load step and preserve incompatible live devices. Bundle source/license from the submodule in ignored build outputs; build succeeds on the current kernel. |
| #10.3 | Configure managed QEMU IVSHMEM, silent audio endpoint and GPU capture | In Progress | 5% | 80% | Extend the bounded configuration editor with AV enable, shared-memory path and explicit GPU assignment. Validate the complete patch before replacing a quiescent device configuration, preserve unrelated bytes and cover incomplete, malformed and reset requests. |
| #10.4 | Deploy guest AV agent and signed drivers with readiness verification | Pending | 3% | 0% | No manually prepared external AV environment assumed. |
| #10.5 | Exercise provisioned real AV session and verify latency/fidelity | Pending | 2% | 0% | Real capture/playback measurements; no mock substitution. |

**Total Feature Completion**: `80.05%`

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

- **Commit `d5d343189ca4`**: `docs(av): include all-in-one environment provisioning in feature scope`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Add the user-required host module, managed VM devices, signed guest driver deployment, readiness probing and provisioned-session verification tasks. Rescale prior tasks to 80 percent and reserve 20 percent for provisioning without assuming an external AV test environment.

- **Commit `2c51b3bb9651`**: `fix(av): align shared mapping size to IVSHMEM PCI BAR requirements`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Reserve a 2 GiB power-of-two mapping for QEMU ivshmem-plain while keeping the used pixel region below the wl_shm signed size limit. Update mapping validation and native sparse-map tests; transport suites pass.

- **Commit `fe639b874cb2`**: `feat(av): parse managed VM audio-video device configuration`
  - **Task Impact**: +20% to #10.3 (+1.00% overall)
  - **Summary**: Add transactional bounded AV configuration for shared-memory paths and explicit GPU PCI addresses, defaulting to disabled for existing CLI-only VMs. Reject malformed BDFs and QEMU-option-injecting paths; parser tests pass.

- **Commit `1d717259ed80`**: `feat(av): attach IVSHMEM and silent render endpoint to managed QEMU`
  - **Task Impact**: +35% to #10.3 (+1.75% overall)
  - **Summary**: Extend managed VM arguments with exact-size IVSHMEM, a none-backed HDA render endpoint and explicitly configured VFIO GPU. Preserve AV-disabled behavior, validate paths before option construction and exercise accepted/rejected configurations.

- **Commit `cfe3cecc417f`**: `feat(av): provision private shared memory before VM startup`
  - **Task Impact**: +40% to #10.1 (+2.00% overall); +35% to #10.2 (+1.75% overall)
  - **Summary**: Prepare exact-size private IVSHMEM backing automatically, validate explicitly assigned VFIO GPU state and reject existing/symlink files without replacing user data. Own mapping/file cleanup and prevent resetting KVMFR slots still held by a compositor; native provisioning tests pass.

- **Commit `56d6fca6f630`**: `feat(av): own shared-memory provisioning in daemon lifecycle`
  - **Task Impact**: +15% to #10.3 (+0.75% overall)
  - **Summary**: Prepare AV memory before managed QEMU startup and release it on spawn failure, shutdown, kill, child exit and supervisor cleanup. Reject duplicate prepare without releasing a live resource; existing server/client lifecycle regressions pass.

- **Commit `0432c8e39a09`**: `fix(av): snapshot ring bounds and preserve foreign shared resources`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Use a single validated PCM capacity and bounded slices for all copies, reject KVMFR mappings with another ABI and remove only the inode created by this environment instance. Preserve live resources on duplicate prepare and report nonregular backing files accurately.

- **Commit `a7a39506d002`**: `feat(av): bundle pinned KVMFR module build and load helper`
  - **Task Impact**: +30% to #10.2 (+1.50% overall)
  - **Summary**: Build the pinned driver through an argv-only child process against running-kernel headers, expose an administrator-only load step and preserve incompatible live devices. Bundle source/license from the submodule in ignored build outputs; build succeeds on the current kernel.

- **Commit `ab7b72843c44`**: `feat(av): stream bounded control frames with nonblocking backpressure`
  - **Task Impact**: +25% to #3.4 (+0.60% overall)
  - **Summary**: Add an allocation-free single-event-thread peer queue handling partial sends and receives, bounded lifecycle bursts and codec rejection. Verify byte-fragmented socket frames, full queues, corruption and peer EOF through a real socketpair fixture.

- **Commit `b3c8c4daf20d`**: `feat(av): validate native session process and CID arguments in Zig`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Add bounded nonzero decimal argument parsing for native AV entry points, rejecting overflow, signs and trailing bytes without mutating the caller result. Keep command-line parsing inside the memory-safe codec boundary.

- **Commit `6145a388dcc2`**: `feat(av): wire native per-game guest capture and audio session`
  - **Task Impact**: +10% to #2.1 (+0.40% overall); +10% to #2.2 (+0.32% overall); +15% to #2.3 (+0.48% overall); +25% to #3.4 (+0.60% overall); +15% to #4.1 (+0.60% overall); +25% to #4.3 (+0.80% overall)
  - **Summary**: Run tracked DXGI crops and a joined WASAPI capture worker over a bounded nonblocking Viosock session. Assign triple-buffer pools, publish frame notifications, cancel unsent slots and retain pool leases until host release. Emit system SDK COM GUIDs and cross-build/link the complete guest executable.

- **Commit `56c2ed63cca1`**: `feat(av): connect managed shared memory to native host playback`
  - **Task Impact**: +15% to #5.1 (+0.60% overall); +10% to #5.2 (+0.48% overall); +20% to #5.3 (+0.80% overall); +20% to #5.4 (+0.80% overall); +10% to #6.1 (+0.40% overall); +10% to #6.3 (+0.32% overall)
  - **Summary**: Run the bounded AV peer alongside Wayland display polling and PipeWire playback, map validated daemon-provisioned memory and translate guest pool assignments into retained compositor buffers. Own shutdown ordering and build/link the complete native host executable.

- **Commit `6df1378ec458`**: `feat(av): isolate per-window Windows Graphics Capture behind C ABI`
  - **Task Impact**: +20% to #3.1 (+0.80% overall)
  - **Summary**: Add a system-SDK C++/WinRT adapter with RAII-owned sessions, free-threaded frame pools, resize recreation and scoped GPU mappings. Expose only borrowed pixel rows through C callbacks; native SDK build and fidelity tests remain pending.

- **Commit `aae9676e3292`**: `feat(av): prefer per-window WinRT capture in the guest pipeline`
  - **Task Impact**: +5% to #3.1 (+0.20% overall); +5% to #3.2 (+0.32% overall)
  - **Summary**: Load the sibling first-party WGC DLL through restricted absolute search, retain its opaque contexts and publish mapped rows through Zig bounded copies. Report visible-window DXGI fallback explicitly and send actual captured dimensions after resize; complete C guest cross-link passes.

- **Commit `47b6c4d840ae`**: `test(av): add native Linux and Windows verification jobs`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Run AV transport sanitizers, cross-link platform clients and compile the WinRT DLL with the base Windows SDK. Add native window lifecycle/filtering and C ABI checks plus allocator tests; publish first-party Windows artifacts for managed deployment.

- **Commit `5dd9996b4f06`**: `fix(av): distinguish orderly closure from corrupted sessions`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Return a separate orderly EOF result, reject truncated final frames and propagate protocol errors through host and guest exit status. Fragmented, burst, corruption and truncation tests and native/cross builds pass.

- **Commit `4d689828c9db`**: `feat(av): expose atomic AV settings through device configuration`
  - **Task Impact**: +10% to #10.3 (+0.50% overall)
  - **Summary**: Extend the bounded configuration editor with AV enable, shared-memory path and explicit GPU assignment. Validate the complete patch before replacing a quiescent device configuration, preserve unrelated bytes and cover incomplete, malformed and reset requests.

- **Commit `HEAD`**: `fix(av): match WinRT SDK namespace and DLL export linkage`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Resolve native MSVC diagnostics by including the apartment API, qualifying the SDK DXGI interface namespace and sharing C ABI export declarations between the adapter header and implementation. Native Windows CI will recompile the boundary with warnings treated as errors.
