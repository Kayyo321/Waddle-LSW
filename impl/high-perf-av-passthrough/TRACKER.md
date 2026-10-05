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
| #1.1 | Define C structs for IVSHMEM window slot header (64-byte aligned, atomic flags) | Done | 1.28% | 100% | Define C11 lock-free ownership states, stable offsets and bounded fixed-format audio metadata; verify layout on Linux and cross-build Windows. |
| #1.2 | Define C structs for IVSHMEM audio ring buffer (lockless SPSC, 48kHz 16-bit) | Done | 1.28% | 100% | Define C11 lock-free ownership states, stable offsets and bounded fixed-format audio metadata; verify layout on Linux and cross-build Windows. |
| #1.3 | Update VSOCK IPC control messages to include full geometries & lifecycle events | Done | 1.92% | 100% | Add isolated AV control IDs and a fixed little-endian Zig codec validating titles, geometry, state flags and damage rectangles before exposing data to C. |
| **2**   | **Guest Window Tracking Agent** | | | | |
| #2.1 | Register `SetWinEventHook` for CREATE, DESTROY, LOCATIONCHANGE, MINIMIZE | Done | 3.2% | 100% | Add an explicit native capture fixture that paints a deterministic target, polls real mapped WinRT frames with a five-second deadline and verifies center pixels before and after an occluding topmost window. Keep the hardware-dependent gate explicit and link only the fixture against system GDI. |
| #2.2 | Implement HWND filtering (exclude non-app windows, tooltips, hidden elements) | Done | 2.56% | 100% | Add an explicit native capture fixture that paints a deterministic target, polls real mapped WinRT frames with a five-second deadline and verifies center pixels before and after an occluding topmost window. Keep the hardware-dependent gate explicit and link only the fixture against system GDI. |
| #2.3 | Extract bounds via `DwmGetWindowAttribute` and send `MsgWindowGeometry` | Done | 2.56% | 100% | Apply host fullscreen requests through the target monitor, retain original styles and bounds, restore them when leaving fullscreen or stopping tracking, and account for invisible DWM borders when resizing the captured window. Native tests verify style restoration through both transitions and teardown alongside real WinRT/audio capture. |
| **3**   | **Video Capture & Transport (Guest)** | | | | |
| #3.1 | Initialize DXGI Desktop Duplication / Windows.Graphics.Capture pipeline | Done | 3.2% | 100% | Negotiate explicit linear or advertised implicit ARGB imports, retain slot ownership through asynchronous creation, and fall back per window after compositor rejection. Preserve pending contexts across retirement and bound teardown; native protocol evidence confirms genuine KVMFR created/attach/release, while the regular-file platform gate still passes. |
| #3.2 | Implement rendering/copying DXGI surface to IVSHMEM double/triple buffer slots | Done | 5.12% | 100% | Negotiate explicit linear or advertised implicit ARGB imports, retain slot ownership through asynchronous creation, and fall back per window after compositor rejection. Preserve pending contexts across retirement and bound teardown; native protocol evidence confirms genuine KVMFR created/attach/release, while the regular-file platform gate still passes. |
| #3.3 | Implement `memory_order_acquire/release` fences for buffer synchronization | Done | 3.2% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #3.4 | Dispatch `MsgFrameReady` over VSOCK with buffer index and damage rects | Done | 1.92% | 100% | Negotiate explicit linear or advertised implicit ARGB imports, retain slot ownership through asynchronous creation, and fall back per window after compositor rejection. Preserve pending contexts across retirement and bound teardown; native protocol evidence confirms genuine KVMFR created/attach/release, while the regular-file platform gate still passes. |
| **4**   | **Audio Capture & Transport (Guest)** | | | | |
| #4.1 | Initialize WASAPI `AUDIOCLIENT_ACTIVATION_PARAMS` for per-process loopback | Done | 3.2% | 100% | Require an occlusion frame timestamp newer than the actual window transition. Activate native per-process WASAPI loopback, render bounded PCM through a Windows endpoint, drain the real stream into the shared-format ring, verify nonzero samples and release all handles; the provisioned guest captures 71520 frames in the native gate. |
| #4.2 | Implement lockless SPSC write loop from WASAPI buffers to IVSHMEM audio ring | Done | 5.12% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #4.3 | Implement audio buffer overrun recovery and latency logging | Done | 2.56% | 100% | Negotiate explicit linear or advertised implicit ARGB imports, retain slot ownership through asynchronous creation, and fall back per window after compositor rejection. Preserve pending contexts across retirement and bound teardown; native protocol evidence confirms genuine KVMFR created/attach/release, while the regular-file platform gate still passes. |
| **5**   | **Host Wayland Client (Video)** | | | | |
| #5.1 | Create `xdg_toplevel` upon receiving `MsgWindowCreate` from guest | Done | 3.2% | 100% | Negotiate explicit linear or advertised implicit ARGB imports, retain slot ownership through asynchronous creation, and fall back per window after compositor rejection. Preserve pending contexts across retirement and bound teardown; native protocol evidence confirms genuine KVMFR created/attach/release, while the regular-file platform gate still passes. |
| #5.2 | Convert IVSHMEM slots to DMA-BUFs using KVMFR / memfds | Done | 3.84% | 100% | Negotiate explicit linear or advertised implicit ARGB imports, retain slot ownership through asynchronous creation, and fall back per window after compositor rejection. Preserve pending contexts across retirement and bound teardown; native protocol evidence confirms genuine KVMFR created/attach/release, while the regular-file platform gate still passes. |
| #5.3 | Handle `MsgFrameReady` by attaching DMA-BUF to `wl_surface` and committing | Done | 3.2% | 100% | Negotiate explicit linear or advertised implicit ARGB imports, retain slot ownership through asynchronous creation, and fall back per window after compositor rejection. Preserve pending contexts across retirement and bound teardown; native protocol evidence confirms genuine KVMFR created/attach/release, while the regular-file platform gate still passes. |
| #5.4 | Process `MsgWindowGeometry` to move/resize/minimize Wayland surfaces | Done | 3.2% | 100% | Treat xdg-shell zero dimensions as client-selected size while still forwarding changed fullscreen state. Preserve guest dimensions, avoid unchanged echoes, and propagate delivery failures. Add isolated callback regressions for zero-size entry/exit, valid resizing, invalid sizes and callback failure; the extended AV sanitizers and real Wayland/PipeWire platform gate pass. |
| **6**   | **Host PipeWire Client (Audio)** | | | | |
| #6.1 | Initialize PipeWire stream matching the hardcoded IVSHMEM format | Done | 3.2% | 100% | Negotiate explicit linear or advertised implicit ARGB imports, retain slot ownership through asynchronous creation, and fall back per window after compositor rejection. Preserve pending contexts across retirement and bound teardown; native protocol evidence confirms genuine KVMFR created/attach/release, while the regular-file platform gate still passes. |
| #6.2 | Implement lockless SPSC read loop from IVSHMEM audio ring buffer | Done | 3.84% | 100% | Implement acquire/release video ownership and Zig SPSC PCM copies with wraparound, drop-new overruns, consumer latency trimming and underrun silence; allocator and invalid-bound tests pass. |
| #6.3 | Feed audio frames into PipeWire playback buffer callback | Done | 2.56% | 100% | Negotiate explicit linear or advertised implicit ARGB imports, retain slot ownership through asynchronous creation, and fall back per window after compositor rejection. Preserve pending contexts across retirement and bound teardown; native protocol evidence confirms genuine KVMFR created/attach/release, while the regular-file platform gate still passes. |
| **7**   | **Testing & CI Verification** | | | | |
| #7.1 | Write unit tests for lockless IVSHMEM queue mechanisms (video and audio) | Done | 1.28% | 100% | Negotiate explicit linear or advertised implicit ARGB imports, retain slot ownership through asynchronous creation, and fall back per window after compositor rejection. Preserve pending contexts across retirement and bound teardown; native protocol evidence confirms genuine KVMFR created/attach/release, while the regular-file platform gate still passes. |
| #7.2 | Run LeakSanitizer/AddressSanitizer and verify zero bytes leaked | Done | 1.28% | 100% | Retain the already-loaded system D-Bus library while PipeWire closes its RTKit connections, then release global bus caches before the final library close. The standalone host owns the only bus lifecycle. Native ASan/LSan/UBSan now reports zero leaks instead of 3677 bytes; all bounded module coverage gates remain at one hundred percent. |
| **8**   | **External Dependencies & Submodules** | | | | |
| #8.1 | Pin and verify Looking Glass driver ABI headers as a submodule | Done | 1.28% | 100% | Add public HTTPS Looking Glass submodule pinned to B7 commit 27fe47c; audit GPL-2.0-or-later headers for GPL-3.0 compatibility and isolate usage to KVMFR/IVSHMEM driver ABI declarations. |
| **10**  | **All-in-one AV environment provisioning** | | | | |
| #10.1 | Probe host/guest prerequisites and report actionable capability failures | Done | 4% | 100% | Reload committed AV settings after configuration and run the same mapping, Wayland and connected PipeWire probe used by av probe before publishing a deployment selector. Real managed setup succeeds with both host and guest; an unavailable Wayland socket fails setup and preserves the previous selector byte-for-byte. |
| #10.2 | Build/install pinned KVMFR module and provision shared-memory access | Done | 4% | 100% | With the isolated acceptance VM stopped, rerun the actual two-GiB capacity, bounded seeking, CLOEXEC DMA-BUF aliasing, out-of-range and exclusive-ownership gate under ASan/LSan/UBSan. Build the pinned patched module for the running kernel and preserve the existing compatible live module. Complete KVMFR distribution/access provisioning; safe GPU rejection and the RTT framework are now complete, with hardware performance explicitly unverified. |
| #10.3 | Configure managed QEMU IVSHMEM, silent audio endpoint and GPU capture | Done | 4% | 100% | Managed UEFI/q35/IVSHMEM and silent audio configured; shared read-only IOMMU probe rejects active graphics/audio drivers and accepts VFIO/unbound snapshots. Physical capture is blocked by busy NVIDIA/AMD GPUs; manual VFIO ownership remains required. |
| #10.4 | Deploy guest AV agent and signed drivers with readiness verification | Done | 2.4% | 100% | Verify the complete source archive after extraction and run actual managed guest deployment plus native host/guest probes from its own CLI and sibling binaries. Record passing repository-wide ASan/LSan/UBSan regressions, coverage and all 1000 storage cycles. Keep real driver-reboot, safe GPU preparation and video/audio performance acceptance explicitly incomplete. |
| #10.5 | Complete host-clock latency/fidelity diagnostic framework | Done | 1.6% | 100% | Host RGB24 challenges, bounded Zig center-pixel verification and Wayland processing acknowledgements; 16 visible plus 16 occluded samples. Framework complete per revised request; physical 144-Hz, scanout and audio latency acceptance remain unmeasured. |

