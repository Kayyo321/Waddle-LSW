# Implementation Description: High-Performance Audio/Video Passthrough for Gaming

## 1. Title & High-Level Scope

This feature presents the visible top-level windows of one selected Windows process
as native Linux Wayland toplevels and plays that process tree's audio through
PipeWire. Windows Graphics Capture (WGC) retains the window's native decorations,
BGRA alpha and content through occlusion; explicit DXGI Desktop Duplication fallback
captures only a visible monitor crop. Setup provisions managed QEMU AV devices,
shared-memory resources, host access and guest AV driver/adapter deployment through
the existing execution bridge. It preserves the caller's licensed OS disk.

The performance acceptance targets remain below 7 ms video latency at actual
144-Hz operation and below 10 ms audio latency. They are requirements, not claims
established by compilation or by the current native acceptance evidence. General
input forwarding, clipboard integration, arbitrary host toplevel positioning,
GPU virtualization implementation, Windows installation/licensing and full desktop
capture are outside this AV implementation. A silent emulated render endpoint is
in scope; a custom virtual audio driver is not required. Rootless capture does not
provide host compositor decorations or synthesize GPU hardware capabilities.

## 2. Architecture & Inter-Component Interactions

```text
managed Linux CLI -> daemon -> QEMU (KVM, IVSHMEM, VSOCK, silent HDA)
        |                       |            |
        | existing execution    |            | two-GiB shared memory
        | bridge deployment     |            | video slots + PCM ring
        v                       v            v
Windows AV agent <---- VSOCK AV port 5001 ----> Linux AV host
  WinEvent/WGC/               lifecycle,          Wayland + DMA-BUF/wl_shm
  WASAPI process loopback     geometry, frames    PipeWire PCM callbacks
```

C owns OS handles, sockets, native threads and process lifetimes. Zig validates
bounded control serialization, numeric commands, mapping layouts and PCM/pixel
copies. C++ is confined to the first-party Windows SDK WinRT DLL. Capture uses
GPU-to-staging and CPU-to-IVSHMEM copies; DMA-BUF avoids a separate host pixel
copy when the compositor accepts the actual KVMFR export. This is not zero-copy
from the Windows GPU to the compositor. Audio is fixed stereo 48-kHz S16LE.
The AV control channel carries create/destroy/geometry/frame/close only; general
pointer and keyboard messages are not implemented here. PulseAudio is not a
second playback implementation. The original CLI protocol/port stays separate.

## 3. Data Structures, Protocols & Memory Layouts

The exact source ABI is in include/waddle/av_memory.h. Both C structs are 64-byte
aligned and exactly 64 bytes, with lock-free C11 uint32 atomics. The target is
x86-64 little endian on both peers. No pointer is serialized.

| Video header offset | Type | Field |
|---|---|---|
| 0 | atomic uint32 | slot_state: Free=0, Writing=1, Ready=2, Consuming=3 |
| 4 | uint32 | immutable local buffer_index, 0..2 |
| 8 | uint64 | nonzero frame_sequence |
| 16 | uint64 | guest capture timestamp_ns; not host clock time |
| 24 / 28 / 32 / 36 | uint32 each | width / height / stride / BGRA format |
| 40..63 | byte[24] | zero reserved padding |

| Audio header offset | Type | Field |
|---|---|---|
| 0 / 4 | atomic uint32 each | write_head / read_head, modular frame cursors |
| 8 / 12 / 16 / 20 | uint32 each | rate=48000 / channels=2 / format=1 / capacity=4096 |
| 24 | atomic uint32 | cumulative producer overrun_frames |
| 28..63 | byte[36] | zero reserved padding |

Mapping identity, video pool offsets, byte capacities and all 328 control wire
bytes are specified in section 8 below. Sixteen pools of three buffers are fixed;
no dynamic shared-memory allocator runs during capture. Zig bounds checks every
row and payload offset. The control decoder writes output only after validation.

## 4. Step-by-Step Execution Sequence

1. Managed setup resolves the selected device, obtains its AV command lease and
   validates/updates AV settings only when configuration changes are quiescent.
   It prepares KVM/VSOCK access and optionally builds/loads compatible pinned KVMFR.
