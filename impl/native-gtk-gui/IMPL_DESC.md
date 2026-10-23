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

QMP adds no CLI commands or supervisor protocol fields. Its path is derived from
the registered device daemon socket (same directory, qmp.sock). A worker connects
with a finite timeout, negotiates capabilities, ignores bounded asynchronous events,
and validates JSON replies before returning. Each request owns its connection.
Only fixed commands are admitted: query-status, stop, cont, system_reset,
screendump, input-send-event and send-key. Requests use JSON-GLib serialization.
No arbitrary QMP or monitor command entry is exposed.

Full desktop viewing uses the existing QEMU scanout via QMP screendump in a
private temporary directory. A single outstanding capture limits load; frames
are decoded through the system GdkPixbuf loader. This also works at firmware/boot
screens. It shows the emulated display, not the separate per-window AV/VDD surface.
The viewer provides keyboard and absolute pointer input, full screen and explicit
refresh. Managed AV setup/probe/process viewing remain available separately.
Pause/resume use QMP stop/cont, and guest reboot uses system_reset with confirmation;
restart remains the existing graceful stop/start CLI operation. The actual QMP
state is displayed separately from supervisor readiness and is never inferred from
a button press. QMP failures are visible and never treated as success.

## Execution sequences

1. Resolve sibling executables; initialize AdwApplication and native window.
2. Build device list and tabbed details; asynchronously request device list JSON.
3. Select requested name, saved default or first available profile. Opening does
   not boot a device. An unknown explicit name shows a useful error.
4. Capture selected identity and form values into a private argv array on submit.
5. Spawn CLI, consume stdout/stderr, report status, then refresh observations.
6. Desktop tab polls only while visible and enabled; capture/QMP work stays off
   the GTK thread. Selection changes invalidate older frame completions.
7. Closing clears timers, stops owned children, drains asynchronous completions,
   releases widgets/application references, and removes temporary frame files.

## Concurrency and synchronization

GTK callbacks and subprocess output completion execute on the GLib main context.
QMP calls execute in GTask workers using copied paths and request values; workers
never touch widgets. CLI registry locks and daemon lifecycle leases remain the
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
A failed capture leaves the last frame with an explicit error; a selection change
clears it immediately. Temporary frames are local, private, and removed at teardown.

## Verification

Build with repository warning-as-error flags. Test launcher dispatch independently
of a display. Run existing CLI tests against the new entrypoint. Run native GTK
smoke tests under Xvfb and a session bus, including empty/error/multi-device states,
creation/settings argv, cancellation/close and selection preservation. Exercise QMP
against a fake socket and a real local QEMU without KVM/Windows, covering handshake,
events, status, pause/resume, reboot, capture and input. Run GUI-owned paths under
ASan/LSan (system GTK process caches require documented library-only suppressions).
Windows/vGPU hardware rendering performance is not claimed by these integration
tests; the existing VM acceptance gate remains applicable.