| #11.1 | Specify mediated GPU architecture and prerequisites | Done | 4% | 100% | Existing host driver and administrator-owned slice. |
| #11.2 | Validate UUID configuration, CLI and read-only mdev probing | Done | 4% | 100% | No kernel or driver modification. |
| #11.3 | Launch QEMU using mdev sysfsdev with audio and IVSHMEM | Done | 4% | 100% | Reject physical passthrough fallback. |
| #11.4 | Remove guest timestamp dependence; warn on display prerequisite | Pending | 4% | 0% | Reuse host-clock RTT framework. |
| #11.5 | Verify coverage, sanitizers and final distribution | Pending | 4% | 0% | Runtime hardware acceptance remains external. |

**Total Feature Completion**: `92.0%`

## Completion scope

The revised request completes safe GPU rejection, the manual VDD prerequisite,
reboot handling and diagnostic framework. Task completion below means implementation
completion. Hardware performance, full guest RTT, native VDD mode verification,
real reboot-required installation and final remote CI remain external acceptance
gates; this branch is in Review and does not claim those gates passed.

## Outstanding acceptance evidence

- The 2026-10-05 program-provisioned Windows display reports 1920x1080 at 64 Hz.
  The validated native benchmark recorded 113 unique frames in 3002.042 ms
  (37.64 FPS), zero duplicates and 12 future WGC timestamps. Capture-age timing
  is invalid; neither video <7 ms at 144 Hz nor audio <10 ms is measured.
  A later pixel-freshness diagnostic accepted 11 unique frames in 3000.955 ms
  (3.67 FPS) and rejected nine future timestamps; performance remains unverified.
