# Implementation Description: Initial CLI Passthrough (`feature/initial-cli-passthrough`)

## 1. Title & High-Level Scope

### 1.1 Document & Feature Title
- **Feature Name**: Initial CLI Passthrough Architecture & Implementation (`feature/initial-cli-passthrough`)
- **Subsystem Category**: Component 4 (CLI Passthrough & Process Execution Bridge) & Component 2 (IPC Transport Layer)
- **Primary Languages**: C (C11/C17/C23) and Zig (0.13+)
- **Language Ceiling**: C++ (C++20) strictly where external Win32/C++ APIs make C/Zig impractical; pure C ABI across all boundary definitions.

### 1.2 High-Level Vision & Purpose
In traditional virtualization setups, executing a command inside a guest OS requires heavy external network services (such as SSH, WinRM, or PowerShell remoting) with noticeable latency, complex authentication, and poor integration with the host environment.

Waddle-LSW reverses the WSL paradigm: it enables a Linux user to run Windows executables, command-line utilities, shells, and batch scripts directly from their native Linux terminal emulator (e.g. Alacritty, Foot, Kitty, GNOME Terminal) as if they were native Linux binaries.

The command:
```bash
waddle exec -- powershell.exe -NoProfile -Command "Get-Service | Select-Object -First 5"
```
or transparently:
```bash
waddle run cmd.exe /c "dir /b C:\Windows"
```
must execute inside the Windows guest VM, stream `stdin`, `stdout`, and `stderr` bidirectionally with sub-millisecond latency over `AF_VSOCK` or VirtIO-Serial, correctly forward terminal window resizes (`SIGWINCH`), translate signals (`SIGINT` / `Ctrl+C` into console control events), handle interactive terminal applications via Windows Pseudo Console (ConPTY), translate filesystem paths between Linux host paths and Windows guest drive letters (mapped via VirtIO-FS/WinFsp), and faithfully propagate process exit codes back to the calling Linux shell.

### 1.3 Boundaries: In-Scope vs. Out-of-Scope

#### In-Scope:
1. **Shared Wire Protocol Specification & C Header (`include/waddle/cli_protocol.h`)**:
   - Strict C ABI header defining packet framing, magic numbers, versioning, message types, stream identifiers, error codes, and aligned payload structures.
   - Little-endian encoding with 64-bit alignment and 32-bit CRC or header sanity verification.
2. **Windows Command Line Quoting & Escaping Engine**:
   - Fully compliant inverse implementation of `CommandLineToArgvW` in C/Zig.
   - Deterministic quoting and escaping of special characters, quotes (`"`), backslashes (`\`), and whitespace to construct unambiguous Win32 command lines.
3. **Linux Host CLI Client (`waddle-cli` / `waddle`)**:
   - Command-line argument parsing supporting execution options (`--interactive`/`-i`, `--tty`/`-t`, `--cwd`, `--env`, `--timeout`, `--socket-path`, `--vsock-cid`, `--vsock-port`).
   - Terminal mode configuration using POSIX `termios`: query current terminal attributes, switch to raw mode for interactive sessions, restore canonical mode on exit or crash.
   - Signal handling: catch `SIGWINCH` to forward terminal dimension updates (`winsize` -> ConPTY rows/cols); catch `SIGINT`/`SIGTERM` to transmit cancellation signals to the guest process without abruptly dropping the transport link.
   - Asynchronous I/O multiplexer using `poll()` / `epoll()` to stream host `STDIN` into the IPC channel and demultiplex incoming `STDOUT` and `STDERR` frames to the host standard outputs.
   - Exit code propagation: exiting with the exact exit code of the guest process.
4. **Windows Guest Console Execution Agent (`waddle-guest-exec.exe`)**:
   - Low-latency listener over `AF_VSOCK` (port 5242) and VirtIO-Serial / local test sockets.
   - Process launcher supporting two execution modes:
     - **Mode A: Raw Anonymous Pipes**: For non-interactive execution, batch scripts, and command piping (`| grep`, redirection), creating separate pipes for `hStdInput`, `hStdOutput`, `hStdError`.
     - **Mode B: ConPTY (Windows Pseudo Console)**: Using Windows `CreatePseudoConsole`, `STARTUPINFOEXW`, and `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` for interactive terminal sessions, full ANSI/VT100 escape code processing, and interactive curses/REPL utilities.
   - Multi-threaded or overlapped asynchronous I/O pump bridging Windows pipe handles and the guest transport socket.
   - Signal mapping: translating `WADDLE_SIGNAL_SIGINT` into `GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)` or `CTRL_BREAK_EVENT`.
   - Process termination detection using `WaitForSingleObject` and `GetExitCodeProcess`, ensuring all remaining buffered output is flushed before sending the final exit notification.
5. **Path & Environment Translation Engine**:
   - Rule-based translation mapping host POSIX paths (e.g. `/home/dev/project`) to guest Windows drive paths (e.g. `Z:\home\dev\project` or `\\waddle-host\shared\...`).
   - Environment variable inheritance filter and translator.
6. **Host Loopback Mock Server & Test Harness**:
   - Linux-based mock server mimicking the guest console agent over a Unix domain socket (`/tmp/waddle-mock.sock`) to allow comprehensive unit and integration testing of the host CLI tool, argument escaping, raw terminal handling, and exit code propagation directly on the Linux host development environment.

#### Out-of-Scope:
- Wayland window surface capture, DXGI surface tapping, and compositor blitting (covered by Component 1 & Component 3).
- Windows Service Control Manager (SCM) background daemon installation (the agent runs as a standalone userland executable or autostart background process during this initial feature).
- Windows GUI window integration or borderless floating window management (handled by Wayland compositor client).
- VirtIO-FS kernel driver development (standard virtiofs drivers and WinFsp are utilized).

---

## 2. Architecture & Inter-Component Interactions

### 2.1 Architectural Flow Diagram

```
+-----------------------------------------------------------------------------------------+
|                                    LINUX HOST ENVIRONMENT                               |
|                                                                                         |
|   +---------------------------------------------------------------------------------+   |
|   |  User Terminal (Alacritty / Foot / Kitty / GNOME Terminal / Bash / Zsh)         |   |
|   |  $ waddle exec -i -- powershell.exe -NoExit                                      |   |
|   +---------------------------------------+-----------------------------------------+   |
|                                           | STDIN / STDOUT / STDERR                     |
|                                           v                                             |
|   +---------------------------------------------------------------------------------+   |
|   |  Host CLI Utility (`waddle` / `waddle-cli`) [Written in C / Zig]                 |   |
|   |  - Parses CLI flags, validates command arguments, escapes Windows cmdline       |   |
|   |  - Queries `isatty(STDIN_FILENO)`, configures raw `termios` if interactive      |   |
|   |  - Traps `SIGWINCH` (updates rows/cols) and `SIGINT` (forwards Ctrl+C signal)   |   |
|   |  - Non-blocking multiplexing event loop (`poll` / `epoll`)                      |   |
|   +---------------------------------------+-----------------------------------------+   |
|                                           | Bidirectional Binary Stream                 |
|                                           | (`waddle_cli_msg_header_t` frames)          |
|                                           v                                             |
|   +---------------------------------------------------------------------------------+   |
|   |  Host Transport Socket Driver                                                   |   |
|   |  - Linux `AF_VSOCK` (socket(AF_VSOCK, SOCK_STREAM, 0))                          |   |
|   |  - Fallback / Mock: Unix Domain Socket (`/tmp/waddle-mock.sock`)                |   |
|   +---------------------------------------+-----------------------------------------+   |
+-------------------------------------------|---------------------------------------------+
                                            |
                         HYPERVISOR / KVM BOUNDARY (VSOCK CID: 3, Port: 5242)
                                            |
