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

### Fixed pool layout

The PCI mapping is 2 GiB (2147483648 bytes), a power-of-two BAR as required by
ivshmem-plain. Used bytes are 1,610,678,272 (64 KiB metadata plus 48 * 32 MiB
pixel regions). wl_shm pools cover only the used region, below INT32_MAX. Metadata: magic 0x57415631 at 0, version 1 at 4, total bytes as
LE u64 at 8, audio header at 128, 48 video headers at 4096 + slot * 64, PCM at
16384 (4096 frames * 4 bytes). Pixels start at 65536 + slot * 33554432. Each
window has three slots; global slot = window_pool * 3 + local_buffer_index.
Window create's buffer_index carries pool 0..15; frame buffer_index remains 0..2.
Destroy does not permit pool reuse until all previously presented slots are Free.
32 MiB holds 3840x2160 BGRA; larger frames are rejected by byte capacity even if
control dimensions are within the protocol's 8192 pixel ceiling. This fixed
bounded pool avoids allocator activity during capture. Host initialization is
explicit and occurs before guest connection, never during a running session.

## 9. All-in-one environment provisioning (user-required scope extension)

AV startup must use the managed subsystem rather than require a separately built
AV test VM. Provisioning, capability probing, install recovery and guest readiness
are part of this feature. Existing task weights are rescaled to 80%; provisioning
receives 20%. Historical commit percentages remain historical; overall progress
is recalculated against this expanded scope. Nothing is marked complete merely
because the native capture source cross-compiles.

### Prerequisites and resource ownership

Host preflight checks KVM read/write access, Wayland compositor/shm/xdg-shell and
optional linear DMA-BUF support, PipeWire connection, running-kernel module build
headers and the pinned KVMFR module source. Guest probe checks OS build >=20348,
signed IVSHMEM/Viosock drivers, D3D capture capability, audio render endpoint and
AV agent version. Unsupported capabilities have explicit statuses and diagnostics;
provisioning must not claim that it can manufacture GPU virtualization support.
Consumer hardware may require exclusive VFIO passthrough rather than GPU slicing.
Only an explicitly configured GPU or already unbound device is eligible; the
program must not silently detach the host desktop GPU.

The program owns a private AV shared-memory resource and guest agent lifecycle.
KVMFR preparation builds the pinned module against the running kernel. Installed
module paths, modprobe configuration, permissions and signing are documented and
verified; failures retain a diagnostic and do not start a half-configured session.
A regular shared-memory file is an explicit fallback with wl_shm, not a DMA-BUF.
Kernel module loading requires privileged setup; normal capture runs unprivileged.
No runtime loose source downloads, unpinned builds or shell-interpolated paths.
Secure Boot failures must be reported; the program cannot silently enroll keys.

### VM and guest setup

Managed QEMU arguments include the AV mapping as memory-backend-file plus
ivshmem-plain, the existing VSOCK device, and a render endpoint connected to the
none audio backend to avoid duplicate host playback. The Windows guest must keep
a render endpoint enabled; process loopback cannot substitute an absent device.
The managed disk image is preserved; guest deployment copies the built agent and
installs already supplied signed driver packages via the execution bridge. Driver
binaries must come from a pinned, verified dependency with compatible distribution
terms. Setup does not overwrite an existing OS disk or assume a Windows license.
A missing image/installation media is an explicit provisionable prerequisite, not
permission to erase data. Existing CLI and filesystem provisioning continues to
work when AV is disabled.

### Operational sequence and failure recovery

1. Load validated AV configuration and run preflight before mutating resources.
2. Prepare/install host module, create or open its exact-size mapping and verify
   permissions. Initialize metadata only with no connected producer/consumer.
3. Add VM devices on startup, then probe guest readiness via the existing bridge.
4. Deploy agent/drivers, perform any required guest restart and re-probe.
5. Start the guest agent for the requested game process, then host video/audio.
6. On failure stop/join all workers and disconnect Wayland before releasing the
   mapping. A reconnect never resets slots still owned by a compositor.
7. Report unsupported GPU, module-signature and guest-driver failures separately
   from transport failure. Do not automatically retry a destructive install.

### Verification

Mock tests cover provisioner decisions, argv construction, exact mapping bounds,
failed installs and rollback without requiring root or a GPU in ordinary CI.
Native platform jobs build/link all adapters. Final completion additionally needs
an actual session created through the program, including driver readiness, window
lifecycle, isolated PCM playback, bounded compositor release, capture alpha and
occlusion fidelity, <7ms video and <10ms audio measured on the provisioned setup.