- Both host GPUs retain active desktop/render clients. No eligible GPU has been
  detached. The read-only IOMMU group probe rejects either active GPU and
  every actively bound companion. Hardware reassignment is a manual prerequisite.
- Driver reboot-required status now has one graceful managed restart and fresh
  re-probe; real reboot-required native acceptance remains unverified.
- Complete archive assembly and required-input failure checks pass locally. The
  new same-run CI transfer and final-commit checks still require remote evidence.
- Live host/guest setup passes; an unavailable Wayland socket fails publication
  and preserves the previous deployment selector. AV and repository-wide native
  sanitizers passed historically; fresh AV sanitizers pass in this session.
  Current protocol/memory lines are 100%; branches are 94.44% audio/pixels,
  95.83% codec and 100% layout/video. These results do not satisfy the missing performance acceptance.

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

- **Commit `3888fc917051`**: `fix(av): match WinRT SDK namespace and DLL export linkage`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Resolve native MSVC diagnostics by including the apartment API, qualifying the SDK DXGI interface namespace and sharing C ABI export declarations between the adapter header and implementation. Native Windows CI will recompile the boundary with warnings treated as errors.

- **Commit `6d147930638a`**: `fix(av): preserve full KVMFR capacity and bound host connection`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Use the 64-bit kernel ioctl return for all two-GiB capacity probes, reject incompatible character mappings before mmap and bound nonblocking VSOCK startup to five seconds. Transport/provisioning tests and native client/helper builds pass.

- **Commit `8bd377922306`**: `feat(av): provision private access for newly loaded KVMFR`
  - **Task Impact**: +15% to #10.2 (+0.75% overall)
  - **Summary**: Assign a newly loaded exact-size device to the validated sudo invoking user with mode 0600, verify ownership operations and preserve pre-existing devices. Build and probe use the pinned module and nontruncating capacity ABI; running-kernel module build passes.

- **Commit `2129177c3c06`**: `chore(deps): correct pinned KVMFR two-GiB capacity boundaries`
  - **Task Impact**: +10% to #10.2 (+0.50% overall)
  - **Summary**: Apply an explicit build-tree patch that widens B7 allocation multiplication and adds a uint64 capacity out-parameter ioctl to avoid kernel/libc return truncation. Preserve the immutable submodule pin and incompatible live devices; running-kernel build and private-access load pass.

- **Commit `fa33272a5946`**: `test(av): verify real KVMFR exports and lease preservation`
  - **Task Impact**: +10% to #5.2 (+0.48% overall)
  - **Summary**: Exercise the program-provisioned driver with a two-GiB capacity probe, CLOEXEC DMA-BUF export, bidirectional mmap aliasing, bounds rejection and refusal to reset a consumed slot. Verify teardown preserves the device; the explicit native gate passes on this host.

