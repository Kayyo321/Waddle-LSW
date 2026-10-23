# Recoverable checkpoint — 2026-10-23

Execution stopped at the user's request after completing the current storage
slice. All three Luna workers have handed ownership back and are stopped.
No automation or background feature work was scheduled.

## State to resume

- Branch: `feat/seamless-interop`; target PR base branch: `origin`.
- Last accepted implementation: `91afd85`. The containing checkpoint commit
  adds evidence/contracts only and earns no additional progress.
- Tracker: **12.85%**, #5 40%, #8 50%, #9 10%, #33 15%; #1/#4 Done,
  #2/#3 still 60%, all other implementation/native gates remain open.
- README is unchanged. No PR or merge has been attempted; the 100% merge gate
  remains in force. Existing completion_plan.md remains the overall plan.

Accepted commits since the plan: config `87dc328`, root mapping `d9def23`,
layout snapshots `2baf303`, heap reservations `e368647`, manifest parsing
`d942bb0`, config formatting `9a6f367`, CLI settings `e6e40fa`, Windows evidence
and CI `bbccef0`, C boundary/contracts `b0d6e96`, QEMU argv `9a6b429`, persistence
`91afd85`. Contract and evidence commits do not earn implementation credit.

## Checks completed

Host `make test` passed, including 24 transport scenarios, shutdown/stress,
config/QEMU/fs/device/server/client fixtures, CLI settings and persistence,
360 storage crash/failure boundaries, path options and the mock filesystem demo.
The filesystem demo uses a mock guest; it is not Windows VirtIO-FS acceptance.
`make -B av-test`, `make -B interop-test` and `make interop-sanitizers` passed.
Focused QEMU/QMP and CLI persistence C consumers passed ASan/LSan/UBSan with
leak detection enabled; Zig objects are ReleaseSafe, with testing-allocator
failure sweeps in their native suites. A full clean-based host sanitizer run
has not been performed in this checkout.

Preserved commands, source/log hashes and raw outputs:
[evidence/checkpoint_host_receipt.json](evidence/checkpoint_host_receipt.json).
Pure production module coverage already accepted: layout 98.44% branch /
98.57% line, heap 97.92% / 100%, manifest 92.75% / 100%; codec 97.23% / 96.65%.

Layout (8), heap (17 including layout) and manifest (12) unit executables also
passed in the actual Windows 11 build 26300 VM after guest SHA256 verification:
[evidence/foundation_native_receipt.json](evidence/foundation_native_receipt.json).
Their temporary guest directory was removed; the VM was preserved. These are
CPU unit results, not capture/input/UIAccess/audio or session qualification.
The C layout declarations' size/offset assertions compile on Linux and the
x86_64 Windows target. The first Zig Windows `-fsyntax-only` invocation returned
FileNotFound; explicit absolute-include object compilation succeeded. This
checks declarations only, not the missing C wrapper implementation.

## Saved unfinished work

The layout C wrapper draft is preserved as
[checkpoint/layout_abi_draft.patch](checkpoint/layout_abi_draft.patch), outside
the active source/build tree. It has **not been compiled, tested or accepted**.
Its C fixture is missing. `git apply --check` passed against this checkpoint.
To resume it, inspect the patch and use `git apply` with that path. Lead review
must verify pointer-range overflow/overlap, alignment and transactional outputs,
then add C ABI tests and >=90% per-module coverage before awarding its 5% of #5.
No heap C ABI, native atomic adapter, slot pin/runtime or mapping initialization
implementation is implied by the header or patch.

The checkpoint tightens the shared header contract: private header bytes and
output cannot overlap the actual mapping range; overflow/overlap is rejected
before reads/writes. Descriptor native adapters still own atomic-safe snapshot
copying and live Ready/Consuming transitions. No wire/layout version changed.

## Next bounded work

1. Finish the saved C wrappers and C fixtures, then native Linux/Windows atomic
   adapters with lock-free assertions and release/acquire stress. #5's remaining
   allocation is C ABI 5%, native atomics 5%, slot pins/growth/runtime 25%,
   integrated qualification 25%. Keep imported pure-unit coverage separate.
2. Fix QEMU source-build feature selection before calling #8 complete.
   Pinned source is QEMU v9.2.4. Existing generated config disables
   CONFIG_AUDIO_PIPEWIRE, CONFIG_SLIRP and CONFIG_USB_LIBUSB. Append explicit
   `--enable-pipewire --enable-slirp --enable-libusb --disable-download`, add a
   configuration signature/stamp that invalidates stale settings, and assert
   all three generated feature macros. On this host PipeWire development
   support 1.4.2 exists; pkg-config cannot find slirp or libusb-1.0. Debian
   development prerequisites are libslirp-dev and libusb-1.0-0-dev. No packages
   were installed for this assessment. Verify the exact pinned source build.
   GNUmakefile currently copies system QEMU 10.0.13 first; a system binary
   smoke cannot prove the pinned build. Build-feature credit remains zero.
3. Complete all-mount daemon lifecycle/QEMU wiring and guest tag/drive binding
   for #9, plus canonical/symlink translation, rollback/restart/reaping tests.
   Current daemon/QEMU API still accepts one VirtIO-FS socket.
4. Continue the #33 durable journal/artifact ownership milestone, then the
   host/guest session/deploy lanes in completion_plan.md. No journal or session
   runtime exists merely because manifest parsing succeeds.

The named QEMU argv now targets `waddle-discard.<device>` and omits in.name for
the default microphone source. It does **not** create a discard sink. #23 must
implement and measure sink lifetime/routing and prevent duplicate legacy/per-app
audio. Session-manager behavior for an unresolved target is unproven. CUPS
forwarding only accepts an actual socket inode; stale sockets/service races can
still fail startup and require normal start rollback. Neither audio routing nor
Windows printing is qualified by argv tests.

## Runner and workspace preservation

The Linux host exposes KVM, DRM, vhost-vsock, a Wayland session and PipeWire when
using host access. The default sandbox hides devices and rejects some socket/
LSan operations; do not interpret those restrictions as absent hardware.

The existing QEMU process is named waddle_windows11_test. At checkpoint it uses
`build/windows11_vm/windows.qcow2` (about 37 GB), OVMF and TPM state. It remains
running. **Do not run make clean or make test-sanitizers in this checkout**:
the clean target removes build/, including the live VM disk. Use an isolated
qualification checkout for clean-based runs. Never use the live writable disk
as a backing image for a new VM. Verify a cold immutable base before making an
isolated acceptance device. Existing dev1/dev2 registrations have invalid
configs/placeholder disks; they were neither repaired nor overwritten.

WinRM on the existing VM works through its loopback forwarding; consume the
existing local credential binding through the SDK without printing secrets.
The bounded readiness probe found an installed but stopped IVSHMEM driver,
no Viosock/VirtIO-FS/Looking Glass drivers, no standard Waddle installation and
no standard Windows SDK Include tree. Re-probe before provisioning; no driver,
policy, user-input or VM lifecycle changes were made during these checks.
Native M0 requires an actual pipeline plus the specified compositor/audio
observations; GNOME/KDE/Sway and peripherals remain unqualified.

The lead owns Git/shared build/ABI/docs and the hardware runner. Workers have
released all source ownership. On resume, reuse Luna workers with fresh bounded
work orders; do not infer permission for concurrent shared builds or VM changes.