### WinRT compatibility boundary

The per-window fidelity path is av_wgc.dll, built with the base Windows SDK's
C++/WinRT projections. C++ is confined to WinRT activation, capture item/session,
frame pool and COM RAII. A narrow C ABI reports borrowed mapped texture rows to
C, which invokes Zig's bounded copy into a claimed slot. No raw new/delete,
heap buffer or frame pointer crosses this boundary. C loads the sibling DLL
through an absolute restricted-search path; it remains loaded until all opaque
sessions are destroyed. DLL deployment is part of managed guest provisioning.

CreateForWindow selects the target HWND (including its non-client content).
CreateFreeThreaded avoids a DispatcherQueue dependency. Polling occurs on the
single MTA capture thread; WinRT owns its internal capture worker. Frame pools
are recreated on content-size change after the old frame is closed. Every mapped
staging texture is unmapped and every frame closed via RAII even on exceptions.
Hardware D3D is preferred; WARP is a functional software fallback and cannot be
reported as achieving the hardware gaming performance target.

API contracts: [Microsoft CreateForWindow](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow)
and [free-threaded frame pools](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded).


### KVMFR two-GiB capacity correction

The immutable B7 dependency uses signed `int` multiplication for
`size_mb * 1024 * 1024`; 2048 MiB overflows before assignment to its unsigned-long
capacity. The owned build tree applies `scripts/patches/kvmfr_capacity.patch`,
which casts the first factor to `unsigned long` before multiplication. The gitlink
remains pinned; no upstream source is copied into tracked application directories.
Patch application fails if its exact source context no longer matches. Both libc and kernel ioctl dispatch truncate the legacy size return to `int`.
The patch therefore adds `_IOR('u', 0x45, __u64)`: an eight-byte native-order
capacity out parameter with ordinary zero/-errno return. Application probes use
this extension exclusively. Unpatched live modules fail ENOTTY and remain intact.
Only a 64-bit Linux host is supported. Module load requests the constant positive
2048 MiB size; external module parameters retain upstream semantics.

### Signed IVSHMEM driver distribution

`make av-drivers` obtains the Windows host artifact for the pinned Looking Glass
B7 release from `https://looking-glass.io/artifact/B7/host`, verifies SHA-256
`c2415a5a0c405f1d6aa936986bdd4b806c50574b4521747e113c3be2be047b1b`, and extracts
only `ivshmem.inf`, `ivshmem.sys`, `ivshmem.cat` and `LICENSE.txt` into the ignored
`build/vendor/av/ivshmem` distribution directory. No Looking Glass host service
is installed or started. The signed kernel package matches the mapped-device
ABI headers in the pinned git submodule; GPL license text accompanies the package.
The host release package is the upstream binary distribution of that submodule,
not a separately unpinned source dependency. A changed artifact fails the digest
check; no execution or extraction occurs before verification. Build-time base
packages `curl`, `sha256sum` and `7z` perform transfer/hash/archive extraction.
The Windows driver store independently verifies catalog signatures via `pnputil`;
test-signing and certificate bypass are never enabled. Viosock and VirtIO-FS remain
the existing execution-bridge prerequisites and are checked before AV deployment.


### Managed CLI session ownership

`waddle av run PROCESS_ID [--device NAME]` resolves a previously configured device,
launches the deployed agent through the existing execution bridge, and waits at
most 90 seconds for the exact bounded `AV guest: ready` line. The guest listens on
AV port 5001; the host connects to that guest CID, rather than the inverse direction.
The existing execution port remains separate. The CLI owns the execution child
and native host playback child; no shell interprets their arguments. On playback
exit/failure the CLI cancels and reaps its own execution child, escalating after
one second. Cancellation targets the AV agent job, never the pre-existing game
PID. A missing bundled executable, malformed readiness line or startup timeout
fails the command and tears down its owned children.


### Optional UEFI boot compatibility