- **Commit `9c770a540ec4`**: `feat(av): define bounded managed setup and run command grammar`
  - **Task Impact**: +10% to #10.1 (+0.50% overall)
  - **Summary**: Parse setup, probe and PID run commands in Zig before side effects, copy fixed-size device/GPU selections and reject duplicate, traversal, malformed PCI and overflow inputs. Native allocator-free grammar tests pass.

- **Commit `bb9acaea59e0`**: `chore(deps): bundle verified B7 signed IVSHMEM package`
  - **Task Impact**: +20% to #10.4 (+0.60% overall)
  - **Summary**: Download the pinned Looking Glass binary distribution over HTTPS, verify its exact SHA-256 before extraction and bundle only the signed IVSHMEM package and license in ignored distribution outputs. Preserve the existing submodule architecture and avoid installing the third-party capture service.

- **Commit `3700ddb0cf2d`**: `feat(av): install signed guest driver and probe native readiness`
  - **Task Impact**: +15% to #10.1 (+0.75% overall); +25% to #10.4 (+0.75% overall)
  - **Summary**: Add native pnputil deployment with bounded paths, child timeout/reaping and explicit reboot status. Probe actual OS build, mapped IVSHMEM ABI, D3D11/WARP, sibling WinRT adapter and enabled audio endpoint before declaring readiness; cross-linked guest and invalid-path fixture pass.

- **Commit `65e55bb6a333`**: `feat(av): integrate provisioning and readiness into managed CLI`
  - **Task Impact**: +10% to #10.3 (+0.50% overall); +20% to #10.4 (+0.60% overall)
  - **Summary**: Connect validated av setup/probe commands to the existing device registry and execution bridge. Configure AV atomically on quiescent devices, optionally provision KVMFR through the bundled helper, stage owned agent/DLL/signed-driver files atomically in private device state and install/probe the guest without shell interpolation.

- **Commit `a5de469c6758`**: `fix(av): wait for connected PipeWire state and surface failures`
  - **Task Impact**: +5% to #6.1 (+0.20% overall)
  - **Summary**: Publish stream states atomically, wait with a five-second readiness deadline and reject disconnected/error states before reporting startup success. Check asynchronous playback failure in the host event loop and stop the worker before releasing borrowed memory.

- **Commit `646dbe9595cf`**: `fix(av): drain compositor leases and handle display backpressure`
  - **Task Impact**: +5% to #5.3 (+0.20% overall)
  - **Summary**: Retire surfaces and wait up to two seconds for real buffer release before destroying presentation contexts, preserving consumed slots on timeout. Poll display writability after EAGAIN and snapshot shared dimensions/stride once before validation and protocol submission.

- **Commit `45091e110c3d`**: `test(av): exercise actual compositor release and PCM playback`
  - **Task Impact**: +10% to #5.1 (+0.40% overall); +5% to #5.3 (+0.20% overall); +5% to #6.1 (+0.20% overall); +10% to #6.3 (+0.32% overall)
  - **Summary**: Present triple-buffered surfaces through the live Wayland compositor, consume shared PCM through connected PipeWire callbacks and verify every slot returns to Free after bounded teardown. The explicit native platform gate passes on this host without emitting audible test samples.

- **Commit `909fded9c865`**: `feat(av): own managed guest and host session startup`
  - **Task Impact**: +10% to #10.4 (+0.30% overall)
  - **Summary**: Launch the deployed target-PID agent through the existing bridge, await its exact bounded readiness line with a deadline and start native playback for the configured CID/mapping. Cancel and reap only owned execution children on failure or playback exit; preserve the user game process.

- **Commit `6d872a5bbc9b`**: `feat(av): provision persistent firmware for UEFI Windows guests`
  - **Task Impact**: +5% to #10.3 (+0.25% overall)
  - **Summary**: Add explicit managed UEFI setup, validate its configuration and create private persistent OVMF variables without replacing an existing store. Add matching q35/pflash arguments while retaining default BIOS behavior; parser, provisioning and QEMU lifecycle regressions pass.

- **Commit `66be4f1ed6d6`**: `fix(av): publish firmware stores atomically and preserve existing state`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Create persistent OVMF variables through flushed staging files and non-overwriting publication, preventing partial stores after interrupted setup. Verify stable inode reuse and symlink preservation, and specify UEFI boot, package and lifecycle boundaries in the implementation description.

- **Commit `b4943111098f`**: `feat(av): provision KVM and VSOCK access before managed startup`
  - **Task Impact**: +10% to #10.1 (+0.50% overall)
  - **Summary**: Probe both host kernel devices and grant the validated invoking user a specific read/write ACL through the privileged bundled helper when needed. Preserve ownership/groups, use no world-write permissions and recheck actual access before boot; native access provisioning passes.

- **Commit `80ac6ff66f82`**: `fix(av): release mappings after early hypervisor exit`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Release AV mappings and stop the filesystem child when readiness polling reaps QEMU before the event loop. Add a reaper fallback for already-reaped children and a regression proving failed startup removes owned memory and permits a clean retry.

