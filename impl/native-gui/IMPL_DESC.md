# Native GTK4/libadwaita GUI

## Title and scope

Implement a native C GTK4/libadwaita device manager on top of the working CLI.
`waddle` opens the manager, `waddle NAME` opens that device in the manager, and
`waddle repl [NAME|--device NAME]` retains the existing interactive terminal.
The `shell` spelling remains a compatibility alias. Explicit CLI commands retain
their grammar, output, transport, validation, locking and lifecycle semantics.
README.md, guest capture protocols, software vGPU and supervisor behavior are
outside this feature. GTK4 >= 4.8, libadwaita >= 1.2 and JSON-GLib are system packages.
No additional vendored dependencies or wire ABI changes are introduced.

## Architecture and interactions

```
waddle launcher -> sibling waddle-gui -> GTK/libadwaita widgets
                                      |-> sibling waddle (argv, no shell)
                                      |-> existing local QMP socket
```

The launcher uses /proc/self/exe to resolve its sibling, preserving argv as
individual strings and environment/XDG paths. CLI-only builds can still run
explicit commands; unavailable GUI dependencies produce a build diagnostic,
and a missing installed GUI executable produces an actionable launch error.
The GUI reads the CLI's versioned JSON list/config results. Other command output
is shown in a bounded, selectable, copyable activity view, with exit status and
errors. No operation silently starts a guest: only explicit start, terminal,
execution and AV actions can start one, following existing CLI behavior.

Device selection is explicit and pinned for each operation. Refresh preserves
selection by name; removed devices clear details. List observation failures retain
the previous list and show an error. Invalid profiles remain selectable for doctor
and removal. Mutations run through existing CLI commands, including dry runs.
Creation supports auto-discovered base disks, explicit base images and blank disks
(which do not contain Windows). Settings cover all ten CLI mutable keys, apply
only edited entries, and preserve backend validation and stopped-device checks.
Tools provide all explicit CLI options through an argv editor in addition to
common native forms. Interactive commands launch a host terminal with an argv
array. No local shell interpolation is used.

## Data structures, protocols and ownership

GUI state is main-thread-owned. GLib objects have explicit references, child
widgets are parent-owned, transient forms own their captured device name and
entry widgets, and asynchronous jobs hold application references until completion.
Each CLI job owns its GSubprocess and stream; output is consumed asynchronously
in bounded chunks and its visible tail is limited. There is one foreground command
slot and one independent device-list observation slot. Long-running log/AV jobs
can be stopped without killing the VM. Closing the application cancels/reaps owned
jobs; it does not shut down devices. JSON is decoded by the system JSON-GLib
parser with type checks at every GUI-facing boundary.

The existing daemon protocol has no pause/resume operation and managed AV accepts
only a process ID, not a full desktop target. These controls remain visible with
precise unavailable reasons. This feature does not add private QMP control,
screenshot-based remote desktop, guest capture changes or hypervisor behavior.
Reboot Windows uses the existing guest execution path with shutdown.exe /r /t 0;
restart subsystem uses the existing graceful stop/start operation. Seamless viewing
uses waddle av run. Viewer fullscreen and input remain owned by that native AV
viewer. A future full desktop backend must expose a managed capture target and
input lease through the existing presentation path before the GUI enables it.

VTE GTK4 is a system dependency. Each embedded terminal owns a PTY child running
waddle repl --device NAME. GUI close sends SIGHUP only to those terminal children;
VM processes remain daemon-owned. Saved launcher and preference files use GLib
GKeyFile in the user's XDG config directory with atomic, private replacement.
Launchers record argv as string lists and retain explicit device identity. Local
shell strings are never executed. The command palette searches registered actions
and displays the selected device in its title. Device pins and recent use are
application preferences, independent of the backend default selection.

## Execution sequences

1. Resolve sibling executables; initialize AdwApplication and native window.
2. Build device list and tabbed details; asynchronously request device list JSON.
3. Select requested name, saved default or first available profile. Opening does
   not boot a device. An unknown explicit name shows a useful error.
4. Capture selected identity and form values into a private argv array on submit.
5. Spawn CLI, consume stdout/stderr, report status, then refresh observations.
6. Stream logs, execution and AV output asynchronously; VTE owns terminal IO.
   Selection changes never relabel results from an earlier operation.
7. Closing clears timers, stops owned children, drains asynchronous completions,
   releases widgets/application references, and removes temporary frame files.

## Concurrency and synchronization

GTK callbacks and subprocess output completion execute on the GLib main context.
All management IO executes in subprocesses; callbacks never block on guest IO. CLI registry locks and daemon lifecycle leases remain the
existing source of truth. Controls are disabled during conflicting foreground
operations. Refresh results are associated with the selection at request time.
No process-global environment changes occur after startup.

## Error handling and recovery

Missing CLI/terminal/display, empty registry, malformed JSON, nonzero exits,
partial batch failure, unavailable QMP, paused VMs and disconnected guests surface
in the UI. Output retention is bounded and pipes continue draining after truncation.
Cancel signals only GUI-owned CLI jobs and waits for them; device processes remain
supervisor-owned. Destructive removal/kill/reboot require a concrete native dialog.
Failed settings/creation keep entered values available for correction.
Unavailable backend capabilities never show success or imply an active connection.

## Verification

Build with repository warning-as-error flags. Test launcher dispatch independently
of a display. Run existing CLI tests against the new entrypoint. Run native GTK
smoke tests under Xvfb and a session bus, including empty/error/multi-device states,
creation/settings argv, cancellation/close and selection preservation. Test saved launchers and preferences with temporary XDG directories, including
malformed files. Run GUI-owned paths under ASan/LSan (system GTK process caches
require documented library-only suppressions). Windows/vGPU hardware rendering
performance is not claimed by these integration tests; the existing VM acceptance
gate remains applicable. Record actual commands and limitations in VERIFICATION.md.