2. Daemon startup takes the device/runtime memory leases, prepares persistent
   firmware and initializes the shared-memory ABI before starting QEMU. Existing
   occupied slots or a different ABI fail before initialization.
3. The existing bridge executes native guest setup from an immutable deployment.
   Signed IVSHMEM installation and the guest probe must succeed. Native host
   mapping, Wayland and ready PipeWire probes must then succeed before publishing
   the deployment selector. Reboot-required driver status triggers one graceful managed restart and a fresh
   guest probe; host readiness must also pass before selection.
4. Run validates a live selected process object, starts its guest agent via the
   existing bridge and waits for the exact listener-ready line. The guest listens;
   the native Linux host connects to guest CID and AV port 5001.
5. The guest maps IVSHMEM, starts an event-signalled WASAPI audio worker, registers
   WinEvent hooks and enumerates visible windows on its message/capture thread.
   Deferred window admissions retry every 250 ms when a complete pool is free.
6. Each successful capture claims a Free slot, copies bounded BGRA rows, publishes
   Ready with release ordering and sends a bounded frame notification. The host
   claims Consuming, validates metadata and waits for initial surface configuration.
   It asynchronously imports KVMFR DMA-BUF or uses wl_shm, attaches, damages and
   commits stable pixels. Only compositor release permits reuse after attachment.
7. The PipeWire callback reads the SPSC PCM ring directly, trims excess backlog,
   fills underrun silence and queues its output buffer; it allocates nothing.
8. Guest geometry updates retain metadata and apply host fullscreen/minimize
   requests where xdg-shell permits them. Host configure requests resize/restore
   the guest HWND; close translates to WM_CLOSE. Host placement remains owned by
   the compositor, so moving a host window does not transmit a global position.
9. Process exit, disconnect, cancellation or failure stops capture and joins audio.
   Managed CLI cancels/reaps only its owned agent/playback children. Guest original
   styles are restored; host retires surfaces and drains compositor leases before
   native clients and mappings are released. Persistent OS disks/firmware survive.

## 5. Concurrency, Threading & Synchronization

The guest message/capture thread is an MTA and owns hooks, WGC sessions, DXGI
handles, video producer state and VSOCK control. An owned high-resolution 1-ms
timer drives capture polling. MMCSS is requested for the capture/audio threads.
The separate guest audio worker initializes its own COM apartment and waits on
stop and WASAPI events. Activation callback state owns its asynchronous lifetime
through COM reference counts; an activation timeout cannot free a live callback.

The host event thread owns Wayland objects, video consumption and VSOCK state.
The PipeWire worker is the only PCM consumer. Acquire/release transitions transfer
slot ownership; audio cursors order PCM publication/consumption. There is no
cross-VM mutex and no forced reuse of a compositor-held slot. Per-device command
leases and daemon-held memory leases cover idle/audio-only sessions as well as
video. The mapping outlives every worker, pending import and retained buffer.

## 6. Error Handling & Failure Modes

A failed or closed VSOCK session tears down its owned clients; it does not run an
unbounded automatic reconnect loop. The managed startup path retries readiness
within its documented deadlines and preserves an already running VM on bridge
timeout. Cancellation never terminates the selected pre-existing application.
Audio overrun drops new incoming frames, increments its counter and preserves
unread bytes. Only the consumer trims backlog. Underrun emits silence. WASAPI
activation/render-endpoint failure is reported independently of video capability.

Capture device/output loss releases that capture session and retries initialization
on the next tick. Invalid dimensions/capacity fail bounded copies. An unsent frame
notification cancels only producer-owned Writing/Ready state. DMA-BUF rejection
falls back asynchronously to wl_shm for that window. Retired windows retain pending
callback context until import/release completes. Teardown is bounded; unknown
outstanding compositor leases remain Consuming after display failure/timeout.

Setup never unloads an active module or detaches an unspecified GPU. Existing
non-VFIO GPU bindings fail without modification. Both acceptance-host GPUs have
active desktop users, so neither can be reassigned for performance verification.
Automatic preparation/recovery of a safely eligible explicit GPU is still
unfinished; a pre-bound GPU requirement is not recorded as fully provisioned.
Secure Boot restrictions, missing running-kernel headers, guest logon/bridge
bootstrap, driver reboot-required status and incompatible live modules produce
explicit failures. Setup does not bypass signatures or overwrite the licensed OS.