- **Commit `a5a3e3131abd`**: `chore(deps): make KVMFR regions importable by system virtiofsd`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Add bounded fixed-size seeking to the pinned driver build patch so vhost-user can determine and map IVSHMEM regions. Require compatible seek/size probes before boot and verify them with real exports; reload only the owned idle validation module and pass the native gate.


- **Commit `4341783b76ea`**: `fix(av): boot UEFI guests with inbox-compatible AHCI storage`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Attach UEFI AV system disks through q35 AHCI so installed Windows images can boot before any optimized VirtIO storage driver is deployed. Preserve BIOS profile behavior and disk contents; QEMU lifecycle and argument regressions pass.

- **Commit `9f4794f26913`**: `fix(av): prioritize the managed UEFI system disk at boot`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Specify strict disk-first firmware boot order for UEFI AV profiles, preventing unrelated virtual network/filesystem boot candidates from preceding the managed Windows disk. Preserve the persistent variables and existing BIOS defaults; QEMU regressions pass.

- **Commit `e17595731cc7`**: `test(av): enforce per-module coverage and complete sanitizer gates`
  - **Task Impact**: +20% to #7.1 (+0.32% overall); +25% to #7.2 (+0.40% overall)
  - **Summary**: Gate production lines and branches independently at ninety percent for audio, control, layout and video; current results are one hundred percent in each module. Prevent constant-folded test calls, cover real sparse mapping initialization and invalid pool/metadata fields, and run all native AV suites under ASan/LSan/UBSan with leak-checking Zig tests.

- **Commit `8960b856330e`**: `feat(av): import companion firmware metadata for installed images`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Allow an explicit bounded firmware metadata path during setup, imply UEFI and stage the supplied image companion atomically under a quiescent device lease. Preserve the source and reject active-device imports; normal restart still retains the owned firmware store.

- **Commit `658d49d2fa6b`**: `fix(av): reuse live managed runtime after readiness timeout`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Retry guest readiness against the existing QEMU child without remapping shared memory or replacing the filesystem worker. Reap stale children before clean startup and prove live retries retain their PID and mapping with a supervisor regression.

- **Commit `679a277683da`**: `fix(av): lease KVMFR across the managed runtime lifecycle`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Hold an exclusive nonblocking descriptor lease before initializing a device so idle video and audio-only sessions cannot be reset by another profile. Verify a second owner fails before mapping and succeeds after teardown; regular-file provisioning remains exclusive through creation.

- **Commit `290819a54b2a`**: `fix(av): start deployment commands in a valid guest directory`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Use the installed Windows directory as the explicit working directory for setup, probe and capture-agent jobs. Host CLI working directories may not exist on the guest export and otherwise fail Windows CreateProcess before any capability probe can run.

- **Commit `ec80d1d8a4ed`**: `fix(av): preserve signed INF driver filename casing on deployment`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Deploy the unchanged vendor SYS bytes under the exact IVSHMEM.sys basename referenced by the signed INF. Native Windows installation logs prove case-sensitive VirtIO-FS lookup otherwise rejects the package despite all lower-case files being present.

- **Commit `3fb55037ab3b`**: `test(av): verify real WinRT pixels through occlusion`
  - **Task Impact**: +10% to #2.1 (+0.40% overall); +10% to #2.2 (+0.32% overall); +5% to #2.3 (+0.16% overall); +10% to #3.1 (+0.40% overall); +10% to #10.1 (+0.50% overall); +15% to #10.4 (+0.45% overall)
  - **Summary**: Add an explicit native capture fixture that paints a deterministic target, polls real mapped WinRT frames with a five-second deadline and verifies center pixels before and after an occluding topmost window. Keep the hardware-dependent gate explicit and link only the fixture against system GDI.

- **Commit `a4b7de1231d2`**: `test(av): capture real process audio and reject stale occlusion frames`
  - **Task Impact**: +15% to #4.1 (+0.60% overall); +10% to #4.3 (+0.32% overall)
  - **Summary**: Require an occlusion frame timestamp newer than the actual window transition. Activate native per-process WASAPI loopback, render bounded PCM through a Windows endpoint, drain the real stream into the shared-format ring, verify nonzero samples and release all handles; the provisioned guest captures 71520 frames in the native gate.

- **Commit `84a47a96daa1`**: `fix(av): cancel and reap managed playback on parent signals`
  - **Task Impact**: +5% to #10.4 (+0.15% overall)
  - **Summary**: Catch SIGINT/SIGTERM during listener startup and playback, stop the owned host child, cancel/reap the owned bridge job and restore caller signal dispositions on every exit. A real twelve-second managed session exits 143 after SIGTERM without orphaned host/guest agents or terminating its selected application.

- **Commit `9b40ca28b739`**: `fix(av): retain and monitor the actual game process lifetime`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Reject nonexistent or exited target PIDs before listener readiness and retain a non-inheritable process synchronization handle until teardown. End normally when that process exits, avoiding silent audio-only success for a stale PID; close the handle on mapping, Winsock and session failure paths.

- **Commit `b831b529b965`**: `fix(av): verify readiness after already-current driver installation`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Treat PnPUtil ERROR_NO_MORE_ITEMS as an idempotent installation outcome, then require the actual mapped-driver and endpoint capability probe. Native repeat setup reports an already-present signed package and current device driver rather than a deployment failure.