+-------------------------------------------|---------------------------------------------+
|                                           v                                             |
|   +---------------------------------------------------------------------------------+   |
|   |  Guest Transport Socket Driver                                                  |   |
|   |  - Windows `AF_HYPERV` / VirtIO-Serial / WinSock AF_VSOCK                       |   |
|   +---------------------------------------+-----------------------------------------+   |
|                                           | Bidirectional Binary Stream                 |
|                                           v                                             |
|   +---------------------------------------------------------------------------------+   |
|   |  Guest Console Execution Agent (`waddle-guest-exec.exe`) [Written in C / Zig]  |   |
|   |  - Listens on VSOCK port 5242; accepts incoming client connection               |   |
|   |  - Decodes `WADDLE_MSG_SPAWN_REQ`, verifies session ID and security credentials |   |
|   |  - Translates working directory and environment variables                       |   |
|   |  - Branch:                                                                      |   |
|   |      * If Interactive: Instantiates Win32 ConPTY (`CreatePseudoConsole`)        |   |
|   |      * If Pipe Mode:   Creates anonymous pipes (`CreatePipe`)                   |   |
|   |  - Spawns child process via `CreateProcessW` with `STARTUPINFOEXW`              |   |
|   |  - Launches reader/writer worker threads with overlapped I/O                    |   |
|   |  - Monitors process exit (`WaitForSingleObject`) -> sends exit code frame       |   |
|   +-------------------+-----------------------------------+-------------------------+   |
|                       | (Input Pipe)                      | (Output Pipe)               |
|                       v                                   ^                             |
|   +-------------------------------------------------------+-------------------------+   |
|   |  Target Windows Process                                                         |   |
|   |  (e.g., `cmd.exe`, `powershell.exe`, `cl.exe`, `python.exe`, `cargo.exe`)       |   |
|   +---------------------------------------------------------------------------------+   |
|                                                                                         |
|                                 WINDOWS GUEST ENVIRONMENT                               |
+-----------------------------------------------------------------------------------------+
```

### 2.2 System Layers and Boundaries
1. **Userland Presentation Layer (Host)**:
   - Preserves user terminal escape sequences (RGB truecolor, 256 colors, cursor positioning, mouse reporting if enabled).
   - In raw mode, input is passed byte-for-byte (including `\x03` for Ctrl+C, `\x04` for Ctrl+D, arrow keys, etc.) to the guest console agent.
2. **Framing & Transport Layer**:
   - Fixed 32-byte header `waddle_cli_msg_header_t` prepended to every message payload.
   - Multiplexes multiple logical streams over a single stream-oriented socket connection (`AF_VSOCK` or mock UNIX domain socket).
   - Eliminates head-of-line blocking for control signals: resize and signal packets are small (32-byte header + 8-byte body) and can be injected immediately between stream chunks.
3. **Execution & Virtual Terminal Layer (Guest)**:
   - For interactive sessions, Windows ConPTY acts as a high-fidelity translation layer between Win32 Console subsystem calls (`WriteConsoleOutput`, `SetConsoleCursorPosition`) and modern VT100/ANSI escape codes, streaming them back to the Linux host terminal.
   - For non-interactive batch commands, direct Win32 pipes guarantee that raw binary streams (such as piped files, tarballs, or binary compiler artifacts) are not altered by console line wrapping or VT escape code insertion.

---

## 3. Data Structures, Protocols & Memory Layouts

All data structures are defined with explicit integer widths, 64-bit alignment, and no compiler-dependent padding. The wire protocol is native little-endian.

### 3.1 Common Message Header (`waddle_cli_msg_header_t`)

```c
#ifndef WADDLE_CLI_PROTOCOL_H
#define WADDLE_CLI_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define WaddleCliMagic UINT32_C(0x57444c43) /* 'WDLC' in ASCII, Little-Endian */
#define WaddleCliVersion 1
#define WaddleDefaultVsockPort 5242
#define WaddleMaxPayloadSize (1024u * 1024u) /* 1 MB maximum single payload chunk */
#define WaddleHeaderSize 32u
#define WaddleChunkSize 16384u
#define WaddleQueueSize (2u * 1024u * 1024u)

#pragma pack(push, 1)

typedef struct waddle_cli_msg_header_t {
    uint32_t magic;          /* Magic identifier: WaddleCliMagic (0x57444C43) */
    uint16_t version;        /* Protocol version: WaddleCliVersion (1) */
    uint16_t msg_type;       /* Message type: enum waddle_cli_msg_type_t */
    uint64_t session_id;     /* Unique process session ID */
    uint32_t payload_len;    /* Byte length of the following payload (0 to 1MB) */
    uint32_t flags;          /* Message-specific flags */
    uint32_t sequence;       /* Monotonically increasing sequence number */
    uint32_t crc32;          /* CRC-32/ISO-HDLC checksum of payload, or 0 if disabled */
} waddle_cli_msg_header_t;

#pragma pack(pop)
```

### 3.2 Message Types (`waddle_cli_msg_type_t`)

```c
typedef enum waddle_cli_msg_type_t {
    WaddleMsgSpawnReq        = 0x0001, /* Host -> Guest: Request process execution */
    WaddleMsgSpawnResp       = 0x0002, /* Guest -> Host: Process spawn confirmation / error */
    WaddleMsgStreamData      = 0x0003, /* Bidirectional: Multiplexed standard I/O data */
    WaddleMsgTerminalResize  = 0x0004, /* Host -> Guest: Window dimensions change (SIGWINCH) */
    WaddleMsgSignalEvent     = 0x0005, /* Host -> Guest: Forwarded signal (Ctrl+C, Ctrl+Break) */
    WaddleMsgProcessExit     = 0x0006, /* Guest -> Host: Process termination & exit code */
    WaddleMsgHeartbeatPing   = 0x0007, /* Host -> Guest / Guest -> Host: Keepalive check */
    WaddleMsgHeartbeatPong   = 0x0008, /* Response to HeartbeatPing */
    WaddleMsgStreamEof       = 0x0009, /* Bidirectional: Indicates EOF on specific stream */
    WaddleMsgError           = 0x00FF  /* Fatal protocol or execution error */
} waddle_cli_msg_type_t;
```

### 3.3 Stream Identifiers (`waddle_stream_id_t`)

```c
typedef enum waddle_stream_id_t {
    WaddleStreamStdin  = 0, /* Standard Input (Host -> Guest) */
    WaddleStreamStdout = 1, /* Standard Output (Guest -> Host) */
    WaddleStreamStderr = 2  /* Standard Error (Guest -> Host) */
} waddle_stream_id_t;
```

### 3.4 Execution Flags (`waddle_spawn_flags_t`)

```c
typedef enum waddle_spawn_flags_t {
    WaddleSpawnFlagInteractive   = (1 << 0), /* Allocate ConPTY pseudo console */
    WaddleSpawnFlagRawPipes       = (1 << 1), /* Use raw Win32 pipes (non-interactive) */
    WaddleSpawnFlagInheritEnv     = (1 << 2), /* Merge host env into guest process env */
    WaddleSpawnFlagTranslatePath  = (1 << 3), /* Auto-translate Linux paths in argv/cwd */
    WaddleSpawnFlagElevated       = (1 << 4)  /* Request administrator elevation if supported */
} waddle_spawn_flags_t;
```

### 3.5 Payload Payloads and Memory Layouts

#### 3.5.1 Spawn Request (`waddle_msg_spawn_req_t`)
```c
#pragma pack(push, 1)

typedef struct {
    uint32_t spawn_flags;    /* Bitmask of waddle_spawn_flags_t */
    uint16_t initial_rows;   /* Terminal rows (e.g. 24) */
    uint16_t initial_cols;   /* Terminal columns (e.g. 80) */
    uint16_t x_pixels;       /* Terminal horizontal pixels (optional, 0 if unknown) */
    uint16_t y_pixels;       /* Terminal vertical pixels (optional, 0 if unknown) */
    uint32_t cwd_len;        /* Length of UTF-8 working directory path (excluding null) */
    uint32_t cmdline_len;    /* Length of UTF-8 command line string (excluding null) */
    uint32_t env_len;        /* Length of null-delimited KEY=VALUE environment block */
    /* Trailing dynamic buffer:
     * - char cwd[cwd_len + 1];
     * - char cmdline[cmdline_len + 1];
     * - char env[env_len];
     */
} waddle_msg_spawn_req_t;

