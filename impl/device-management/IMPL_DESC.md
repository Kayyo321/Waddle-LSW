# Implementation Description: Device Management & Multi-Device CLI Lifecycle

## 1. Title & High-Level Scope

### 1.1 Purpose
This specification formalizes the **Device Management Subsystem** for Waddle-LSW. It provides the command-line interface (CLI) and background supervisor daemon with the ability to initialize named device profiles (`./build/waddle init <device-name>` and `./build/waddle --init <device-name>`), manage multiple isolated virtual subsystem environments, and enforce strict single vs. multi-device targeting constraints across subsystem lifecycle commands (`start`, `stop`, `restart`, `status`, `kill`, `logs`, `exec`, and the zero-flag interactive terminal launcher).

### 1.2 In-Scope
1. **Device Initialization (`waddle init` / `waddle --init`)**:
   - Validation of alphanumeric device names (`[a-zA-Z0-9_-]+`, max 64 chars).
   - Creation of per-device storage directory in `~/.local/state/waddle/devices/<device-name>/`.
   - Creation of per-device INI configuration in `~/.config/waddle/devices/<device-name>.ini`.
   - Automated creation of copy-on-write QCOW2 overlay disks backed by the system base image (`/home/dev/.local/share/waddle-vm-validation/windows.qcow2` or fallback candidate locations) using `qemu-img create -f qcow2 -b <base> -F qcow2 <target>`.
   - Dynamic CID allocation ensuring non-colliding `vsock_cid` assignment (starting at CID 3) across registered devices.
2. **Device Discovery & Registry**:
   - Enumeration of registered devices by scanning `~/.config/waddle/devices/` and `~/.local/state/waddle/devices/`.
   - Querying single vs. multiple device counts.
3. **Targeting Constraint Enforcement**:
   - Single-device mode: When exactly one device exists in the registry, lifecycle operations (`start`, `stop`, `restart`, `status`, zero-flag interactive terminal) automatically target that device without requiring the user to specify it.
   - Multi-device mode: When more than one device exists in the registry, invoking `start` or `stop` without explicitly specifying a device (`waddle start <device-name>` or `waddle start --device <device-name>`) is rejected with a clear diagnostic listing all available devices and usage instructions.
   - Zero-flag default terminal invocation (`./build/waddle`): If exactly one device exists, it automatically starts that device (if stopped) and launches ConPTY interactive PowerShell. If multiple devices exist, it prompts the user to select the device or pass the device name as an argument.
4. **Daemon & Hypervisor Startup Fix**:
   - Rectify the VirtIO-FS (`virtiofsd`) readiness probing in `src/daemon/daemon_fs.c` where an active TCP/UNIX `connect()` caused `virtiofsd` (a 1:1 vhost-user server) to conclude the hypervisor disconnected and shut down prematurely.
   - Replace active socket connection probing with non-destructive filesystem socket inode presence verification (`stat()` with `S_ISSOCK`) and startup stabilization delay.
   - Ensure QEMU receives valid socket and disk paths so it launches cleanly without exiting with exit code 110.

### 1.3 Out-of-Scope
- Cloud image downloading (local images and local copy-on-write overlays only).
- GUI device manager (CLI-first architecture).

---

## 2. Architecture & Inter-Component Interactions

```
+-------------------------------------------------------------------------------+
|                                 USER TERMINAL                                 |
|                                                                               |
|   waddle init "win11"           waddle start [name]        waddle [name]      |
+---------------------------------------+---------------------------------------+
                                        |
+---------------------------------------v---------------------------------------+
|                             HOST CLI (src/cli/host.c)                         |
|  - Parse commands and options (--init, init, --device, start, stop, exec)     |
|  - Consult Device Registry (daemon_device_list)                               |
|  - If 1 device: default to it; If > 1 device: require explicit name           |
+---------------------------------------+---------------------------------------+
                                        |
        +-------------------------------+-------------------------------+
        |                                                               |
+-------v-------------------------------+       +-----------------------v-------+
|  DEVICE REGISTRY (daemon_device.c)    |       | DAEMON CLIENT (daemon_client) |
|  - ~/.config/waddle/devices/*.ini     |       | - Connect to device socket    |
|  - ~/.local/state/waddle/devices/     |       |   /run/user/$UID/waddle/      |
|  - qemu-img CoW overlay generation    |       |   <device>/daemon.sock        |
|  - vsock_cid auto-allocation          |       | - Issue start / stop / status |
+---------------------------------------+       +-------------------------------+
```

### 2.1 File System Hierarchy
- **Device Configurations**: `~/.config/waddle/devices/<device-name>.ini`
- **Device Persistent State & Disks**: `~/.local/state/waddle/devices/<device-name>/`
  - `disk.qcow2`: Copy-on-write overlay disk referencing base Windows image.