- **Commit `4ebef372279d`**: `feat(av): round-trip fullscreen state and visible window dimensions`
  - **Task Impact**: +5% to #2.3 (+0.16% overall); +25% to #5.4 (+1.00% overall)
  - **Summary**: Apply host fullscreen requests through the target monitor, retain original styles and bounds, restore them when leaving fullscreen or stopping tracking, and account for invisible DWM borders when resizing the captured window. Native tests verify style restoration through both transitions and teardown alongside real WinRT/audio capture.

- **Commit `b51c8b55f77e`**: `fix(av): poll capture with an owned high-resolution timer`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Replace the rounded seven-millisecond message wait with a one-millisecond high-resolution waitable timer so capture polling can service 144-Hz frames without changing the global timer resolution. Keep Windows messages and cancellation in the same wait; cancel and close the timer on all setup and session exits.

- **Commit `dab6f31f6978`**: `fix(av): publish immutable guest agent and adapter bundles`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Deploy each complete agent/DLL pair under a fresh private version directory and select it atomically only after native setup/probe passes. Validate the selector before run/probe; native repeat setup now executes the new image and accepts the already-current signed driver instead of reusing a cached Windows image section.

- **Commit `712121b2a5b3`**: `fix(av): serialize managed AV commands per device`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Acquire a private same-user no-symlink command lease for setup, probe and the full playback lifetime, releasing it on every command exit. A concurrent probe during genuine managed playback fails immediately while cancellation still reaps owned children; separate profiles retain independent leases.

- **Commit `0a5df99697eb`**: `feat(av): import DMA buffers asynchronously with retained fallback`
  - **Task Impact**: +5% to #3.1 (+0.20% overall); +10% to #3.2 (+0.64% overall); +15% to #3.4 (+0.36% overall); +5% to #4.3 (+0.16% overall); +5% to #5.1 (+0.20% overall); +10% to #5.2 (+0.48% overall); +5% to #5.3 (+0.20% overall); +5% to #6.1 (+0.20% overall); +5% to #6.3 (+0.16% overall); +5% to #7.1 (+0.08% overall)
  - **Summary**: Negotiate explicit linear or advertised implicit ARGB imports, retain slot ownership through asynchronous creation, and fall back per window after compositor rejection. Preserve pending contexts across retirement and bound teardown; native protocol evidence confirms genuine KVMFR created/attach/release, while the regular-file platform gate still passes.

- **Commit `6703a69e9505`**: `fix(av): retry window admission after retained pool release`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Allow a synchronous Create callback to defer admission without failing the session, clear only that unaccepted registry entry and rescan at a bounded 250-ms interval. Retain compositor-owned pool states, retry newly available windows without duplicate events and verify deferred admission in the native capture/audio fixture.

- **Commit `e49aae39abee`**: `feat(av): probe actual host mapping and playback connections`
  - **Task Impact**: +10% to #10.1 (+0.50% overall)
  - **Summary**: Extend managed probe to validate the guest first, then map the configured region and establish real Wayland and ready PipeWire clients without opening a capture session. Tear down workers before unmapping on every path; the program-provisioned native host/guest probe passes.

- **Commit `3d231fea96cf`**: `fix(av): release RTKit bus caches after playback teardown`
  - **Task Impact**: +10% to #7.2 (+0.16% overall)
  - **Summary**: Retain the already-loaded system D-Bus library while PipeWire closes its RTKit connections, then release global bus caches before the final library close. The standalone host owns the only bus lifecycle. Native ASan/LSan/UBSan now reports zero leaks instead of 3677 bytes; all bounded module coverage gates remain at one hundred percent.

- **Commit `348245209eb5`**: `feat(av): provision full-HD high-refresh virtual capture display`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Advertise a 1920x1080 144-Hz virtual VGA EDID with sufficient framebuffer memory for AV profiles that use the software capture path. Keep explicitly assigned GPUs in control of their display modes and preserve non-AV defaults; hypervisor argument/lifecycle regressions pass.

- **Commit `9af0394811a8`**: `fix(av): instantiate the configured virtual display explicitly`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Instantiate the full-HD high-refresh VGA device explicitly instead of relying on global properties and an implicit default. Native boot inspection showed those globals suppress the default video device, leaving Windows without a capture display; argument and lifecycle regressions pass with the explicit device.

- **Commit `97089db0e8a7`**: `docs(tracker): fix markdown table formatting in feature progress tracker`
  - **Task Impact**: 0% overall; formatting fix
  - **Summary**: Remove blank lines breaking table parsing before #8.1 and section 10, add the section 8 category row, and restore continuous table rendering in markdown preview mode.


- **Commit `14ce1823114a`**: `test(av): validate native capture throughput and timestamp diagnostics`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Finish the optional native benchmark with a changing pixel, fresh unique-frame accounting, bounded QPC age and interval statistics, and deterministic timer cleanup. Real acceptance reports 64 Hz and invalid future WGC timestamps; return failure for invalid age measurements and leave end-to-end video/audio acceptance incomplete.

