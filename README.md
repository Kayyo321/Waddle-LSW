# Waddle-LSW (Waddle Linux Subsystem for Windows)

<img align="right" width="180" height="180" alt="Waddle-LSW Logo" src="imgs/logo.png" style="margin-left: 24px; margin-bottom: 20px;">

**Waddle-LSW** is an open-source compatibility layer and subsystem architecture designed to deliver seamless, borderless integration of Microsoft Windows applications directly into Linux Wayland desktop environments.

Unlike traditional full-screen virtual machine viewers, Waddle-LSW tracks individual Windows application windows inside a lightweight guest VM, captures their surfaces with hardware acceleration, transfers frames across domain boundaries via high-throughput IVSHMEM shared memory, and composes them as native Wayland surfaces alongside your Linux applications.

<br clear="right" />

---

## Waddle Usage

<img align="left" width="180" height="180" alt="Waddle-LSW Application Icon" src="imgs/default-window-icon.png" style="margin-right: 24px; margin-bottom: 20px;">

The `waddle` CLI provides command-line control for subsystem lifecycle management, guest process execution, and device configuration:

- **Interactive Terminal**: Run `waddle` to start background services and open an interactive Windows ConPTY terminal session.
- **Process Passthrough**: Execute commands directly inside the Windows guest using `waddle exec -- <command>`.
- **Subsystem Lifecycle**: Manage background daemons and virtualization with `waddle start`, `waddle status`, and `waddle stop`.
- **Filesystem Verification**: Validate VirtIO-FS shared directory mappings and read/write integrity with `waddle fs test`.

<br clear="left" />
<br />

```bash
# Launch interactive Windows shell (auto-starts background subsystem)
waddle

# Execute a command inside the Windows guest
waddle exec -- cmd.exe /c "dir C:\\"

# Inspect subsystem daemon and VM status
waddle status
```

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

---