## 7. Verification & Testing Criteria

`make av-test` covers production video ownership, SPSC overflow/wrap/trimming,
bounded BGRA copies, malformed control fields, sparse full layouts, resource
leases, host setup reuse and xdg-shell state-only configure events. Packaging tests
verify all required payloads/checksums and preservation of prior archives on every
missing-input failure. `make av-sanitizers` runs native tests with ASan/LSan/UBSan
and Zig's testing allocator; zero leaked bytes are required. `make av-coverage`
gates each protocol/memory module's production line and branch coverage separately
at 90%; current production lines are 100% in all four measured modules, with
branch coverage 94.44% audio/pixels, 95.83% codec and 100% layout/video.

Native gates require actual Windows WGC occlusion pixels, process-tree loopback
PCM, KVMFR DMA-BUF aliasing, compositor attachment/release, PipeWire callbacks and
managed startup/cancellation. Hardware-independent mocks establish neither native
fidelity nor latency. Full performance acceptance additionally requires real
144-Hz operation and timestamped video presentation/audio delivery. A backlog
counter, Wayland commit, WGC-to-CPU age or guest-only FPS diagnostic cannot substitute
for that end-to-end evidence. The native benchmark currently finds a 64-Hz Windows
mode and invalid future WGC timestamps; task #10.5 now records diagnostic
framework completion under the revised request, while performance remains
unverified. Every
applicable GitHub check must pass on the final source commit before completion.

## 8. Binding implementation refinements

The source ABI and tables above agree; no C++ atomic layout is used. The C ABI is
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
Managed setup builds/loads the bundled patched module when requested; installing
base running-kernel headers and satisfying signature policy remain host prerequisites.
Host uses KVMFR_DMABUF_GETSIZE/CREATE; guest uses the signed Red Hat IVSHMEM
interface GUID and map/unmap ioctls. No nested vendor sources are used.

### DMA-BUF export ownership

The host binds linux-dmabuf v3 only if present and requires advertised linear
ARGB8888. Each slot exports its page-aligned payload offset/capacity using the
pinned KVMFR ioctl ABI. GETSIZE bounds-checks the region before CREATE. Returned
CLOEXEC FD is closed immediately after the protocol duplicates it. A regular
file fails GETSIZE and selects wl_shm. The compositor owns the buffer import
until wl_buffer.release. Asynchronous DMA-BUF rejection selects wl_shm for the same stable pixels and
disables further DMA-BUF attempts for that window; callback ownership is retained.

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

### Immutable guest deployment versions

Windows can retain an executable image section for a shared-filesystem pathname
after its file bytes are atomically replaced. Each deployment therefore creates a
private `av/bundle_XXXXXX` directory containing the complete agent/DLL pair. The
signed driver SYS retains the external INF's required `IVSHMEM.sys` basename.
Only successful guest setup/probe followed by a real host mapping, Wayland and
connected PipeWire probe atomically publishes a private 13-byte
`current_bundle` selector. Run/probe validate its ownership, type, length and
alphanumeric suffix before constructing the executable path. They never execute a
partially published bundle or reinterpret selector contents as a path or command.

### Asynchronous compositor DMA-BUF import

The host prefers explicit linear ARGB modifiers and can also attempt an advertised
implicit ARGB modifier. It uses `zwp_linux_buffer_params_v1.create`, never
`create_immed`, because a valid export can still be rejected by the compositor's
GPU. The slot remains Consuming while import is pending. A created callback owns
the resulting buffer, attaches stable pixels and retains the slot until release.
A failed callback creates a wl_shm buffer over the same region and disables further
DMA-BUF attempts for that window. Retired windows remain allocated while callbacks
or buffer releases are outstanding; pending imports that complete after retirement
are destroyed without attachment and release their slots. Bounded teardown keeps
unknown outstanding leases Consuming rather than making them reusable prematurely.
Native acceptance on the NVIDIA compositor confirms an actual implicit ARGB
KVMFR import, surface attachment and subsequent buffer release.