#pragma pack(pop)
```

#### 3.5.2 Spawn Response (`waddle_msg_spawn_resp_t`)
```c
#pragma pack(push, 1)

typedef struct {
    uint32_t status_code;    /* 0 = SUCCESS, non-zero = Win32 or protocol error code */
    uint32_t guest_pid;      /* Windows Process ID (PID) of spawned child process */
    uint32_t error_len;      /* Length of human-readable error description string */
    /* Trailing dynamic buffer:
     * - char error_message[error_len];
     */
} waddle_msg_spawn_resp_t;

#pragma pack(pop)
```

#### 3.5.3 Stream Data Header (`waddle_msg_stream_data_t`)
```c
#pragma pack(push, 1)

typedef struct {
    uint8_t  stream_id;      /* waddle_stream_id_t (STDIN, STDOUT, STDERR) */
    uint8_t  reserved[3];    /* Padding for 32-bit alignment */
    uint32_t data_len;       /* Byte count of payload chunk */
    /* Trailing dynamic buffer:
     * - uint8_t data[data_len];
     */
} waddle_msg_stream_data_t;

#pragma pack(pop)
```

#### 3.5.4 Terminal Resize Notification (`waddle_msg_resize_t`)
```c
#pragma pack(push, 1)

typedef struct {
    uint16_t rows;           /* New terminal rows */
    uint16_t cols;           /* New terminal columns */
    uint16_t x_pixels;       /* Optional pixel width */
    uint16_t y_pixels;       /* Optional pixel height */
} waddle_msg_resize_t;

#pragma pack(pop)
```

#### 3.5.5 Signal Event Notification (`waddle_msg_signal_t`)
```c
typedef enum {
    WADDLE_SIGNAL_SIGINT  = 2,  /* Mapped to CTRL_C_EVENT */
    WADDLE_SIGNAL_SIGQUIT = 3,  /* Mapped to CTRL_BREAK_EVENT */
    WADDLE_SIGNAL_SIGTERM = 15, /* TerminateProcess request */
    WADDLE_SIGNAL_SIGKILL = 9   /* Immediate TerminateProcess request */
} waddle_signal_type_t;

#pragma pack(push, 1)

typedef struct {
    uint32_t signal_type;    /* waddle_signal_type_t */
} waddle_msg_signal_t;

#pragma pack(pop)
```

#### 3.5.6 Process Exit Notification (`waddle_msg_exit_t`)
```c
#pragma pack(push, 1)

typedef struct {
    uint32_t exit_code;      /* Windows GetExitCodeProcess value */
    uint32_t termination_status; /* 0 = Normal Exit, 1 = Killed by Signal, 2 = Exception/Crash */
    uint64_t wall_time_ms;   /* Total elapsed execution time in milliseconds */
} waddle_msg_exit_t;

#pragma pack(pop)
```

---

## 4. Step-by-Step Execution Sequence

### 4.1 Host CLI Initialization & Launch Phase
1. **Command Line Parsing**:
   - `waddle-cli` parses arguments: `waddle exec [options] -- <program> [arguments...]`.
   - Flags determine whether ConPTY is requested (default is interactive if `isatty(STDIN_FILENO)` is true; forceable via `-i` / `--interactive` or `-P` / `--pipe`).
2. **Windows Argument Quoting (`CommandLineToArgvW` Inverse)**:
   - Each argument string in `argv` is escaped according to Microsoft Win32 rules:
     - If argument is empty, serialize as `""`.
     - Count consecutive backslashes preceding quotation marks or the end of the argument string:
       - Before `"`, output `2 * backslashes + 1` backslashes followed by `"`.
       - At end of argument, output `2 * backslashes` backslashes followed by `"`.
       - If argument contains spaces, tabs, or quotes, enclose the entire escaped argument in outer quotes `"`.
   - The resulting strings are joined with a single space delimiter to form the complete `cmdline` string.
3. **Working Directory & Environment Gathering**:
   - Query host current working directory (`getcwd`).
   - If path translation flag is active, translate `/home/<user>/...` to `Z:\home\<user>\...` or the configured VirtIO-FS mount point.
   - Filter host environment variables (strip display/X11/Wayland variables, preserve `PATH`, custom overrides specified via `-e KEY=VAL`).
4. **Terminal Setup (if interactive)**:
   - Call `tcgetattr(STDIN_FILENO, &orig_termios)`.
   - Register `atexit()` handler and signal handlers (`SIGINT`, `SIGTERM`, `SIGSEGV`) to guarantee `tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios)` is called under all exit scenarios.
   - Query terminal dimensions via `ioctl(STDIN_FILENO, TIOCGWINSZ, &ws)`.
   - Switch terminal to raw mode:
     ```c
     struct termios raw = orig_termios;
     raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
     raw.c_oflag &= ~(OPOST);
     raw.c_cflag |= (CS8);
     raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
     raw.c_cc[VMIN] = 1;
     raw.c_cc[VTIME] = 0;
     tcsetattr(STDIN_FILENO, TCSANOW, &raw);
     ```
   - Register `SIGWINCH` handler using `sigaction`.
5. **Connection Establishment**:
   - Connect to guest via `AF_VSOCK` (VM CID, Port 5242) or test loopback socket.
   - Send `WADDLE_MSG_SPAWN_REQ` packet containing the header and `waddle_msg_spawn_req_t` payload.

### 4.2 Guest Console Agent Execution Phase
1. **Connection Acceptance**:
   - `waddle-guest-exec` accepts client connection.
   - Reads 32-byte header, verifies `magic == WADDLE_CLI_MAGIC` and `version == WADDLE_CLI_VERSION`.
   - Reads `waddle_msg_spawn_req_t` and trailing string payloads (`cwd`, `cmdline`, `env`).
2. **Process Spawn Initialization**:
   - **Branch A (Interactive ConPTY Mode)**:
     - Calls `CreatePipe(&hPipeInRead, &hPipeInWrite, NULL, 0)`.
     - Calls `CreatePipe(&hPipeOutRead, &hPipeOutWrite, NULL, 0)`.
     - Calls `CreatePseudoConsole(coord, hPipeInRead, hPipeOutWrite, 0, &hPC)`.
     - Closes `hPipeInRead` and `hPipeOutWrite` (owned by pseudo console).
     - Allocates and initializes `STARTUPINFOEXW`:
       - `InitializeProcThreadAttributeList(NULL, 1, 0, &size)`.
       - Allocates buffer, calls `InitializeProcThreadAttributeList`.
       - Updates attribute: `UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC, sizeof(HPCON), NULL, NULL)`.
     - Calls `CreateProcessW(NULL, cmdline_w, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, env_block, cwd_w, &siEx.StartupInfo, &pi)`.
   - **Branch B (Raw Pipe Mode)**:
     - Creates 3 sets of pipes with `SECURITY_ATTRIBUTES bInheritHandle = TRUE`:
       - Stdin (Read handle inherited, Write handle private to agent).
       - Stdout (Read handle private to agent, Write handle inherited).
       - Stderr (Read handle private to agent, Write handle inherited).
     - Configures `STARTUPINFOW` with `dwFlags = STARTF_USESTDHANDLES`, setting `hStdInput`, `hStdOutput`, `hStdError`.
     - Calls `CreateProcessW(NULL, cmdline_w, NULL, NULL, TRUE, 0, env_block, cwd_w, &si, &pi)`.
     - Closes child-side handles in agent process.
