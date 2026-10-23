# Building and using the native manager

On Debian/Ubuntu, install the existing Waddle toolchain (including Zig 0.13) and
these system development packages:

```
libgtk-4-dev libadwaita-1-dev libjson-glib-dev libvte-2.91-gtk4-dev pkg-config
```

Runtime requires GTK4 >= 4.8, libadwaita >= 1.2, JSON-GLib, GTK4 VTE >= 0.70,
a graphical session, and the existing Waddle runtime dependencies. Symbolic SVG
icons require the usual desktop icon theme and SVG loader (`librsvg2-common` on
Debian). Terminal font rendering uses the desktop's fonts. Native dialogs use
AdwAlertDialog when libadwaita >= 1.5 is available and the compatible message
dialog on older installations.

```
make gui                         # build waddle and its adjacent waddle-gui
./build/waddle                   # open/focus dashboard; never boots a device
./build/waddle NAME              # select a registered device; never boots it
./build/waddle gui --device NAME # explicit GUI selection
./build/waddle repl --device NAME
./build/waddle shell NAME        # retained terminal alias
```

Explicit management and execution commands still work without GTK libraries or
a display in the launcher process. `make cli` remains available without GUI
development dependencies. `make gui-install PREFIX=/usr/local` installs the
launcher, GUI, supervisor, desktop entry and icon; it supports `DESTDIR` staging.
This is a host integration install, not a replacement for the complete AV bundle.
Use the existing `make av-distribution` for matched AV/guest/driver artifacts;
that bundle now requires and includes `waddle-gui` beside `waddle`.

Open a device to see Overview, Applications, Terminal, Shared Folders, Graphics &
Display, Logs and Configuration. Mutations remain subject to backend validation
and stopped-device restrictions. The creation wizard defaults to a dry run:
review its output in Activity, then uncheck Preview to submit creation. Starting
a created device is a separate explicit checkbox. A blank disk has no Windows
installation. Import keeps host sharing disabled.

The command palette is Ctrl+K. F5 refreshes observations. Ctrl+Shift+T opens a
terminal for the selected device. Terminal sessions offer copy/paste, literal
search, zoom, clear, reconnect and close. Existing session output and exit status
remain available. External terminals can be kgx, gnome-terminal, konsole,
xfce4-terminal or xterm; a preference can select an executable. Closing the GUI
stops its CLI readers/viewers and PTY sessions, and leaves supervised VMs running.
External terminal windows are independent and can continue after the manager closes.

Saved launchers retain an explicit device, executable, argv text, cwd,
environment, translation and extra execution options. Quoting splits argv only;
there is no local shell expansion. Command mode captures output. Terminal mode
uses VTE. Graphical mode requires a Windows executable path and working managed
AV setup: guest PowerShell Start-Process returns a JSON PID, then the manager
attaches `waddle av run PID --device NAME`. AV/Windows readiness is verified only
by the existing backend and its actual result. Closing its native viewer does
not stop Windows. Probe/latency output appears in Activity.

Settings are in `$XDG_CONFIG_HOME/waddle/gui.ini` (or `~/.config/waddle/gui.ini`),
written atomically with mode 0600. The file contains preferences, pins, launcher
text and bounded activity metadata. Pending batches are never persisted. Activity
and log text are bounded in memory; Save exports a snapshot of the visible output.
Review that text before sharing it. Malformed preference files are preserved:
move the damaged file aside and restart Waddle to recover persistence.

## Backend integration dependencies

| Control | Current boundary |
| --- | --- |
| View Full Desktop | Managed AV takes a process ID only; no desktop capture target/input lease is exposed. GUI explains this and offers per-process AV attachment. |
| Pause / Resume | Daemon protocol has no execution pause/resume operation. GUI explains the unavailable capability and never simulates it by disconnecting. |
| Reboot Windows | Uses `exec --pipe -- shutdown.exe /r /t 0`, after confirmation. It can disconnect the execution session before a normal exit; inspect readiness before retrying. |
| Advanced effective AV configuration | `device config get` currently returns the six base settings. Four supported AV/UEFI/path/GPU fields remain blank until edited; individual backend resets and atomic set remain available. |
| Shared folder editing | Existing CLI lists/tests mappings but has no supported mutation command. GUI opens/copies paths and runs the actual filesystem test. |
| Viewer scaling, fullscreen, focus, audio | Owned by the existing per-process native AV viewer. The manager does not replace presentation/input or claim an active full-desktop connection. |

No hypervisor, software vGPU, driver, guest-agent, storage or AV protocol behavior
was changed for this GUI. Hardware GPU ownership and dedicated VRAM are not implied
by software context or quota settings.
