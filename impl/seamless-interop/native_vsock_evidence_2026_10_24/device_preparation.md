# Isolated Waddle guest-exec device preparation

## Prepared private profile

The profile is isolated under `/tmp/waddle_guestexec_device_20261024`; its
`XDG_CONFIG_HOME`, `XDG_STATE_HOME`, `XDG_RUNTIME_DIR`, and `HOME` point only
inside that directory. It does not modify the normal Waddle registry or
`dev1`/`dev2`.

Creation command run:

```sh
HOME=/tmp/waddle_guestexec_device_20261024/home \
XDG_CONFIG_HOME=/tmp/waddle_guestexec_device_20261024/config \
XDG_STATE_HOME=/tmp/waddle_guestexec_device_20261024/state \
XDG_RUNTIME_DIR=/tmp/waddle_guestexec_device_20261024/run \
./build/waddle device init seamless-guest-exec \
  --base-disk /home/dev/Waddle-LSW/build/windows11_vm/windows.qcow2 \
  --memory-mb 4096 --vcpus 2 --json
```

The generated overlay is
`/tmp/waddle_guestexec_device_20261024/state/waddle/devices/seamless-guest-exec/disk.qcow2`.
`qemu-img info --output=json` reports it as QCOW2 with backing file
`/home/dev/Waddle-LSW/build/windows11_vm/windows.qcow2`; it occupies 200704
bytes at preparation time. The base's device/inode/size/mtime/ctime/mode were
identical before and after initialization. Its exact snapshot is in
`base-stat-before.txt` and `base-stat-after.txt`.

The isolated generated profile initially has CID 3 and port 5242 because its
private registry is empty. CID 3 is only a local planning value; it has not
been checked against the normal device registry or running guests and must not
be used to boot this profile. The default host export points only to the empty
private `$HOME` directory. I disabled user networking, CUPS forwarding, USB,
audio, clipboard, guest menus and the Waddle executable integration in the
private profile. No QEMU or daemon was started.

Dry-run check also succeeded before creation using the same `device init`
arguments plus `--dry-run`; the isolated XDG directories remained empty.
The actual profile and the `device show` output are retained in this directory.

## Guest-exec path status

No usable existing guest-exec channel was found. `src/guest/guest_listener.c`
is a standalone VSOCK listener, and `tests/windows/windows_guest.c` starts it
as a child for its native test; those sources do not install it as a Windows
service or startup task. The runner's `guestfwd` endpoint is explicitly a
one-way guest-to-host result channel and does not expose host-to-guest command
execution. The reviewed Windows runner helpers use WinRM, whose authentication
material remains out of scope and was never read.

The repository QEMU command for managed devices adds
`vhost-vsock-pci,guest-cid=<cid>`. QEMU 9.2.4 advertises `vhostfd`, but that
property still requires an already-open vhost-vsock device descriptor. This
host has no `/dev/vhost-vsock` node, `/sys/class/misc/vhost-vsock/dev` is
`10:241`, and `CapEff` is zero. Therefore a private node cannot be created by
this process. Successful `AF_VSOCK` socket-family creation is not evidence of
a guest route. A managed-device start is blocked until authorized device-node
access is supplied by the host/device manager; no node, ACL, module, or device
policy was changed.

## Exact next actions if host access is provided

1. Obtain normal managed access to `/dev/vhost-vsock` (character device
   `10:241`) without changing this profile's base disk, firmware vars, TPM
   state, or global device policy from the runner.
2. Inspect the normal Waddle registry and live VSOCK/VM inventory read-only,
   then allocate a noncolliding CID through that registry. Do not reuse the
   isolated CID 3 by assumption.
3. Establish that the Windows image has a pre-logon listener. If it does not,
   a separate guest setup action is required to install/start
   `waddle-guest-exec.exe`; the source tree alone does not establish startup
   registration.
4. Only after those gates, start the isolated profile with the four XDG/HOME
   variables above and a bounded deadline. The candidate command is:

```sh
HOME=/tmp/waddle_guestexec_device_20261024/home \
XDG_CONFIG_HOME=/tmp/waddle_guestexec_device_20261024/config \
XDG_STATE_HOME=/tmp/waddle_guestexec_device_20261024/state \
XDG_RUNTIME_DIR=/tmp/waddle_guestexec_device_20261024/run \
./build/waddle start seamless-guest-exec --wait --timeout 120 --json
```

That command was not run. Without the verified host vhost-vsock endpoint and
CID, it is expected to fail at QEMU device realization and should not be used
as a probe.
