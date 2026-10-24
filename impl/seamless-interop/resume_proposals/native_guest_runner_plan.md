# Windows guest native acceptance runner plan

## Safest existing route

Use the device-management acceptance path as the lifecycle owner: `tests/acceptance/device_vm.py` exercises real managed guests and calls the public `waddle device show`, `waddle exec`, `waddle stop`, `waddle start`, and `waddle device remove` interfaces. The device-management spec says device creation uses `qemu-img create -f qcow2 -b ABSOLUTE_BASE -F qcow2 TARGET`, so the guest disk is a disposable overlay rather than a write to its base. Give the acceptance run isolated `XDG_CONFIG_HOME`, `XDG_STATE_HOME`, and `XDG_RUNTIME_DIR` rooted under a new `/tmp/wdvm-*` directory and set `WADDLE_DEVICE_VM_ACCEPTANCE=1`.

This is the best existing managed path, but it is not plug-and-play for `build/windows11_vm/windows.qcow2`: `device_vm.py` accepts two already booted fixture device names, it does not provision them, and the documented local Windows fixture needs a launcher adapter for UEFI pflash, an IDE boot disk, and the `waddle_export` tag. The normal daemon QEMU path attaches a virtio boot disk by default. No repository helper was found that safely creates this exact Windows 11 image overlay plus the required UEFI/TPM copies and starts it as a managed device. Do not use `build/windows11_vm/provision.py`; it creates a new blank disk, rewrites credentials/answer media/UEFI vars, and daemon-starts QEMU.

`tests/acceptance/vm_acceptance.sh` is the existing one-guest VSOCK/guest-exec fixture suite. It requires `WADDLE_VSOCK_CID`, `WADDLE_EXPORT_SOURCE`, and an installed guest listener; it validates stream I/O, exit status, arguments, cwd/environment, mapped-file access, cancellation, reconnects, and a terminal session. Its EXIT trap deletes `marker.txt`, `result.txt`, and `waddle_日本語` under the supplied export. Point it only at a newly created dedicated export directory. It does not start or stop QEMU, so the outer managed-device runner owns shutdown.

## Asset boundaries and cleanup

The existing VM directory contains `windows.qcow2` (37,632,999,424 bytes), `vars.fd` (540,672 bytes), `answer.iso` (380,928 bytes), TPM state under `tpm/`, and `credentials.json` (82 bytes, mode 0600). I inspected only the credentials filename and metadata; this preparation left its contents unread. Future automation should use the existing protected credential mechanism only when necessary and never emit its values into logs or reports. Keep the base QCOW2, UEFI variable store, TPM state, answer media, and credentials unchanged.

For a disposable direct overlay of that image, a future owner should first establish that the source VM is cleanly shut down and no QEMU/swtpm process holds the disk, vars, or TPM state. Create a new QCOW2 with the current base as an absolute backing path in a private temporary directory. Use a private copy of the matching UEFI vars and private TPM state for that run; never point QEMU at the originals for a write-capable pflash/TPM attachment. Preserve the overlay and logs on a failed shutdown for diagnosis. Remove only the temporary overlay/state after QEMU and swtpm are confirmed exited and the overlay is no longer open.

Prefer `waddle stop <device> --timeout ... --json` and wait for the daemon's supervisor/lease cleanup. `src/daemon/daemon_qemu.c:qemu_shutdown_graceful` uses QMP `system_powerdown`; daemon state waits for QEMU and owned mount processes and preserves resources if shutdown fails. Do not kill by stale PID or unlink a disk under a live VM. `build/windows11_vm/qmp.py` currently supports only `status`, `key`, and `screenshot`; it does not provide shutdown. `tests/acceptance/device_vm.py` performs stop/remove during its success path but does not wrap the entire scenario in a `finally`, so an outer harness should have an independent best-effort managed `stop --all` and retain evidence if cleanup fails.

## Guest prerequisites and remaining unknowns

The device acceptance doc says the Windows fixture has a SYSTEM-started VSOCK listener at `C:\\waddle\\waddle-guest-exec.exe`; `device_vm.py` relies on it and an `X:` export. This establishes a documented fixture expectation, not that the currently installed Windows 11 image has it. Verify the target guest's VSOCK PCI device/driver, service/listener state, CID and port before relying on `exec`; the daemon's QEMU builder attaches `vhost-vsock-pci`, but the current direct `provision.py` command does not.

The daemon QEMU builder attaches `ivshmem-plain` only when its AV shared-memory mode is enabled. No inspected acceptance helper or Windows loader log proves that the installed image has the required IVSHMEM driver/device binding or that a mapping is exposed to the guest. Verify PnP device status and driver/service state in a disposable guest before attempting adapter mapping. This preparation performed no driver installation or ACL mutation; Vulkan ICD loader evidence does not prove these prerequisites.

Desktop state is also unverified. Existing screen captures are historical evidence only; the VSOCK exec fixture does not establish an interactive logged-in desktop or GUI session. Before desktop-dependent adapter tests, query the disposable guest's active console/session and confirm the required process/window station. No VM was started, stopped, queried, or modified for this preparation.

## Exact repository references

- `tests/acceptance/device_vm.py` — isolated-XDG, two-device managed acceptance; guest-exec and cleanup success path.
- `tests/acceptance/vm_acceptance.sh` — real VSOCK guest-exec and export suite; destructive cleanup trap is scoped to the supplied export.
- `impl/device-management/IMPL_DESC.md` §4.2–4.3 and acceptance notes — QCOW2 overlay creation, daemon lifecycle, Windows fixture launcher caveat, and ACL restoration expectations.
- `src/daemon/daemon_qemu.c` — managed QEMU VSOCK and optional AV IVSHMEM argument construction; graceful QMP helper.
- `src/daemon/daemon_state.c` — supervised startup/stop ordering and retained resources on failed shutdown.
- `build/windows11_vm/provision.py` — reference only; unsafe for reuse because it creates/replaces assets and launches a VM.
- `build/windows11_vm/qmp.py` — status/key/screenshot helper only; no shutdown command.

## Preparation performed

Read helper source and implementation documentation; enumerated VM asset filenames and metadata while excluding credential contents. No QEMU, swtpm, guest command, ACL, device install, Git operation, or Claude path was touched.
