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

- **[PROJECT.md](file:///home/dev/Waddle-LSW/PROJECT.md)**: Architectural pillars, subsystem responsibilities (Host Compositor Client, Transport Layer, Guest Agent, CLI Passthrough), IVSHMEM layouts, and language policies (C/Zig with C++ ceiling).
- **[AGENTS.md](file:///home/dev/Waddle-LSW/AGENTS.md)**: Authoritative developer and agent workflow rules, atomic commit standards, branching models, submodule policies, and `impl/` tracker specifications.
- **[CONTRIBUTING.md](file:///home/dev/Waddle-LSW/CONTRIBUTING.md)**: Comprehensive contributor guidelines, toolchain setups, testing and sanitizer harnesses, in-depth Git submodule management, and PR review checklists.