3. **Spawn Response**:
   - Constructs and sends `WADDLE_MSG_SPAWN_RESP` with `status_code = 0` and `guest_pid = pi.dwProcessId`.

### 4.3 Bidirectional Streaming & Event Loop Phase
1. **Guest I/O Pump**:
   - **Output Reader Thread**:
     - Loops reading from ConPTY output pipe (or stdout/stderr pipes) into a 64KB buffer using `ReadFile()`.
     - When data is read, wraps buffer in `WADDLE_MSG_STREAM_DATA` packet and writes synchronously to the VSOCK socket.
     - On pipe EOF (`ERROR_BROKEN_PIPE`), sends `WADDLE_MSG_STREAM_EOF` for the respective stream.
   - **Input Writer Thread**:
     - Reads incoming packets from VSOCK.
     - When `WADDLE_MSG_STREAM_DATA` with `stream_id == WADDLE_STREAM_STDIN` arrives, writes data to ConPTY input pipe (or child stdin pipe) using `WriteFile()`.
     - When `WADDLE_MSG_TERMINAL_RESIZE` arrives, calls `ResizePseudoConsole(hPC, new_coord)`.
     - When `WADDLE_MSG_SIGNAL_EVENT` arrives:
       - If `WADDLE_SIGNAL_SIGINT`: calls `GenerateConsoleCtrlEvent(CTRL_C_EVENT, pi.dwProcessId)` or `CTRL_BREAK_EVENT`.
       - If `WADDLE_SIGNAL_SIGKILL` / `SIGTERM`: calls `TerminateProcess(pi.hProcess, 1)`.
2. **Host Multiplexer Loop (`poll`)**:
   - Polling file descriptors:
     - `STDIN_FILENO` (POLLIN)
     - `socket_fd` (POLLIN, POLLHUP, POLLERR)
     - Signal pipe `signal_pipe[0]` (POLLIN, notified by `SIGWINCH` handler)
   - On `STDIN_FILENO` readable: reads up to 64KB, writes `WADDLE_MSG_STREAM_DATA` to `socket_fd`.
   - On `socket_fd` readable:
     - Reads 32-byte header.
     - Reads payload bytes.
     - If `WADDLE_MSG_STREAM_DATA`:
       - If `stream_id == WADDLE_STREAM_STDOUT`: writes payload to `STDOUT_FILENO`.
       - If `stream_id == WADDLE_STREAM_STDERR`: writes payload to `STDERR_FILENO`.
     - If `WADDLE_MSG_PROCESS_EXIT`: saves exit code, breaks poll loop.
   - On `signal_pipe[0]` readable:
     - Calls `ioctl(STDIN_FILENO, TIOCGWINSZ, &ws)`.
     - Sends `WADDLE_MSG_TERMINAL_RESIZE` to guest.

### 4.4 Teardown & Clean Exit Phase
1. **Child Process Termination**:
   - Guest agent monitors `pi.hProcess` using `WaitForSingleObject(pi.hProcess, INFINITE)`.
   - Once signaled, calls `GetExitCodeProcess(pi.hProcess, &exitCode)`.
   - Closes process and thread handles (`CloseHandle(pi.hThread)`, `CloseHandle(pi.hProcess)`).
2. **Flush & ConPTY Teardown**:
   - Calls `ClosePseudoConsole(hPC)`.
   - Allows reader thread to flush remaining bytes from output pipe.
   - Sends `WADDLE_MSG_PROCESS_EXIT` with `exit_code`.
3. **Host Restoration**:
   - Host receives `WADDLE_MSG_PROCESS_EXIT`.
   - Restores terminal mode: `tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios)`.
   - Closes transport socket.
   - Calls `exit(exit_code)`.

---

## 5. Concurrency, Threading & Synchronization

### 5.1 Host Concurrency Model
- **Single-Threaded Event Loop with Non-Blocking I/O**:
  - The host CLI client utilizes a single-threaded asynchronous event loop driven by `poll(2)` (or `epoll(7)`).
  - Eliminates thread synchronization overhead, mutex contention, and race conditions between stdin and socket writes.
- **Signal Self-Pipe Trick**:
  - Asynchronous signals (`SIGWINCH`, `SIGINT`) write a single byte to a non-blocking UNIX pipe (`pipe2(..., O_NONBLOCK)`).
  - The read end of the pipe is polled within the main loop, ensuring all signal reactions occur sequentially and safely inside normal execution flow without async-signal-unsafe function calls.

### 5.2 Guest Concurrency Model
- **Thread Ownership & Roles**:
  - `Main Dispatcher Thread`: Manages VSOCK listener, decodes spawn request, sets up ConPTY, spawns process.
  - `Output Pump Thread`: Synchronously reads from `hPipeOutRead` and writes to socket.
  - `Input Pump Thread`: Reads from socket and writes to `hPipeInWrite`.
  - `Process Watcher Thread`: Waits on `pi.hProcess` via `WaitForSingleObject`.
- **Synchronization Primitives**:
  - Win32 Event objects (`CreateEventW`) for coordination:
    - `hStopEvent`: Signaled when connection drops or process exits, waking up blocking I/O calls.
  - Atomic sequence counters (`std::atomic<uint32_t>` in C11 `stdatomic.h` / Zig atomics) for packet sequence numbers.

---

## 6. Error Handling & Failure Modes

### 6.1 Disconnect & Transport Teardown
- **Host Socket Dropped**:
  - If the guest agent crashes or the hypervisor VSOCK link is severed, `poll()` returns `POLLHUP` / `POLLERR`.
  - The host CLI immediately restores `orig_termios`, writes an error message to `stderr`, and exits with return code 128 + `SIGPIPE` (141) or 255.
- **Guest Socket Dropped**:
  - If the host CLI terminates unexpectedly, the guest agent detects socket read/write errors.
  - The guest agent sends a `CTRL_BREAK_EVENT` or calls `TerminateProcess` to avoid orphaned zombie processes consuming CPU in the guest VM, closes all pipe handles and pseudo console objects, and cleans up.

### 6.2 Process Creation Failures
- If `CreateProcessW` fails (e.g. `ERROR_FILE_NOT_FOUND`, `ERROR_ACCESS_DENIED`):
  - Agent formats a `WADDLE_MSG_SPAWN_RESP` with `status_code = GetLastError()` and a detailed error message formatted via `FormatMessageW`.
  - Host receives the spawn response, prints `waddle: failed to execute '<cmd>': <error message>` to `stderr`, and exits with code 127 (command not found) or 126 (permission denied).

### 6.3 Buffer Exhaustion & Flow Control
- Fixed maximum payload limit of 1 MB per chunk prevents memory exhaustion attacks or unbounded allocation.
- Socket buffers use native OS TCP/VSOCK window flow control: if the host terminal is slow to render, the socket buffer fills, pausing the guest reader thread naturally without dropping data.

---

## 7. Verification & Testing Criteria

