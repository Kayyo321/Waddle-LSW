# Implementation Description: Subsystem Daemon Manager (`feature/subsystem-daemon-manager`)

## 1. Title & High-Level Scope

### 1.1 Document & Feature Title
- **Feature Name**: Subsystem Daemon Manager & Integrated Background Subsystem Architecture (`feature/subsystem-daemon-manager`)
- **Subsystem Category**: Component 4 (CLI Passthrough & Process Execution Bridge) & Component 2 (IPC Transport Layer) & Hypervisor Subsystem Lifecycle Management
- **Primary Languages**: C (C11/C17/C23) and Zig (0.13+)
- **Language Ceiling**: C++ (C++20) strictly where external interfaces dictate; pure C ABI across all boundary definitions.

### 1.2 High-Level Vision & Purpose
Waddle-LSW delivers seamless, rootless integration of Windows applications and CLI environments into native Linux desktop systems. In prior milestones, the initial CLI passthrough bridge established low-latency process spawning, bidirectional I/O streaming, ConPTY terminal handling, and path translation over `AF_VSOCK`. However, initiating an interactive Windows session previously required manual hypervisor provisioning, external QEMU launch scripts, and pre-running guest listeners.

The **Subsystem Daemon Manager** transforms Waddle-LSW into an intuitive, zero-friction subsystem. Much like WSL on Windows or containers on Linux, running `waddle` without flags immediately drops the user into an interactive Windows terminal session (e.g. PowerShell or CMD). If the background hypervisor subsystem (QEMU with KVM, VirtIO-FS filesystem daemon, and VSOCK bridge) is not running, Waddle automatically launches the background daemon, spawns the virtual machine, verifies guest readiness, and connects transparently.

