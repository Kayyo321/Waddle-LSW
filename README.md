# Waddle-LSW (Waddle Linux Subsystem for Windows)

**Waddle-LSW** is an open-source compatibility layer and subsystem architecture designed to deliver seamless, borderless integration of Microsoft Windows applications directly into Linux Wayland desktop environments.

Unlike traditional full-screen virtual machine viewers, Waddle-LSW tracks individual Windows application windows inside a lightweight guest VM, captures their surfaces with hardware acceleration, transfers frames across domain boundaries via high-throughput IVSHMEM shared memory, and composes them as native Wayland surfaces alongside your Linux applications.

---

## Getting Started & Repository Cloning

Waddle-LSW utilizes **Git submodules** for all external dependencies to guarantee hermetic, reproducible, and offline-capable builds.

### Cloning with Submodules

To clone the repository along with all required submodules:

```bash
git clone --recurse-submodules https://github.com/Kayyo321/Waddle-LSW.git
cd Waddle-LSW
```

### Initializing Submodules on Existing Checkouts

If you already cloned the repository without `--recurse-submodules`, or after switching branches that introduce new dependencies, initialize and update all submodules:

```bash
git submodule update --init --recursive
```

To configure Git to automatically update submodules on branch checkouts:

```bash
git config submodule.recurse true
```

---

## Core Documentation

- **[PROJECT.md](file:///home/dev/Waddle-LSW/PROJECT.md)**: Architectural pillars, subsystem responsibilities (Host Compositor Client, Transport Layer, Guest Agent, CLI Passthrough), IVSHMEM layouts, language policies (C default, Zig for memory safety, C++ solely for compatibility), code beauty naming invariants (`snake_case`, `PascalCase` constants, `type_name_t`, zero `camelCase`), documentation enforcement, zero-leak memory safety policy, and Zig built-in test specifications.
- **[AGENTS.md](file:///home/dev/Waddle-LSW/AGENTS.md)**: Authoritative developer and agent workflow rules, atomic commit standards, branching models, submodule policies, `impl/` tracker specifications, and code beauty / zero-leak quality PR merge checklists.
- **[CONTRIBUTING.md](file:///home/dev/Waddle-LSW/CONTRIBUTING.md)**: Comprehensive contributor guidelines, toolchain setups, deterministic memory architecture, testing with Zig built-in test specifications, code coverage, LeakSanitizer zero-leak gating, in-depth Git submodule management, and PR review checklists.

## Initial CLI passthrough PoC

A working **Linux host + Linux mock guest** now exercises command launch and
framed I/O. The mock executes Linux programs; a Windows guest agent and ConPTY
are still pending. This increment uses C11, Make, and base-system libc/libutil,
with no third-party dependencies.

```bash
make
make demo
make test
```

`make demo` starts a private mock socket, sends stdin across the protocol,
prints the guest's separate stdout/stderr, verifies exit code **42**, and cleans
up. The mock accepts one connection and exits. For manual testing, start the
mock in one terminal with a fresh path in a private directory:

```bash
mkdir -m 700 /tmp/my-waddle-demo
./build/waddle-mock-guest --socket-path /tmp/my-waddle-demo/guest.sock
```

Then run commands in a second terminal:

```bash
printf 'hello through the protocol\n' | ./build/waddle exec --pipe \
    --socket-path /tmp/my-waddle-demo/guest.sock -- /bin/cat
```

Restart the mock for each new command. Both stdin and stdout being terminals
selects interactive PTY mode automatically; `-i` forces it and `--pipe` disables
it. Other options include `--cwd`, repeatable `--env KEY=VALUE`, and a total
`--timeout SECONDS`. Run `./build/waddle --help` for the full syntax. Without
`--socket-path`, the host attempts Linux VSOCK CID 3, port 5242; no Windows peer
exists yet, so the VM path has not been verified. `--translate-path` is an
opt-in mapping to the future Windows `Z:` export and should be omitted with
the Linux mock.

Tests cover byte layouts, CRC, argument quoting, malformed/fragmented frames,
16 MiB of simultaneous binary transfer with slow consumers, signals, deadlines,
and PTY resizing/restoration. To build and run with address/undefined sanitizers:

```bash
make clean
make test CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' \
    LDFLAGS='-fsanitize=address,undefined'
```

Run `make clean && make` afterward to restore a normal build. Changing compiler
flags requires a clean rebuild. The mock returns POSIX error/status values;
Windows error mapping and execution will be validated when that peer exists.
See [the implementation contract](impl/initial-cli-passthrough/IMPL_DESC.md#8-executable-poc-contract-authoritative-for-this-increment)
and [feature tracker](impl/initial-cli-passthrough/TRACKER.md) for verified scope
and remaining work. This is a development PoC, not a completed feature.
>>>>>>> 4ae2c21 (docs(cli): provide a runnable mock passthrough demonstration)
>>>>>>> 5f74ae7 (docs(cli): provide a runnable mock passthrough demonstration)