`waddle av setup --uefi` writes `[av] uefi=1`. Default zero preserves BIOS boot
for existing images. The daemon requires the base-system OVMF files
`/usr/share/OVMF/OVMF_CODE_4M.fd` and `OVMF_VARS_4M.fd`; missing packages fail with
a capability diagnostic. It creates `<device-disk>.av_uefi.fd` with private 0600
permissions through a flushed staging file and non-overwriting hard-link publish.
An existing same-user regular store with matching template length is preserved;
symlinks, incorrect ownership/permissions and mismatched sizes fail. Firmware
variables persist across VM/AV teardown and are not reinitialized on restart.
QEMU uses q35, read-only code pflash and the persistent writable variables pflash.
The managed Windows disk is never altered by provisioning. Setup cannot convert
an installed BIOS Windows image to UEFI automatically; the caller selects firmware
matching that image. Secure Boot key enrollment is outside automated mutation.


### KVM/VSOCK host access provisioning

Setup probes read/write access to both `/dev/kvm` and `/dev/vhost-vsock` before
starting the managed VM. If either fails, the bundled privileged helper validates
`SUDO_UID` through the bounded decimal codec, checks non-symlink character devices
and adds a specific-user read/write ACL through absolute `/usr/bin/setfacl`.
Existing ownership/group permissions remain intact; no world-write mode is added.
Missing kernel devices or the base `acl` package produce explicit errors. Setup
rechecks actual access afterward. ACLs may be reset by reboot/hotplug policy, so
idempotent setup reprobes them; no session starts by assuming a previous grant.


### KVMFR and vhost-user filesystem compatibility

The B7 driver lacks `llseek`, while the system virtiofsd maps each shared QEMU
memory region after seeking its descriptor to determine its length. A KVMFR
IVSHMEM region therefore fails import with ESPIPE even though ordinary mappings
work. The owned driver patch adds a bounded `fixed_size_llseek` implementation
using the device's actual capacity; no read/write semantics change. AV preflight
requires both the explicit 64-bit size ioctl and SEEK_END to report 2 GiB, so an
older incompatible live module fails before a VM starts. Native tests check the
seek result/reset as well as actual DMA-BUF aliasing. Existing live modules are
never silently unloaded by setup; only the explicitly owned validation instance
is replaced while no VM/mapping/export retains it.


UEFI AV guests attach their existing system disk through q35's inbox-compatible
AHCI controller (`if=ide`) so an installed Windows image does not require a
preinstalled VirtIO boot-storage driver before AV provisioning can run. BIOS
profiles retain the existing VirtIO storage behavior. This controls disk attachment,
not disk contents; it does not migrate or overwrite partitions. The chosen firmware
and controller must match the installed image; native acceptance boots the managed
copy-on-write image with the same inbox storage path used during OS installation.


### Coverage gate accounting

`make av-coverage` requires at least 90% production source lines and conditional/
switch branches separately for each bounded audio/copy, control codec, layout and
C video state module. The Zig gate instruments ReleaseSafe LLVM IR using source
function debug scopes, excludes compiler-generated panic guards and test bodies,
and records every matching CFG location for a source line. Unit calls to exported
functions use `@call(.never_inline, ...)` so constant inputs cannot fold validation
out of the executable test. Sparse two-GiB OS mappings exercise successful layout
initialization without touching pixel payloads. C state transitions use gcov's
source counters with all ownership states and concurrent stress. Reports persist
under `build/coverage/av`; no subsystem is averaged with another to hide a failure.
ASan/LSan/UBSan executes every native AV unit suite; Zig tests use its leak-checking
allocator for all owned dynamic buffers and deterministically release OS mappings.


### Importing an installed image's firmware metadata

An existing UEFI Windows image may depend on its companion NVRAM file for the
Windows Boot Manager device path. `av setup --firmware-vars ABSOLUTE_PATH` implies
UEFI and atomically imports that supplied metadata into the owned device variables
store under a quiescent device lease. The source is never changed, symlinks are
rejected, and the size/type checks for the OVMF template still run before startup.
This explicit import may replace the owned profile store; ordinary setup/restart
continues to preserve it. Firmware metadata is part of the supplied licensed OS
image, not a manually configured AV environment. No enrollment/key contents are
parsed, generated or printed by the application. The command cannot import while
any runtime owns the device, and closes its lease before boot/deployment.

The daemon takes a nonblocking exclusive `flock` on a KVMFR descriptor before
validating or initializing its mapping and retains that descriptor throughout the
VM lifecycle. Another managed profile fails with a runtime-ownership diagnostic,
even when all video slots are Free or only audio is active. Closing the descriptor
after workers/QEMU stop releases the lease; playback borrowers do not take this
exclusive daemon lease. External applications must honor the same advisory lease
before sharing a managed device.