- **Commit `48b4d1f6dc40`**: `fix(av): gate deployment publication on native host readiness`
  - **Task Impact**: +3% to #10.1 (+0.15% overall)
  - **Summary**: Reload committed AV settings after configuration and run the same mapping, Wayland and connected PipeWire probe used by av probe before publishing a deployment selector. Real managed setup succeeds with both host and guest; an unavailable Wayland socket fails setup and preserves the previous selector byte-for-byte.

- **Commit `ad51961595e9`**: `feat(av): assemble a complete host and guest distribution archive`
  - **Task Impact**: +1% to #10.4 (+0.03% overall)
  - **Summary**: Make all assemble a coherent AV archive with host binaries, matching guest execution/capture binaries, native WinRT DLL, signed driver package, patched kernel-module sources, source archives, licenses and SHA-256 manifest. Keep CLI regressions independent of native Windows SDK packaging. Test every missing required input and verify failed assembly preserves the prior complete archive; same-run CI assembly remains pending.

- **Commit `8caa40fcf5ef`**: `chore(av): publish a verified same-run distribution from native CI`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Receive the native Windows SDK artifact in the Linux AV job, assemble and extract-check the single complete distribution, and upload it only after the existing gates pass. Add parser and package failure regressions to the AV suite. Move Linux runner allocation to ubuntu-22.04 while preserving the Debian trixie verification environment after hosted ubuntu-24.04 jobs repeatedly failed before acquiring a runner.

- **Commit `468bb43da1f9`**: `fix(av): provision invoking-user access when reusing live KVMFR`
  - **Task Impact**: +2% to #10.2 (+0.10% overall)
  - **Summary**: Validate SUDO_UID and grant a specific-user ACL even when the privileged helper can already open a compatible live KVMFR device. Preserve its ownership and active mappings, fail on an invalid identity or grant failure, and add syscall-isolated tests that never mutate kernel devices. The extended AV sanitizer suite and real compatible-device reuse pass.

- **Commit `bed28222608c`**: `fix(av): forward fullscreen state from zero-size Wayland configure`
  - **Task Impact**: +3% to #5.4 (+0.12% overall)
  - **Summary**: Treat xdg-shell zero dimensions as client-selected size while still forwarding changed fullscreen state. Preserve guest dimensions, avoid unchanged echoes, and propagate delivery failures. Add isolated callback regressions for zero-size entry/exit, valid resizing, invalid sizes and callback failure; the extended AV sanitizers and real Wayland/PipeWire platform gate pass.

- **Commit `7a4df1104b11`**: `docs(av): align architecture and acceptance claims with observed behavior`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Replace stale conceptual C++ ABI and zero-copy/input/reconnect claims with the implemented C/Zig interfaces, exact layouts, startup/teardown ownership and drop-new audio policy. Document observed 64-Hz/37.64-FPS capture with invalid WGC age timing, active desktop GPUs, pending automatic GPU recovery and driver reboot handling. Preserve incomplete tracker status and acceptance thresholds.

- **Commit `1ea0f5e818d2`**: `feat(av): restart and re-probe managed guests after driver reboot status`
  - **Task Impact**: +1% to #10.4 (+0.03% overall)
  - **Summary**: Handle reboot-required signed-driver setup with one graceful restart of the selected managed device, a fresh probe of the immutable guest bundle and native host readiness before publication. Propagate every failure without a force kill, repeated installation or false selector update. Verify ordered steps, first-failure termination, ordinary ready setup and invalid callbacks under the native sanitizer suite; real reboot-required installation remains unverified.

- **Commit `298c4b1b979e`**: `docs(av): record extracted distribution readiness and regression evidence`
  - **Task Impact**: +1% to #10.4 (+0.03% overall)
  - **Summary**: Verify the complete source archive after extraction and run actual managed guest deployment plus native host/guest probes from its own CLI and sibling binaries. Record passing repository-wide ASan/LSan/UBSan regressions, coverage and all 1000 storage cycles. Keep real driver-reboot, safe GPU preparation and video/audio performance acceptance explicitly incomplete.

- **Commit `40c4a2208f06`**: `test(av): prove occlusion freshness through newly painted pixels`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Change the target color while an actual topmost window covers it and require the newly painted center pixels, ignoring only known queued old content. Count benchmark samples only when pixel validation accepts them. Real native capture and process-loopback fidelity pass; the later 64-Hz benchmark still fails timestamp validity and performance acceptance, which remains incomplete.

- **Commit `23d64f7485d1`**: `fix(av): preserve live guests when driver restart times out`
  - **Task Impact**: 0% overall; design/audit only
  - **Summary**: Add a distinct ACPI-only daemon request for AV driver restarts; older supervisors reject it without interpreting a nonzero legacy force flag. On timeout retain VM, filesystem, sockets, mapping leases and prior subsystem state, returning ETIMEDOUT. Test an actual child/mapping timeout, native client/server request exchange and malformed payload rejection under ASan/LSan/UBSan; repository-wide sanitizer regressions pass.