### Host audio library teardown

The standalone AV host owns one PipeWire instance per process and has no other
D-Bus consumers. The RTKit module closes its connections but the system D-Bus
library retains process-global caches. The audio owner retains an optional
`RTLD_NOLOAD` library reference after context creation; after the worker, stream,
context and PipeWire modules stop, it invokes the library's `dbus_shutdown` and
closes that final reference. Missing D-Bus/RTKit is allowed and requires no cleanup.
This lifecycle is confined to the standalone AV process. The live platform gate
passes ASan/LSan/UBSan with zero reported bytes leaked after this cleanup; no leak
suppression or disabled sanitizer check is used.

### Native capture timing diagnostic

The Windows fixture's optional `--benchmark` paints a changing pixel while pumping
real window messages, polls the native WGC DLL with a one-millisecond owned
high-resolution timer, and measures three seconds using QueryPerformanceCounter.
It verifies deterministic center pixels, counts distinct WGC timestamps and reports relative intervals. QPC measures only the duration of the guest throughput run. Absolute WGC-to-CPU age subtraction has been removed after historical native runs produced future timestamps. Newly painted pixels, rather than guest timestamps, establish occlusion freshness. Stack-owned statistics and the owned high-resolution timer are released after each run. This is throughput evidence only; the host-clock diagnostic below measures compositor commit RTT.

The historical program-created guest reports 1920x1080 at 64 Hz despite the QEMU 144-Hz VGA request. This is an observed Basic Display Driver limitation in that configuration, not a universal guarantee about every Windows/QEMU combination. A requested EDID alone cannot establish actual selected mode or 144-Hz capture throughput. Installing/configuring a real indirect display is a manual prerequisite for the software path.

Setup reloads the committed profile after configuration and probes the exact
configured mapping after guest deployment. A host readiness failure returns nonzero
and preserves the previous current_bundle selector. An unused immutable deployment
directory may remain for diagnosis; it is never selected for subsequent run/probe.
Both setup and explicit probe invoke the same native host gate, which stops
PipeWire and Wayland clients before unmapping on every error or success path.

### Complete distributable AV bundle

`make all` includes `av-distribution`, which requires host CLI/daemon/playback/helper,
execution and AV guest binaries, the native Windows SDK-built WinRT DLL, the
verified signed IVSHMEM package, and patched pinned module sources. Missing inputs
fail before publishing an archive; Linux cannot fabricate the SDK projection DLL.
Native CI produces that DLL on Windows and automatically transfers the same-run
artifact to the Linux package job. Users download one `waddle_av.tar.gz`, extract
it into a private user-owned writable directory and keep the sibling executable
and vendor/av layout intact. No user assembly of different CI artifacts is needed.

The archive contains GPL notices, exact application and original Looking Glass
source archives, patched module sources and a per-file SHA-256 manifest. Provenance
records the application commit and verifies the Looking Glass checkout matches
the immutable gitlink. Base-system QEMU, virtiofsd, Wayland, PipeWire, libc, sudo,
acl, kmod, OVMF and running-kernel development headers remain OS packages. They
are not loosely vendored runtime dependencies. The licensed Windows image and
existing execution bridge belong to the caller's managed device. Kernel modules
are built for the actual target kernel, not the CI kernel. Packaging uses a private
staging directory removed on all exits; a failed package preserves the previous
archive, and only a successful complete archive atomically replaces the output.
The bundled source is git HEAD; rebuild packaging after committing source edits.

