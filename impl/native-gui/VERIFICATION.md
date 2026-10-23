# Native GUI verification — 2026-10-23

Base: `feature/software-vgpu-slicing` at
`22e86e32fef21e720857199a4d8a9ad58f16823d`.
Feature branch: `feature/native-gui`.
Verified implementation: `1c8131c`; packaging integration: `880eaf4`.

Environment: Debian 13, GCC 14, Zig 0.13.0 (verified upstream SHA256),
GTK 4.18.6, libadwaita 1.7.6, GTK4 VTE 0.80.1 and JSON-GLib 1.10.6.
Builds use the repository C11 warning-as-error flags. Dependencies are system
packages. GUI tests run under a private session bus and Xvfb; no Windows VM is
started by these tests.

## Results

| Command / check | Result and scope |
| --- | --- |
| `make gui` | Builds launcher and native application with warnings treated as errors. |
| `make test` | Passed existing C/Zig unit, daemon, CLI, protocol, storage, fault-injection, shutdown stress and filesystem acceptance tests. Includes 32 real CLI/mock-peer sessions. |
| `make test-sanitizers` | Passed existing CLI suite under ASan/LSan/UBSan and Zig allocator tests. |
| `make coverage` | Passed existing implementation/codec coverage gates; see values below. |
| `make gui-test` | Passed schema/store/stream tests, native GTK workflows, real launcher dispatch and real GApplication forwarding. |
| `make gui-sanitizers` | Passed with zero unsuppressed leaks, memory errors or UB; system-only suppressions are documented below. |
| `GDK_SCALE=2 … build/gui_window_test` | Passed native workflow at double scale with a 2560×1800 Xvfb screen. |
| `make gui-install DESTDIR=… PREFIX=/usr/local` | Staged launcher, manager, supervisor, desktop entry and icon successfully. |
| `python3 tests/av/package.py` | Passed complete-bundle checks, checksums and preservation of existing archives for every missing required input, including the GUI. |
| `ldd build/waddle` | Launcher links libc only; GTK libraries and a display are unnecessary for explicit CLI use. |
| `git diff --check` | Passed. README.md and backend implementation/protocols remain unchanged. |

Coverage results from the existing repository gates:

- `protocol.c`: 92.50% (185/200 implementation lines).
- `arguments.c`: 93.18% (123/132).
- `guest_codec.zig`: 98.56% (137/139).
- `path_rules.zig`: 100% (50/50).
- Storage codec conditional/switch branches: 92.93% (92/99; one compiler panic guard excluded by the existing gate).
- CLI codec conditional/switch branches: 100% (140/140; two compiler panic guards excluded by the existing gate).

The initial default Docker profile rejected kcov's process-personality operation.
Coverage passed after rerunning in a test container with SYS_PTRACE and the
repository CI's unconfined seccomp setting. No source or coverage threshold was
changed to accommodate that environment restriction.

## What the native tests exercise

`tests/gui/backend.c` verifies schema/type/version rejection, ASCII identifiers,
spawn errors, nonzero exits, streamed UTF-8 output, the 2 MiB retention bound,
cancellation, preference round trips/private permissions, malformed-file recovery
protection and exclusion of pending operations from persistent preferences.

`tests/gui/window.c` constructs real native GTK/libadwaita widgets against a
deterministic CLI peer. It exercises device/default observation, running and
malformed profiles, selection preservation, unsaved configuration, native review
and cancellation, invalid creation names, blank-disk creation dry runs and submit,
export to a new directory under a chosen parent, saved argument literalness,
Windows process launch → validated JSON PID → managed AV attachment, partial
batch failure with continuation, spawn failure clearing the remaining batch,
plain log snapshots, live-follow cancellation, selection isolation, shared folders,
dark appearance, palette construction, VTE input/resize/search/zoom/exit output,
and close while a CLI operation and PTY session are active.

`tests/gui/dispatch.py` uses the actual CLI executable with a sibling argv-recording
GUI. It verifies bare/named/explicit dispatch, exact argument forwarding, headless
errors, missing GUI errors, invalid syntax, repl/shell compatibility and unchanged
version/device commands. It creates actual QCOW2 profiles and verifies that GUI
opening creates no supervisor socket.

`tests/gui/instance.py` starts the actual application in a private bus/display,
forwards a second launch to another registered device, verifies a single primary
window and no VM startup, and exercises Ctrl+K, filtered Enter activation and Escape
through X11 keyboard events. Native widgets provide GTK accessibility roles and
labels; an external screen reader was not run.

## Sanitizer interpretation

The adapter/store tests run without leak suppressions. The GTK test uses
`tests/gui/lsan.supp` with slow ASan stack unwinding to identify three specific
system-library allocation paths:

1. Fontconfig's retained font cache (`libfontconfig.so.1`).
2. GTK 4.18.6 temporary mnemonic-label lists (`gtk_widget_list_mnemonic_labels`),
   allocated inside the GTK builder/label path when constructing entry rows.
3. VTE 0.80.1 search selection's GTK clipboard formats builder
   (`gdk_content_formats_builder_new`, 40 bytes in the observed run).

No Waddle function, generic allocator, GLib allocation function or entire GTK/VTE
library is suppressed. All Waddle-owned jobs, callbacks, copied argv, preferences,
JSON results and PTY session storage remain checked. These system allocations are
reported explicitly rather than claimed as zero total process allocations.

## Limits and integration dependencies

The managed AV backend exposes a process capture target, not a desktop capture
target. The daemon protocol has no pause/resume request. These controls show a
precise native capability explanation and never pretend to connect or pause.
Viewer scaling/fullscreen/input/audio remain owned by the existing native AV
viewer. No replacement presentation or remote desktop stack was added.

Actual Windows reboot, driver provisioning, GPU/mdev assignment, native seamless
rendering/audio/input, hardware latency and guest reconnection require the existing
Windows/hardware acceptance environment and were not tested here. The GUI adapter
and its error/status paths were tested with deterministic peers. The lower supported
GTK/libadwaita versions were not installed in this verification environment.

The existing config-get response omits four effective AV fields. The GUI states
that limitation and preserves omitted values unless explicitly edited/reset.
Shared folder editing has no supported existing CLI command and is not fabricated.
See [USAGE.md](USAGE.md) for operation grammar, persistence and backend dependencies.