- **Commit `73268e3187aa`**: `docs(av): complete native KVMFR provisioning verification`
  - **Task Impact**: +8% to #10.2 (+0.40% overall)
  - **Summary**: With the isolated acceptance VM stopped, rerun the actual two-GiB capacity, bounded seeking, CLOEXEC DMA-BUF aliasing, out-of-range and exclusive-ownership gate under ASan/LSan/UBSan. Build the pinned patched module for the running kernel and preserve the existing compatible live module. Complete KVMFR distribution/access provisioning; safe GPU rejection and the RTT framework are now complete, with hardware performance explicitly unverified.

- **Commit `d92436d6289b`**: `fix(av): preserve independently unspecified Wayland dimensions`
  - **Task Impact**: +2% to #5.4 (+0.08% overall); +5% to #7.2 (+0.08% overall).
  - **Summary**: Preserve each zero configure axis independently; test mixed zero axes and invalid sizes. Fresh native Wayland/PipeWire teardown and full AV ASan/LSan/UBSan gates pass with no reported leaks. HEAD identifies this containing commit until the next log update resolves its hash.

- **Commit `bca23c1008aa`**: `feat(av): reject active GPU groups without detaching host drivers`
  - **Task Impact**: +5% to #10.3 (+0.25% overall).
  - **Summary**: Share read-only sysfs group probing between setup and daemon startup. Validate PCI paths with Zig, reject missing/malformed groups and active companion drivers, accept unbound/VFIO snapshots. Synthetic tests and fresh AV sanitizers pass; actual NVIDIA 0000:01:00.0 and AMD 0000:0e:00.0 probes reject active drivers. No sysfs writes or service stops occur.

- **Commit `a22cb4e5d106`**: `feat(av): measure host-clock color challenge commit round trips`
  - **Task Impact**: +100% to #10.5 (+2.00% overall), framework scope explicitly revised by user.
  - **Summary**: Add opt-in fixture flash protocol, selected-process/class/title validation, bounded RGB matching in Zig, owned Wayland sync callback, host CLOCK_MONOTONIC RTT and five-second deadlines. Package the fixture and expose managed --latency. Verify wire/pixel/callback tests, native compositor acknowledgement and Linux/Windows cross-builds. Protocol/memory line coverage is 100%; measured branch coverage is 94.44%, 95.83%, 100%, 100%. Full guest RTT and performance remain unmeasured because no acceptance VM is running.

- **Commit `1e1fa22514ca`**: `fix(av): classify native reboot codes and probe primary display mode`
  - **Task Impact**: +2% to #10.1 (+0.10% overall); +2% to #10.4 (+0.06% overall).
  - **Summary**: Share/test native PnPUtil classification for 3010 and 1641 through the ordered graceful restart/re-probe state machine. Add a read-only interactive primary 1920x1080/144-Hz display capability probe and document upstream signed VDD installation/configuration as a manual prerequisite. Fresh AV sanitizers pass; Windows builds link the shared classifier. Real driver-triggered reboot and VDD runtime verification remain unexecuted.

- **Commit `06fbd58bf545`**: `chore: final regression and distribution packaging for high-perf-av-passthrough`
  - **Task Impact**: 0% to #5.4, #7.2, #10.1, #10.3, #10.4 and #10.5; final verification of 100% implementation completion (+0.00% overall).
  - **Summary**: Fresh make av-coverage, av-test, av-sanitizers, av-distribution and repository-wide make test pass. Native Wayland/PipeWire matching-commit acknowledgement and teardown passed under ASan/LSan/UBSan; Linux clients and Windows executables cross-build. Extracted distribution manifest passes every checksum and includes the diagnostic fixture. Reassemble/check the archive after this documentation commit so bundled sources and provenance match HEAD. README is unchanged; no GPU detached, PR opened or merge performed. Remote CI, full guest RTT, native reboot-required install, VDD mode and physical video/audio latency remain unverified.

- **Commit `ccae02c1f50b`**: `docs(av): specify mediated GPU slicing architecture`
  - **Task Impact**: +100% to #11.1 (+4% overall); reweight previous completed work to 80% for the expanded scope.
  - **Summary**: Specify administrator-owned UUID slices, optional parent checks, retained host drivers, no physical passthrough fallback, vendor prerequisites and honest runtime acceptance boundaries.

- **Commit `da11440864b0`**: `feat(av): validate and probe administrator-owned mdev slices`
  - **Task Impact**: +100% to #11.2 (+4% overall).
  - **Summary**: Add transactional UUID config/edit/reset and bounded CLI parsing, early read-only setup probes with optional parent matching, and daemon preparation checks. Synthetic sysfs tests accept an active host driver and reject malformed, missing and wrong-API slices; AV tests and all 12 configuration tests pass. No GPU state is changed.

- **Commit `HEAD`**: `feat(av): launch mediated GPU UUIDs without physical assignment`
  - **Task Impact**: +100% to #11.3 (+4% overall).
  - **Summary**: Generate exact vfio-pci sysfsdev/display=off arguments in daemon_qemu.c, where QEMU provisioning actually lives. Preserve silent HDA, IVSHMEM and boot VGA; reject BDF-only assignment. Native QEMU argument/lifecycle tests pass for UUID with/without optional parent and verify absence of host=BDF.