- **Runtime Sockets & Locks**:
  - If single/default device: `/run/user/<uid>/waddle/daemon.sock` and `/run/user/<uid>/waddle/<device-name>/`
  - Per-device: `/run/user/<uid>/waddle/<device-name>/daemon.sock`, `/run/user/<uid>/waddle/<device-name>/qmp.sock`, `/run/user/<uid>/waddle/<device-name>/virtiofsd.sock`

---

## 3. Data Structures & Constants

### 3.1 Device Information (`device_info_t`)
```c
#define WaddleMaxDeviceNameLen 64

typedef struct device_info_t {
    char name[WaddleMaxDeviceNameLen];
    char config_path[WaddleMaxPathLen];
    char state_dir[WaddleMaxPathLen];
    char disk_image[WaddleMaxPathLen];
    uint32_t vsock_cid;
    uint32_t vsock_port;
    uint32_t memory_mb;
    uint32_t vcpus;
    int is_running;
} device_info_t;

typedef struct device_list_t {
    device_info_t devices[32];
    size_t count;
} device_list_t;
```

### 3.2 Constants (`PascalCase`)
- `MaxDeviceCount`: 32
- `DefaultDeviceMemoryMb`: 4096
- `DefaultDeviceVcpus`: 4
- `BaseVsockCid`: 3

---

## 4. Execution Sequence

### 4.1 Device Initialization (`waddle init <device-name>`)
1. Validate `device-name`: must match `^[a-zA-Z0-9_-]+$`.
2. Check if device already exists. If yes, return error `EEXIST`.
3. Locate base disk image:
   - Check `/home/dev/.local/share/waddle-vm-validation/windows.qcow2`.
   - Check `~/.local/state/waddle/vm/windows.qcow2`.
   - Check candidate paths in `/var/lib/waddle/` or search environment.
4. Create device directory `~/.local/state/waddle/devices/<device-name>/`.
5. Run `qemu-img create -f qcow2 -b <base_image> -F qcow2 ~/.local/state/waddle/devices/<device-name>/disk.qcow2`.
6. Allocate next available `vsock_cid` (e.g. 3, 4, 5, ...).
7. Generate and write `~/.config/waddle/devices/<device-name>.ini`.
8. Print confirmation details to user stdout.

### 4.2 Starting a Device (`waddle start [device-name]`)
1. Scan device registry via `daemon_device_list()`.
2. If `device_list.count == 0`:
   - Error: `waddle: no devices configured. Run 'waddle init <device-name>' first.`
3. If `device-name` not specified:
   - If `device_list.count == 1`: Target `device_list.devices[0].name`.
   - If `device_list.count > 1`:
     - Reject request. Output list of all devices and usage message.
4. If `device-name` specified: Target that specific device.
5. Resolve device socket and configuration. Launch supervisor if not running, then send `DaemonMsgStartReq`.

### 4.3 Stopping a Device (`waddle stop [device-name]`)
1. Scan device registry.
2. If `device-name` not specified:
   - If `count == 1`: Target `device_list.devices[0].name`.
   - If `count > 1`: Reject request with device listing.
3. Send `DaemonMsgStopReq` to the corresponding daemon.

### 4.4 Default Zero-Flag Interactive Terminal (`waddle [device-name]`)
1. If `device-name` is given as sole argument: Target that device.
2. If no argument is given:
   - If `count == 1`: Target `device_list.devices[0].name`.
   - If `count > 1`: Reject with device listing.
3. Auto-start target device if stopped.
4. Connect to target device's VSOCK CID/port and launch ConPTY interactive terminal (`powershell.exe`).

---

## 5. Concurrency, Threading & Synchronization
- File-based locking (`F_SETLK`) ensures two daemons cannot manage the same device simultaneously.
- Atomic state updates in daemon supervisor state machine.

---

## 6. Error Handling & Failure Modes
- `EINVAL`: Malformed device name.
- `EEXIST`: Device already exists during `init`.
- `ENOENT`: Specified device not found in registry.
- `EADDRINUSE`: Multiple devices attempting to bind identical VSOCK CIDs.
- Premature QEMU exit: Captured via `waitpid()`, error logged to `qemu.log`, status returned as code 110/ECHILD.

---

## 7. Verification & Testing Criteria
1. Unit tests for device name validation, CID allocation, config parsing, and device listing.
2. Integration test verifying:
   - `waddle init dev1` creates valid configuration and disk overlay.
   - `waddle start` starts `dev1` without specifying name.
   - `waddle init dev2` creates second device.
   - `waddle start` without name fails with multiple devices error.
   - `waddle start dev2` works.
   - `waddle stop` without name fails with multiple devices error.
   - `waddle stop dev1` and `waddle stop dev2` succeed.
   - Zero memory leaks under AddressSanitizer/LeakSanitizer.