The native Linux verification job depends on the same workflow's Windows job,
downloads its artifact by name without cross-run selection, assembles the archive
and checks every checksum after extraction before uploading `waddle-av-complete`.
Both Linux workflows run the same Debian trixie container on ubuntu-22.04 runners.
This runner choice avoids observed ubuntu-24.04 acquisition failures (the failed
check's annotation reports no hosted runner acquired, with no test steps executed);
it does not change compiler, sanitizer, coverage thresholds or container packages.
The AV suite also exercises the bounded managed-command parser and packaging
failure tests. Final-commit checks remain required regardless of older green runs.

Reusing a compatible already-loaded KVMFR also grants a specific-user read/write
ACL to the validated SUDO_UID. A root-openable descriptor alone does not establish
the invoking user's access. The helper preserves existing device ownership and
mode policy instead of chowning or resetting a live mapping; the exclusive daemon
lease still prevents concurrent profile initialization. Missing acl tools or invalid
invoking-user identity fail setup. No live module is unloaded for this operation.

Host xdg_toplevel configure callbacks preserve guest dimensions when the
compositor sends zero size (client chooses size), but still forward changed
fullscreen state. Positive pairs within the protocol's bounds replace dimensions.
Unchanged geometry/state emits no request; callback delivery failures become
session errors. Isolated callback tests verify zero-size enter/exit fullscreen,
unchanged states, valid resize, invalid size and failed delivery; the separate
native platform suite validates real compositor attachment/release.

### Managed driver reboot completion

Native PnPUtil statuses 3010/1641 remain normalized to guest status 3. The managed
deployment completion sequence handles 3 by requesting one graceful restart of
the exact selected device through the existing daemon client, waiting up to its
180-second deadline for bridge readiness, probing the same immutable guest bundle
again, then probing the host. No force flag, repeated driver install or restart
loop is used. Other guest failures return unchanged without any restart or probe.
An already-ready guest runs only the host probe. Every step's nonzero error stops
selection publication and preserves the prior selector. If OS logon/bridge startup
is unavailable after restart, setup returns failure while the daemon preserves
its owned running VM; it does not invent readiness or change OS credentials.
The allocation-free completion state machine has ordered step/failure tests under
native sanitizers. Real reboot-required driver-install acceptance is still pending.

Native distribution verification extracts the complete final-source archive into
a separate ignored private directory, checks its complete SHA-256 manifest and
executes setup/probe from the extracted CLI with its sibling binaries and vendor
layout. The actual acceptance profile passes both guest deployment readiness and
host mapping/Wayland/PipeWire readiness from this archive. This validates portable
package layout and deployment, not the still-missing high-refresh latency targets
or a real reboot-required install transition. Repository-wide sanitizer regression,
CLI coverage and the rerun of all 1000 storage stress cycles also pass locally.

### Pixel-based occlusion freshness

The native fixture first verifies the original deterministic center color, then
places a topmost tool window over the target and repaints the covered target with
a different deterministic color. Capture must deliver the new color before its
five-second deadline. Only the known old color is ignored while draining queued
frames; unexpected center pixels fail. This prevents future/rounded WGC timestamps
from allowing a pre-occlusion frame to count as fresh fidelity evidence. The
benchmark validates the new expected color in every accepted frame afterward.

The strengthened native test passes newly painted occluded content and captures
nonzero process-loopback PCM. Its later diagnostic run still reports a 64-Hz mode,
only 11 accepted unique frames over 3000.955 ms (3.67 FPS), and nine invalid future
WGC timestamps. Throughput varies substantially across these software-display
runs; neither the earlier 37.64 FPS nor this later result meets the acceptance
requirement. Small positive ages from the subset of valid timestamps are not a
passing latency result because the aggregate timing validation fails.

AV driver restart uses a distinct daemon request ID 0x000f, with the existing
eight-byte stop payload (force=0 and timeout=180), rather than the legacy stop
request's force flag. New daemons select the internal ACPI-only mode. If QEMU
remains live after the deadline, they return ETIMEDOUT, restore the previous
subsystem state and retain VM, filesystem workers, sockets and shared-memory
leases. Older daemons reject the unknown request without changing VM state.
The legacy CLI stop command retains its existing optional force fallback. A
native state test uses an actual owned child and sparse two-GiB mapping to verify
the new timeout path preserves both, then explicitly reaps its fixture child.

After the owned acceptance VM stops, the real KVMFR gate passes again under
ASan/LSan/UBSan: exact 2-GiB capacity, SEEK_END/reset, CLOEXEC export descriptor,
shared/exported pixel aliasing, out-of-range rejection and competing ownership
lease preservation. `make av-module` also builds the patched pinned sources
against the running kernel. The loaded compatible module remains intact; no
unload/reload is needed to claim this verification. This completes KVMFR build,
distribution and access provisioning (#10.2). Under the revised safe-rejection
policy, #10.3 completes read-only GPU eligibility checks; #10.5 completes the
diagnostic framework. Physical high-refresh and latency performance remain
external acceptance evidence.

### Configure-axis verification (2026-10-05)

Each zero xdg_toplevel configure dimension independently preserves the corresponding guest dimension. Nonzero valid axes replace that dimension; negative or oversized pairs preserve both. Fullscreen transitions still propagate with zero dimensions. The callback regression includes both mixed-zero cases. Fresh AV sanitizers and the real Wayland/PipeWire platform gate verify teardown without reported leaks.

### Safe GPU group capability boundary

`waddle-av-setup --probe-gpu PCI_BDF` and daemon AV preparation call the same read-only probe. Zig validates exactly twelve PCI address bytes (hex domain/bus, slot below 32, function below eight) before path construction. The probe resolves the device's IOMMU group and requires a direct child of the canonical sysfs group root. It enumerates that group's devices, requires the requested BDF to be present and each member to exist, and reads every driver symlink. Missing driver links on existing devices mean unbound; only vfio-pci or unbound members are eligible. All other drivers return EBUSY with the offending member and driver. Missing IOMMU support, corrupt links and I/O errors fail closed. Directory ownership ends at closedir on success and all errors; no allocation survives the call.

Eligibility is a snapshot, not a hardware lease or completed VFIO binding. An unbound member is safe to prepare manually but QEMU still needs a usable VFIO group; concurrent administrative changes may make launch fail. The program never writes driver_override/unbind/bind, stops a display manager, kills render clients or unloads GPU modules. Consequently no rollback writes are needed. Physical 144-Hz acceptance is blocked here: fresh probes of 0000:01:00.0 and 0000:0e:00.0 report nvidia and amdgpu respectively. Companion functions are also checked. Synthetic sysfs fixtures cover unbound/VFIO, active graphics/audio companions, invalid PCI paths, missing group membership and malformed driver/group links under ASan/LSan/UBSan.

### Host-clock color challenge diagnostic

The diagnostic uses the existing bounded AV VSOCK peer, not a second transport. `MsgDiagnosticFlash` is AV ID 6 in the unchanged 328-byte little-endian frame. `window_id` selects the fixture; `sequence` is a nonzero 24-bit `0xRRGGBB` token; flags bit 0 requests guest occlusion. Other metadata is unused. Zig rejects zero/oversized tokens and flags above one. Ordinary application windows never receive this message: the agent requires the selected target PID, exact `WaddleAvFixture` class and exact `Waddle AV latency fixture` title. It posts private WM_APP message 0x8025 only after those checks; a stale or different window fails the session.

Run `av_windows_test.exe --round-trip-fixture` in the interactive Windows desktop. It reports its PID, creates the dedicated target and a filtered tool window, and pumps for at most 120 seconds. Each accepted flash paints the requested color synchronously. Flag 1 moves the tool window topmost over the target before repaint, testing WGC capture of newly changed occluded content. The fixture owns/destroys both windows and every temporary GDI brush. It uses guest time only for its lifetime bound, never as a latency endpoint. Run `waddle av run PID --device NAME --latency` from the host after deploying this revision. The CLI starts the matching agent then its sibling host with --latency; a standalone host also accepts `waddle-av-host --latency CID SHARED_MEMORY_PATH` when the agent is already listening.

The host identifies the exact fixture title in Create, arms a single watch, records `Time_Start` from CLOCK_MONOTONIC immediately before queuing its control frame, then pumps normal peer/display traffic. Tokens start from a host-clock-derived seed and increment without repetition during the 32-sample run. A held SlotConsuming buffer is inspected with Zig bounds checks: dimensions 3..8192, stride at least width*4, rows within slot capacity, and all center 3x3 BGR bytes equal the token. Stale queued colors cannot acknowledge a different token. The watch remains armed until a match reaches the common attachment path, including asynchronous DMA-BUF success and shared-memory fallback.

After attaching and committing matching pixels, Wayland queues wl_display.sync on that same connection. Its done callback proves the compositor has processed the preceding surface commit; it destroys the callback and records `Time_End` with the same host clock. RTT = Time_End - Time_Start includes control queueing, guest rendering/capture, transport, import and compositor processing. It does not include physical scanout, presentation feedback or audio output. Calling it physical input-to-photon latency would be incorrect. Sixteen visible and sixteen guest-occluded samples print individual RTT and aggregate mean/max. Missing fixture, failed color transition, occlusion failure, import/display disconnect or missing acknowledgement fails after five seconds per challenge. Peer closure, signal cancellation, clock failure or incomplete sample count returns failure. No guest QPC/WGC timestamp participates in subtraction.

One event thread owns the watch, mapping references and callback. The mapping remains borrowed until Wayland teardown; the callback is destroyed on completion or before draining/freeing the client. Slot leases remain unchanged and compositor release still controls reuse. Guest throughput diagnostics retain timestamp uniqueness/relative ordering only, remove absolute capture-age calculations and ignore timestamp values when proving occlusion freshness. Linux wire/pixel/watch tests, real compositor matching-commit acknowledgement under sanitizers, and Windows cross-builds pass. No running acceptance VM was available for a full guest RTT run in this session. Task #10.5 records framework completion as requested, not measured 144-Hz or physical latency acceptance.

### Software 144-Hz display prerequisite and verification

Microsoft's [IddSampleDriver documentation](https://learn.microsoft.com/en-us/samples/microsoft/windows-driver-samples/indirect-display-driver-sample/) provides an IddCx indirect display sample. [VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver) is an IddSampleDriver-derived implementation exposing configurable resolutions/refresh rates. It is an external manual prerequisite, not redistributed or installed by Waddle; no new loose vendor files or submodules are introduced. Waddle's bundled signed IVSHMEM driver transports memory and does not create a Windows display.

1. In the licensed interactive Windows guest, obtain the signed Virtual Display Driver installer from its upstream releases and follow that release's installation/dependency instructions. Preserve Windows signature validation; Waddle does not enable test-signing or enroll certificates. Record the installed release, package hash and signature when gathering acceptance evidence.
2. In Virtual Driver Control (or that release's documented vdd_settings.xml controls), expose one 1920x1080 mode at 144 Hz. Restart the guest if the installer requires it. Exact installer/UI details are release-specific and remain the upstream installer's responsibility.
3. In Windows Settings > System > Display, select the virtual monitor and make it the primary display. Use Advanced display to select 1920x1080 at 144 Hz; do not rely on the virtual VGA EDID. Keep the capture target on that monitor, with the guest interactive desktop active and unlocked.
4. Run `waddle-guest-av.exe --probe-display` or `av_windows_test.exe --probe-display` in the same interactive session. The read-only Win32 current-primary query requires exactly 1920x1080 and nominal 144 Hz; mismatches return 1 with the observed mode and VDD prerequisite. This separate performance probe does not turn basic setup/readiness into a claim of 144-Hz performance.
5. Run the native `--benchmark` and then the host color RTT diagnostic. The selected mode can allow 144-Hz composition but does not guarantee it: WARP, VM CPU scheduling, capture copies and compositor behavior can still limit throughput. Record actual frames and host RTT; physical video <7 ms/audio <10 ms remain unverified without their respective presentation/audio endpoints.

The current physical GPUs are busy, and no VDD-equipped acceptance VM is running here. Thus physical 144-Hz capture and the manual VDD path remain unexecuted hardware/environment gates. Research, prerequisite instructions, mode verification and the host RTT framework are complete; their existence is not a passing native performance measurement.

### PnPUtil reboot classification verification

One shared allocation-free classifier maps native 3010 (restart required) and 1641 (restart initiated) to guest setup status 3; 0 and 259 (already-current package) permit the actual capability probe; all other statuses fail. Tests feed both native reboot codes through av_deploy_finish and assert restart, fresh guest probe, host probe in that order, once only. The managed restart uses daemon request 0x000f (DaemonMsgStopGracefulReq), preserving the VM/resources on ACPI timeout and refusing selector publication on failure. The already-implemented timeout/first-failure tests remain in place. Real Windows installation returning these codes has not been observed in this session and remains explicitly unverified.