Crucially, this feature showcases **unified filesystem integration**:
- The host Linux user home directory and workspace trees are exported into the guest via `virtiofsd` (VirtIO-FS) and mounted inside Windows as high-performance drive letters (e.g. `Z:\` or `X:\`).
- When a user executes `waddle` or `waddle --exec` inside any host directory (such as `/home/dev/Waddle-LSW`), the working directory is dynamically mapped to the corresponding guest mount (`Z:\home\dev\Waddle-LSW`).
- Windows compilers, shells, and CLI tools can read and write files directly to the host filesystem with full POSIX/NT translation and zero file-copying overhead.

### 1.3 Boundaries: In-Scope vs. Out-of-Scope

#### In-Scope:
1. **Waddle Background Supervisor Daemon (`waddled` / `waddle daemon run`)**:
   - Lightweight user-level daemon process supervising the background subsystem.
   - Lifecycle management of `virtiofsd` (VirtIO-FS host daemon) and `qemu-system-x86_64` (KVM hypervisor).
   - UNIX domain control socket (`$XDG_RUNTIME_DIR/waddle/daemon.sock` or `~/.local/state/waddle/daemon.sock`) exposing the binary daemon control protocol.
   - Robust single-instance locking via `fcntl(F_SETLK)` on a dedicated lockfile (`waddle.lock`).
   - QMP (QEMU Machine Protocol) client in C: communicates via UNIX socket to monitor VM status, issue clean ACPI `system_powerdown`, query vCPU/memory stats, and detect spontaneous guest termination.
   - Guest readiness prober: polls VSOCK CID 3, port 5242 until `waddle-guest-exec` confirms readiness before allowing client connections.
   - Clean shutdown orchestration: sends guest shutdown signal, issues QMP powerdown, waits for VM exit with configurable timeout, falls back to SIGTERM/SIGKILL if unresponsive, and terminates `virtiofsd`.
2. **Unified Host CLI Frontend (`waddle`)**:
   - Zero-flag invocation (`$ waddle`): default action initiates an interactive ConPTY terminal session. If subsystem is stopped, auto-starts the daemon and VM with clean status feedback, then connects.
   - Start command (`waddle --start` / `waddle start`): initiates background daemon and VM; supports `--wait` or non-blocking startup.
   - Stop command (`waddle --stop` / `waddle stop`): triggers graceful guest shutdown and stops daemon; supports `--force` / `-f`.
   - Restart command (`waddle --restart` / `waddle restart`): sequential clean stop and start.
   - Status command (`waddle --status` / `waddle status`): displays detailed subsystem health, state, PIDs (daemon, QEMU, virtiofsd), memory/vCPU metrics, active VirtIO-FS exports, and uptime; supports `--json`.
   - Execution command (`waddle --exec <cmd>` / `waddle -e <cmd>` / `waddle exec -- <cmd>`): executes command inside guest with transparent auto-start.
   - Emergency kill (`waddle --kill` / `waddle kill`): immediately SIGKILLs orphaned QEMU/virtiofsd processes and clears stale socket files and lockfiles.
   - Log inspection (`waddle --logs` / `waddle logs`): displays stdout/stderr logs from daemon, QEMU, and virtiofsd; supports tailing (`-f`).
   - Filesystem management & showcase (`waddle --mount`, `waddle fs`, `waddle fs test`): displays active directory mappings and provides an automated test demonstrating real-time bidirectional file modification across Linux and Windows.
3. **VirtIO-FS Multi-Export & Filesystem Bridge**:
   - Supervision of `virtiofsd` vhost-user socket creation and lifecycle.
   - Multi-mount configuration mapping host directories (e.g. `/home/$USER`, workspace) to guest drive letters (`Z:\`, `X:\`).
   - Integration with `path_rules.zig` to automatically infer drive-letter path mappings based on active daemon export configurations.
4. **Daemon Control Wire Protocol (`include/waddle/daemon_protocol.h`)**:
   - Strict C ABI header defining packed Little-Endian messages, magic `0x57414444` (`WADD`), versioning, 64-bit alignment, and status payloads.
5. **Mock Subsystem & Acceptance Test Harness**:
   - Linux-based mock hypervisor harness simulating QEMU QMP and virtiofsd behavior for automated CI testing and memory sanitizer verification (`-fsanitize=address,leak`).
   - End-to-end integration test suite exercising CLI commands, lifecycle state transitions, concurrent client multiplexing, and filesystem synchronization.

#### Out-of-Scope:
- Wayland graphical surface capture and compositor rendering (Component 1 & Component 3, reserved for subsequent feature branches).
- Windows Kernel VirtIO-FS driver compilation (standard VirtIO-FS driver and WinFsp binaries are consumed inside the guest).
- System-wide systemd service installation in `/etc/systemd/system` requiring root privileges (the daemon runs strictly in userland under `$XDG_RUNTIME_DIR` or `~/.local/state`).

---

### 1.4 Command-Line Interface Specification

The unified `waddle` binary provides a streamlined CLI surface adhering to POSIX standards and modern subsystem conventions:

```
waddle [command|option] [parameters...]

Default Action:
  waddle                         Enter the interactive Windows terminal (ConPTY)
                                 Auto-starts subsystem if not currently running.

Subsystem Lifecycle Commands:
  waddle --start, waddle start   Start the background subsystem (daemon + QEMU + virtiofsd)
                                 Options: --wait (default), --no-wait, --timeout <sec>
  waddle --stop, waddle stop     Gracefully shut down the background subsystem
                                 Options: --force, -f, --timeout <sec>
  waddle --restart, waddle restart Restart the background subsystem
  waddle --status, waddle status Display current subsystem status and metrics
                                 Options: --json (machine-readable JSON output)
  waddle --kill, waddle kill     Forcefully terminate subsystem processes and clean locks

Process Execution:
  waddle --exec <cmd>            Execute a command in the Windows guest
  waddle -e <cmd>                Short flag for --exec
  waddle exec [opts] -- <cmd...> Explicit passthrough syntax with advanced options
  waddle run [opts] -- <cmd...>  Alias for waddle exec

Filesystem Integration:
  waddle fs, waddle --mount      List active VirtIO-FS shared directory mappings
  waddle fs test                 Run live cross-filesystem read/write verification test

Diagnostics & Tooling:
  waddle --logs, waddle logs     View daemon, hypervisor, and filesystem logs
                                 Options: -f (follow/tail), -n <lines>
  waddle --version, -v           Show version information
  waddle --help, -h              Show comprehensive help message
```

### 1.5 Primary Subsystem & Filesystem Validation Milestone

This feature is designed around a mandatory, end-to-end acceptance milestone demonstrating a live, responsive background subsystem and unified filesystem interoperability:

1. **Step 1: Start Subsystem**:
   ```bash
   $ waddle start
   ```
   Starts `waddled`, spawns `virtiofsd` (exporting `/home/$USER`), boots `qemu-system-x86_64` with KVM acceleration, performs QMP handshake, and verifies guest agent readiness over VSOCK.
2. **Step 2: Enter Integrated Terminal**:
   ```bash
   $ waddle
   ```
   Invoked without flags from any host directory (e.g. `/home/dev/Desktop` or `/home/dev/Waddle-LSW`). Auto-maps the current working directory to the guest drive (e.g. `Z:\home\dev\Desktop` or `Z:\home\dev\Waddle-LSW`) and attaches the user's terminal to an interactive ConPTY session (`cmd.exe` or `powershell.exe`).
3. **Step 3: List Working Directory**:
   ```cmd
   Z:\home\dev\Desktop> dir
   ```
   Lists the live contents of the host Linux directory directly inside the Windows guest shell.
4. **Step 4: Create Directory from Inside Guest**:
   ```cmd
   Z:\home\dev\Desktop> mkdir new_project_folder
   ```
5. **Step 5: Verify Live Linux Visibility**:
   The directory `new_project_folder` immediately appears on the host Linux filesystem without file-copying, caching delays, or manual sync commands:
   ```bash
   $ ls -ld /home/dev/Desktop/new_project_folder
   drwxr-xr-x 2 dev dev 4096 Oct  4 19:45 /home/dev/Desktop/new_project_folder
   ```

---

## 2. Architecture & Inter-Component Interactions

### 2.1 System Architecture Diagram

```
+---------------------------------------------------------------------------------------------------+
|                                        LINUX HOST USERLAND                                        |
|                                                                                                   |
|  +---------------------------------------------------------------------------------------------+  |
|  | User Shell / Terminal: $ waddle (or $ waddle --exec "dir")                                  |  |
|  +----------------------------------------------+----------------------------------------------+  |
|                                                 | UNIX Socket / Auto-Spawn Pipe                   |
|                                                 v                                                 |
|  +---------------------------------------------------------------------------------------------+  |
|  | Waddle CLI Client (`waddle`)                                                                |  |
|  | - Evaluates command: start, stop, status, exec, or default interactive terminal              |  |
|  | - If subsystem stopped: spawns daemon supervisor, waits for readiness                      |  |
|  | - Connects to Daemon Socket for management, or connects to VSOCK for process execution      |  |
|  +-------------------+---------------------------------------------------+---------------------+  |
|                      | Daemon IPC Protocol                               | VSOCK Port 5242        |
|                      v                                                   | (via /dev/vhost-vsock) |
|  +-------------------------------------------------------------+         |                        |
|  | Waddle Background Daemon (`waddled`)                        |         |                        |
|  | - Manages PID locks and lifecycle state machine             |         |                        |
|  | - Spawns and supervises virtiofsd and QEMU                  |         |                        |
|  | - Probes QMP socket and VSOCK guest agent readiness         |         |                        |
|  +-------------------+-----------------------------------+-----+         |                        |
|                      |                                   |               |                        |
|         vhost-user   |                      QMP Socket   |               |                        |
|         UNIX Socket  |                      UNIX Socket  |               |                        |
|                      v                                   v               |                        |
|  +---------------------------+       +---------------------------------+ |                        |
|  | VirtIO-FS Host Daemon     |       | QEMU Hypervisor Process         | |                        |
|  | (`virtiofsd`)             |       | (`qemu-system-x86_64`)          | |                        |
|  | - Exports /home/$USER     |       | - KVM Acceleration (-enable-kvm)| |                        |
|  | - High-performance cache  |       | - vhost-user-fs-pci device      | |                        |
|  | - POSIX / NT mapping      |       | - vhost-vsock-pci (CID: 3)      | |                        |
|  +-------------+-------------+       +----------------+----------------+ |                        |
+----------------|--------------------------------------|------------------|------------------------+
                 | Shared Memory (vhost-user)           | VM Bus Boundary  |
=================v======================================v==================v=========================
                                       WINDOWS GUEST VM
+---------------------------------------------------------------------------------------------------+
|  +---------------------------+       +---------------------------------+                          |
|  | VirtIO-FS Windows Driver  |       | Waddle Guest Execution Agent    |                          |
|  | + WinFsp Service          |       | (`waddle-guest-exec.exe`)       |                          |
|  | - Mounts as Z:\ (or X:\)  |       | - Listens on VSOCK port 5242    |                          |
|  | - Live host filesystem    |       | - Spawns ConPTY / Pipe children |                          |
|  +---------------------------+       +---------------------------------+                          |
+---------------------------------------------------------------------------------------------------+
```

### 2.2 Component Roles & Separation of Concerns

1. **Host CLI (`waddle`)**:
   - Thin, responsive client binary.
   - Interacts with `waddled` over a lightweight UNIX domain socket for lifecycle queries and commands.
   - For process execution (`waddle` no flags or `waddle --exec`), ensures the subsystem is running, then establishes direct, high-throughput streaming with the guest over `AF_VSOCK` (or mock socket during testing).
   - Manages local terminal raw modes (`termios`), forwards window resize signals (`SIGWINCH`), and forwards interrupt signals (`SIGINT`).

2. **Waddle Daemon Supervisor (`waddled`)**:
   - Single-instance background process owning subsystem lifecycle.
   - Creates and binds `$XDG_RUNTIME_DIR/waddle/daemon.sock` with restrictive permissions (`0700`).
   - Acquires `F_WRLCK` on `$XDG_RUNTIME_DIR/waddle/waddle.lock`.
   - Spawns `virtiofsd` with isolated vhost-user UNIX socket.
   - Spawns `qemu-system-x86_64` configured with shared memory backend (`/dev/shm`), vhost-user-fs-pci, vhost-vsock-pci, and QMP monitor socket.
   - Connects to QEMU QMP socket to negotiate capabilities (`qmp_capabilities`), monitors VM power states, and issues graceful shutdown signals (`system_powerdown`).
   - Actively probes guest VSOCK port 5242 until the guest agent replies with a ready status packet, then transitions the daemon state to `DaemonStateRunning`.
   - Monitors child processes via non-blocking `waitpid()` and `epoll()` / `poll()` on logging pipes, auto-reaping deceased processes and updating state to `DaemonStateStopped` or `DaemonStateError`.

3. **VirtIO-FS Daemon (`virtiofsd`)**:
   - Dedicated Linux userland filesystem daemon exposing host paths over vhost-user shared memory protocol.
   - Provides near-native I/O performance by bypassing network filesystem overhead.
   - Bound to an isolated UNIX socket in `$XDG_RUNTIME_DIR/waddle/virtiofsd.sock`.

4. **QEMU Machine Protocol (QMP) Client**:
   - Built directly into `waddled` in pure C.
   - Communicates using JSON-RPC over UNIX socket `$XDG_RUNTIME_DIR/waddle/qmp.sock`.
   - Executes initialization handshake (`qmp_capabilities`).
   - Queries VM execution state (`query-status`).
   - Triggers graceful ACPI power button event (`system_powerdown`).
   - Dispatches emergency power cut if graceful shutdown exceeds timeout (`quit` or process kill).

---

### 2.3 Filesystem Integration Architecture

The VirtIO-FS bridge is central to Waddle's goal of presenting Windows as a seamless extension of the Linux desktop.

#### Host-to-Guest Path Resolution:
- Host export directory: By default, exports `/home/$USER` (or custom configured directories in `~/.config/waddle/config.ini`).
- VirtIO-FS tag: `waddle_fs`.
- Guest mount point: WinFsp mounts the `waddle_fs` volume as drive `Z:\` (or `X:\`).
- When a user runs:
  ```bash
  cd /home/dev/Waddle-LSW
  waddle --exec "powershell.exe -Command Get-ChildItem"
  ```
  The CLI checks the current working directory (`/home/dev/Waddle-LSW`) against active daemon export rules. It matches the `/home/dev` prefix, replaces it with `Z:\`, and sets the guest working directory to `Z:\Waddle-LSW`.
- Windows tools run directly in the project directory, writing build artifacts, test outputs, or edits directly into the Linux workspace.

---

### 2.4 Subsystem State Machine

```
                      +-------------------+
                      |   STATE_STOPPED   | <-------------------------+
                      +---------+---------+                           |
                                |                                     |
                                | waddle --start / auto-start         |
                                v                                     |
                      +-------------------+                           |
                      | STARTING_VIRTIOFS |                           |
                      +---------+---------+                           |
                                | socket ready                        |
                                v                                     |
                      +-------------------+                           |
                      |   STARTING_QEMU   |                           |
                      +---------+---------+                           |
                                | QMP capabilities ok                 |
                                v                                     |
                      +-------------------+                           |
                      |   WAITING_GUEST   |                           |
                      +---------+---------+                           |
                                | VSOCK handshake ready               |
                                v                                     |
                      +-------------------+                           |
                      |   STATE_RUNNING   |                           |
                      +---------+---------+                           |
                                |                                     |
                                | waddle --stop / SIGTERM             |
                                v                                     |
                      +-------------------+                           |
                      | STOPPING_GRACEFUL |                           |
                      +---------+---------+                           |
                                |                                     |
               +----------------+----------------+                    |
               |                                 |                    |
               | VM exits cleanly                | Timeout expired    |
               v                                 v                    |
     +-------------------+             +-------------------+          |
     |   CLEANUP_EXITS   |             |   FORCE_KILLING   |          |
     +---------+---------+             +---------+---------+          |
               |                                 |                    |
               +----------------+----------------+                    |
                                |                                     |
                                v                                     |
                      +-------------------+                           |
                      |   STATE_STOPPED   | --------------------------+
                      +-------------------+
```

---

### 2.5 Directory Hierarchy & Runtime Artifacts

Waddle respects XDG Base Directory specifications, isolating runtime sockets, configurations, and persistent state:

```
$XDG_RUNTIME_DIR/waddle/           (Typically /run/user/<uid>/waddle, 0700 permissions)
├── waddle.lock                    (Process lockfile with fcntl F_WRLCK)
├── daemon.sock                    (Daemon IPC control UNIX domain socket)
├── qmp.sock                       (QEMU QMP monitor UNIX domain socket)
├── virtiofsd.sock                 (VirtIO-FS vhost-user UNIX domain socket)
├── daemon.pid                     (PID of active waddled process)
├── qemu.pid                       (PID of active qemu-system-x86_64 process)
└── virtiofsd.pid                  (PID of active virtiofsd process)

$XDG_STATE_HOME/waddle/            (Typically ~/.local/state/waddle/)
├── logs/
│   ├── daemon.log                 (Waddle supervisor daemon log)
│   ├── qemu.log                   (QEMU console and debug output)
│   └── virtiofsd.log              (VirtIO-FS host daemon log)
└── vm/
    └── waddle_disk.qcow2          (Windows guest base/overlay image)

$XDG_CONFIG_HOME/waddle/           (Typically ~/.config/waddle/)
└── config.ini                     (Subsystem configuration file)
```

---

## 3. Data Structures, Protocols & Memory Layouts

### 3.1 Daemon Control Protocol Header (`include/waddle/daemon_protocol.h`)

All communication between `waddle` CLI and `waddled` supervisor daemon travels over the local control UNIX domain socket using packed, Little-Endian binary frames aligned to 8-byte boundaries.

```c
/**
 * @file daemon_protocol.h
 * @brief Wire protocol definitions for the Waddle subsystem supervisor daemon.
 */

#ifndef WaddleDaemonProtocolH
#define WaddleDaemonProtocolH

#include <stdint.h>
#include <stddef.h>

/** @brief Magic identifier value: 'WADD' in Little-Endian byte order. */
#define WaddleDaemonMagic 0x57414444U

/** @brief Current daemon wire protocol version number. */
#define WaddleDaemonVersion 1U

/** @brief Maximum allowed payload size for daemon control messages (64 KiB). */
#define WaddleDaemonMaxPayloadSize 65536U

/** @brief Maximum length for filesystem path strings including NUL terminator. */
#define WaddleMaxPathLen 1024U

/**
 * @brief Enumeration of daemon IPC message types.
 */
typedef enum waddle_daemon_msg_type_t {
    DaemonMsgNone             = 0x0000,
    DaemonMsgStartReq         = 0x0001,
    DaemonMsgStartResp        = 0x0002,
    DaemonMsgStopReq          = 0x0003,
    DaemonMsgStopResp         = 0x0004,
    DaemonMsgStatusReq        = 0x0005,
    DaemonMsgStatusResp       = 0x0006,
    DaemonMsgKillReq          = 0x0007,
    DaemonMsgKillResp         = 0x0008,
    DaemonMsgFsListReq        = 0x0009,
    DaemonMsgFsListResp       = 0x000A,
    DaemonMsgErrorResp        = 0x00FF
} waddle_daemon_msg_type_t;

/**
 * @brief Subsystem operational state machine states.
 */
typedef enum waddle_subsystem_state_t {
    SubsystemStateStopped          = 0,
    SubsystemStateStartingVirtiofs = 1,
    SubsystemStateStartingQemu     = 2,
    SubsystemStateWaitingGuest     = 3,
    SubsystemStateRunning          = 4,
    SubsystemStateStopping         = 5,
    SubsystemStateFailed           = 6
} waddle_subsystem_state_t;

/**
 * @brief Common 16-byte header prepended to every daemon control message.
 */
typedef struct waddle_daemon_header_t {
    uint32_t magic;          /**< Magic number: WaddleDaemonMagic (0x57414444). */
    uint16_t version;        /**< Protocol version: WaddleDaemonVersion (1). */
    uint16_t msg_type;       /**< Message type enum (waddle_daemon_msg_type_t). */
    uint32_t sequence;       /**< Client-assigned request/response correlation ID. */
    uint32_t payload_len;    /**< Length of trailing payload in bytes. */
} waddle_daemon_header_t;

/**
 * @brief Payload for DaemonMsgStartReq.
 */
typedef struct waddle_daemon_start_req_t {
    uint32_t flags;          /**< Start flags: bit 0: wait for guest ready, bit 1: headless. */
    uint32_t timeout_sec;    /**< Maximum wait time in seconds (0 = default 60s). */
} waddle_daemon_start_req_t;

/**
 * @brief Payload for DaemonMsgStartResp and DaemonMsgStopResp.
 */
typedef struct waddle_daemon_result_resp_t {
    uint32_t status_code;    /**< 0 on success, POSIX errno on failure. */
    uint32_t subsystem_state;/**< Current waddle_subsystem_state_t. */
    char error_msg[256];     /**< Human-readable error message if status_code != 0. */
} waddle_daemon_result_resp_t;

/**
 * @brief Payload for DaemonMsgStopReq.
 */
typedef struct waddle_daemon_stop_req_t {
    uint32_t force;          /**< 1 for immediate termination, 0 for graceful ACPI. */
    uint32_t timeout_sec;    /**< Timeout before force kill (0 = default 15s). */
} waddle_daemon_stop_req_t;

/**
 * @brief Filesystem export mapping descriptor.
 */
typedef struct waddle_daemon_fs_mount_t {
    char host_path[WaddleMaxPathLen];   /**< Host export directory (e.g. /home/dev). */
    char guest_drive[32];               /**< Guest drive letter or UNC tag (e.g. Z:\). */
    uint32_t read_only;                 /**< 1 if read-only, 0 for read-write. */
} waddle_daemon_fs_mount_t;

/**
 * @brief Payload for DaemonMsgStatusResp.
 */
typedef struct waddle_daemon_status_resp_t {
    uint32_t subsystem_state;/**< Current waddle_subsystem_state_t. */
    uint32_t daemon_pid;     /**< PID of the waddled supervisor. */
    uint32_t qemu_pid;       /**< PID of qemu-system-x86_64 (0 if not running). */
    uint32_t virtiofsd_pid;  /**< PID of virtiofsd (0 if not running). */
    uint32_t vsock_cid;      /**< VSOCK CID assigned to guest (default 3). */
    uint32_t vsock_port;     /**< VSOCK port of guest agent (default 5242). */
    uint64_t uptime_sec;     /**< Seconds elapsed since subsystem transitioned to Running. */
    uint32_t memory_mb;      /**< Configured guest RAM in megabytes. */
    uint32_t vcpus;          /**< Configured guest vCPU count. */
    uint32_t mount_count;    /**< Number of active VirtIO-FS mount descriptors following. */
    waddle_daemon_fs_mount_t mounts[8]; /**< Array of active export mount mappings. */
} waddle_daemon_status_resp_t;

#endif /* WaddleDaemonProtocolH */
```

---

### 3.2 Configuration File Format & Schema (`~/.config/waddle/config.ini`)

The configuration loader parses standard INI syntax with bounds validation:

```ini
[subsystem]
# Virtual machine memory in megabytes
memory_mb = 4096

# Number of virtual CPU cores
vcpus = 4

# Base Windows disk image path (qcow2 format)
disk_image = ~/.local/state/waddle/vm/windows.qcow2

# VSOCK guest context ID and listener port
vsock_cid = 3
vsock_port = 5242

# Default shell executable for zero-flag waddle invocation
default_shell = powershell.exe

[filesystem]
# Primary user home export mapped to Z: drive
export_primary = /home/dev:Z:\:rw

# Optional secondary export
# export_secondary = /mnt/data:X:\:ro

[timeouts]
# Subsystem start readiness timeout in seconds
start_timeout = 60

# Graceful ACPI shutdown timeout in seconds before force kill
stop_timeout = 15
```

---

## 4. Step-by-Step Execution Sequence

### 4.1 Subsystem Start Sequence (`waddle --start` or Auto-Start)

1. **Invocation & Lock Check**:
   - `waddle` receives `--start` (or initiates auto-start before an interactive/exec session).
   - Probes `$XDG_RUNTIME_DIR/waddle/daemon.sock` via `connect()`.
   - If daemon socket responds: sends `DaemonMsgStartReq` over UNIX socket.
   - If daemon socket is absent or connection refused:
     - Forks and execs `waddled` in the background (or daemonizes directly).
     - Polls for `daemon.sock` creation with a 3-second deadline.
2. **Daemon Spawns VirtIO-FS Daemon (`virtiofsd`)**:
   - Verifies export directory exists on host.
   - Creates UNIX domain socket `/run/user/<uid>/waddle/virtiofsd.sock`.
   - Spawns `/usr/libexec/virtiofsd` (or `/usr/lib/qemu/virtiofsd`) with flags:
     `--socket-path=.../virtiofsd.sock --shared-dir=/home/dev --cache=auto --sandbox=none`.
   - Waits for `virtiofsd.sock` to become connectable.
3. **Daemon Spawns QEMU (`qemu-system-x86_64`)**:
   - Assembles QEMU command line:
     ```bash
     qemu-system-x86_64 \
       -name waddle-lsw,debug-threads=on \
       -enable-kvm -cpu host -smp 4 -m 4096M \
       -object memory-backend-file,id=mem,size=4096M,mem-path=/dev/shm,share=on \
       -numa node,memdev=mem \
       -chardev socket,id=char0,path=.../virtiofsd.sock \
       -device vhost-user-fs-pci,queue-size=1024,chardev=char0,tag=waddle_fs \
       -device vhost-vsock-pci,id=vsock0,guest-cid=3 \
       -chardev socket,id=qmp0,path=.../qmp.sock,server=on,wait=off \
       -mon chardev=qmp0,mode=control \
       -drive file=.../windows.qcow2,if=virtio,cache=writeback \
       -display none -daemonize -pidfile .../qemu.pid
     ```
   - Spawns process and records PID in `qemu.pid`.
4. **QMP Initialization Handshake**:
   - Daemon connects to `qmp.sock`.
   - Reads initial QMP greeting banner: `{"QMP": {"version": ...}}`.
   - Sends: `{"execute": "qmp_capabilities"}`.
   - Verifies response: `{"return": {}}`.
5. **Guest Agent Readiness Probing**:
   - Daemon enters polling loop connecting to VSOCK CID 3, port 5242 (or mock socket during test mode).
   - Once connected, exchanges framed handshake message.
   - Transitions state to `SubsystemStateRunning`.
6. **Client Confirmation**:
   - Daemon responds to client with `DaemonMsgStartResp` containing `status_code = 0`.
   - Client outputs success confirmation or seamlessly proceeds to launch the requested session.

---

### 4.2 Subsystem Stop Sequence (`waddle --stop`)

1. **Client Dispatches Stop Request**:
   - `waddle --stop` sends `DaemonMsgStopReq` with `force = 0` and `timeout_sec = 15`.
2. **Graceful ACPI Shutdown via QMP**:
   - Daemon transitions state to `SubsystemStateStopping`.
   - Issues QMP command: `{"execute": "system_powerdown"}`.
   - QEMU sends ACPI power button event to Windows guest kernel.
   - Windows begins standard OS shutdown sequence.
3. **Monitored Wait Loop**:
   - Daemon polls QEMU process status via non-blocking `waitpid(qemu_pid, &status, WNOHANG)`.
   - If QEMU exits within timeout:
     - Reaps QEMU process.
     - Sends `SIGTERM` to `virtiofsd` process and reaps it.
     - Cleans up `qmp.sock` and `virtiofsd.sock`.
     - Transitions state to `SubsystemStateStopped`.
     - Replies to client with `DaemonMsgStopResp (status_code = 0)`.
4. **Timeout Expiry & Force Kill Fallback**:
   - If QEMU does not exit within `timeout_sec`:
     - Daemon logs warning: "ACPI powerdown timed out; issuing SIGKILL".
     - Sends `SIGKILL` to QEMU PID.
     - Sends `SIGKILL` to `virtiofsd` PID.
     - Reaps processes, cleans sockets, transitions to `SubsystemStateStopped`.

---

### 4.3 Interactive Shell Execution Sequence (`waddle` no flags)

1. **Zero-Flag Detection**:
   - User executes `waddle` without arguments.
2. **Daemon Status & Auto-Start Check**:
   - Queries `daemon.sock` via `DaemonMsgStatusReq`.
   - If state is not `SubsystemStateRunning`:
     - Displays brief status message: `[waddle] Starting background subsystem...`.
     - Sends `DaemonMsgStartReq` and waits for readiness.
3. **Working Directory & Drive Translation**:
   - Determines current host working directory via `getcwd()`.
   - Translates POSIX path using active VirtIO-FS mount rules (`/home/dev/...` -> `Z:\...`).
4. **ConPTY Session Launch**:
   - Connects to VSOCK CID 3, Port 5242.
   - Places host terminal in raw mode via `termios`.
   - Sends `WaddleMsgSpawnReq` configured for interactive ConPTY with `powershell.exe`.
   - Streams terminal I/O, forwards window resize events (`SIGWINCH`), and restores terminal on exit.

---

### 4.4 Non-Interactive Command Execution Sequence (`waddle --exec <cmd>`)

1. **Argument Parsing & Translation**:
   - `waddle --exec "dir"` extracts target command string.
   - Automatically applies path translation and working directory resolution.
2. **Auto-Start & Connect**:
   - Ensures subsystem is running (auto-starting if stopped).
   - Connects to guest VSOCK port.
3. **Execution & Exit Code Propagation**:
   - Sends spawn request, streams stdout/stderr directly to host streams.
   - Exits with exact return code of the guest process.

---

## 5. Concurrency, Threading & Synchronization

1. **Supervisor Process Model**:
   - The daemon runs as a dedicated single process supervising child processes (`qemu-system-x86_64`, `virtiofsd`).
   - Communication with clients occurs over non-blocking UNIX domain sockets managed via a single-threaded `poll()` event loop.
2. **Single-Instance Lockfile Enforcement**:
   - `waddled` opens `$XDG_RUNTIME_DIR/waddle/waddle.lock` and applies `fcntl(fd, F_SETLK, &fl)` with `F_WRLCK`.
   - If lock acquisition fails with `EAGAIN` or `EACCES`, another daemon instance is already active; the new process logs an error and exits immediately.
3. **Signal Trapping & Child Reaping**:
   - Traps `SIGCHLD`: uses a self-pipe or `signalfd` to wake up the `poll()` loop safely without signal handler race conditions.
   - Reaps terminated children using `waitpid(-1, &status, WNOHANG)`.
   - Traps `SIGTERM` and `SIGINT`: initiates clean subsystem shutdown before terminating supervisor.

---

## 6. Error Handling & Failure Modes

1. **Daemon Socket Stale or Unresponsive**:
   - If `daemon.sock` exists on disk but `connect()` fails with `ECONNREFUSED`, the previous daemon process crashed without cleaning up.
   - The CLI attempts to acquire the lockfile. If the lockfile is free, it unlinks the stale socket and spawns a fresh daemon supervisor.
2. **Orphaned QEMU / Virtiofsd Detection (`waddle --kill`)**:
   - If processes remain running after a crash, `waddle --kill` reads recorded PIDs from `qemu.pid` and `virtiofsd.pid`.
   - Validates that the processes belong to QEMU/virtiofsd (via `/proc/<pid>/cmdline`), dispatches `SIGKILL`, and cleans all runtime files.
3. **Guest Readiness Timeout**:
   - If guest agent does not become ready on VSOCK within `start_timeout` seconds, daemon transitions to `SubsystemStateFailed`, logs diagnostic output from QEMU console log, and reports an actionable error to the user.
4. **VirtIO-FS Driver / Mount Failure**:
   - If `virtiofsd` fails to bind its vhost-user socket, QEMU will fail on startup. Daemon captures stderr from both processes and surfaces the exact error message.

---

## 7. Verification & Testing Criteria

### 7.1 Automated Unit & Integration Tests
1. **Daemon Protocol Serialization Tests (`tests/test_daemon_protocol.c` / `tests/test_daemon.zig`)**:
   - Frame encoding/decoding, Little-Endian correctness, CRC validation, corrupt message rejection, buffer overflow bounds checking.
2. **Configuration Parser Tests (`tests/test_daemon_config.c` / `tests/test_daemon_config.zig`)**:
   - Parsing valid, missing, and malformed INI files; range verification on memory/vCPU values; export path parsing.
3. **Mock Subsystem Lifecycle Tests (`tests/test_daemon_lifecycle.c`)**:
   - Uses a mock QEMU QMP server and mock virtiofsd over UNIX sockets.
   - Exercises full state transitions: `Stopped -> Starting -> Running -> Stopping -> Stopped`.
   - Exercises timeout handling and force kill fallbacks.
   - Exercises multiple concurrent client queries on `daemon.sock`.
4. **Filesystem Integration End-to-End Test (`tests/demo_fs.sh` / `waddle fs test`)**:
   - Creates a temporary file in host shared directory.
   - Executes guest command modifying the file.
   - Verifies modification immediately appears on host without manual syncing.
5. **Sanitizer & Leak-Free Verification**:
   - Test suite executes cleanly under AddressSanitizer and LeakSanitizer (`-fsanitize=address,leak`) with **zero memory leaks**.

---

## 8. Bundled Hypervisor & All-in-One Distribution Architecture

### 8.1 Rationale for Bundling QEMU
In earlier iterations of Waddle-LSW, users were required to manually provision system-level dependencies on their Linux host (such as `qemu-system-x86_64`, `virtiofsd`, and associated virtualization libraries via distributions' package managers like `apt`, `dnf`, or `pacman`). This approach introduced several critical friction points:
1. **Host Configuration Drift & Version Incompatibilities**: Differing host distributions ship disparate versions of QEMU (e.g. 7.x vs 8.x vs 9.x) with differing command-line syntax, varying feature sets for `vhost-user-fs-pci`, and inconsistent socket permission models.
2. **Missing Optional Components**: Many base QEMU distribution packages omit or unbundle `virtiofsd` into distinct package names (`qemu-system-virtio-fs` or separate Rust `virtiofsd`), leading to cryptic startup failures when users try to launch the subsystem.
3. **Friction-Free "All-in-One" Experience**: By directly bundling QEMU into the repository as a Git submodule, Waddle-LSW achieves an all-in-one developer and user experience. Users clone the repository and compile everything required with a single invocation of `make`, without needing root privileges to install system packages.

### 8.2 Architectural Boundary & Isolation
Per `AGENTS.md` and `PROJECT.md`, loose source vendoring is strictly forbidden. The QEMU upstream source is tracked as a Git submodule located under `submodules/qemu`.

The architectural relationship between Waddle-LSW and QEMU maintains strict process-level boundaries:
- **Process Isolation**: QEMU is executed as an independent child process via standard POSIX `fork()` and `execvp()`. There is no dynamic or static C ABI linking against QEMU internal object files or libraries within Waddle's executable.
- **IPC Protocol Boundary**: All control and status monitoring take place across standard UNIX domain sockets:
  - **QMP Socket**: JSON-RPC control protocol for capabilities negotiation, ACPI graceful shutdown (`system_powerdown`), and VM status queries.
  - **vhost-user Socket**: VirtIO shared memory interface connecting `virtiofsd` and QEMU's memory backend.
  - **AF_VSOCK Device**: Transparent guest/host transport connecting `waddle` CLI sessions directly to `waddle-guest-exec` inside the Windows VM.
- **Zero Kernel/Root Escalation**: Both the bundled QEMU hypervisor and `waddled` daemon run entirely in userland under the unprivileged user's session (`$XDG_RUNTIME_DIR/waddle`).

### 8.3 License Compatibility & Distribution Analysis
- **Waddle-LSW License**: GNU General Public License Version 3 (GPLv3).
- **QEMU License**: GNU General Public License Version 2 (GPLv2), with individual subsystem components under GPLv2+, LGPLv2.1, BSD, and MIT licenses.
- **Legal & License Interaction**:
  - Waddle-LSW and QEMU communicate strictly across inter-process communication (IPC) boundaries (UNIX sockets, pipes, and process invocation). Under established Free Software Foundation (FSF) licensing guidelines, communicating via command-line arguments and standard IPC mechanisms establishes separate programs rather than a single combined work.
  - When distributed together as an "all-in-one" package or container, Waddle-LSW and QEMU form an aggregate software bundle.
  - Both licenses mandate that full corresponding source code must be made readily available. Vendoring QEMU as a public Git submodule pointing to immutable upstream commits guarantees complete compliance with GPLv2 and GPLv3 source distribution obligations.

### 8.4 Local Build System Integration
The root `GNUmakefile` defines dedicated build recipes for the QEMU submodule:
1. **Target Minimization**:
   To prevent excessive compile times and multi-gigabyte build artifacts, QEMU is configured to compile only the specific emulator binary required:
   ```bash
   ./configure --target-list=x86_64-softmmu --enable-kvm --disable-docs --disable-gtk --disable-sdl --disable-vnc
   ```
2. **Output Artifact Locations**:
   Compiled binaries are output to a local build directory structured as:
   ```
   build/vendor/
   ├── qemu-system-x86_64
   └── virtiofsd
   ```
3. **Idempotence & Build Cache**:
   The makefile checks for the existence of `build/vendor/qemu-system-x86_64` and invokes the submodule's configure/make only when the target is out of date or explicitly requested.

### 8.5 Relative Binary Discovery Mechanism
Instead of hardcoding absolute system paths (`/usr/bin/qemu-system-x86_64`, `/usr/libexec/virtiofsd`), both `daemon_qemu.c` and `daemon_fs.c` implement relative binary discovery:
1. **Self-Executable Directory Resolution**:
   The daemon inspects `/proc/self/exe` via `readlink()` to determine the absolute canonical directory of the running `waddle` or `waddled` binary.
2. **Vendor Directory Search**:
   The daemon constructs candidate paths relative to its own location:
   - `<executable_dir>/vendor/qemu-system-x86_64`
   - `<executable_dir>/../build/vendor/qemu-system-x86_64`
   - `<executable_dir>/vendor/virtiofsd`
   - `<executable_dir>/../build/vendor/virtiofsd`
3. **Graceful Fallback**:
   If the local vendor directory does not contain the binary (e.g. during lightweight test harnesses or when external QEMU is purposefully supplied), the discovery logic falls back to standard system paths (`/usr/libexec/virtiofsd`, `/usr/bin/virtiofsd`, and `$PATH`), guaranteeing backward compatibility and maximum execution flexibility.