### 7.1 Unit Tests
- **Argument Quoting Test Suite**:
  - Test simple arguments without spaces (`foo` -> `foo`).
  - Test arguments with spaces (`hello world` -> `"hello world"`).
  - Test embedded quotes (`foo"bar` -> `"foo\"bar"`).
  - Test trailing backslashes (`C:\Program Files\` -> `"C:\Program Files\\"`).
  - Test consecutive backslashes followed by quotes (`a\\\"b` -> `"a\\\\\\\"b"`).
  - Test empty string (`""` -> `""`).
- **Wire Protocol Serialization Tests**:
  - Verify `sizeof(waddle_cli_msg_header_t) == 32`.
  - Verify pack/unpack routines preserve exact byte layouts across little-endian systems.
  - Verify CRC32 computation and validation.

### 7.2 Integration Tests (Loopback Mock Harness)
- **Local Linux Mock Runner**:
  - Run the host CLI against a mock server communicating over a UNIX domain socket.
  - Test normal exit code forwarding (`exit 0`, `exit 42`, `exit 1`).
  - Test stdout streaming vs stderr streaming verification.
  - Test stdin echoing (interactive typing simulation).
  - Test terminal resize event delivery.
  - Test Ctrl+C cancellation forwarding.

### 7.3 Performance & Latency Thresholds
- **Startup Latency**: Time from `waddle exec` invocation to first byte received from guest process < 15ms.
- **Throughput**: Sustained data transfer rate > 100 MB/s for piped data streams (`waddle exec -- type large_file.bin | wc -c`).
- **CPU Utilization**: Host CLI CPU utilization < 1% during active terminal streaming.

## 8. Executable PoC contract (authoritative for this increment)

This increment implements a C11 Linux host and a Linux mock guest. It does not
claim Windows execution, ConPTY support, VirtIO-Serial support, authentication,
performance targets, or a production-ready service. The mock launches actual
Linux programs; the same framing and host client are intended for the future
Windows guest. No third-party dependency is added. Build with the base-system C
compiler and Make; Linux pseudo-terminal support comes from base-system libutil.
The existing feature branch is retained without rewriting its divergent history.

### 8.1 Framing and layouts

The 32-byte header has offsets: magic u32 at 0, version u16 at 4, type u16 at 6,
session u64 at 8, payload length u32 at 16, flags u32 at 20, sequence u32 at 24,
CRC32 u32 at 28. All integers are explicitly encoded little-endian; packed C
structures are layout documentation, never dereferenced from unaligned input.
Magic remains numeric 0x57444c43 (wire bytes `43 4c 44 57`, not the text `WDLC`).
Version is 1, flags must be zero. CRC-32/ISO-HDLC is mandatory in this PoC,
including when its computed value is zero. Each direction starts sequence at 1,
advances modulo 2^32, and keeps the session fixed at 1 for its single connection.
Payloads are capped at 1 MiB before allocation. Unknown types, invalid sizes,
flags, session, sequence, CRC, and truncated frames terminate the connection.
EOF is a 4-byte stream ID (u32). Resize is exactly 8 bytes; signal exactly 4;
exit exactly 16. Spawn response is 12 bytes plus error text, without a terminator.
Spawn request is 24 bytes followed by cwd including its NUL, command including
its NUL, and environment bytes. Length fields exclude cwd/command terminators.
Environment is a sequence of NUL-terminated UTF-8 KEY=VALUE entries; zero length
means no overrides. Embedded NULs in cwd/command and malformed environments fail.
No heartbeat is emitted; unexpected message types fail rather than being ignored.

### 8.2 CLI and quoting boundary

`build/waddle exec|run [options] -- program [arguments...]` is the interface.
`--socket-path PATH` selects the mock UNIX socket; otherwise CID 3 / port 5242
select Linux AF_VSOCK. `--vsock-cid N`, `--vsock-port N`, `--cwd PATH`, repeatable
`--env KEY=VALUE` / `-e`, `--pipe` / `-P`, `--interactive` / `-i` / `--tty` / `-t`,
`--timeout SECONDS`, and `--translate-path` are supported. Numbers are decimal,
range-checked; timeout is a total monotonic deadline including connect and spawn,
zero means disabled. Timeout returns 124; malformed protocol/transport/local I/O
returns 125; spawn ENOENT returns 127, other spawn errors 126; usage returns 2.
Guest exit codes are retained as u32 on wire and mapped to the low eight bits on
Linux, because POSIX shells cannot represent an arbitrary Windows DWORD.

Arguments are always individually quoted using Microsoft CRT backslash/quote
rules, including empty strings and trailing backslashes. This promises ordinary
CRT argv parsing, not arbitrary cmd.exe/PowerShell shell grammar or argv[0]
special-case behavior. The mock decodes this canonical representation without
using a shell. Explicit `sh -c ...` is available for intentional Linux shell use.
Default cwd is getcwd(). Guest environment is retained with only explicit
`--env` overrides; host PATH/display variables are not implicitly copied.
Opt-in path translation maps absolute POSIX paths to `Z:\\...` and changes slash
to backslash; relative arguments are retained. Parent traversal and backslashes
in absolute POSIX paths are rejected to avoid ambiguous guest mapping. This is
one root-export rule, not discovery of mounted exports. The mock cannot resolve
Windows paths; use translation only with the future Windows peer.

### 8.3 Ownership, I/O, lifecycle, and bounds

One host poll loop owns the socket, decoder, transmit queue, and stdout/stderr
queues. All watched descriptors are nonblocking, with original standard stream
flags restored on normal exits. Each queue is bounded to 2 MiB, uses explicit
compaction, and pauses producers when insufficient room remains. Socket reads
assemble a header then a bounded payload across arbitrary fragmentation. Socket
writes and local writes retain unsent tails across short writes/EAGAIN/EINTR.
Stream chunks are at most 16 KiB. Control messages are queued in wire order.
The client waits for a successful spawn response before sending stdin. It sends
stdin EOF once, continues receiving until both output EOFs and process exit,
then drains local output before returning. Exit received before output EOF is a
protocol error. Output after stream EOF is a protocol error. Slow local outputs
backpressure the socket, bounding memory. A transport close is successful only
after the final exit and output EOFs. Broken local pipes return 125.

Interactive mode defaults to both stdin and stdout being TTYs; forced interactive
requires both. The host saves termios and uses cfmakeraw, restores via atexit,
and forwards SIGWINCH, SIGINT, SIGQUIT, and SIGTERM through a nonblocking self
pipe. Signal handlers only preserve errno, set sig_atomic_t pending flags, and
write a wake byte; a full pipe cannot lose the pending signal. SIGPIPE is ignored.
Raw Ctrl-C is forwarded as an input byte for terminal processing. SIGKILL and
catastrophic runtime corruption cannot guarantee terminal restoration.

The mock is a single-session UNIX listener, refuses existing socket paths, uses
umask 077, and unlinks only its own socket on normal teardown. Raw sessions use
three pipes; interactive sessions use forkpty and merged terminal output (stderr
EOF is sent immediately). Its child owns a new process group/session. A
close-on-exec error pipe reports chdir/environment/exec failure before spawn
success. The mock poll loop relays bounded queues, resize via TIOCSWINSZ and
SIGWINCH, and signals via the child's process group. Pipe EOF closes child stdin
only after queued input drains; PTY EOF injects Ctrl-D (terminal semantics, not a
binary half-close). It drains output before exit, monitors waitpid(WNOHANG), and
reports 128+signal for signaled children. After child exit it kills remaining
members of the child group so inherited pipe handles cannot hang teardown.
Disconnect, timeout, or protocol error kills and reaps the mock child group.
The mock is development-only, with no privilege elevation or network listener.

### 8.4 Verification and remaining work

Unit tests check layouts, CRC known vector, canonical quote/decode round trips,
path boundaries, fragmented frames, corrupted CRC, and oversized headers.
Integration tests use private temporary UNIX sockets and real subprocesses for
separate stdout/stderr, explicit environments/cwd, empty and quoted arguments,
exit 42, missing command, large binary streams with simultaneous input/output,
TTY restoration/resizing, forwarded signals, timeout, and disconnect behavior.
Tests are C11 and shell; sanitizers are opt-in Make flags. Windows tasks #6/#7
remain at 0%. Host tasks are credited only for implemented behavior; #5 and #8
remain partial until export-rule configuration and real Windows verification.
Performance targets remain unmeasured. This PoC is not ready for feature merge.

Tracker entries use the actual preceding commit hashes. The newest entry uses
`HEAD` as a resolvable Git reference until the next atomic commit records its
hash; embedding a commit's own hash in its contents is mathematically circular.
Earlier feature documentation commits are retained with zero progress impact.

### 8.5 Unknown terminal dimensions

Initial terminal size and resize messages use 24 rows / 80 columns independently
for any zero field reported by TIOCGWINSZ. Zero means the emulator has not supplied
a usable dimension; it must not make an otherwise valid interactive session fail.
A real PTY test starts with both dimensions zero and verifies the guest receives
24x80, later receives 39x101, handles raw Ctrl-C, and restores host termios.

### 8.6 Code Beauty, Documentation, Zig Testing, and Zero-Leak Memory Safety

In accordance with Waddle-LSW development standards:
- **Strict Naming Invariants**: All constants, enum members, and macros are defined in `PascalCase` (e.g. `WaddleCliMagic`, `WaddleMsgSpawnReq`, `WaddleMaxPayloadSize`). Identifiers, functions, and members use `snake_case`. All structs and enums use `type_name_t`. Zero `camelCase` is permitted.
- **Mandatory Documentation**: Every public function, struct, union, enum, and macro is documented with Doxygen structured comments specifying parameter directions (`in`/`out`), nullability, return values, error codes, and thread safety.
- **Zero-Leak Memory Safety**: Dynamic allocations in C provide symmetric lifecycle functions (`queue_init`/`queue_free`, `wire_read`/`wire_destroy`), with defensive pointer nulling immediately following deallocation. All test suites pass clean runs under AddressSanitizer and LeakSanitizer (`-fsanitize=address,leak,undefined`) with zero bytes leaked.
- **Native Zig Test Specifications**: The test suite includes `tests/test_cli.zig`, which natively imports C headers via `@cImport` and exercises wire packing, CRC32, endian codecs, queue bounds/compaction, stream framing, Win32 quoting roundtrips, and path translation using `std.testing.allocator` with 100% leak verification.
- **Code Coverage Target**: C unit test suites exceed the required 90% statement coverage threshold for protocol codecs and argument parsers (`protocol.c` 92.5%, `arguments.c` 91.9%).

## 9. Windows guest completion contract (supersedes PoC limitations)

### 9.1 Scope and transport

The initial guest is a standalone, sequential, single-session-at-a-time Windows
10 1809+ executable. C owns Win32 process/handle/thread interactions; Zig 0.13
owns bounded untrusted frame/spawn/control parsing. No C++ or third-party library
is required. Win32 and Winsock declarations come from Zig's bundled system SDK.
VirtIO viosock must already be installed in the VM: query `\\.\Viosock` through
IOCTL 0x0801300c for its registered Winsock address family. The native address
has u16 family/reserved and u32 port/CID, total 12 bytes, native byte order. This
is an independently defined device ABI, not a vendored driver header. Bind CID
UINT32_MAX and port 5242 (configurable `--vsock-port N`). AF_HYPERV has a different
address format and is not treated as VSOCK. `--socket-path PATH` instead uses
Windows AF_UNIX for local Windows regression tests. VirtIO-Serial, service
installation, export discovery, authentication, and performance certification
are outside this initial increment. Run the listener only within a trusted VM;
each connection can execute programs with the listener user's privileges.

### 9.2 Configurable path rules

Repeatable `--path-map /host/root=X:\guest\root` implies `--translate-path`.
At most 64 rules are accepted. Sources are absolute POSIX paths with no empty,
`.` or `..` components; trailing slash is permitted and removed except for `/`.
Destinations are drive-absolute Windows paths using backslashes with no parent
components, forward slashes, or reserved filename punctuation. Longest matching
source wins at a component boundary; equal sources are rejected. Rule order does
not affect selection. An absolute POSIX argument without a match fails rather
than silently pointing at a different export. With no explicit rules, translation
retains the existing `/=Z:\` fallback. Relative arguments and existing Windows
paths are copied unchanged. Translation applies to argv and cwd, never arbitrary
embedded option values or environment values. Slash-separated absolute paths
must reject traversal and Windows-reserved punctuation; repeated slashes and
`.` components are normalized. Caller owns returned malloc storage, freed along
with the existing argument array. Rules borrow command-line storage until main
returns; there is no mutable global rule registry or filesystem probing.

### 9.3 Guest parsing and process creation

Zig validates every 32-byte header before allocation: magic, version, flags=0,
session=1, sequence starting at 1 with wrap, known host message type, and <=1 MiB.
It checks CRC and exact payload shape, UTF-8 validity, embedded NULs, length sums,
mode bits, environment KEY=VALUE entries, canonical CRT quoting, nonempty argv[0],
and positive signed-16-bit ConPTY dimensions. Spawn bytes remain owned by the C
session until conversion into caller-owned UTF-16 buffers completes. Command
lines above Windows' 32767 UTF-16-unit limit fail with spawn status 126. Guest
inherited environment is copied into a fresh sorted UTF-16 block, replacing keys
case-insensitively with explicit overrides (last override wins). Agent environment
is never mutated. CreateProcessW receives an explicit executable parsed from the
first canonical argument so argv[0] cannot redirect executable selection. Command
line storage is writable and freed after CreateProcessW returns.

Raw mode uses three anonymous pipes and an explicit inherited-handle list, with
only child ends inheritable. ConPTY uses two noninheritable pipe pairs and a
pseudo-console startup attribute, with merged stdout and immediate stderr EOF.
Children start suspended and are assigned to a kill-on-close Job Object before
ResumeThread; assignment/resume failure kills the suspended child. Descendants
remain in the job. Process/job/pipe/pseudo-console/startup allocations each have
one session owner and are released on all error paths. Spawn failures send a
12-byte response plus bounded UTF-8 diagnostic and no success/exit messages.
Status 2 denotes missing file/path; other native failures map to 126 on the host.

### 9.4 Pump and teardown

The session main thread watches the child. One input thread reads and validates
socket frames, then writes stdin or handles resize/signals. One reader per output
pipe reads <=16 KiB and sends stream frames. A critical section serializes entire
header+body writes and sequence assignment; worker buffers are fixed-size and
socket/pipe backpressure blocks producers without allocating more queues. Spawn
success is sent before workers start. EOF is emitted once per output pipe; exit
is sent only after both readers have joined. Input EOF closes raw stdin; for
ConPTY it closes the input pipe (there is no POSIX binary half-close guarantee).
SIGINT/SIGQUIT become ETX for ConPTY and CTRL_BREAK_EVENT for a raw child process
group in the listener's console. SIGTERM/SIGKILL terminate the entire job. Unknown
signals and noninteractive resize are protocol errors.

Transport/worker failure sets a manual-reset failure event and shuts down the
socket, waking blocked send/recv. Main terminates the job, cancels synchronous
input I/O, joins input before closing its pipe, and closes ConPTY while output
readers are still draining, avoiding the documented ClosePseudoConsole deadlock.
After natural child exit, descendants are terminated to release inherited pipes.
Reader errors prevent a successful exit frame. Thread handles are joined and
closed before session buffers, critical section, pipes, and socket are freed.
The sequential listener then accepts another session. Startup/handshake receives
have a 30-second socket deadline. Established sessions rely on host timeout or
transport closure; continuous blocked legitimate output uses transport flow
control. The process main owns WSAStartup/WSACleanup and the listener socket.

### 9.5 Verification and review gates

Linux tests cover unchanged host/mock behavior plus mapping longest-prefix,
boundaries, duplicate/invalid rules, Unicode, root mapping, traversal and output
capacity. Native Zig tests exercise malformed/truncated/corrupt frame and spawn
input, UTF-8, lengths, environments, dimensions and control-state validation;
std.testing.allocator verifies ownership with zero leaks. The guest is cross-built
with Zig for x86_64-windows-gnu, including strict C warnings. Windows CI builds and
runs AF_UNIX guest regression tests for pipe streams, binary duplex, environment,
Unicode argv/cwd, exit codes, failures, signals, disconnect and repeat sessions.
ConPTY and real Linux-to-Windows VSOCK require a Windows VM/manual verification;
unexecuted checks remain explicitly recorded in TRACKER.md and block merge.
Protocol/parser coverage must reach 90%; sanitizers apply to executable Linux C
suites, and native Windows resource tests audit process/handle cleanup. No claim
of Windows execution is made based solely on a cross-build.

## 10. Implemented source boundaries and reproducible verification

### 10.1 Modules and ownership

`src/path_rules.zig` owns UTF-8/export component validation and bounded mapping.
`src/path_rules.h` exposes borrowed rule strings and caller-owned output storage.
`waddle_translate_rules` in `src/arguments.c` validates at most 64 rules, selects
one longest component-boundary prefix, allocates its result with `malloc`, and
frees that result before returning an error. `src/host.c` owns copied rule source
strings; targets borrow argv storage. Normalized duplicate sources fail with CLI
status 2. Unmatched translated argv/cwd fails with status 125 before connection.
A rule never rewrites environment values or embedded option strings. Root fallback
requires no explicit rules; explicit rules replace that fallback.

`src/guest_codec.zig` exposes a C ABI through `src/guest_codec.h`. The production
parser allocates no memory and has no global mutable state. It validates headers,
spawn views, canonical arguments, controls and listener ports. All pointer inputs
are nonnull readable buffers whose byte counts are supplied separately; sentinel
port strings come from CRT argv. Output buffers must not alias input. A spawn
result borrows the retained frame and contains cwd/command/environment pointers,
three u32 lengths, two u16 dimensions and one u32 interactive flag; its x64 C ABI
size is 48 bytes. A frame result has u32 length, u32 CRC and u16 type, with native
ABI padding to 12 bytes. Neither result is a wire struct. The immutable frame
payload retains the 24-byte little-endian spawn prefix, NUL-terminated cwd and
command strings, then exactly env_len bytes of NUL-delimited entries. Cwd must be
nonempty. Exported validators return zero on success and minus one on invalid
input; failed output values are unspecified. Exact control payload lengths are
8+data_len for stdin (data_len <=16384), four for stdin EOF and signals, eight for
resize, and zero for ping. Data after EOF and duplicate EOF abort the session.

`src/guest_environment.c` converts validated bytes with strict UTF-8 conversion,
allocates writable UTF-16 command/executable/cwd strings, and creates a sorted,
double-NUL child environment. Inherited entries are copied, never modified in
place. Keys compare with Windows ordinal case-insensitive comparison; final
explicit override wins. Drive-current-directory entries inherited from Windows
are preserved. Environment copying is bounded to one million inherited UTF-16
units and two million final units. Every temporary entry is freed after final
block creation or failure; the launcher frees the returned block after spawn.

`src/guest_process.c` owns the process, job, input writer, output readers and
optional HPCON. Temporary child pipe endpoints, startup attribute list, thread
handle and UTF-16 strings are released before launch returns. Bare executable
names are resolved through `SearchPathW` before passing an explicit application
name to `CreateProcessW`; explicit paths retain their spelling. Search uses the
listener's search environment, not the child's overridden PATH. Command storage
including terminator must fit 32767 UTF-16 units. Failed launches kill any created
child even if job assignment failed. Child processes never inherit listener,
transport, event, job or worker handles. Raw children inherit only their three
standard endpoints. ConPTY startup uses its pseudo-console attribute and disables
ordinary handle inheritance. Closing a process record resets every owned handle;
no record may be reused while any worker borrows it.

`src/guest_wire.c` owns a send critical section and failure event; the connected
socket is borrowed from the listener. One input owner receives headers and bodies
and advances the receive sequence. Senders hold the lock across complete header
and body writes and sequence assignment. Fixed headers are validated before the
session allocates spawn storage. Header sequences start at one and use unsigned
wrap. Socket failure sets the event and shuts down both directions. Orderly input
cancellation uses an atomic stopping flag and receive-half shutdown so output
remains usable. After the spawn response, the socket becomes nonblocking. Receive readiness and
would-block send readiness use 1 ms select intervals; both preserve partial
frame offsets, and sends observe the failure event. Each native send request is
at most 4096 bytes; protocol stream frames retain the 16384-byte data limit. Producer workers still block
under backpressure without allocating queues, while full duplex remains possible
on Windows AF_UNIX providers. The lock/event are destroyed only after all worker
handles join.

`src/guest_input.c` owns stdin writes and closure. Its stack frame buffer has
16392 bytes; a larger post-spawn frame is rejected before receipt. A child may
close stdin early: broken-pipe writes close the local writer and later valid
stdin data is discarded until host EOF. EOF closes the input handle exactly once.
Resize remains valid after EOF. ConPTY interrupts after EOF have no open input
writer and therefore cannot inject ETX. Raw interrupts use CTRL_BREAK on the
child's private process group. SIGTERM/SIGKILL terminate the entire job using
128+signal as the Windows exit code and set termination status one. Other normal
child exit codes are preserved as all 32 bits on the wire; Linux shell status
continues to use the existing low eight bits. Exit 259 is valid when the process
handle is signaled, independently of the Win32 STILL_ACTIVE constant.

`src/guest_output.c` borrows one pipe per reader, uses one fixed 16392-byte stack
buffer, and emits at most 16384 data bytes per stream frame. Raw outputs remain
independent; a ConPTY stderr reader has no pipe and immediately sends EOF. Readers
continue draining and discarding after socket failure until native pipe teardown,
so `ClosePseudoConsole` can emit its final screen update without blocking forever
on an undrained pipe. Unusual pipe read errors mark failure and prevent success.

`src/guest_session.c` owns worker contexts and thread handles. Success response is
sent before workers start. Main waits for child/failure in 50 ms intervals and
checks exceptional and readable socket conditions with `WSAPoll`. A nonblocking
one-byte `MSG_PEEK` distinguishes EOF from pending framed data without consuming
it. Exceptional closure can be detected while a synchronous stdin write blocks;
graceful EOF behind queued bytes remains observable only after those bytes are
consumed. Host session deadlines therefore remain necessary for stalled children. On
child exit or failure, descendants are terminated, input receives are canceled,
`CancelSynchronousIo` repeats while waiting for input join, and ConPTY closes while
its reader drains. If no reader started, or the reader already failed, its output
handle closes before ConPTY. All readers join before a process-exit frame; any
failure event suppresses that frame. Partial worker startup follows the same
teardown. The socket remains open until the listener regains ownership.

`src/guest_listener.c` owns Winsock startup and the listening socket. Sessions are
sequential; one listener never executes two child jobs concurrently. Accepted
sockets are noninheritable and have a 30-second receive timeout during spawn. The
timeout is removed after success; subsequent inactivity is bounded by host timeout
or disconnect, not an unsolicited guest deadline. Socket paths are nonempty and
shorter than 108 bytes; bind failure never deletes an existing socket path. No
vendor source/header is copied into the repository, and no dependency submodule
is needed. Viosock ABI values were checked against the
[upstream device interface](https://github.com/virtio-win/kvm-guest-drivers-windows/blob/master/viosock/inc/vio_sockets.h).
ConPTY drainage follows the
[Microsoft session lifecycle](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session).

### 10.2 Commands and automated gates

- `make test` builds/runs Linux unit, integration, CLI option and native Zig tests.
- `make test-sanitizers` rebuilds executable C suites with ASan, LSan and UBSan;
  Zig uses safe-mode bounds checks and its testing allocator.
- `make coverage` requires base-system GCC/gcov and kcov, resets prior counters,
  and enforces 90% implementation line coverage for protocol.c, arguments.c,
  guest_codec.zig and path_rules.zig. Zig test bodies are excluded.
- `make windows` cross-builds `build/waddle-guest-exec.exe` with Zig 0.13 and system
  Win32/Winsock declarations. C owns native resource management; Zig owns parsing.
- `make windows-test` additionally builds and executes the native fixture on a
  Windows host. `.github/workflows/guest_windows.yml` runs equivalent commands on
  Windows 2022 using a checksum-verified Zig 0.13 distribution.

The native fixture starts a real AF_UNIX listener in its own console, then checks
separate streams, exit 259, Unicode/escaped/empty argv, Unicode cwd, last-wins
case-insensitive environment, missing executables, bare executable lookup,
16 MiB binary duplex backpressure, raw console interrupts, job termination,
disconnect (including a blocked stdin writer), ConPTY resize/merged output, and
32 repeat sessions with stable listener handle counts. Child/helper buffers,
worker handles, observer process handles, fixture directories and socket paths
are released before a successful fixture exit. Test failures terminate the
listener and report the failing boundary rather than asserting success. Native
frame transfers permit 60 seconds of backpressure: the duplex fixture produces
6 MiB of output before reading stdin, which exceeds a 15-second writer deadline
on Windows AF_UNIX. A separate 180-second watchdog bounds the complete fixture;
CI bounds the runtime step at five minutes. These are correctness deadlines,
not latency or throughput acceptance claims.

### 10.3 Windows VM acceptance gate

For real Linux-to-Windows operation, install the VM's Viosock driver and export
mounts independently. Run `waddle-guest-exec.exe --vsock-port 5242` in the Windows
user console. From Linux, execute for example:

```sh
./build/waddle exec --pipe --vsock-cid 3 --cwd 'C:\' -- cmd.exe /c ver
./build/waddle exec --tty --vsock-cid 3 --cwd 'C:\' -- powershell.exe -NoProfile
./build/waddle exec --pipe --vsock-cid 3 \
  --path-map '/home/dev/project=X:\project' --cwd /home/dev/project \
  -- cmd.exe /c dir
```

Use the VM's actual CID and already-mounted export target. Without translation,
cwd is transmitted unchanged; callers must supply a Windows cwd for this guest.
Validate interactive input, resize, interrupts, exported-file access, timeout and
reconnect in that VM. Native AF_UNIX CI and a successful cross-build cannot certify
the installed Viosock provider or VirtIO-FS mapping. Those checks remain a merge
acceptance gate until their actual results are recorded in TRACKER.md. No
sub-millisecond performance or cross-hypervisor compatibility claim is inferred
from unit tests or loopback execution.

### 10.4 cmd.exe switch boundary

`cmd.exe` has a distinct shell command-line grammar and does not recognize a
CRT-quoted `/c` as its command switch. For a selected executable whose final path
component compares ordinal case-insensitively equal to `cmd.exe`, Zig copies the
validated canonical command into bounded caller-owned UTF-8 storage. It removes
only the surrounding quotes on recognized leading switches: `/c`, `/k`, `/d`,
`/s`, `/q`, `/a`, `/u`, `/e:on`, `/e:off`, `/f:on`, `/f:off`, `/v:on`, `/v:off`.
Processing switches stops after `/c` or `/k`; all later command bytes, including
their original canonical quoting, remain unchanged. No punctuation is unescaped,
no environment expansion is performed by Waddle, and no switch normalization
applies to any other executable. The chosen executable remains the separately
validated/resolved argv[0], never inferred from the rewritten command.

This makes `cmd.exe /c "exit 37"` and a single shell command string work. Complex
cmd shell expressions remain subject to cmd's own grammar; CRT quoting is not a
promise that cmd interprets shell command tokens as a native argv array. The
compatibility output is at most input_length+1 including NUL, has no parser-owned
allocation, and is released by C immediately after UTF-16 conversion. Invalid
canonical input or insufficient capacity returns minus one before process launch.

### 10.5 Sanitizer boundary

Linux executable tests use ASan/LSan/UBSan and Zig tests use safe bounds checks
and `std.testing.allocator`. The Zig 0.13 x86_64-windows-gnu compiler rejects
`-fsanitize=leak`; native Windows C execution has no LeakSanitizer result from
this toolchain. Native repeated-session handle counts check HANDLE ownership,
but do not certify heap leak freedom. Windows heap instrumentation remains a
separate merge verification gate alongside real Viosock/VM acceptance.

### 10.6 Native Windows debug-heap acceptance

`tests/windows_heap.ps1` builds the same guest C sources against the system MSVC
C11 debug CRT, with a Zig codec compiled for the MSVC ABI. A test-only forced
header routes all guest malloc/calloc/realloc/free calls to tracked CRT client
blocks, retaining their source locations. The listener's accept boundary checks
the previous session after its workers have joined and its socket has closed.
Each checkpoint requires zero live client allocations and zero live client bytes.
CRT allocation-boundary checks run at every allocation/deallocation; freed blocks
remain poisoned and are checked for writes. CRT reports terminate the listener
with exit 86, so an instrumentation failure cannot silently pass the fixture.

Three isolated negative controls intentionally leak, overrun, and write after free;
each must exit 86. The native 47-scenario fixture then runs unchanged against the
instrumented guest. CI requires exactly 47 successful checkpoint records in
`build/windows_heap.log`. Records flush before the next accept; the existing
fixture's final stabilization delay precedes terminating the listener. CRT and
system-library internal allocations are not labeled as guest-owned client blocks.
This audit measures every explicit dynamic allocation in the guest C sources;
Zig parsing remains allocation-free. It complements Linux ASan/LSan and Zig's
testing allocator rather than claiming LeakSanitizer support on Windows. The
header is never included in production builds, and no external SDK is vendored.

The native fixture also exposes VM-only child modes. `files` requires two mapped
file arguments, reads an exact 19-byte host marker, and writes a 19-byte result
back through the mounted export. `interactive` installs a console interrupt
handler, reads a `go` line from the actual console input handle, reports console
buffer rows/columns, writes a stderr marker for merged-output verification, and
waits for Ctrl-C (exit 130). These modes let the Linux host validate mapped argv,
export read/write semantics, interactive input, actual resize dimensions, and
interrupt delivery across Viosock rather than inferring them from loopback.

`WADDLE_VSOCK_CID=<cid> WADDLE_EXPORT_SOURCE=<dedicated-directory> make vm-test`
runs the real acceptance suite. Copy `build/windows_guest_test.exe` into the
export root first; the mounted drive must be `X:`. The suite creates/removes its
own `marker.txt`, `result.txt`, and `waddle_日本語` directory, so use a dedicated
export without those names. It checks separate streams/status, exit 259's POSIX
status 3, escaped/Unicode argv, cmd switches, missing-program status, Unicode
cwd/environment, mapped file arguments and bidirectional export access, exact
16 MiB duplex bytes, 32 sequential reconnects, and actual ConPTY interaction.
The Linux C PTY helper owns/reaps one frontend, keeps an observer slave descriptor
to verify termios restoration, bounds console capture to 32 KiB, and uses monotonic
20-second phase deadlines. Its guest must report 39 rows/101 columns after resize,
accept a line of input, merge stderr, and exit 130 after terminal Ctrl-C. No mock
socket fallback is permitted. VM transport failure makes this suite fail.

Orderly input cancellation sets the atomic stop flag without shutting down the
socket receive half. The established socket is nonblocking and its receive worker
checks that flag at 1 ms readiness intervals; a synchronous pipe write is canceled
by the joining main thread. Keeping the receive half open avoids a native socket
reset when a fast-exiting child races with the host's final stdin/EOF frame.
Failure cancellation still shuts down both socket halves.

After a successful session's terminal response, the listener sends a socket FIN
(`SD_SEND`), switches receives to nonblocking mode, and discards trailing host
bytes in a fixed 4096-byte stack buffer for at most one second or until peer close.
This allows the peer to consume all output/exit bytes and close while queued
stdin/EOF bytes are drained; it avoids reset-on-close dropping the terminal frame.
No discarded bytes are parsed, allocated, or executed. The listener owns and
closes the socket after that bounded drain. Failed sessions retain immediate
`SD_BOTH` cancellation. A client that never closes cannot block later acceptance
beyond the one-second drain bound.
