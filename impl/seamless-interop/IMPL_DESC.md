# Implementation Description: Seamless Interop

Status of this document: contract freeze for TODO #1 of
[TRACKER.md](TRACKER.md). Sections marked **Frozen** are binding wire, memory,
file-format and configuration contracts; changing one after freeze requires a
protocol/schema version bump and a TRACKER commit entry. Sections marked
**Design** fix the mechanism and API choice; their per-task acceptance detail is
extended in the same section by the commit that implements the task, before the
task is marked Done.

### Contract amendment 2026-10-23

The original protocol v1/layout v2 freeze is superseded by **protocol v2 and
layout v3**. Versioned corrections are intentional: v1 peers are rejected by
header decoding; v2 layout mappings are rejected by the future layout-v3
validator. There is no auto-detection or reinterpretation of old mappings.
The existing per-PID AV protocol/layout remain unchanged. The integration
manifest schema stays 1 because these corrections change no persisted record.

Protocol v2 adds optional `min_size` and an optional menu-trigger `serial` on
WindowAnnounce, separates input counters from Wayland serials, and fixes AUMID
key truncation. Layout v3 has SMW3 magic, an exact 4096-byte header with reserved
bytes starting at offset 72, and explicit 64-byte struct alignment. Public C ABI
assertions are in `include/waddle/interop_memory.h`; no layout allocator or
runtime session is implemented by this amendment. Later incompatible changes
still require a version bump. CLI lifecycle/force behavior below is now explicit.

---

## 1. Title & High-Level Scope

### 1.1 What the feature does

Seamless interop makes applications running inside the Waddle Windows guest
behave, from the Linux user's point of view, like native Linux desktop
applications:

1. Every guest application window (top-level windows, owned dialogs, menus,
   tooltips, UAC consent dialogs) appears as its own Wayland surface, managed by
   the Linux compositor like any other window: stacking, focus, Alt+Tab,
   taskbar entry, workspace placement.
2. The window keeps its own Windows-drawn frame (title bar, borders, caption
   buttons). Interacting with that frame drives the Linux compositor: dragging
   the caption moves the window, dragging an edge resizes it, the caption
   buttons minimize/maximize/close it, double-clicking the caption toggles
   maximize. Fullscreen requests work in both directions.
3. All input reaches the application: keyboard (every PC key, layouts, repeat,
   lock keys, IME), pointer (all buttons, high-resolution and horizontal wheel),
   cursor shape, pointer lock/relative motion, touch, pen/tablet with pressure
   and tilt, touchpad pinch gestures.
4. Peripherals: microphone into the guest, per-application audio out of the
   guest, USB device redirection (webcams, wired gamepads, other USB devices),
   Bluetooth/evdev gamepads through a virtual XInput device (dependency
   approval gated, TODO #25), printing to Linux CUPS printers.
5. Clipboard (copy/paste) in both directions for plain text, HTML, RTF, images
   and files (copy and cut), and drag-and-drop of files and text in both
   directions.
6. Each application shows its real icon in the Linux taskbar, dock and
   Alt+Tab switcher.
7. Guest applications are listed in the Linux application menu through
   generated `.desktop` entries (File Explorer included). Launching such an
   entry opens the application window in the guest seamlessly. Shortcuts placed
   on the Windows desktop are mirrored to the Linux desktop.
8. Installing a Windows application (example: Adobe Creative Cloud installer)
   inside the guest automatically creates its Linux `.desktop` entry and icons;
   uninstalling removes them. Installer elevation (UAC) works through one of
   three configurable modes.
9. Double-clicking a `.exe`, `.msi` or `.lnk` file in a Linux file manager
   runs it in the guest. Running `./program.exe` in a Linux terminal runs it in
   the guest through binfmt_misc when the system-level integration is
   installed.
10. Every file, MIME default, autostart entry, guest policy and guest component
    that seamless interop creates is recorded in an integration manifest, so a
    later Waddle install/update/uninstall lifecycle can migrate, repair or
    remove it deterministically and restore what it replaced.

### 1.2 In scope

- New guest component `waddle-guest-session.exe` (session agent).
- New host component `waddle-session` (session client).
- Interop protocol v2 (VSOCK port 5002) and IVSHMEM layout v3.
- New CLI command families: `waddle open`, `waddle integration`, `waddle usb`.
- Daemon protocol additions for guest readiness, session ensure and USB.
- QEMU device additions: explicit network with CUPS forwarding, `qemu-xhci`,
  PipeWire audio backend.
- Export of every configured virtiofs mount and correction of the root path
  fallback.
- `data/` integration templates (desktop entries, binfmt.d rule, polkit action).
- Pinned `submodules/wayland_protocols`.

### 1.3 Out of scope (explicit)

- Changing or removing the existing per-PID AV path (`waddle av run`, AV
  protocol V1-V3 on VSOCK port 5001, `waddle-av-host`, `waddle-guest-av.exe`).
  It stays byte-for-byte compatible; seamless mode is a parallel stack that
  reuses its modules. The two modes are mutually exclusive per device at run
  time (Section 6.10).
- The system package itself (deb/rpm/flatpak). This feature only ships the
  artifacts the future package installs and the `--system` commands it calls.
- GPU API remoting (owned by `feature/software-vgpu-slicing`). Seamless interop
  consumes whatever pixels the WGC/DXGI capture path produces.
- Wintab-only pen applications (only Windows Ink / `WM_POINTER` pen input is
  delivered).
- Touchpad swipe gestures (they stay with the Linux compositor).
- Mirroring Linux window positions into the guest (Wayland gives clients no
  global coordinates; guest positions are guest-internal, Section 2.4).
- Primary-selection (middle-click paste) into the guest: Windows has no
  primary selection. Host primary selection is read only for drag-free middle
  click paste *from* Linux into a focused Waddle text field (Section 3.10.6).
- Hardware-isolated USB or GPU partitioning.

### 1.4 User decisions recorded (2026-10-22)

| Decision | Choice |
|---|---|
| Window frame | Windows' own frame kept (client-side decorations), mapped to Wayland move/resize/state requests |
| `.exe` double-click | Per-user XDG MIME handler controlled by a setting **plus** a system-wide binfmt_misc rule owned by the system installer |
| Peripheral scope | All classes: keyboard/pointer/cursor/clipboard plus touch, pen, gestures, IME, gamepads, drag-and-drop, microphone, webcam, USB, printing |
| Clipboard | Copy/paste interop required in both directions |
| UAC default | The genuine Windows UAC dialog appears as a Linux window (`uac=window`) |
| UAC modes | All three modes (`window`, `auto`, `native`) selectable as a setting |

---

## 2. Architecture & Inter-Component Interactions

### 2.1 Components

```
LINUX HOST (user session)                                   WINDOWS GUEST (interactive desktop)
+------------------------------------------+                +---------------------------------------------+
| Wayland compositor (GNOME/KDE/wlroots)   |                | waddle-guest-session.exe (UIAccess, 1/desk) |
+--------------------^---------------------+                |  - window graph (WinEvent hooks)            |
                     | xdg_shell, seat, ...                 |  - WGC capture (av_wgc.dll) / DXGI fallback |
+--------------------+---------------------+   VSOCK 5002   |  - SendInput / synthetic pointer injection  |
| waddle-session  (1 per device)           |<-------------->|  - cursor, clipboard, DnD, IME preedit      |
|  - interop protocol client               |  interop v2    |  - app catalog, icons, launch, UAC handling |
|  - toplevel/popup manager                |                |  - audio sessions (WASAPI process loopback) |
|  - input/cursor/clipboard/DnD/IME        |   IVSHMEM v3   |  - printers                                 |
|  - desktop-entry/icon materializer       |<===============|  frames + audio rings                       |
|  - per-app PipeWire streams, discard sink|                +---------------------------------------------+
+---^---------------^----------------------+                | waddle-guest-exec.exe (existing, VSOCK 5242)|
    | daemon.sock   | spawn/ensure                          +---------------------------------------------+
+---+---------------+----------------------+
| waddled (existing VM supervisor)         |---- QEMU: virtio-vsock, ivshmem, virtiofs (all mounts),
|  + GuestReady, SessionEnsure, Usb*       |          qemu-xhci + usb-host, virtio-net user + CUPS guestfwd,
+---^--------------------------------------+          ich9-hda + hda-duplex on -audiodev pipewire
    |
+---+--------------------------------------+
| waddle CLI: open / integration / usb      |---- writes ~/.local/share/applications, icons, mimeapps,
|  + waddle-binfmt (system, optional)       |     autostart, ~/Desktop links; integration manifest
+------------------------------------------+
```

### 2.2 Process ownership and lifetimes

| Process | Started by | Lifetime | Instances |
|---|---|---|---|
| `waddled` | CLI ensure-daemon (existing) | Until `waddle stop` / shutdown | 1 per device |
| QEMU, virtiofsd | `waddled` | VM lifetime | 1 per device (virtiofsd: 1 per exported mount) |
| `waddle-session` | `waddle open`, XDG autostart (`start_at_login`), `waddle integration repair` | Until the Wayland display disconnects, `waddle stop`, or `waddle session stop` | 1 per device per Wayland user session, enforced by `session.lock` |
| `waddle-guest-session.exe` | Guest shell from `HKLM\...\Run` at logon (UIAccess) | Guest interactive logon session | 1 per interactive desktop, enforced by named mutex `Global\WaddleSessionAgent` |
| `waddle-guest-exec.exe` | Existing, unchanged | Existing | Existing |
| `waddle-binfmt` | Kernel binfmt_misc | One invocation | Per launch |

### 2.3 Data paths

1. **Control**: interop protocol v2 frames over AF_VSOCK, guest listens on port
   5002 (`InteropControlPort`), host connects to `(device_cid, 5002)`.
2. **Pixels**: IVSHMEM layout v3 window slot heap (Section 3.3). Frame
   readiness and release are control messages; pixels never travel on VSOCK.
3. **Audio out**: IVSHMEM v3 audio rings (up to 8), one per guest audio
   session, each played as its own PipeWire stream.
4. **Audio in**: QEMU `-audiodev pipewire` capture into the guest HDA codec.
5. **Bulk control data** (icons, cursor images, clipboard payloads, DnD
   payloads, catalog snapshots): chunked transfers on the control channel
   (Section 3.2.6).
6. **Files**: virtiofs shares (all configured mounts). Paths cross the boundary
   only after host-side translation with `path_rules.zig`.
7. **USB**: QMP `device_add usb-host` on the device's `qemu-xhci` bus.
8. **Printing**: guest IPP to `10.0.2.100:631`, QEMU user-network `guestfwd`
   to the host CUPS UNIX socket.

### 2.4 Coordinate model

- The guest desktop is a virtual coordinate space owned by the guest. Guest
  window positions never leave the guest except as **relative** offsets
  between a popup and its parent.
- Host surfaces are positioned by the compositor. Waddle never tries to
  position a toplevel.
- Every pointer/touch/tablet coordinate on the wire is **surface-local**,
  in **guest physical pixels**, relative to the top-left of the window's
  exported buffer (which is the DWM extended frame bounds of the window). The
  host converts from Wayland surface-local logical coordinates using the
  surface's current scale (Section 3.6.4).
- The guest agent keeps each exported window fully inside the guest virtual
  desktop's work area (Section 4.6) so that capture and injection hit tests are
  well defined.

### 2.5 Relationship to existing code

| Existing module | Reuse in seamless interop |
|---|---|
| `src/av/av_codec.zig` | Validation style (allocation-free, output unchanged on error) copied into `interop_codec.zig` |
| `src/av/av_peer.c` | Bounded send queue pattern; interop queue depth `InteropPeerQueueFrames=512` |
| `src/av/av_lease.c` | Input authorization epoch handshake; reused verbatim for interop input (Section 3.6.2) |
| `src/av/av_input.c` | Held key/button state machine; scancode table extended (Section 3.6.3) |
| `src/av/av_wayland.c` | Seat, dmabuf/shm buffer import and frame release code extracted into `interop_host_*` modules; `av_wayland.c` keeps calling the extracted functions so `waddle-av-host` is unchanged in behavior |
| `src/av/av_wgc.dll`, `av_capture.c` | Per-window capture, unchanged API |
| `src/av/av_wasapi.c` | Process loopback activation, called once per audio session |
| `src/av/av_pipewire.c` | Stream creation, generalized to N named streams |
| `src/av/av_deploy.c` | Bundle staging and redeploy, generalized for the session agent bundle |
| `src/common/path_rules.zig`, `arguments.c` | Host-to-guest path translation |
| `src/daemon/daemon_client.c` | Ensure-daemon spawning |
| `src/daemon/device_storage.zig` | Journaled transaction pattern for the integration manifest |
| `tests/av/wayland_state.c`, `tests/av/windows_lease.c` | `--wrap` Wayland fakes and Win32 doubles extended for interop tests |

---

### 2.6 Decoration configure lifecycle (implemented AV fix, TODO #3)

The event thread owns each decoration object and its window context. Creation
registers the `zxdg_toplevel_decoration_v1.configure` listener before requesting
client-side mode or committing the initial empty surface. Listener registration
or object creation failure follows `free_window`; the decoration is destroyed
before its toplevel and no window remains admitted. No allocation is introduced.

The decoration event stages a mode; it does not acknowledge a surface or allow
the first frame to attach. The following `xdg_surface.configure` acknowledges
its serial and applies the staged mode. Subsequent surface configurations may
reuse the last applied mode. A decorated surface's first configuration without
a mode, or a decoration mode other than client/server, terminates delivery;
buffers remain ineligible until a valid initial configuration completes.

Windows frames are already drawn into the guest buffer. If the compositor
selects server-side mode, acknowledge the corresponding surface configuration,
then destroy the decoration extension. Its protocol-defined destructor switches
back to no server decoration at the next surface commit. Do not strip guest
pixels, repeatedly request a rejected mode, or attach a double-framed buffer.
The surface remains configured and renders its full guest frame on the next
normal buffer commit. Destruction clears the pointer; queued events from an old
decoration, retired window, failed session or shutdown have no effect.

The same lifecycle is required when TODO #11 extracts the host window manager.
`tests/av/wayland_state.c` delivers the registered decoration and surface
listeners and verifies staging, acknowledgement order, repeated configurations,
server fallback, stale events, invalid/missing modes, creation/listener failures,
title changes and decoration-before-toplevel destruction. Real GNOME/KDE
acceptance remains TODO #3; these callback fixtures do not replace it.

## 3. Data Structures, Protocols & Memory Layouts

All multi-byte integers on the wire and in shared memory are **little-endian**.
All strings are **UTF-8**, not NUL-terminated on the wire, validated as UTF-8
by the receiver, and rejected if they contain U+0000.

### 3.1 Constants (Frozen)

Defined in `include/waddle/interop_protocol.h`:

| Constant | Value | Meaning |
|---|---|---|
| `InteropControlPort` | 5002 | Guest VSOCK listen port |
| `InteropMagic` | `0x504F4957` | Bytes `W I O P` |
| `InteropVersion` | 2 | Protocol version |
| `InteropLayoutVersion` | 3 | Required shared-memory layout |
| `InteropHeaderBytes` | 16 | Frame header size |
| `InteropMaxPayloadBytes` | 65520 | Max payload; max frame 65536 |
| `InteropMaxFields` | 64 | Max TLV fields per frame |
| `InteropMaxStringBytes` | 4096 | Max string field (paths, titles) |
| `InteropMaxTitleBytes` | 1024 | Window title cap |
| `InteropMaxAppKeyBytes` | 200 | App key cap (fits desktop-file ID rules) |
| `InteropMaxWindows` | 64 | Concurrent exported windows |
| `InteropMaxAudioStreams` | 8 | Concurrent audio rings |
| `InteropMaxTransfers` | 16 | Concurrent chunked transfers per direction |
| `InteropChunkBytes` | 61440 | Max chunk payload (60 KiB) |
| `InteropHelloTimeoutMs` | 5000 | Hello/HelloAck deadline |
| `InteropPingIntervalMs` | 2000 | Liveness ping interval |
| `InteropPingTimeoutMs` | 6000 | Missing-pong disconnect deadline |
| `InteropHitTestTimeoutMs` | 50 | `WM_NCHITTEST` probe timeout |
| `InteropPeerQueueFrames` | 512 | Per-direction bounded send queue |

### 3.2 Interop protocol v2 framing (Frozen)

#### 3.2.1 Frame header

```c
/** @brief Fixed 16-byte interop frame header (little-endian on the wire). */
typedef struct interop_header_t {
    uint32_t magic;       /**< InteropMagic. */
    uint16_t version;     /**< InteropVersion. */
    uint16_t type;        /**< interop_message_type_t. */
    uint32_t length;      /**< Payload bytes following the header, 0..InteropMaxPayloadBytes. */
    uint32_t request_id;  /**< 0 for unsolicited; responses echo the request's id. */
} interop_header_t;
```

Offsets: magic 0, version 4, type 6, length 8, request_id 12. A frame whose
magic, version, or length is invalid is a **fatal** protocol error: the
receiver closes the connection after sending `Error{code=ProtocolViolation}`
when possible.

`request_id` is allocated by the sender of a request from a per-direction
counter starting at 1, incrementing by 1, wrapping from `0xFFFFFFFF` to 1.
A request id must not be reused while its response is outstanding.

#### 3.2.2 Payload: TLV fields

The payload is a sequence of fields, each:

```
u16 tag | u16 length | value[length] | zero padding to a 4-byte boundary
```

- `tag & 0x8000` set = **critical**: a receiver that does not know the tag
  rejects the frame with `Error{code=UnsupportedField}`. Clear = optional:
  unknown optional tags are skipped.
- Tags must appear in strictly increasing numeric order of `tag & 0x7FFF`;
  duplicates are rejected (`MalformedFrame`).
- Padding bytes must be zero.
- Field count ≤ `InteropMaxFields`.
- Every message type lists required fields; a missing required field is
  `MalformedFrame`.
- Value encodings: `u8/u16/u32/u64/i32` fixed width; `bool` = u8 0 or 1;
  `str` = UTF-8 bytes; `bytes` = opaque; `rect` = `i32 x, i32 y, u32 width,
  u32 height` (16 bytes, width/height ≤ 16384); `point` = `i32 x, i32 y`.

#### 3.2.3 Field tag registry (Frozen)

Critical tags have the `0x8000` bit; the table lists the base tag and whether
it is sent as critical.

| Tag | Name | Type | Crit | Notes |
|---|---|---|---|---|
| 0x0001 | `window` | u64 | yes | HWND value |
| 0x0002 | `generation` | u64 | yes | Per-agent-session monotonically increasing window incarnation, never 0 |
| 0x0003 | `parent` | u64 | yes | Parent HWND, 0 = none |
| 0x0004 | `parent_generation` | u64 | yes | |
| 0x0005 | `role` | u8 | yes | `interop_window_role_t` |
| 0x0006 | `rect` | rect | yes | Window bounds in guest desktop pixels |
| 0x0007 | `title` | str | no | ≤ InteropMaxTitleBytes |
| 0x0008 | `app_key` | str | yes | Section 3.8.2 |
| 0x0009 | `process_id` | u32 | no | |
| 0x000A | `state` | u32 | yes | `interop_window_state_t` bitmask |
| 0x000B | `dpi` | u32 | yes | 96..960 |
| 0x000C | `slot` | u32 | yes | IVSHMEM slot index |
| 0x000D | `frame_sequence` | u64 | yes | |
| 0x000E | `damage` | bytes | no | Array of up to 16 rects; empty = full |
| 0x000F | `edge` | u8 | yes | `interop_resize_edge_t` |
| 0x0010 | `serial` | u32 | yes | Host protocol input counter or its guest echo; never a raw Wayland seat serial |
| 0x0011 | `point` | point | yes | Surface-local guest pixels |
| 0x0012 | `button` | u16 | yes | `interop_button_t` |
| 0x0013 | `pressed` | bool | yes | |
| 0x0014 | `key` | u16 | yes | Linux evdev code 1..767 |
| 0x0015 | `axis_v120` | i32 | yes | Vertical, value120 units, ±15360 |
| 0x0016 | `axis_h120` | i32 | yes | Horizontal, value120 units, ±15360 |
| 0x0017 | `delta` | bytes | yes | `i32 dx_fixed, i32 dy_fixed` (24.8 fixed) |
| 0x0018 | `touch_id` | u32 | yes | 0..9 |
| 0x0019 | `pressure` | u16 | yes | 0..1024 |
| 0x001A | `tilt` | bytes | yes | `i16 tilt_x, i16 tilt_y` degrees ×100, ±9000 |
| 0x001B | `rotation` | u16 | no | degrees ×100, 0..35999 |
| 0x001C | `tool` | u8 | yes | `interop_tablet_tool_t` |
| 0x001D | `text` | str | yes | Commit/preedit text ≤ 4096 |
| 0x001E | `cursor_begin` | i32 | no | Preedit cursor byte offset, -1 hidden |
| 0x001F | `cursor_end` | i32 | no | |
| 0x0020 | `layout` | str | yes | Windows KLID, 8 hex digits |
| 0x0021 | `locks` | u8 | yes | bit0 caps, bit1 num, bit2 scroll |
| 0x0022 | `cursor_shape` | u16 | yes | `interop_cursor_shape_t` |
| 0x0023 | `hotspot` | point | yes | Cursor hotspot |
| 0x0024 | `image_size` | bytes | yes | `u32 width, u32 height` ≤ 256×256 for cursors, ≤ 512×512 icons |
| 0x0025 | `content_hash` | bytes | yes | 32-byte SHA-256 |
| 0x0026 | `transfer` | u32 | yes | Transfer id |
| 0x0027 | `total_bytes` | u64 | yes | |
| 0x0028 | `offset` | u64 | yes | |
| 0x0029 | `data` | bytes | yes | Chunk payload ≤ InteropChunkBytes |
| 0x002A | `mime` | str | yes | MIME type, ≤ 255 bytes, `[!-~]` only |
| 0x002B | `mime_list` | bytes | yes | Concatenated `u8 len + mime` entries, ≤ 32 entries |
| 0x002C | `clipboard_epoch` | u64 | yes | Selection generation |
| 0x002D | `drop_effect` | u8 | yes | 1 copy, 2 move, 4 link (bitmask for offers) |
| 0x002E | `display_name` | str | yes | App display name ≤ 255 |
| 0x002F | `target_path` | str | no | Guest path of the target executable |
| 0x0030 | `arguments` | str | no | Windows command-line tail |
| 0x0031 | `working_dir` | str | no | Guest path |
| 0x0032 | `aumid` | str | no | AppUserModelID |
| 0x0033 | `app_flags` | u32 | yes | `interop_app_flag_t` |
| 0x0034 | `icon_sizes` | bytes | no | Array of u16 available sizes |
| 0x0035 | `icon_size` | u16 | yes | |
| 0x0036 | `launch_kind` | u8 | yes | `interop_launch_kind_t` |
| 0x0037 | `activation_token` | str | no | XDG activation token ≤ 255, opaque to guest |
| 0x0038 | `status` | u32 | yes | `interop_status_t` |
| 0x0039 | `message` | str | no | Human-readable diagnostic ≤ 1024 |
| 0x003A | `capabilities` | u64 | yes | `interop_capability_t` bitmask |
| 0x003B | `agent_version` | str | yes | Semantic version of the sender |
| 0x003C | `layout_version` | u32 | yes | IVSHMEM layout version |
| 0x003D | `session_epoch` | u64 | yes | Fresh nonzero epoch per authorized mapping initialization; never per transport accept |
| 0x003E | `output` | bytes | yes | `u32 output_id, u32 width_px, u32 height_px, u32 scale_120` (scale ×120, 120..480) |
| 0x003F | `stream` | u32 | yes | Audio stream / ring index |
| 0x0040 | `printer_name` | str | yes | CUPS queue name ≤ 127 |
| 0x0041 | `printer_uri` | str | yes | ≤ 255 |
| 0x0042 | `publisher` | str | no | UAC verified publisher ≤ 255 |
| 0x0043 | `decision` | u8 | yes | 0 deny, 1 allow |
| 0x0044 | `usb_id` | bytes | yes | `u16 vendor, u16 product, u8 bus, u8 address` |
| 0x0045 | `lease_epoch` | u64 | yes | Input lease epoch (Section 3.6.2) |
| 0x0046 | `error_code` | u32 | yes | `interop_error_t` |
| 0x0047 | `refused_type` | u16 | no | Message type that caused an Error |
| 0x0048 | `uri_list` | str | yes | `text/uri-list` body (host paths) or CRLF list of guest paths |
| 0x0049 | `interval_ms` | u32 | yes | |
| 0x004A | `rate` | u32 | yes | Key repeat rate per second, 0 = no repeat |
| 0x004B | `min_size` | bytes | no | `u32 width, u32 height`, each 0..16384 guest physical pixels; zero clears that minimum |

#### 3.2.4 Enumerations (Frozen)

```c
/** @brief Guest window classification for host surface mapping. */
typedef enum interop_window_role_t {
    InteropRoleToplevel = 1,    /**< Ownerless or WS_EX_APPWINDOW: xdg_toplevel. */
    InteropRoleDialog = 2,      /**< Owned, activatable window: xdg_toplevel + set_parent. */
    InteropRoleMenu = 3,        /**< #32768, combo list, WS_POPUP owned activating popups: xdg_popup + grab. */
    InteropRoleTooltip = 4,     /**< tooltips_class32, WS_EX_NOACTIVATE/TOOLWINDOW popups: xdg_popup, no grab. */
    InteropRoleUacConsent = 5,  /**< consent.exe credential dialog: xdg_toplevel, app_id waddle.<dev>.uac. */
    InteropRoleImePreedit = 6   /**< Agent-owned layered preedit window: xdg_popup, no grab. */
} interop_window_role_t;

/** @brief Window state bitmask (both directions). */
typedef enum interop_window_state_t {
    InteropStateMinimized = 1u << 0,
    InteropStateMaximized = 1u << 1,
    InteropStateFullscreen = 1u << 2,
    InteropStateActivated = 1u << 3,    /**< Host->guest only: compositor focus. */
    InteropStateResizable = 1u << 4,    /**< Guest->host: WS_THICKFRAME present. */
    InteropStateMaximizable = 1u << 5,  /**< Guest->host: WS_MAXIMIZEBOX present. */
    InteropStateMinimizable = 1u << 6   /**< Guest->host: WS_MINIMIZEBOX present. */
} interop_window_state_t;
/* Mask of defined bits: InteropStateMask = 0x7F; other bits are MalformedFrame. */

/** @brief Resize edge, numerically equal to xdg_toplevel_resize_edge. */
typedef enum interop_resize_edge_t {
    InteropEdgeTop = 1, InteropEdgeBottom = 2, InteropEdgeLeft = 4,
    InteropEdgeTopLeft = 5, InteropEdgeBottomLeft = 6, InteropEdgeRight = 8,
    InteropEdgeTopRight = 9, InteropEdgeBottomRight = 10
} interop_resize_edge_t;

/** @brief Pointer buttons. */
typedef enum interop_button_t {
    InteropButtonLeft = 1, InteropButtonRight = 2, InteropButtonMiddle = 3,
    InteropButtonBack = 4,   /**< BTN_SIDE  -> XBUTTON1 */
    InteropButtonForward = 5 /**< BTN_EXTRA -> XBUTTON2 */
} interop_button_t;

/** @brief Tablet tool kinds. */
typedef enum interop_tablet_tool_t {
    InteropToolPen = 1, InteropToolEraser = 2
} interop_tablet_tool_t;

/** @brief Launch request kinds. */
typedef enum interop_launch_kind_t {
    InteropLaunchApp = 1,        /**< app_key from catalog. */
    InteropLaunchPath = 2,       /**< target_path (+arguments, working_dir): ShellExecuteExW "open". */
    InteropLaunchInstaller = 3,  /**< .msi: msiexec.exe /i "<path>". */
    InteropLaunchShortcut = 4,   /**< .lnk: ShellExecuteExW on the link. */
    InteropLaunchConsole = 5     /**< CUI image without host terminal: new guest console window. */
} interop_launch_kind_t;

/** @brief Catalog entry flags. */
typedef enum interop_app_flag_t {
    InteropAppSystem = 1u << 0,        /**< Under %WINDIR% or a Microsoft.Windows.* AUMID. */
    InteropAppSeed = 1u << 1,          /**< Member of the seed list (Section 3.8.4). */
    InteropAppDesktopShortcut = 1u << 2, /**< Has a .lnk on the Public or user Desktop. */
    InteropAppPackaged = 1u << 3,      /**< UWP/MSIX package. */
    InteropAppUninstallable = 1u << 4  /**< Has an Uninstall registry entry. */
} interop_app_flag_t;

/** @brief Negotiated optional facilities. */
typedef enum interop_capability_t {
    InteropCapWindows = 1ull << 0, InteropCapInput = 1ull << 1, InteropCapCursor = 1ull << 2,
    InteropCapRelativePointer = 1ull << 3, InteropCapTouch = 1ull << 4, InteropCapTablet = 1ull << 5,
    InteropCapGestures = 1ull << 6, InteropCapTextInput = 1ull << 7, InteropCapClipboard = 1ull << 8,
    InteropCapDragDrop = 1ull << 9, InteropCapCatalog = 1ull << 10, InteropCapLaunch = 1ull << 11,
    InteropCapAudio = 1ull << 12, InteropCapPrinters = 1ull << 13, InteropCapUacNative = 1ull << 14,
    InteropCapDisplay = 1ull << 15, InteropCapKeyboardLayout = 1ull << 16
} interop_capability_t;

/** @brief Status codes in results. */
typedef enum interop_status_t {
    InteropOk = 0, InteropNotFound = 1, InteropAccessDenied = 2, InteropCancelled = 3,
    InteropTimedOut = 4, InteropBusy = 5, InteropTooLarge = 6, InteropUnsupported = 7,
    InteropOutsideShare = 8, InteropGuestError = 9
} interop_status_t;

/** @brief Error frame codes. */
typedef enum interop_error_t {
    InteropErrorProtocolViolation = 1, InteropErrorMalformedFrame = 2,
    InteropErrorUnsupportedField = 3, InteropErrorUnsupportedType = 4,
    InteropErrorStaleWindow = 5, InteropErrorQuotaExceeded = 6,
    InteropErrorVersionMismatch = 7, InteropErrorUnexpected = 8
} interop_error_t;
```

Cursor shapes (`interop_cursor_shape_t`) equal the `wp_cursor_shape_device_v1`
enum values 1..34 (default=1 … zoom_out=34) plus `InteropCursorHidden = 0`
and `InteropCursorCustom = 0xFFFF` (bitmap follows by transfer). Guest mapping:

| Windows `IDC_*` | Shape |
|---|---|
| ARROW | default |
| IBEAM | text |
| WAIT | wait |
| APPSTARTING | progress |
| CROSS | crosshair |
| UPARROW | n_resize |
| SIZENWSE | nwse_resize |
| SIZENESW | nesw_resize |
| SIZEWE | ew_resize |
| SIZENS | ns_resize |
| SIZEALL | move |
| NO | not_allowed |
| HAND | pointer |
| HELP | help |
| PIN, PERSON | custom bitmap |

#### 3.2.5 Message types (Frozen)

Direction: G→H guest to host, H→G host to guest. "Req" fields are required,
"Opt" optional. Responses echo `request_id`.

| Type | Name | Dir | Req fields | Opt fields |
|---|---|---|---|---|
| 0x0001 | `Hello` | G→H | agent_version, capabilities, layout_version, session_epoch | message |
| 0x0002 | `HelloAck` | H→G | agent_version, capabilities (intersection), layout_version | — |
| 0x0003 | `Goodbye` | both | status | message |
| 0x0004 | `Ping` | H→G | — | — |
| 0x0005 | `Pong` | G→H | — | — |
| 0x0006 | `Error` | both | error_code | refused_type, message |
| 0x0100 | `WindowAnnounce` | G→H | window, generation, role, rect, app_key, state, dpi | title, parent, parent_generation, process_id, min_size, serial (menu trigger only) |
| 0x0101 | `WindowUpdate` | G→H | window, generation, rect, state, dpi | title, parent, parent_generation, min_size |
| 0x0102 | `WindowRetire` | G→H | window, generation | — |
| 0x0103 | `WindowConfigure` | H→G | window, generation, state | rect (width/height only meaningful, x=y=0) |
| 0x0104 | `WindowClose` | H→G | window, generation | — |
| 0x0105 | `WindowMoveRequest` | G→H | window, generation, serial | — |
| 0x0106 | `WindowResizeRequest` | G→H | window, generation, serial, edge | — |
| 0x0107 | `WindowStateRequest` | G→H | window, generation, state | — |
| 0x0108 | `FrameReady` | G→H | window, generation, slot, frame_sequence | damage |
| 0x0109 | `FrameRelease` | H→G | window, generation, slot, frame_sequence | — |
| 0x010A | `WindowIconChanged` | G→H | window, generation, content_hash | — |
| 0x010B | `PopupRepositioned` | H→G | window, generation, rect (x/y relative to parent) | — |
| 0x010C | `PopupDismissed` | H→G | window, generation | — |
| 0x0200 | `InputFocus` | H→G | window, generation, lease_epoch, serial, pressed (true=enter) | — |
| 0x0201 | `InputKey` | H→G | window, generation, lease_epoch, serial, key, pressed | — |
| 0x0202 | `InputPointerMotion` | H→G | window, generation, lease_epoch, serial, point | — |
| 0x0203 | `InputPointerRelative` | H→G | window, generation, lease_epoch, serial, delta | — |
| 0x0204 | `InputPointerButton` | H→G | window, generation, lease_epoch, serial, point, button, pressed | — |
| 0x0205 | `InputPointerAxis` | H→G | window, generation, lease_epoch, serial, axis_v120, axis_h120 | — |
| 0x0206 | `InputTouch` | H→G | window, generation, lease_epoch, serial, touch_id, point, pressed | — |
| 0x0207 | `InputTouchFrame` | H→G | window, generation, lease_epoch, serial | — |
| 0x0208 | `InputTouchCancel` | H→G | window, generation, lease_epoch, serial | — |
| 0x0209 | `InputTablet` | H→G | window, generation, lease_epoch, serial, tool, point, pressure, tilt, pressed (tip down) | rotation, button (barrel = 1, 2) |
| 0x020A | `InputTabletProximity` | H→G | window, generation, lease_epoch, serial, tool, pressed (true=in) | — |
| 0x020B | `InputGesturePinch` | H→G | window, generation, lease_epoch, serial, point, axis_v120 (Ctrl+wheel delta) | — |
| 0x020C | `InputTextCommit` | H→G | window, generation, lease_epoch, serial, text | — |
| 0x020D | `InputTextPreedit` | H→G | window, generation, lease_epoch, serial, text | cursor_begin, cursor_end |
| 0x020E | `InputRelease` | H→G | window, generation, lease_epoch, serial | — |
| 0x020F | `InputLockState` | H→G | lease_epoch, serial, locks | — |
| 0x0210 | `InputRepeatInfo` | H→G | rate, interval_ms (delay) | — |
| 0x0211 | `KeyboardLayout` | H→G | layout | — |
| 0x0212 | `CaretRect` | G→H | window, generation, rect (surface-local) | — |
| 0x0213 | `LeaseRevoked` | G→H | lease_epoch | — |
| 0x0214 | `LeaseAck` | H→G | lease_epoch, serial | — |
| 0x0215 | `LeaseReady` | G→H | lease_epoch, serial | — |
| 0x0300 | `CursorShape` | G→H | cursor_shape | hotspot, content_hash (custom) |
| 0x0301 | `CursorConstraint` | G→H | window, generation, pressed (true = lock + relative) | — |
| 0x0400 | `ClipboardOffer` | both | clipboard_epoch, mime_list | drop_effect (2 = cut) |
| 0x0401 | `ClipboardRequest` | both | clipboard_epoch, mime, transfer | — |
| 0x0402 | `ClipboardClear` | both | clipboard_epoch | — |
| 0x0500 | `DndEnter` | H→G | window, generation, point, mime_list, drop_effect | — |
| 0x0501 | `DndMotion` | H→G | window, generation, point | — |
| 0x0502 | `DndLeave` | H→G | window, generation | — |
| 0x0503 | `DndDrop` | H→G | window, generation, point, transfer, mime | — |
| 0x0504 | `DndResult` | G→H | status, drop_effect | — |
| 0x0505 | `DndSourceStart` | G→H | window, generation, mime_list, drop_effect | — |
| 0x0506 | `DndSourceRequest` | H→G | mime, transfer | — |
| 0x0507 | `DndSourceFinished` | H→G | status, drop_effect | — |
| 0x0600 | `CatalogBegin` | G→H | — | — |
| 0x0601 | `CatalogAdd` | G→H | app_key, display_name, app_flags | target_path, arguments, working_dir, aumid, icon_sizes, content_hash |
| 0x0602 | `CatalogUpdate` | G→H | app_key, display_name, app_flags | same as Add |
| 0x0603 | `CatalogRemove` | G→H | app_key | — |
| 0x0604 | `CatalogEnd` | G→H | — | — |
| 0x0605 | `IconRequest` | H→G | app_key, icon_size, transfer | window, generation (per-window icon) |
| 0x0700 | `LaunchRequest` | H→G | launch_kind | app_key, target_path, arguments, working_dir, activation_token |
| 0x0701 | `LaunchResult` | G→H | status, process_id | activation_token, message |
| 0x0702 | `ElevationNotice` | G→H | display_name, process_id | target_path, publisher |
| 0x0703 | `UacPrompt` | G→H | display_name, transfer (id used as prompt id) | publisher, target_path |
| 0x0704 | `UacDecision` | H→G | transfer (prompt id), decision | — |
| 0x0705 | `DesktopSwitch` | G→H | pressed (true = secure desktop active) | — |
| 0x0800 | `AudioStreamOpen` | G→H | stream, app_key, display_name, process_id | — |
| 0x0801 | `AudioStreamClose` | G→H | stream | — |
| 0x0900 | `PrinterSync` | H→G | printer_name, printer_uri, pressed (true=add, false=remove) | display_name |
| 0x0901 | `PrinterResult` | G→H | printer_name, status | message |
| 0x0A00 | `DisplayConfig` | H→G | output (repeated, 1..8 fields allowed as exception: tag 0x003E may repeat) | — |
| 0x0A01 | `DisplayApplied` | G→H | status, rect (guest virtual desktop bounds) | message |
| 0x0F00 | `TransferBegin` | both | transfer, total_bytes, mime | content_hash |
| 0x0F01 | `TransferChunk` | both | transfer, offset, data | — |
| 0x0F02 | `TransferEnd` | both | transfer, content_hash | — |
| 0x0F03 | `TransferCancel` | both | transfer, status | — |
| 0x7F00 | `LocalLaunch` | local socket only | launch_kind | app_key, target_path, arguments, working_dir, activation_token |
| 0x7F01 | `LocalStop` | local socket only | — | — |
| 0x7F02 | `LocalStopResult` | local socket only | status | message |

Exception rule: in `DisplayConfig` only, tag 0x003E may repeat (1..8 times);
repeated tags are still required to be contiguous and the ordering rule applies
to the remaining tags.

#### 3.2.6 Chunked transfers (Frozen)

- A transfer id is allocated by whoever **requests** the data
  (`ClipboardRequest`, `IconRequest`, `DndDrop`, `DndSourceRequest`, and the
  `UacPrompt` prompt id), from a per-connection counter, nonzero, unique among
  open transfers in that direction.
- The data **sender** replies `TransferBegin{transfer, total_bytes, mime}`,
  then `TransferChunk`s with strictly contiguous `offset`s starting at 0, each
  with 1..InteropChunkBytes bytes, then `TransferEnd{content_hash}` where
  `content_hash` is SHA-256 of the full payload.
- Either side may send `TransferCancel` at any time; the receiver discards
  partial data.
- Caps: icons ≤ 1 MiB, cursor images ≤ 256 KiB, clipboard and DnD ≤
  `[clipboard] max_mb` (default 64 MiB, maximum 1024 MiB). `total_bytes` above
  the cap → `TransferCancel{status=TooLarge}` by the receiver.
- At most `InteropMaxTransfers` open transfers per direction; a 17th request is
  answered `TransferCancel{status=Busy}`.
- The receiver accumulates into a caller-owned buffer allocated once at
  `TransferBegin` (size `total_bytes`) or streams directly to a file
  descriptor (clipboard `send(fd)`); there is no reallocation.

#### 3.2.7 Connection state machine (Frozen)

```
Host:  Disconnected -> Connecting -> AwaitHello -> Ready -> Draining -> Disconnected
Guest: Listening -> AwaitHelloAck -> Ready -> Draining -> Listening
```

1. Host connects to `(cid, 5002)` with a 2 s connect timeout; retries with
   exponential backoff 250 ms → 4 s while the daemon reports `Running`.
2. Guest sends `Hello` within `InteropHelloTimeoutMs`.
3. Host verifies: `version == 2`, `agent_version` major equals host major,
   `layout_version == 3`. Mismatch → `Error{VersionMismatch}`, close, and the
   host triggers agent redeploy (Section 4.2) once per host process.
4. Host replies `HelloAck` with the capability intersection. Only negotiated
   facilities may be used; a frame for an unnegotiated facility is
   `UnsupportedType`.
5. Guest then sends a full window snapshot (`WindowAnnounce` per eligible
   window) and a catalog snapshot (`CatalogBegin`, `CatalogAdd`*, `CatalogEnd`).
6. Liveness: host `Ping` every 2 s; no `Pong` (or any frame) for 6 s →
   disconnect.
7. On disconnect both sides drop all window, transfer, clipboard, DnD and input
   state; the guest releases every held key/button (Section 6.3); the host
   destroys every surface for that device.
8. Only one host connection is accepted at a time; a second connect while
   Ready is rejected with `Error{ProtocolViolation, message="busy"}` and closed.

#### 3.2.8 Window identity rules (Frozen)

- `(session_epoch, window, generation)` identifies a window. `generation` is
  assigned at admission from a 64-bit counter (starting at 1) and never
  reused within a session epoch. HWND reuse after retirement yields a new
  generation.
- Any H→G frame whose `(window, generation)` is not currently exported is
  answered with `Error{StaleWindow}` and has no effect. Input frames for a stale
  window additionally consume their serial (Section 3.6.2).
- `parent`/`parent_generation` must name a window announced earlier and still
  exported; otherwise the guest defers the announcement until the parent is
  announced (bounded: 64 deferred windows, older deferrals are dropped and
  retried on the next 250 ms refresh).

### 3.3 IVSHMEM layout v3 (Frozen)

The mapping stays 2 GiB (`AvMappingBytes`, unchanged size so QEMU arguments do
not change). Layout v3 is selected per session by `HelloAck.layout_version=3`;
the per-PID AV mode keeps layout v1.

```
Offset           Size        Contents
0x0000_0000      4 KiB       interop_shm_header_t
0x0000_1000      60 KiB      interop_slot_descriptor_t[192] (64 B each = 12 KiB) + reserved (zero)
0x0001_0000      8 x 128 KiB audio rings at stride 0x20000 (64 B audio_ring_header_t,
                             64 B zero, PCM 16384 frames x 4 B at +128; 65664 B used)
0x0011_0000      960 KiB     reserved (zero)
0x0020_0000      1022 pages  slot heap, 2 MiB pages (heap starts 2 MiB aligned)
0x7FE0_0000      2 MiB       reserved (zero): keeps every used byte below INT32_MAX
                             because wl_shm pool sizes and buffer offsets are int32
```

```c
/** @brief Shared header, 4096 bytes, guest-initialized, host read-only except host_heartbeat. */
typedef struct interop_shm_header_t {
    _Alignas(64) uint32_t magic; /**< 0x33574D53 'SMW3'. */
    uint32_t layout_version;     /**< 3. */
    uint64_t session_epoch;      /**< Equals Hello.session_epoch. */
    uint64_t total_bytes;        /**< Mapping size. */
    uint32_t slot_count;         /**< Descriptors in use capacity, 192. */
    uint32_t audio_ring_count;   /**< 8. */
    uint64_t heap_offset;        /**< 0x00200000. */
    uint64_t heap_bytes;         /**< 1022 * 2 MiB = 0x7FC00000; heap ends at 0x7FE00000. */
    uint32_t page_bytes;         /**< 2097152. */
    uint32_t reserved0;
    _Atomic uint64_t guest_heartbeat; /**< Incremented every 500 ms by guest. */
    _Atomic uint64_t host_heartbeat;  /**< Incremented every 500 ms by host. */
    uint8_t reserved[4096 - 72];
} interop_shm_header_t;

/** @brief One frame slot, 64-byte aligned, three per exported window. */
typedef struct interop_slot_descriptor_t {
    _Alignas(64) _Atomic uint32_t state; /**< Free=0, Writing=1, Ready=2, Consuming=3 (same as v1). */
    uint32_t format;             /**< AvPixelFormat BGRA 0x34325241, premultiplied alpha. */
    uint64_t window;             /**< Owning HWND, 0 when unassigned. */
    uint64_t generation;         /**< Owning window generation. */
    uint64_t frame_sequence;
    uint64_t data_offset;        /**< From mapping start, page aligned. */
    uint32_t data_bytes;         /**< Reserved bytes for this slot (page multiple). */
    uint32_t width;
    uint32_t height;
    uint32_t stride;             /**< >= width*4, multiple of 64. */
    uint64_t reserved;           /**< Zero. */
} interop_slot_descriptor_t;     /* sizeof == 64: 4+4+8+8+8+8+4+4+4+4+8 */
```

The declarations above are implemented in `include/waddle/interop_memory.h`.
Both types are aligned to 64 bytes. Compile-time assertions freeze every field
offset, both sizes (4096/64), and both alignments in native Linux and Windows
C ABI builds. Header heartbeats are at offsets 56 and 64; reserved storage starts
at 72 and contains 4024 zero bytes. Do not use the superseded `4096 - 80` array.
The allocator and untrusted mapping validator remain TODO #5; declarations alone
do not make a layout-v3 mapping usable by either runtime.

Window minima: `WindowAnnounce.min_size` absent means `(0,0)` (no minimum);
`WindowUpdate.min_size` absent leaves the previous minimum unchanged. Present
zero clears an axis. `rect` always carries actual current bounds; it is never
reinterpreted as a minimum. Hosts convert each positive physical-pixel minimum
with `ceil(value / effective_scale)` for `xdg_toplevel.set_min_size`; zero stays
zero. The guest clamps OS-reported minima into 0..16384. Toplevel/dialog only;
minima on popups are ignored by the runtime. A popup announce may carry a
trigger `serial`; non-menu announces must omit it. Menu runtime validates its
family and live press as described in section 3.6.2.

Slot heap allocation (guest-owned, single writer):

- Each exported window reserves 3 slots. A slot's `data_bytes` is
  `round_up(stride*height, 2 MiB)` with stride = `round_up(width*4, 64)`.
- Allocation is first-fit over a page bitmap of `heap_bytes / 2 MiB` pages
  (1022 pages). The host creates one `wl_shm_pool` of size `0x7FE00000` over
  the mapping fd; every buffer offset is `data_offset` (< 0x7FE00000). On growth (window resized larger than the reserved
  size) the guest allocates a new run, publishes frames into it, and frees the
  old run only after the host has `FrameRelease`d every frame in the old run.
- If allocation fails, the window is exported with `state` unchanged but no new
  frames until space frees; the guest logs `heap exhausted` and retries on each
  release. No window is ever retired because of heap pressure.
- Ceiling: one window larger than 7680×4320 is clamped to capture at
  7680×4320 (stride 30720, 127 MiB per slot run of 64 pages).

Audio ring v3: the 64-byte `audio_ring_header_t` of
`include/waddle/av_memory.h` unchanged (same fields, SPSC semantics and
assertions), `capacity_frames = 16384` (≈341 ms at 48 kHz). Stream open/closed
state is carried only by `AudioStreamOpen`/`AudioStreamClose` control messages;
the guest resets `write_head`/`read_head`/`overrun_frames` to 0 before sending
`AudioStreamOpen` and the host starts consuming only after receiving it. Ring `n` header is at
`0x00010000 + n * 0x20000`; its PCM data begins 128 bytes after the header.

### 3.4 Host↔daemon protocol additions (Frozen)

Added to `include/waddle/daemon_protocol.h` (existing 16-byte `WADD` header,
version 1; new types are additive, old daemons answer `ENOTSUP`):

| Type | Name | Payload |
|---|---|---|
| 0x10 | `DaemonMsgGuestReadyReq` | `u32 timeout_ms` |
| 0x11 | `DaemonMsgGuestReadyResp` | `i32 status (0 guest-exec reachable, -ETIMEDOUT, -ENOTCONN)`, `u32 cid` |
| 0x12 | `DaemonMsgUsbListReq` | — |
| 0x13 | `DaemonMsgUsbListResp` | `u32 count`, then per device `u16 vid, u16 pid, u8 bus, u8 addr, u8 attached, u8 reserved, char product[64]` |
| 0x14 | `DaemonMsgUsbAttachReq` | `u16 vid, u16 pid, u8 bus, u8 addr, u16 reserved` |
| 0x15 | `DaemonMsgUsbAttachResp` | `i32 status`, `char qom_id[32]` |
| 0x16 | `DaemonMsgUsbDetachReq` | `char qom_id[32]` |
| 0x17 | `DaemonMsgUsbDetachResp` | `i32 status` |

`GuestReady` waits in the daemon's existing nonblocking poll loop, with a
deadline per client, until the configured guest-exec VSOCK service is reachable
(`vsock_port`, normally5242). It closes that probe immediately. Status0 means
only guest-exec reachability; it never grants interop readiness or mapping
ownership. Only the actual host session owner connects to reserved interop
port5002 and negotiates Hello/HelloAck. The seamless entry path must reject a
profile whose guest-exec port is5002 before ensuring/starting that VM; legacy
non-interop profiles keep their existing configurable-port behavior. No grace
timer may distinguish a probe from a real interop peer. The current daemon's
startup reachability probe already uses config.vsock_port; the new GuestReady
request and seamless entry guard remain implementation work.

### 3.5 Configuration keys (Frozen)

Added sections in the per-device INI parsed by `daemon_config.zig`. Unknown
keys inside these sections are rejected (same policy as `[av]`). Every key is
editable through `waddle device config set|get|reset`.

| Section.key | Type / values | Default | Effect |
|---|---|---|---|
| `network.mode` | `user` \| `none` | `user` | `user`: `-netdev user` + `virtio-net-pci`; `none`: `-nodefaults`-style no NIC (explicit `-nic none`) |
| `network.cups_forward` | bool | `true` | Adds `guestfwd=tcp:10.0.2.100:631-chardev:waddle_cups` when `/run/cups/cups.sock` exists at VM start |
| `usb.controller` | bool | `true` | Adds `-device qemu-xhci,id=waddle_xhci` |
| `usb.auto_attach` | list `VVVV:PPPP` comma-separated, ≤ 16 | empty | Devices attached at VM start and on hotplug |
| `audio.backend` | `pipewire` \| `none` | `pipewire` | HDA audiodev backend |
| `audio.microphone` | bool | `true` | When false, `in.` voices disabled (`-audiodev ...,in.voices=0`) |
| `audio.per_app` | bool | `true` | Per-app loopback streams (else one combined stream of all sessions) |
| `input.ime` | `guest` \| `host` | `guest` | Section 3.10.5 |
| `input.inhibit_shortcuts` | `never` \| `fullscreen` \| `focused` | `fullscreen` | keyboard-shortcuts-inhibit policy |
| `input.repeat` | `host` \| `off` | `host` | Host-synthesized key repeat |
| `clipboard.enabled` | bool | `true` | |
| `clipboard.max_mb` | 1..1024 | 64 | Transfer cap |
| `clipboard.files` | bool | `true` | File copy/paste |
| `integration.start_at_login` | bool | `false` | XDG autostart entry for `waddle-session` + VM warm start |
| `integration.menu_apps` | `installed` \| `all` \| `none` | `installed` | Section 3.8.4 |
| `integration.desktop_shortcuts` | `mirror` \| `off` | `mirror` | Section 3.8.6 |
| `integration.exe_handler` | `waddle` \| `off` | `waddle` | Section 3.9.1 |
| `integration.uac` | `window` \| `auto` \| `native` | `window` | Section 3.11 |
| `integration.notify` | bool | `true` | Desktop notifications |

### 3.6 Input contracts

#### 3.6.1 Routing and focus (Frozen)

- Exactly one exported window holds **input focus**: the window whose host
  surface has `wl_keyboard` focus. Popups (menus) receive pointer input while
  their grab is active; keyboard input continues to go to the toplevel that
  owns the grab, as Windows menus read keyboard through the owner thread's
  message loop.
- Pointer, button, axis, touch and tablet events are forwarded for the surface
  under the pointer/touch point **if** that surface belongs to the focused
  window's family (the window, its owned dialogs, and its popups). Events over
  other Waddle surfaces are dropped except the activating click (below).
- **Activating click**: a button press on an unfocused Waddle surface is
  queued (one event, 300 ms validity). When `keyboard.enter` for that surface
  arrives within 300 ms the guest receives `InputFocus{enter}` followed by the
  queued press, so the first click is not lost. If enter does not arrive, the
  queued press is discarded and the matching release is suppressed.

#### 3.6.2 Lease (Frozen)

The input lease of `impl/input-lease-recovery/IMPL_DESC.md` is reused
unchanged in semantics, carried by `LeaseRevoked`/`LeaseAck`/`LeaseReady` with
`lease_epoch` instead of the 328-byte envelope:

- Guest owns the epoch; every guest foreground change between exported windows
  not caused by an `InputFocus` it executed revokes the lease (epoch + 1).
- Host owns the protocol input serial: a 32-bit counter starting at 1,
  strictly increasing across H→G messages that carry `serial` (input and
  `LeaseAck`); wrap → disconnect. Messages without a serial, including
  `InputRepeatInfo` and `KeyboardLayout`, do not consume it. Guest requests
  echo the initiating serial; they do not allocate or advance this counter.
- Wayland seat serials are a separate opaque u32 namespace, may wrap or be
  zero, and never serve as protocol input counters. On an eligible physical
  press, retain `(protocol serial T, Wayland serial S, seat, window,
  generation, lease_epoch, press kind/code)` in a bounded 64-entry host table.
  Only pointer button down, touch down or a non-repeat key down is eligible;
  synthetic repeat has no grab entry. Never evict a live entry to admit a new
  press: if all 64 are live, fail the session and release held input.
- Remove an entry on its corresponding release/cancel, focus/lease loss,
  window retirement, seat removal or disconnect. Match guest move/resize to
  a still-held latest left-button press on that toplevel and match popup grabs
  to a still-live trigger in its parent family, same seat/generation/epoch.
  Resolve T to S only at the compositor request. Missing, stale or wrong-kind
  entries cause no compositor request. Never pass T to Wayland.
- An activating press queued before keyboard focus retains S locally, receives
  T only when it is actually forwarded after focus/lease readiness, and expires
  under the same 300 ms rule. Focus loss invalidates all existing grab entries.
- Input frames from an old epoch consume their serial and have no effect.
- Handshake deadline 2000 ms; expiry → disconnect.

#### 3.6.3 Keyboard (Frozen)

- `key` is the Linux evdev code from `wl_keyboard.key` (XKB keycode − 8),
  1..767. Translation to Windows Set-1 scan codes extends
  `av_input_scan_code()`; the complete table lives in
  `src/av/av_input.c` and is reproduced in `tests/interop/scancodes.csv`
  (golden file, tested both directions). Additions beyond 1..127:

| evdev | Key | Scan |
|---|---|---|
| 85 | KEY_ZENKAKUHANKAKU | 0x29 (JIS layout) |
| 89 | KEY_RO | 0x73 |
| 90 | KEY_KATAKANA | 0x70 (KANA) |
| 92 | KEY_HENKAN | 0x79 |
| 93 | KEY_KATAKANAHIRAGANA | 0x70 |
| 94 | KEY_MUHENKAN | 0x7B |
| 95 | KEY_KPJPCOMMA | 0x5C |
| 113 | KEY_MUTE | E0 20 |
| 114 | KEY_VOLUMEDOWN | E0 2E |
| 115 | KEY_VOLUMEUP | E0 30 |
| 116 | KEY_POWER | E0 5E (suppressed by default, Section 6.3) |
| 117 | KEY_KPEQUAL | 0x59 |
| 119 | KEY_PAUSE | E1 1D 45 / E1 9D C5 (special sequence) |
| 121 | KEY_KPCOMMA | 0x7E |
| 122 | KEY_HANGEUL | 0xF2 (`VK_HANGUL` virtual key path) |
| 123 | KEY_HANJA | 0xF1 (`VK_HANJA` virtual key path) |
| 124 | KEY_YEN | 0x7D |
| 127 | KEY_COMPOSE | E0 5D |
| 128 | KEY_STOP | E0 68 |
| 140 | KEY_CALC | E0 21 |
| 142 | KEY_SLEEP | E0 5F (suppressed by default) |
| 150 | KEY_WWW | E0 32 |
| 155 | KEY_MAIL | E0 6C |
| 156 | KEY_BOOKMARKS | E0 66 |
| 158 | KEY_BACK | E0 6A |
| 159 | KEY_FORWARD | E0 69 |
| 163 | KEY_NEXTSONG | E0 19 |
| 164 | KEY_PLAYPAUSE | E0 22 |
| 165 | KEY_PREVIOUSSONG | E0 10 |
| 166 | KEY_STOPCD | E0 24 |
| 172 | KEY_HOMEPAGE | E0 32 |
| 173 | KEY_REFRESH | E0 67 |
| 183..194 | KEY_F13..KEY_F24 | 0x64..0x6E, 0x76 |
| 217 | KEY_SEARCH | E0 65 |
| 226 | KEY_MEDIA | E0 6D |

  Every other code fails closed (dropped with a rate-limited log line), as
  today. Hangul/Hanja use `SendInput` with `wVk` instead of a scan code
  because they have no stable Set-1 code across keyboards.
- Repeat: when `input.repeat=host`, the host owns repeat timing using
  `wl_keyboard.repeat_info(rate, delay)`. After `delay` ms of a key held
  without another key press, the host sends `InputKey{pressed=true}` again
  every `1000/rate` ms until release or focus loss. Modifier keys
  (Shift/Ctrl/Alt/Meta), lock keys and Pause never repeat. The guest injects
  each repeated down as a down without an intervening up, which is how Windows
  hardware typematic presents (`WM_KEYDOWN` with the repeat bit).
  `InputRepeatInfo` informs the guest of the values for diagnostics only.
- Lock state: on every `InputFocus{enter}` the host sends `InputLockState`
  from the `wl_keyboard.modifiers` locked mask (XKB `Lock`, `Mod2`=NumLock,
  ScrollLock via the keymap's `ScrollLock` modifier when present). The guest
  compares with `GetKeyState(VK_CAPITAL/VK_NUMLOCK/VK_SCROLL) & 1` and injects
  a press+release of each differing lock key before forwarding any other key.
- Layout sync: the host maps the XKB keymap's first group layout and variant
  (parsed with libxkbcommon from the `wl_keyboard.keymap` fd, then
  `xkb_keymap_layout_get_name` plus the RMLVO from the `xkb_symbols` include
  line) to a Windows KLID with the table `src/interop/keyboard_layouts.zig`
  (≥ 80 layouts; unknown → no change). `KeyboardLayout{layout}` is sent on
  keymap change and on connect. The guest calls
  `LoadKeyboardLayoutW(klid, KLF_ACTIVATE | KLF_SETFORPROCESS)` for the
  foreground window's thread via `PostMessageW(hwnd, WM_INPUTLANGCHANGEREQUEST,
  0, hkl)`.
- Shortcut inhibit: per `input.inhibit_shortcuts`, the host requests
  `zwp_keyboard_shortcuts_inhibitor_v1` for the focused surface (`focused`),
  only when the window is fullscreen or pointer-locked (`fullscreen`,
  default), or never.

#### 3.6.4 Pointer (Frozen)

- Seat bound at version `min(advertised, 9)`; requires ≥ 5. With version ≥ 8,
  `axis_value120` is used; otherwise `axis_discrete × 120`; otherwise
  continuous `axis` converted as today (`value × 12 / 256` per 10 units = 120).
  Values are accumulated per `wl_pointer.frame` and sent once per frame as one
  `InputPointerAxis`. `axis_relative_direction` (v9) inverted means natural
  scrolling: Waddle sends the logical direction (negates the raw value) so the
  guest scrolls the same way the host does.
- Coordinate conversion: `guest_px = floor(surface_logical × buffer_scale)`
  where `buffer_scale` is the effective scale factor of the surface
  (fractional scale numerator / 120 when `wp_fractional_scale_v1` is used,
  else integer `wl_surface` scale). Points outside the buffer are clamped to
  the last row/column only while a button is held (implicit grab); otherwise
  dropped.
- Buttons: BTN_LEFT/RIGHT/MIDDLE/SIDE/EXTRA → `MOUSEEVENTF_{LEFT,RIGHT,MIDDLE}`
  and `MOUSEEVENTF_XDOWN/XUP` with `XBUTTON1/XBUTTON2`. Others dropped.
- Guest injection keeps the current absolute virtual-desk path with the
  `WindowFromPoint` family check: the hit window must be the target or a
  descendant/owned window of the target family.

#### 3.6.5 Relative pointer and lock (Frozen)

- Guest condition for lock: foreground window is the focused exported window,
  cursor is hidden (`CURSORINFO.flags` lacks `CURSOR_SHOWING`) **and**
  `GetClipCursor` rect is fully inside the window's client rect and smaller
  than the window. Evaluated on `EVENT_OBJECT_HIDE/SHOW` for `OBJID_CURSOR` and
  every 100 ms while focused. On transition → `CursorConstraint{pressed}`.
- Host on `pressed=true`: `zwp_pointer_constraints_v1.lock_pointer(surface,
  pointer, NULL, persistent)`; on `locked` event start sending
  `InputPointerRelative` from `zwp_relative_pointer_v1.relative_motion`
  (`dx_unaccel/dy_unaccel` 24.8 fixed, scaled by buffer scale). On
  `pressed=false` or focus loss: destroy the lock; send absolute motion again.
- Guest injects `MOUSEEVENTF_MOVE` (relative, no `ABSOLUTE`) with the integer
  part, carrying fractional remainders per axis.

#### 3.6.6 Touch (Frozen)

- `wl_touch` down/motion/up per id (0..9), `frame` → `InputTouchFrame`,
  `cancel` → `InputTouchCancel`.
- Guest: one `CreateSyntheticPointerDevice(PT_TOUCH, 10,
  POINTER_FEEDBACK_DEFAULT)` per agent; each `InputTouchFrame` injects one
  `InjectSyntheticPointerInput` call with all active contacts
  (`POINTER_FLAG_DOWN|INRANGE|INCONTACT`, `UPDATE`, `UP`), contact ids mapped
  1:1. `InputTouchCancel` injects `POINTER_FLAG_CANCELED|UP` for all.
  Requires Windows 10 1809+; otherwise the capability bit is not offered.

#### 3.6.7 Tablet / pen (Frozen)

- Host binds `zwp_tablet_manager_v2` and per seat `zwp_tablet_seat_v2`. Tools
  of type `pen`, `pencil`, `brush` map to `InteropToolPen`; `eraser` to
  `InteropToolEraser`; others ignored.
- Axes: `pressure` (0..65535 → 0..1024 by `p*1024/65535`), `tilt` (degrees,
  wl_fixed → ×100), `rotation`; tip `down/up` → `pressed`; buttons
  `BTN_STYLUS`=1, `BTN_STYLUS2`=2. Each `zwp_tablet_tool_v2.frame` emits one
  `InputTablet`.
- Guest: `CreateSyntheticPointerDevice(PT_PEN, 1, …)`, injecting
  `POINTER_TYPE_INFO{PT_PEN}` with `penFlags` (`PEN_FLAG_BARREL`,
  `PEN_FLAG_INVERTED`/`PEN_FLAG_ERASER` for eraser), `penMask` = pressure |
  rotation | tiltX | tiltY.

#### 3.6.8 Gestures (Frozen)

- `zwp_pointer_gesture_pinch_v1`: on each `update` with cumulative `scale`
  s(t), the host computes `steps = round(log2(s(t)/s(last_sent)) × 4 × 120)`
  and, when nonzero, sends `InputGesturePinch{axis_v120=steps}`; the guest
  injects Ctrl down, wheel `steps`, Ctrl up (Ctrl state restored if the user
  holds Ctrl). Swipe and hold gestures are not bound.

### 3.7 Cursor (Frozen)

- Guest: `SetWinEventHook(EVENT_OBJECT_NAMECHANGE/SHOW/HIDE)` filtered to
  `OBJID_CURSOR`, plus a 50 ms poll of `GetCursorInfo` while a Waddle window is
  focused. On change of `hCursor` or showing flag: if the handle equals one of
  the system cursors loaded with `LoadCursorW(NULL, IDC_*)` (Section 3.2.4
  table) send `CursorShape{shape}`; if hidden send `CursorShape{Hidden}`;
  otherwise extract with `GetIconInfoExW` + `GetDIBits` to 32-bit
  premultiplied ARGB (≤ 256×256, monochrome cursors converted with AND/XOR
  rules: XOR-only inverting pixels rendered black with alpha 255), hash with
  SHA-256, send `CursorShape{Custom, hotspot, content_hash}`; the host
  requests the bitmap through a transfer only if the hash is not in its LRU
  cache (64 entries).
- Host: standard shapes use `wp_cursor_shape_device_v1.set_shape` when the
  global exists; otherwise a fallback bitmap is loaded from the host cursor
  theme with `wl_cursor_theme_load` (libwayland-cursor, part of the base
  wayland package) by the matching CSS cursor name. Custom bitmaps use a
  `wl_shm` cursor surface with buffer scale matching the pointer's output.
  `Hidden` sets a NULL cursor surface. The cursor is applied on each
  `wl_pointer.enter` with that enter's serial and whenever the shape changes
  while the pointer is inside a Waddle surface.
- WGC capture keeps `IsCursorCaptureEnabled(false)` so the cursor is never
  drawn twice.

### 3.8 App catalog, icons, desktop entries

#### 3.8.1 Guest catalog source (Design)

- Enumeration: `SHGetKnownFolderItem(FOLDERID_AppsFolder)` →
  `IShellItem::BindToHandler(BHID_EnumItems)` → for each child:
  `PKEY_AppUserModel_ID` (AUMID), `PKEY_ItemNameDisplay` (display name),
  `PKEY_Link_TargetParsingPath` (target, when a desktop app),
  `PKEY_AppUserModel_IsDestListSeparator` skipped, packaged apps detected with
  `PKEY_AppUserModel_PackageFamilyName`.
- Additional sources merged by app key: `.lnk` files in
  `FOLDERID_CommonPrograms`, `FOLDERID_Programs`, `FOLDERID_PublicDesktop`,
  `FOLDERID_Desktop` (resolved with `IShellLinkW`), and
  `HKLM/HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\*`
  (`DisplayName`, `DisplayIcon`, `InstallLocation`) to set
  `InteropAppUninstallable`.
- Change detection: `SHChangeNotifyRegister` on the AppsFolder PIDL
  (`SHCNE_CREATE|DELETE|RENAMEITEM|UPDATEITEM|UPDATEDIR`),
  `ReadDirectoryChangesW` on the four folders above (recursive for Programs),
  `RegNotifyChangeKeyValue` on both Uninstall keys; each event schedules a
  debounced (1500 ms) re-enumeration; a full re-diff also runs every 30 s.
  The diff emits `CatalogAdd/Update/Remove` only for changed entries.

#### 3.8.2 App key (Frozen)

- If the entry has an AUMID: `app_key = "a-" + sanitize(aumid)`.
- Else (unpackaged exe without AUMID): `app_key = "p-" + lowercase hex of the
  first 16 bytes of SHA-256(UTF-16LE of the upper-cased canonical target
  path)`.
- Convert the original AUMID to UTF-8 without a trailing NUL; accept 1..4096
  bytes of valid UTF-8 with no U+0000. Preserve ASCII `[A-Za-z0-9_-]`, map
  every other UTF-8 byte to `_`. Hash the original UTF-8 bytes, with no Unicode
  normalization or case folding. If any byte was mapped or the original is
  longer than 198 bytes, reserve 9 bytes for `-` plus the first 8 lowercase
  hex digits of SHA-256: keep at most **189** sanitized bytes. Otherwise keep
  all original bytes (at most 198), with no suffix. Add the two-byte `a-`
  prefix. Final length is 3..200 bytes and is not NUL-terminated on the wire.
  The suffix reduces collisions; it does not prove uniqueness. The future
  catalog must reject a duplicate app key for distinct canonical identities
  with a diagnostic, rather than silently replacing an entry.
- `interop_app_key_from_aumid()` is the allocation-free C ABI implementation
  used by the future catalog. It consumes all input before writing, permits
  overlapping input/output byte storage, and leaves output bytes/length
  unchanged for invalid input or insufficient capacity. Tests use independent
  SHA-256 vectors, 198/199/4096-byte inputs, multibyte text and sanitization
  collisions. The `p-` canonical-path hash mechanism is unchanged.
- Windows are tagged with the app key of
  `SHGetPropertyStoreForWindow(hwnd)→PKEY_AppUserModel_ID` if set, else the
  process's explicit AUMID (`GetApplicationUserModelId`), else the process
  image path (`QueryFullProcessImageNameW`) mapped through the `p-` rule.
  This matches how the Windows taskbar groups windows.

#### 3.8.3 Icons (Frozen)

- Sizes: 16, 24, 32, 48, 64, 128, 256 px. Extraction:
  `IShellItemImageFactory::GetImage(size, SIIGBF_ICONONLY |
  SIIGBF_BIGGERSIZEOK)` on the catalog item; fallback
  `SHGetFileInfoW(SHGFI_SYSICONINDEX)` + `IImageList::GetIcon` for 16/32/48.
  Per-window icons: `WM_GETICON(ICON_BIG/ICON_SMALL2)` via
  `SendMessageTimeoutW` (100 ms), fallback `GetClassLongPtrW(GCLP_HICON)`.
- Encoding: PNG, 8-bit RGBA, non-interlaced, written by the agent's own
  minimal encoder (`src/interop/png_writer.c`, stored deflate blocks, CRC32 and
  Adler32; no compression library needed). Icon content hash = SHA-256 of the
  PNG bytes.
- Host install path:
  `$XDG_DATA_HOME/icons/hicolor/<size>x<size>/apps/waddle.<device>.<app_key>.png`;
  after any icon write the host touches `$XDG_DATA_HOME/icons/hicolor` mtime
  (themes are re-read by mtime) and runs `gtk-update-icon-cache -q -t -f` if
  it exists in `PATH` (best effort, failure ignored).

#### 3.8.4 Menu filter (Frozen)

- `menu_apps=installed`: entries without `InteropAppSystem`, plus entries with
  `InteropAppSeed`. `all`: every entry. `none`: no menu entries (taskbar icons
  still work through `NoDisplay` entries, Section 3.8.5).
- Seed list (AUMIDs): `Microsoft.Windows.Explorer` (File Explorer),
  `Microsoft.Windows.Notepad` / `Microsoft.WindowsNotepad_8wekyb3d8bbwe!App`,
  `Microsoft.Windows.Shell.RunDialog` excluded, `Microsoft.WindowsTerminal_8wekyb3d8bbwe!App`
  (if present), `{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\cmd.exe` (Command
  Prompt), `{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\WindowsPowerShell\v1.0\powershell.exe`,
  `windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel`
  (Settings).

#### 3.8.5 Desktop entry format (Frozen)

File: `$XDG_DATA_HOME/applications/waddle.<device>.<app_key>.desktop`.
`<device>` is the device name sanitized with the same rule as app keys.
Desktop-file ID = basename without `.desktop`, which is also the Wayland
`app_id` of every window tagged with that app key.

```ini
[Desktop Entry]
Type=Application
Version=1.5
Name=<display_name>
Comment=Windows application (<device>)
Exec=<waddle_abs_path> open --device <device> --app <app_key> %F
TryExec=<waddle_abs_path>
Icon=waddle.<device>.<app_key>
StartupWMClass=waddle.<device>.<app_key>
StartupNotify=true
Categories=X-Waddle;X-Waddle-<device>;
Terminal=false
NoDisplay=<true when filtered out by menu_apps or uncatalogued>
X-Waddle-Device=<device>
X-Waddle-AppKey=<app_key>
X-Waddle-Schema=1
```

Escaping per the Desktop Entry spec (`\s \n \t \r \\` in values; `Exec`
arguments quoted by the spec's reserved-character rules) is implemented and
validated in `src/interop/desktop_entry.zig`; display names containing control
characters are rejected and replaced by the app key.

Uncatalogued windows (process image without a catalog entry) receive an entry
with `NoDisplay=true`, `Name` = window title at first sight (truncated to 64
bytes), `Exec` launching the image path, created **before** the toplevel's
first commit so the compositor finds it.

KDE/XFCE submenu: `$XDG_CONFIG_HOME/menus/applications-merged/waddle-<device>.menu`
including `X-Waddle-<device>` in a "Windows (<device>)" submenu, plus
`$XDG_DATA_HOME/desktop-directories/waddle-<device>.directory`.

#### 3.8.6 Linux desktop shortcuts (Frozen)

With `desktop_shortcuts=mirror`, each catalog entry with
`InteropAppDesktopShortcut` gets a copy of its desktop entry at
`$(xdg-user-dir DESKTOP)/waddle.<device>.<app_key>.desktop` (fallback
`$HOME/Desktop`), mode 0755, then `gio set <file> metadata::trusted true`
(best effort, GNOME/Nautilus; KDE needs only the executable bit). Removal of
the guest `.lnk` removes the Linux file only if its content hash still equals
the manifest record (user-edited files are left in place and reported by
`status`).

### 3.9 Launching

#### 3.9.1 `waddle open` (Frozen grammar)

```
waddle open [--device|-d NAME] [--wait] [--new-console] (--app APP_KEY | PATH... | -- EXE [ARG...])
```

- `--app`: launch catalog entry (`InteropLaunchApp`).
- `PATH...`: each path is classified by extension and, for `.exe`, by the PE
  parser (Section 3.9.2): `.exe` GUI → `InteropLaunchPath`; `.exe` CUI → if
  stdin/stdout are a TTY and `--new-console` is absent, delegated to the
  existing `waddle exec --translate-path -- <guest path> args` (stdio and exit
  code); otherwise `InteropLaunchConsole`; `.msi` → `InteropLaunchInstaller`;
  `.lnk` → `InteropLaunchShortcut`; anything else → `InteropLaunchPath` (the
  guest's own file association handles it).
- `-- EXE ARG...`: like a single `.exe` PATH with explicit arguments, argv
  quoted with `waddle_quote()`.
- Every host path is canonicalized with `realpath` and translated with the
  device's mount rules; a path outside every share fails with exit code 126,
  stderr message, and (when `integration.notify`) a desktop notification
  "'<file>' is outside the folders shared with Windows (<device>). Add a share
  with: waddle device config set filesystem.<name> <dir>:<drive>:\".
- `XDG_ACTIVATION_TOKEN` from the environment (and `DESKTOP_STARTUP_ID` as
  fallback) is forwarded as `activation_token`.
- `--wait` waits for `LaunchResult` and returns 0 / 1; without it `waddle open`
  returns as soon as the request is handed to `waddle-session` (0) or fails
  (1). Desktop entries use the default (no wait).
- Exit codes: 0 ok, 1 guest launch failure, 2 usage, 125 local error, 126
  outside share / not executable, 127 not found.

`waddle open` itself talks to `waddle-session` over the UNIX socket
`$XDG_RUNTIME_DIR/waddle/<device>/session.sock` with a small request frame
(`interop_header_t` reused, type `0x7F00 LocalLaunch` carrying the
`LaunchRequest` fields); `waddle-session` forwards it to the guest. If no
`waddle-session` is running, `waddle open` spawns it (detached, `setsid`) and
retries the socket for up to 120 s while sending the "Starting Windows…"
notification once after 1.5 s.

#### 3.9.2 PE classification (Frozen)

`src/interop/pe_header.zig` reads at most the first 4096 bytes:
`e_magic == "MZ"`, `e_lfanew` (u32 at 0x3C) in `[0x40, 4096-24]`, signature
`"PE\0\0"`, `OptionalHeader.Magic` 0x10B or 0x20B, `Subsystem` u16 at
optional-header offset 68. `IMAGE_SUBSYSTEM_WINDOWS_GUI (2)` → GUI;
`WINDOWS_CUI (3)` → console; anything else or any malformed field → GUI
(ShellExecute decides). Read errors → exit 126.

#### 3.9.3 Guest launch (Design)

- `InteropLaunchApp` with AUMID: packaged apps via
  `IApplicationActivationManager::ActivateApplication(aumid, args,
  AO_NONE, &pid)`; unpackaged AUMID or `p-` keys via
  `ShellExecuteExW(SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI,
  verb="open", file="shell:AppsFolder\<aumid>")` or the target path.
- `Path/Installer/Shortcut`: `ShellExecuteExW` with the translated guest path,
  `lpDirectory` = working_dir or the file's directory, `nShow=SW_SHOWNORMAL`.
  `.msi` → `msiexec.exe /i "<path>"`.
- `Console`: `CreateProcessW` with `CREATE_NEW_CONSOLE`; the console window
  (conhost or Windows Terminal) is a normal exported window.
- Launched processes are **not** placed in a job and are not tied to the
  connection; they outlive host disconnects.
- File Explorer: app key `a-Microsoft_Windows_Explorer-<hash>` launches
  `explorer.exe` with no arguments via the AUMID path, which opens a
  `CabinetWClass` window in the running shell process.
- `LaunchResult{status, process_id}`; the host stores `(activation_token,
  process_id)` for 30 s and, when the first window announced with that
  process id (or, for `explorer.exe` and `ApplicationFrameHost.exe`, the first
  new toplevel of the launched app key) gets its first commit, calls
  `xdg_activation_v1.activate(token, surface)`.

### 3.10 Clipboard and drag-and-drop

#### 3.10.1 Format mapping (Frozen)

| Host MIME (offer order) | Guest format | Conversion |
|---|---|---|
| `text/plain;charset=utf-8`, `text/plain`, `UTF8_STRING`, `STRING`, `TEXT` | `CF_UNICODETEXT` | UTF-8 ↔ UTF-16LE, `\n` ↔ `\r\n`, terminating NUL added/stripped |
| `text/html` | `HTML Format` (registered) | Add/strip CF_HTML header (`Version:0.9`, `StartHTML`/`EndHTML`/`StartFragment`/`EndFragment` byte offsets, `<!--StartFragment-->` markers) |
| `text/rtf`, `application/rtf` | `Rich Text Format` (registered) | Bytes unchanged |
| `image/png` | `PNG` (registered) and `CF_DIBV5` | Guest→host: prefer `PNG`; else convert `CF_DIBV5`/`CF_DIB` to PNG with `png_writer.c`. Host→guest: provide `PNG` and synthesize `CF_DIBV5` (32-bit BI_BITFIELDS, top-down) from a decoded PNG; host decodes PNG with `src/interop/png_reader.zig` (bounded, supports 8-bit RGB/RGBA/gray, non-interlaced; other PNGs offered only as `PNG`) |
| `text/uri-list`, `x-special/gnome-copied-files`, `application/x-kde-cutselection` | `CF_HDROP` + `Preferred DropEffect` | Paths translated with mount rules both ways; `gnome-copied-files` first line `copy`/`cut` ↔ `DROPEFFECT_COPY`/`DROPEFFECT_MOVE`; KDE cutselection `1` = cut |

#### 3.10.2 Selection ownership (Frozen)

- `clipboard_epoch` increases by 1 for every new selection by either side;
  each side tracks the last epoch it observed; stale requests are answered
  `TransferCancel{NotFound}`.
- Host → guest: on `wl_data_device.selection(offer)` (delivered only while a
  Waddle surface has keyboard focus), the host sends `ClipboardOffer` with the
  mapped MIME list (only types in 3.10.1). The guest takes ownership with
  `OpenClipboard(agent_hwnd)`, `EmptyClipboard`, `SetClipboardData(fmt, NULL)`
  for each mapped format (delayed rendering). On `WM_RENDERFORMAT` the agent
  sends `ClipboardRequest`, waits up to 5 s (pumping only transfer frames) and
  calls `SetClipboardData` with the converted data; on timeout it renders
  nothing (application sees failure). `WM_RENDERALLFORMATS` on agent exit
  renders all formats with the same rules.
- Guest → host: `AddClipboardFormatListener(agent_hwnd)`; on
  `WM_CLIPBOARDUPDATE` where the owner is not the agent, the agent enumerates
  formats and sends `ClipboardOffer`. The host creates a `wl_data_source`
  offering mapped MIME types and calls `wl_data_device.set_selection(source,
  serial)` with the latest keyboard-enter/key serial of a focused Waddle
  surface. If no Waddle surface is focused, the offer is held and applied on
  the next `keyboard.enter`. On `wl_data_source.send(mime, fd)` the host sends
  `ClipboardRequest` and streams `TransferChunk`s to the fd (non-blocking,
  bounded 1 MiB pending, closed at `TransferEnd` or on cancel).
- File payloads guest → host: if all `CF_HDROP` paths translate to host paths,
  the URIs reference them directly. Otherwise (e.g. `C:\Users\...`), on the
  first host `send(text/uri-list)` the agent copies each file/directory
  (recursive, stop at `clipboard.max_mb` cumulative) into
  `<home share>\.cache\waddle\clipboard\<epoch>\`, where `<home share>` is the
  guest drive of the mount whose host path is `$HOME` (default `Z:\`; if no
  mount covers `$HOME`, file formats are not offered guest → host for
  non-shared paths), and returns those translated host paths; the host deletes clipboard cache directories older
  than the 4 most recent epochs and all of them at `waddle-session` start.
- File payloads host → guest: every URI must be `file://` and translate to a
  share; non-translatable entries are dropped; if none remain the format is not
  offered and a notification explains the share rule.
- `clipboard.files=false` removes the file formats from both directions.

#### 3.10.3 Drag and drop host → guest (Design)

1. `wl_data_device.enter` on a Waddle surface with an offer containing a mapped
   type → `DndEnter{point, mime_list, drop_effect}`; host `offer.accept` of the
   best type and `set_actions(copy|move, copy)`.
2. `motion` → `DndMotion`. Guest agent, on `DndEnter`, starts a dedicated STA
   thread that calls `DoDragDrop(data_object, drop_source, DROPEFFECT_COPY |
   DROPEFFECT_MOVE)` with a data object rendering formats lazily via
   `ClipboardRequest`-style transfers; the agent injects absolute mouse moves
   to each `DndMotion` point and a left-button down at start (OLE drag loop
   requires a held button) — the injected button is the agent's synthetic
   state; the host user's real button is not held in the guest.
3. `drop` → `DndDrop`; agent injects the move to the point and the button up,
   `DoDragDrop` returns the effect, agent sends `DndResult`; host
   `offer.finish` and `set_actions` from the result. `leave` → `DndLeave`:
   agent injects `Esc` to cancel the OLE loop and releases the synthetic
   button.

#### 3.10.4 Drag and drop guest → host (Design)

1. While a guest button is held on a focused Waddle window and the host
   receives `wl_pointer.leave` for that surface, the host sends nothing new;
   the guest agent, on detecting an OLE drag in progress (a `DoDragDrop` loop
   is observable as the system drag cursor `IDC_*` shape + `GetCapture` owned
   by an OLE window class `CLIPBRDWNDCLASS`), shows a transparent topmost
   layered window covering the guest desktop that implements `IDropTarget`.
2. The catcher's `DragEnter` receives the source `IDataObject`; the agent sends
   `DndSourceStart{mime_list, drop_effect}`; the host calls
   `wl_data_device.start_drag(source, origin_surface, NULL, last_button_serial)`.
3. The compositor drives the drag on the host. On the destination's
   `send(mime, fd)` the host sends `DndSourceRequest`; the agent renders from
   the held `IDataObject`. On `dnd_finished`/`cancelled` the host sends
   `DndSourceFinished`; the agent completes `Drop` (or `DragLeave`) on the
   catcher with the effect, so the source app sees the result (move deletes the
   source file only when the effect is move), then hides the catcher.

#### 3.10.5 IME (Frozen)

- `input.ime=guest` (default): no host text-input object is created. Keys go as
  scan codes; the guest's Windows IME (selected by `KeyboardLayout` sync)
  composes inline; its candidate window is a guest popup exported with
  `InteropRoleTooltip`. Host IBus/Fcitx shortcuts are inhibited for Waddle
  surfaces through `zwp_keyboard_shortcuts_inhibitor_v1` only while
  `inhibit_shortcuts=focused`.
- `input.ime=host`: the host creates `zwp_text_input_v3` for the seat, enables
  it on keyboard enter of a Waddle surface, and sets content type `normal`.
  `commit_string` → `InputTextCommit`; the guest injects each UTF-16 code unit
  with `SendInput(KEYEVENTF_UNICODE)` (surrogate pairs as two events).
  `preedit_string` → `InputTextPreedit`; the guest shows an agent-owned layered
  window (`InteropRoleImePreedit`) at the caret, drawing the text with GDI in
  the system UI font, underline, and caret at `cursor_begin..cursor_end`;
  empty text hides it. Guest `CaretRect` (from `GetGUIThreadInfo` of the
  foreground thread, `rcCaret` mapped to window-relative pixels, polled every
  100 ms while focused and on `EVENT_OBJECT_LOCATIONCHANGE` for `OBJID_CARET`)
  → host `set_cursor_rectangle` in surface logical coordinates, then `commit`.
  While host IME composes, key events the input method consumes never reach
  `wl_keyboard`, so no double input occurs.

#### 3.10.6 Primary selection (Frozen)

The host binds `zwp_primary_selection_device_manager_v1` read-only. A middle
click on a focused Waddle surface whose window is not a text-editing control
is forwarded as a middle click. Primary selection text is never pushed into
the guest clipboard. (Recorded so the behavior is unambiguous.)

### 3.11 UAC handling (Frozen)

Registry base: `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System`.

| Mode | `PromptOnSecureDesktop` | `EnableUIADesktopToggle` | `ConsentPromptBehaviorAdmin` | Behavior |
|---|---|---|---|---|
| `window` (default) | 0 | 1 | unchanged (default 5) | `consent.exe` dialog exported as `InteropRoleUacConsent` toplevel, app_id `waddle.<device>.uac`, icon = Windows shield (`SIID_SHIELD` via `SHGetStockIconInfo`), activated with the most recent launch's activation token if any, else `xdg_activation` without token (compositor may show "demands attention") |
| `auto` | 0 | 1 | 0 | Elevation without prompt; agent sends `ElevationNotice` for each process started with `TokenElevationTypeFull` (WMI `__InstanceCreationEvent` on `Win32_Process` polled at 1 s, then `OpenProcessToken` + `GetTokenInformation(TokenElevationType)`); host shows notification "<display_name> was granted administrator rights in Windows (<device>)" |
| `native` | 0 | 1 | unchanged | Agent hides the consent window from export, reads it with UI Automation (`IUIAutomation::ElementFromHandle`, names of the program, publisher and file-path text elements), sends `UacPrompt`; host sends a notification with actions `allow`/`deny` via `org.freedesktop.Notifications.Notify` (libdbus, session bus); action → `UacDecision`; agent invokes the "Yes"/"No" button through `IUIAutomationInvokePattern::Invoke` (buttons found by `AutomationId` `TryAgainButton`/`CancelButton` fallback by control type + order); 60 s timeout or notification closed → deny |

- Before writing, the agent records prior values (absent/DWORD) in the guest
  manifest (Section 3.12.3); `waddle integration remove --guest` restores them.
- `EnableLUA` is never modified.
- `auto` requires the logged-on user to be in Administrators; otherwise
  setup fails with `status=AccessDenied` and message "uac=auto requires an
  administrator guest account", leaving policy unchanged.
- Secure-desktop safety net: the agent polls `OpenInputDesktop` every 250 ms;
  when the input desktop name is not `Default` (e.g. `Winlogon`), it sends
  `DesktopSwitch{pressed=true}` once; when it returns, `pressed=false`. The
  host shows the notification "Windows (<device>) is showing a secure prompt
  Waddle cannot display. Run: waddle integration repair --device <device>"
  and stops forwarding input until `pressed=false`.

### 3.12 Integration manifest (Frozen)

#### 3.12.1 Location and journaling

- User manifest: `$XDG_STATE_HOME/waddle/integration/manifest.json`
  (fallback `~/.local/state/...`).
- System manifest: `/var/lib/waddle/integration/manifest.json` (root only).
- Mutation uses the `device_storage.zig` journal protocol: write the intended
  new manifest to `transactions/<id>.json`, fsync, perform filesystem actions,
  rename the new manifest over the old one, fsync the directory, delete the
  journal. On start, `waddle integration` and `waddle-session` replay or roll
  back any journal left behind (`recover`).
- Exclusive lock: `flock` on `$XDG_STATE_HOME/waddle/integration/manifest.lock`.

#### 3.12.2 Schema version 1

```json
{
  "schema": 1,
  "waddle_version": "0.2.0",
  "updated": "2026-10-22T01:00:00Z",
  "records": [
    {
      "id": "desktop:waddle.default.a-Microsoft_Windows_Explorer-1a2b3c4d",
      "kind": "desktop_entry",
      "path": "/home/u/.local/share/applications/waddle.default.a-Microsoft_Windows_Explorer-1a2b3c4d.desktop",
      "sha256": "<64 hex>",
      "device": "default",
      "app_key": "a-Microsoft_Windows_Explorer-1a2b3c4d",
      "created_by": "0.2.0",
      "prior": null
    }
  ]
}
```

`kind` values and their `prior` meaning:

| kind | path | prior |
|---|---|---|
| `desktop_entry` | applications/*.desktop | null |
| `icon` | hicolor PNG | null |
| `menu` | applications-merged/*.menu or desktop-directories/*.directory | null |
| `desktop_shortcut` | Desktop/*.desktop | null |
| `mime_default` | `$XDG_CONFIG_HOME/mimeapps.list` + `"mime": "<type>"` | previous default desktop ID or null |
| `autostart` | autostart/*.desktop | null |
| `handler_entry` | applications/waddle-open.desktop | null |
| `binfmt` (system) | /etc/binfmt.d/waddle.conf | previous file sha256 or null |
| `polkit` (system) | /usr/share/polkit-1/actions/... | null |
| `guest_policy` | `HKLM\...\Policies\System\<value>` + `"device"` | previous DWORD or `"absent"` |
| `guest_component` | `C:\Program Files\Waddle\session\<version>` | null |
| `guest_autostart` | `HKLM\...\Run\WaddleSession` | previous string or `"absent"` |
| `guest_certificate` | `Cert:\LocalMachine\Root\<thumbprint>` | null |
| `guest_printer` | printer name | null |

Records are unique by `id`. Unknown `kind` in a newer schema → commands refuse
to modify and report `schema newer than this Waddle`.

#### 3.12.3 Guest manifest

The guest agent keeps the guest-side records in
`C:\ProgramData\Waddle\manifest.json` (same schema, guest kinds only) and
mirrors them to the host manifest through `waddle integration status`.
`remove --guest` is executed by the agent (it runs as the guest user with
UIAccess; HKLM writes are performed by an elevated helper invocation
`waddle-guest-session.exe --apply-policy <file>` started with
`ShellExecuteExW(verb="runas")`, which prompts once under the currently active
UAC mode).

#### 3.12.4 Commands (Frozen grammar)

```
waddle integration install [--device N] [--system]
waddle integration remove  [--device N | --all] [--guest] [--system]
waddle integration repair  [--device N] [--system] [--force]
waddle integration status  [--device N] [--system] [--json]
waddle integration migrate [--system]
waddle integration shortcut add|remove [--device N] APP_KEY
```

- `install`: handler entry + MIME defaults (when `exe_handler=waddle`),
  autostart (when `start_at_login`), guest component deployment and policies,
  then a catalog sync if the guest is running.
- `remove`: deletes every record's artifact whose current sha256 equals the
  record (others are reported and kept), restores `prior` values (MIME
  defaults, guest policies, guest autostart), deletes the records.
- `repair`: recreates missing managed artifacts from current state. Modified
  managed files are preserved and reported unless `--force` is explicit;
  without it exit 1 if any repair is blocked. `--force` applies only to
  recorded artifacts in the selected user/system scope, never foreign files,
  symlinks or a newer manifest schema. Before replacing a modified regular
  file, atomically preserve its exact bytes and mode under
  `<manifest directory>/backups/<transaction id>/<record id SHA-256>` (mode
  0600 for the backup), fsync it, then perform the journaled replacement.
  If backup fails, leave the artifact untouched and fail. Retain backups
  after success and rollback; `remove` does not delete user backups. No flag
  bypasses schema, path, permission or journal validation. Exit 0 only when
  every selected artifact is repaired, 1 for blocked/failed work, 2 for usage.
- `status`: lists records with state `ok|missing|modified|foreign`, binfmt
  conflicts (another `MZ` magic rule, e.g. Wine's `DOSWin`, with its order in
  `/proc/sys/fs/binfmt_misc/`), and guest policy drift.
- `migrate`: rewrites records from older schema/waddle_version; invoked by the
  future package's post-install hook; `waddle-session` also runs the
  user-level migration at start when `waddle_version` differs.
- `--system` operations require euid 0 and touch only the system manifest.
- Uninstall model: the package's pre-remove hook runs `remove --system`. User
  artifacts cannot be removed for every user by a package; they become inert
  because every desktop entry has `TryExec=<waddle path>` (desktop environments
  hide entries whose `TryExec` is missing) and MIME defaults pointing at a
  missing desktop ID are ignored by `xdg-open`/GIO. `waddle integration remove
  --all` is documented for full cleanup.

#### 3.12.5 Session lifecycle commands (Frozen grammar)

```
waddle session stop [--device|-d NAME]
```

The CLI resolves the selected device using existing device-selection rules and
connects to that device's `session.sock` with same-user filesystem permissions
and peer credential checks. Protocol v2 local-only type `0x7F01 LocalStop`
has no fields, request_id nonzero, and is invalid on VSOCK. The session client
replies local-only `0x7F02 LocalStopResult` with required `status` and optional
`message`, echoing request_id, after draining held input, surfaces, audio and
transfers and releasing both session/AV command locks. A successfully queued
reply may be sent by a bounded closing connection after resource cleanup.
The session does not reconnect or auto-respawn itself in response to stop.
Existing guest applications and the VM keep running. A later explicit `open`
may start another session. This command does not remove autostart settings.

No socket and an unlocked session lock means already stopped (exit 0). A held
lock while the socket is missing, EOF without a result, or no result within
5 seconds means stop is unconfirmed (exit 1); do not unlink a live lock/socket
or signal an arbitrary PID. Non-Ok result exits 1; usage exits 2; unknown device
uses existing CLI device errors. `waddle stop` still shuts down the whole device;
its existing daemon/VM shutdown causes session cleanup through disconnect.
Runtime command dispatch and socket credential enforcement remain TODO #7.

#### 3.12.6 Implemented codec interface rules

Every C declaration and Zig export documents each input/output direction,
non-NULL requirements, permitted zero-length NULL buffers, return codes,
ownership and threading. Callers own frame, payload, writer, transfer and hash
storage; these functions allocate nothing. Field readers return borrowed views
valid only while the validated payload is unchanged. Except for the AUMID key
helper's explicitly supported overlapping byte buffers, outputs/state must be
disjoint from inputs and other output objects.

Readers, AUMID generation and transfer updates leave outputs unchanged on
failure. Writer failure is intentionally different: it sets a sticky `failed`
flag, preserves previously accepted frame bytes, and prevents publication.
Only successful finish writes the header. Init borrows frame storage until the
writer is reset or discarded; successful finish does not free or take ownership
of it. Transfer helpers account/hash bytes but do not own payload storage or
enforce the future connection's concurrent-transfer quota. Callers serialize
shared writer/frame or transfer state; independent instances are thread-safe.

Owned Zig types, including namespace type aliases, use `snake_case_t`; registry
arrays, message tables, tag constants and compile-time fixtures use PascalCase.
Runtime locals, members, functions and generic type parameters use snake_case
(type parameters also end in `_t`). Vendor standard-library and imported C ABI
member spellings remain unchanged. The public C symbol names and ABI layout are
unchanged by this naming/documentation cleanup. Declaration checks verify all
18 public C functions and all 18 Zig exports have structured documentation;
existing codec, C ABI, coverage, sanitizer and Windows cross-link gates exercise
the renamed implementation.

### 3.13 binfmt_misc (Frozen)

- File `data/binfmt.d/waddle.conf`, installed to `/usr/lib/binfmt.d/` by the
  package or to `/etc/binfmt.d/` by `waddle integration install --system`:
  `:waddle-pe:M::MZ::/usr/libexec/waddle/waddle-binfmt:`
  (no flags: argv[1] is the full path of the executable; `P` not used).
- `waddle-binfmt PATH [ARGS...]`: classify with the PE parser; console images
  run as `waddle exec --translate-path -- PATH ARGS...` with stdio and exit
  code; GUI images run as `waddle open --wait -- PATH ARGS...`. Device: the
  default device.
- Applied with `systemd-binfmt /etc/binfmt.d/waddle.conf` if available, else
  by writing the rule to `/proc/sys/fs/binfmt_misc/register`.
- Wine coexistence: Wine registers `:DOSWin:M::MZ::/usr/bin/wine:`; the kernel
  uses the first enabled matching rule in registration order. `status` reports
  both rules and which one wins; `install --system` refuses to proceed when
  another `MZ` rule is enabled unless `--replace-mz` is given, in which case it
  writes `-1` to that rule's `binfmt_misc/<name>` entry (disabling it until
  reboot) and records it as `prior`.

### 3.14 Per-user MIME handler (Frozen)

`$XDG_DATA_HOME/applications/waddle-open.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=Run with Windows (Waddle)
Exec=<waddle_abs_path> open %F
TryExec=<waddle_abs_path>
Icon=waddle
NoDisplay=true
Terminal=false
MimeType=application/x-ms-dos-executable;application/x-msdownload;application/vnd.microsoft.portable-executable;application/x-msi;application/x-ms-shortcut;application/x-msdos-program;application/x-ms-installer;
X-Waddle-Schema=1
```

With `exe_handler=waddle`, for each listed MIME type the previous default from
`xdg-mime query default <type>` is recorded as `prior`, then the
`[Default Applications]` group of `$XDG_CONFIG_HOME/mimeapps.list` is edited
directly (parsed and rewritten by `src/interop/mimeapps.zig`, preserving all
other groups and lines) and `update-desktop-database
$XDG_DATA_HOME/applications` is run best effort. With `off`, priors are
restored. `.exe` files without the executable bit still open through this
association; files with it also do (file managers prefer the MIME association
for non-native executables).

### 3.15 Audio (Frozen)

- Output: `IAudioSessionManager2::RegisterSessionNotification` on the default
  render endpoint plus initial `GetSessionEnumerator`. Each session with a
  nonzero process id and state `AudioSessionStateActive` gets a stream index
  (lowest free of 8); the agent activates process loopback for that PID with
  `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE` (existing
  `av_wasapi.c`), writes into ring `n`, sends `AudioStreamOpen{stream, app_key,
  display_name, process_id}`. Session expired or inactive for 10 s → close.
  With `audio.per_app=false` the agent still captures per process (Windows
  offers loopback only per process tree), mixes all active sessions by
  saturating 16-bit addition into ring 0, and opens a single host stream named
  "Windows (<device>)".
- Host: one `pw_stream` per open stream, properties
  `node.name=waddle.<device>.<stream>`, `application.name=<display_name>`,
  `application.icon-name=waddle.<device>.<app_key>`,
  `media.category=Playback`, `media.role=Music`, and the same backlog trimming
  as `av_pipewire.c` (384 frames).
- Microphone: QEMU `-audiodev pipewire,id=waddle_audio,out.name=waddle-discard.<device> (in.name omitted: default source)`
  with `ich9-intel-hda` + `hda-duplex,audiodev=waddle_audio`. `waddle-session`
  creates the sink `waddle-discard.<device>` at start through
  `pw_core_create_object("adapter", factory.name=support.null-audio-sink,
  media.class=Audio/Sink, node.name=waddle-discard.<device>,
  node.passive=true, node.virtual=true)` and keeps it unlinked, so the guest
  HDA output (which the per-app loopback already duplicates) is discarded.
  When `waddle-session` is not running the VM is started with
  `out.name` unchanged and `-audiodev` still pipewire, so HDA output plays once
  (no per-app streams exist without a session); this is the documented
  degraded mode.

### 3.16 USB (Frozen)

- `waddle usb list [--device N]`: host USB devices from
  `/sys/bus/usb/devices/*/{idVendor,idProduct,busnum,devnum,product}`
  (hubs and the root hub excluded), with attached state from the daemon.
- `waddle usb attach [--device N] VVVV:PPPP | BUS-ADDR` and
  `waddle usb detach [--device N] VVVV:PPPP | BUS-ADDR | --all`.
- Permission: if `/dev/bus/usb/BBB/DDD` is not read/write for the user, the CLI
  runs `pkexec /usr/libexec/waddle/waddle-usb-grant grant BBB DDD` which
  adds a user ACL (`setfacl -m u:<uid>:rw`); detach runs `... revoke`. Polkit
  action `org.waddle.usb.grant`, default `auth_admin_keep`, shipped in
  `data/polkit/org.waddle.usb.policy`.
- Daemon: QMP `device_add {"driver":"usb-host","bus":"waddle_xhci.0",
  "hostbus":B,"hostaddr":D,"id":"waddle_usb_<B>_<D>"}`; detach `device_del`.
  Attachments are not persisted across VM restarts except `usb.auto_attach`.
- Webcams and wired/2.4 GHz-dongle gamepads use this path. The host loses
  access to an attached device until detach.

### 3.17 Printing (Frozen)

- QEMU (when `network.mode=user` and `cups_forward=true` and the socket
  exists): `-chardev socket,id=waddle_cups,path=/run/cups/cups.sock` and
  `-netdev user,id=waddle_net,guestfwd=tcp:10.0.2.100:631-chardev:waddle_cups`
  with `-device virtio-net-pci,netdev=waddle_net`.
- Host: `waddle-session` lists queues with libcups (`cupsGetDests2` on
  `CUPS_HTTP_DEFAULT`) at start and on `cupsGetDests2` re-poll every 60 s;
  differences → `PrinterSync{printer_name, printer_uri="http://10.0.2.100:631/printers/<name>",
  pressed, display_name}`.
- Guest: elevated helper invocation (as in 3.12.3) runs
  `Add-PrinterPort`-equivalent via the Print Spooler API: `AddPrinterW` level 2
  with `pDriverName="Microsoft IPP Class Driver"`, `pPortName` an IPP port
  created by `XcvDataW(L"AddPort")` on the standard TCP/IP monitor in IPP mode
  for the URI; name `"<display_name> (Linux)"`. Removal: `DeletePrinter`.
  Results → `PrinterResult`. Records `guest_printer`.

### 3.18 Bluetooth / evdev gamepads (Design, approval gated)

Planning decision recorded 2026-10-23: the user selected **"Include ViGEmBus
implementation"** for the completion plan. TODO #25 remains pending until its
dependency pins, implementation and acceptance evidence pass. No driver was
installed or guest configuration changed by this planning decision.

Requires the user's approval of a new dependency before any work: ViGEmBus
(BSD-3-Clause) pinned as `submodules/vigembus` for source audit plus the
upstream signed driver installer pinned by SHA-256. If the user declines, TODO
#25 is closed as `Done (declined by user)` at 100% with the decision quoted in
its Notes and a commit entry recording it; no code ships for it and the
documentation lists Bluetooth/evdev gamepads as unsupported. With approval:
host reads evdev gamepads (`/dev/input/event*` with `ABS_X` + `BTN_GAMEPAD`)
while a Waddle window is focused, maps to XInput report fields, and the guest
agent feeds a ViGEm virtual Xbox 360 controller.

### 3.19 Guest display (Design)

- The guest uses the virtual display driver already required by
  `impl/high-perf-av-passthrough/IMPL_DESC.md:743-751`. `DisplayConfig` lists
  host outputs (`wl_output` modes, scale via `wp_fractional_scale_v1`
  preferred scale or `wl_output.scale`). The guest sets one virtual monitor of
  `max(width) × max(height)` physical pixels and DPI `96 × max_scale_120 /
  120` via `ChangeDisplaySettingsExW` and per-monitor DPI
  (`DisplayConfigSetDeviceInfo` with the documented DPI scale offset), and
  replies `DisplayApplied` with the resulting desktop bounds.
- Host surfaces use `wp_viewporter` destination = buffer size / scale and
  `wp_fractional_scale_v1` when available, integer `wl_surface.set_buffer_scale`
  otherwise.

---

## 4. Step-by-Step Execution Sequence

### 4.1 First-time setup (`waddle integration install --device N`)

1. Acquire the manifest lock; recover journals.
2. Ensure daemon and VM (`DaemonMsgStartReq` with `WaitGuest`); wait for the
   existing exec agent (port 5242).
3. Stage the session bundle (agent exe, `av_wgc.dll`, PowerShell setup script)
   into `<state>/interop/bundle_XXXXXX` on the home share (reusing `av_deploy`
   staging), translate its guest path.
4. `waddle exec` (elevated helper via `--apply-policy`) the setup script in the
   guest, which: creates the code-signing certificate (once), signs the agent,
   copies to `C:\Program Files\Waddle\session\<version>\`, writes
   `HKLM\...\Run\WaddleSession`, applies the UAC mode policy, records guest
   manifest entries, and starts the agent via the shell (`explorer.exe
   <path>`) so it receives UIAccess immediately.
5. Write host artifacts: handler entry, MIME defaults (per `exe_handler`),
   autostart entry (per `start_at_login`); record each.
6. Start `waddle-session`; it connects, performs `Hello`, receives the catalog
   snapshot and materializes desktop entries, icons, menu files and desktop
   shortcuts.
7. Commit manifest.

### 4.2 Session start (`waddle-session --device N`)

1. Validate the selected profile, rejecting guest-exec port5002 before VM startup.
   Take `session.lock` (non-blocking; held → exit0, another instance owns it).
2. Recover manifest journals; run user `migrate` if `waddle_version` differs.
3. Connect to Wayland; bind globals (required: `wl_compositor` ≥4,
   `xdg_wm_base` ≥2, `wl_shm`, `wl_seat` ≥5; optional: everything else, each
   gating its capability bit).
4. Create the PipeWire discard sink.
5. Open the local socket `session.sock` for `waddle open`.
6. Ask the daemon `GuestReadyReq` (timeout 120 s), then connect VSOCK 5002;
   handshake; on `VersionMismatch` run the redeploy sequence (4.1 steps 3-4)
   once, then reconnect.
7. Map IVSHMEM (`[av] shm_path`) and validate the layout-v3 (SMW3) header.
8. Enter the event loop (Section 5.1).

### 4.3 Window appears

1. Guest WinEvent `EVENT_OBJECT_SHOW` (or refresh) → classification (Section
   4.4) → admission: allocate generation, reserve 3 slots, start WGC capture.
2. `WindowAnnounce` → host creates `wl_surface`; for toplevel/dialog:
   `xdg_surface` + `xdg_toplevel`, `set_app_id(waddle.<dev>.<app_key>)`,
   `set_title`, `set_parent` for dialogs, `zxdg_toplevel_decoration_v1` with
   `client_side` when the manager exists (lifecycle in section 2.6),
   `set_min_size` from optional `min_size` (`WM_GETMINMAXINFO.ptMinTrackSize`),
   desktop entry ensured (uncatalogued case); for menu/tooltip: positioner +
   `xdg_popup` (menu grabs resolve the optional announce `serial` through the
   press table; absent/stale trigger creates an ungrabbed popup, never a
   fabricated serial).
3. Initial commit without buffer; wait for `configure`; ack; then attach the
   first frame when `FrameReady` arrives.
4. `FrameReady` → host imports slot (shm buffer over the mapped IVSHMEM range,
   or dmabuf per existing code), attaches, damages, commits, sends
   `FrameRelease` when the compositor `wl_buffer.release`s it.

### 4.4 Classification (guest)

For a window `h` visible, enabled-or-not, on the input desktop:

1. Excluded classes: `Shell_TrayWnd`, `Shell_SecondaryTrayWnd`, `Progman`,
   `WorkerW`, `Windows.UI.Core.CoreWindow` owned by `ShellExperienceHost.exe`
   or `StartMenuExperienceHost.exe` or `SearchHost.exe`, `NotifyIconOverflowWindow`,
   `TaskListThumbnailWnd`, `MSCTFIME UI`, `IME`, the agent's own windows except
   the preedit window.
2. `consent.exe` main dialog (`$$$Secure UAP Dummy Window Class For Interim
   Dialog` excluded; the `Credential Dialog Xaml Host` window admitted) →
   `UacConsent` (or hidden in `native` mode).
3. `GA_ROOT(h) != h` → not exported (child window).
4. Class `#32768` or `ComboLBox`, or (`WS_POPUP` and owned and not
   `WS_CAPTION`) and not `WS_EX_NOACTIVATE` → `Menu`.
5. Class `tooltips_class32`, or `WS_EX_NOACTIVATE`, or (`WS_EX_TOOLWINDOW` and
   owned) → `Tooltip`.
6. Owned (`GW_OWNER != NULL`) and not `WS_EX_APPWINDOW` → `Dialog` with
   parent = owner's root.
7. Otherwise → `Toplevel`.
8. Zero-area or fully transparent (`WS_EX_LAYERED` with alpha 0) windows are
   not exported until they become visible with nonzero area.

### 4.5 Move / resize / state (Frozen sequences)

Move:
1. Host `wl_pointer.button(serial S, BTN_LEFT, pressed)` on a toplevel.
2. Host allocates the next monotonic protocol serial T, stores the press-table
   entry `(T,S,seat,window,generation,lease_epoch,Left)`, then sends
   `InputPointerButton{serial=T, point, Left, pressed}`.
3. Guest probes `SendMessageTimeoutW(h, WM_NCHITTEST, 0, MAKELPARAM(screen
   x,y), SMTO_ABORTIFHUNG, 50, &r)`.
4. `r == HTCAPTION` (and the window is not maximized-and-unmovable): the press
   is **not** injected; guest sends `WindowMoveRequest{serial=T}`. If the same
   window received a caption press within `GetDoubleClickTime()` ms and
   within `SM_CXDOUBLECLK/2` px, the guest instead sends
   `WindowStateRequest{state ^ Maximized}` and nothing else.
5. Host: resolve echoed T through the press table. If its left button is still
   held, it is the latest press for that window, and seat/generation/epoch
   match, call `xdg_toplevel_move(seat, S)` with the stored **Wayland** serial;
   otherwise ignore. The compositor sends
   `wl_pointer.leave`/re-enter around the grab; the host suppresses the
   matching release toward the guest (it never saw the press).
6. Resize: `r ∈ {HTLEFT…HTBOTTOMRIGHT}` with `InteropStateResizable` →
   `WindowResizeRequest{serial=T, edge}` → `xdg_toplevel_resize(seat, S, edge)`.
   Mapping: HTTOP=Top, HTBOTTOM=Bottom, HTLEFT=Left, HTRIGHT=Right,
   HTTOPLEFT=TopLeft, HTTOPRIGHT=TopRight, HTBOTTOMLEFT=BottomLeft,
   HTBOTTOMRIGHT=BottomRight.
7. Any other `r` (including HTMINBUTTON/HTMAXBUTTON/HTCLOSE/HTSYSMENU/
   HTCLIENT, timeout, or error) → inject the press normally.
8. Right-click on caption is always injected (system menu becomes a `Menu`
   popup).

Resize from compositor:
1. `xdg_toplevel.configure(w, h, states)` with nonzero w,h → after ack, host
   sends `WindowConfigure{state, rect(0,0,w×scale,h×scale)}`.
2. Guest: if `Maximized` newly set → `ShowWindow(SW_MAXIMIZE)` then
   `SetWindowPos(h, NULL, work.x, work.y, w', h', SWP_NOZORDER|SWP_NOACTIVATE)`
   so the Windows maximized frame matches the compositor's maximized size; if
   cleared → `ShowWindow(SW_RESTORE)`; `Fullscreen` → existing fullscreen
   code; otherwise `SetWindowPos(... SWP_NOMOVE ...)` with the DWM border
   adjustment already in `av_windows.c:516-527`.
3. Guest frames at the new size flow; the host attaches the new buffer and
   `set_window_geometry(0,0,buffer/scale)`.

States from guest:
- `WS_MAXIMIZE` set/cleared (observed through `EVENT_OBJECT_LOCATIONCHANGE` +
  style read) → host `set_maximized` / `unset_maximized`.
- `IsIconic` became true → host `set_minimized`; the toplevel stays mapped
  (xdg-shell has no minimized state). The next `configure` containing
  `activated` while the guest window is iconic → guest `ShowWindow(SW_RESTORE)`
  then `SetForegroundWindow`.
- Fullscreen as today (`InteropStateFullscreen`).
- `WindowStateRequest` from caption double click: host applies
  `set_maximized`/`unset_maximized`; the guest acts only on the resulting
  `WindowConfigure`.

Close:
- Host `xdg_toplevel.close` → `WindowClose` → guest `PostMessageW(h, WM_CLOSE)`.
  Guest-initiated close (Alt+F4, caption X, app exit) → `EVENT_OBJECT_HIDE`/
  `DESTROY` → `WindowRetire` → host destroys the surface.

### 4.6 Guest window placement

On admission and on every `EVENT_OBJECT_LOCATIONCHANGE`, if any part of the
window's extended frame bounds lies outside the guest virtual desktop work area,
the guest moves it (`SetWindowPos ... SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE`)
to be fully inside, preferring the top-left corner. Windows larger than the
work area are positioned at the work area origin (they are clamped to the
desktop by Windows when maximized).

### 4.7 Teardown

Close new user-work admission and release held input on shutdown. Clipboard
rendering and transfer/DnD cancellation remain bounded independently of mapping
ownership. An orderly mapping stop must keep the control and Wayland dispatch
loops alive while the guest stops/joins all frame and audio writers, resolves
provisional slot claims and flushes final FrameReady publications. The host
processes that entire FIFO prefix, completes pending imports, waits for actual
release of every attached compositor buffer, sends exact FrameRelease records,
stops/joins audio and metadata/heartbeat readers and irrevocably ends host map
access. The guest processes all exact releases and verifies zero outstanding
pins before retiring its old runtime. A shared Free state alone is insufficient.

Goodbye, a successful socket write, EOF, timeout, process restart, local proxy
or surface destruction and the legacy two-second Wayland drain are never these
proofs. Current protocolv2 has no accepted epoch-wide orderly-stop receipt;
until the versioned amendment and native owners are implemented, an uncertain
stop retains/quarantines the mapping, pins and callback ownership and prohibits
in-place metadata initialization. Do not return to Listening with a reusable
BAR merely because the transport closed. A new random epoch cannot authorize
reuse. Crash recovery requires a retained owner with valid evidence or a trusted
reset that supplies a distinct backing object after old producers stopped;
old compositor imports must continue to refer to the old object. Such recovery
is unimplemented and earns no acceptance credit.

Applications keep running when a session stops. Guest logoff may unhook window
and input callbacks only after their owners stop/join safely; WM_RENDERALLFORMATS
and synthetic-pointer teardown do not release mapping pins. Session lock and
backing-owner lifetime must outlive any retained uncertain resource. The protocolv3
research documents are unaccepted proposals, not permission to skip these facts.

---

## 5. Concurrency, Threading & Synchronization

### 5.1 Host `waddle-session`

- One main thread runs a `poll()` loop over: Wayland display fd, VSOCK fd,
  `session.sock` listener and clients (≤ 16), PipeWire loop fd
  (`pw_loop_get_fd`, iterated with `pw_loop_iterate(…, 0)`), D-Bus fd (native
  UAC mode only), a `timerfd` (repeat, liveness, debounce timers), and clipboard
  pipe fds. All Wayland, protocol, desktop-entry and manifest work happens on
  this thread; there are no locks.
- PipeWire realtime `process` callbacks run on the PipeWire data thread and
  touch only their ring (SPSC consumer side) and stream buffers, as today.
- IVSHMEM: the host reads slot descriptors with `memory_order_acquire` on
  `state` and writes `Consuming`/`Free` with release, identical to v1.

### 5.2 Guest `waddle-guest-session.exe`

- **Main thread** (STA, message loop): WinEvent hooks (out-of-context, so
  callbacks arrive on this thread), agent windows (clipboard listener, preedit,
  DnD catcher), classification, input injection, protocol send/receive (socket
  in non-blocking mode registered with `WSAEventSelect`; loop uses
  `MsgWaitForMultipleObjectsEx`).
- **Capture threads**: one per exported window inside `av_wgc.dll` (existing
  model), writing slots (single producer per slot).
- **Audio threads**: one per open stream (`audio_worker` model), SPSC producer.
- **Catalog thread** (STA): enumeration, icon extraction, change
  notifications; communicates with the main thread through a bounded
  (256-entry) single-producer/single-consumer queue plus an auto-reset event.
- **DnD thread** (STA): runs `DoDragDrop`; communicates via a bounded queue.
- **Printer / policy** helper processes are separate elevated processes, never
  threads.
- Protocol frames are only written by the main thread; other threads enqueue
  requests to it.

### 5.3 Ordering guarantees

- Frames on the control connection are processed in order.
- `FrameReady` for generation G is never sent before `WindowAnnounce` for G.
- `WindowRetire` is sent only after every slot of the window has been
  exactly released by the host. Connection loss quarantines outstanding slots;
  it never retires their pins. The host rejects late FrameReady for a retired
  generation while retaining any earlier imports until their own completion.
- Input serials are strictly increasing; the guest rejects a non-increasing
  serial as `ProtocolViolation`.

---

## 6. Error Handling & Failure Modes

### 6.1 Connection

- GuestReady probes the legacy guest-exec service, not interop5002. An aborted
  pre-Hello connection grants no mapping ownership or reinitialization right.
- A fatal codec error sends Error when possible and closes the connection.
  Existing epoch owners retain uncertain resources. Backoff and snapshot rebuild
  can begin only after the native owner authorizes a safe replacement epoch.
- VM stop/restart removes Running status and stops new work. Surface destruction
  is a retirement request, not actual buffer-release evidence. Retain old
  references until their exact completion; do not reuse the old backing object
  from daemon Running state alone (Section4.7).

### 6.2 Resource exhaustion

- More than 64 windows: windows beyond the limit are not exported; the agent
  logs once per minute and admits them when slots free.
- Slot heap exhausted: Section 3.3.
- Send queue full (512 frames): input and control frames are never dropped;
  a full queue is terminal for the connection (as in AV).
  Exception: `InputPointerMotion` and `InputTablet` without state change are
  coalesced (latest replaces queued unsent motion for the same window).
- Transfers over cap or count: Section 3.2.6.

### 6.3 Input safety

- On focus loss, lease revocation, window retirement, disconnect and agent
  exit, every held key/button/touch contact/pen tip is released (existing
  `av_input.c` cleanup, extended to XBUTTONs, touch contacts and pen).
- `KEY_POWER` and `KEY_SLEEP` are never forwarded (they would act on the VM).
- `Ctrl+Alt+Delete` is not injectable by `SendInput`; the host never attempts
  it. (Recorded for clarity.)

### 6.4 Desktop artifacts

- Every artifact write is atomic (write temp file in the same directory,
  fsync, rename).
- A desktop entry whose sha256 differs from its record is treated as user
  modified: never overwritten by sync; `repair` overwrites only with
  `--force`.
- Disk full: the transaction rolls back via the journal; the catalog sync is
  retried on the next delta.

### 6.5 Launch failures

- Outside share → exit 126 + notification (Section 3.9.1).
- Guest not reachable within 120 s → exit 125 + notification "Windows
  (<device>) did not start".
- `LaunchResult` with error → exit 1 (`--wait`) + notification with
  `message`.

### 6.6 UAC

Section 3.11, including the secure-desktop safety net and the `auto` mode
admin requirement.

### 6.7 Clipboard and DnD

- Request timeout (5 s guest render, 10 s host send) → the requester gets no
  data; the other side cancels the transfer.
- Disconnect during a DnD loop: guest injects `Esc` + synthetic button up and
  ends the loop; host `cancelled`.

### 6.8 USB

- Device busy (claimed by a host driver): QMP error is returned verbatim; CLI
  exit 1; ACL revoked.
- VM stops with attachments: daemon forgets attachments; ACLs are revoked by
  the CLI on next `usb list` (records in `<state>/usb_acl.json`).

### 6.9 Printing

- CUPS socket missing at VM start: no guestfwd; `PrinterSync` not sent; status
  reports `cups: unavailable`.
- Guest printer creation failure: `PrinterResult` with message; retried at the
  next 60 s sync, at most 3 times per printer per session.

### 6.10 Mode exclusion

`waddle av run` and `waddle-session` both need the IVSHMEM mapping. The AV
command lock (`<state>/av/command.lock`) is taken by `waddle-session` as well;
`waddle av run` fails with "seamless session active; stop it with waddle
session stop" while it is held, and vice versa.

---

## 7. Verification & Testing Criteria

### 7.1 Unit tests (Linux CI)

| Area | Tests |
|---|---|
| `interop_codec.zig` | Every message type round-trip; every required field missing; duplicate/out-of-order tags; critical unknown tag; non-zero padding; oversize payload, string, field count; invalid UTF-8 and NUL; state mask bits; `DisplayConfig` repeat exception; fuzz corpus (≥ 10 000 mutated frames, no crash, output unchanged on error) |
| Transfers | Contiguous offsets, gaps, overlaps, over cap, 17th transfer, hash mismatch, cancel at every stage |
| Layout v3 | Header validation, slot size math, first-fit allocation, growth with deferred free, exhaustion, 7680×4320 clamp |
| Keyboard | `scancodes.csv` golden table both directions; repeat timing (fake clock); lock reconciliation; layout table lookups |
| `pe_header.zig` | GUI, CUI, PE32/PE32+, truncated, bad `e_lfanew`, non-PE, 4096-byte bound |
| `desktop_entry.zig` | Escaping, Exec quoting, rejection of control characters, ID sanitization |
| `mimeapps.zig` | Preserve unrelated groups/comments, add/replace/restore defaults |
| `manifest.zig` | Journal replay/rollback after crash at every step, schema refuse-newer, modified-artifact protection |
| `png_writer.c` / `png_reader.zig` | Encoder output decodes with the reader and with a reference decoder in tests (Python `zlib` + manual PNG chunk parse in `tests/interop/png_check.py`); reader rejects malformed PNGs |
| Clipboard conversions | CF_HTML header build/strip, CRLF, UTF-16 surrogates, uri-list ↔ HDROP with translation, gnome-copied-files cut/copy |
| Host Wayland logic | `--wrap` fakes: toplevel/popup creation, app_id/title updates, move/resize requests with serial validity, state mapping, cursor set on enter, relative lock lifecycle, touch frames, tablet frames, pinch steps, text-input commit/preedit, data-device selection/offers, activation token use |

### 7.2 Guest tests (native Windows CI, `guest_windows.yml`)

- Classification against fixture windows created by the test (toplevel,
  owned dialog, `#32768` menu via `TrackPopupMenu`, tooltip, tool window).
- `WM_NCHITTEST` routing with a test window returning each HT code.
- Catalog enumeration returns File Explorer's AUMID; icon PNG extraction for
  `explorer.exe` at all sizes decodes.
- Clipboard: delayed rendering round trip within one process pair.
- Synthetic pointer: `CreateSyntheticPointerDevice` for touch and pen succeeds
  and an injected contact reaches a test window as `WM_POINTERDOWN`.
- Printer creation in dry-run mode (validates arguments, does not add).
- Heap audit (`tests/windows/windows_heap.ps1` pattern) with zero leaks.

### 7.3 Integration tests (Linux CI)

- `waddle-mock-guest` extended with a mock session agent speaking interop v2:
  `waddle open --app`, `waddle open file.exe` (PE fixtures), outside-share
  refusal, catalog snapshot → desktop entries and icons in temporary `XDG_*`
  roots, catalog removal → files removed, manifest `install/remove/repair/status`
  round trip including restoring a prior MIME default.

### 7.4 Gates

- `make interop-test`, `make interop-sanitizers` (ASan/LSan/UBSan, zero
  leaks), `make interop-coverage` (≥ 90 % line and branch on
  `interop_codec.zig`, layout v3, `manifest.zig`, `desktop_entry.zig`,
  `mimeapps.zig`, `pe_header.zig`, clipboard conversions).
- Existing `make test test-sanitizers coverage av-test av-sanitizers
  av-coverage` stay green.
- `.github/workflows/interop.yml` runs the Linux gates; `guest_windows.yml`
  gains the guest tests.

### 7.5 Manual hardware acceptance (recorded in `VERIFICATION_M0.md` and the acceptance matrix)

M0 (existing pipeline, TODO #2-#3): audio audible, stream visible in
`pw-dump`, ring counters without sustained drops over 5 minutes, latency
sample recorded; Notepad window: taskbar entry, Alt+Tab, focus, typing,
resize, fullscreen, close, on GNOME and KDE.

Final (TODO #34), on GNOME, KDE Plasma and Sway:

1. Explorer from the app menu and from a Linux desktop shortcut; correct icon in
   taskbar/dock/Alt+Tab.
2. Move by caption drag, resize from all 8 edges, caption double-click
   maximize, min/max/close buttons, fullscreen (F11 in Edge), restore from
   taskbar.
3. Menus, context menus, tooltips, combo boxes, owned dialogs (Notepad Save As)
   appear attached to their parents.
4. Typing in US, DE, FR layouts; repeat; Caps/Num lock; media keys; Japanese
   IME in both `ime` modes.
5. Mouse back/forward in Explorer; high-res wheel; horizontal scroll; cursor
   shapes (text, resize, hand, custom app cursor); pointer lock in a game.
6. Touchscreen tap/scroll; pen pressure in Photoshop.
7. Clipboard text/HTML/image/files both ways, cut-paste of files.
8. Drag a file from Files into Explorer and from Explorer into Files.
9. Microphone in Voice Recorder; per-app audio entries with icons in the Linux
   mixer.
10. USB webcam in the Camera app; wired gamepad in a game.
11. Print a test page to a CUPS printer.
12. Adobe Creative Cloud: download, install with the UAC dialog as a Linux
    window, `.desktop` entry auto-created, launch, uninstall removes the entry;
    repeat elevation in `uac=auto` (notification) and `uac=native` (Allow and
    Deny); force the secure desktop on and observe the safety-net notification.
13. Double-click a GUI `.exe` and a console `.exe` in Files; `./tool.exe` in a
    terminal with system binfmt installed.
14. `waddle integration remove --all --guest` restores the prior MIME default,
    guest UAC policies and leaves no Waddle files.

## 8. Foundation implementation boundaries (2026-10-23)

This section refines mechanisms without changing protocol v2, layout v3 or
manifest schema 1. Each library below is a partial deliverable until connected
to the host/guest runtime. Native/hardware evidence is not inferred from tests.

### 8.1 Mapping authority and bounded layout primitives (TODO #5)

The host takes the per-device AV command lock before establishing the session
connection and holds it through draining. The guest session agent at logon
only listens: it never initializes shared memory in the absence of that
connection. Before sending Hello on the accepted session connection, the guest
joins old capture/audio writers, verifies no previous host-owned buffers remain,
initializes a fresh nonzero epoch and publishes the header. A busy second
connection cannot initialize or alter it. On disconnect the guest stops and
joins producers before reclaiming their runs; the host destroys compositor
buffers before unmapping. Reconnect cannot reinterpret a previous epoch.

The initial Zig module is allocation-free and owns no mapping or OS handle.
It accepts bounded immutable byte snapshots plus the separately supplied real
mapping length and expected epoch. Header parsing requires exactly 4096
readable bytes, mapping length 0x80000000, SMW3/version 3, nonzero matching
epoch, 192 slots, eight audio rings, heap offset 0x200000, heap length
0x7FC00000, page size 0x200000 and zero reserved bytes/word. Heartbeat values
are unconstrained. Descriptor validation accepts one 64-byte snapshot after
the native adapter acquires Ready; it validates state, format, identity,
sequence, checked geometry/stride/capacity and heap bounds before exposing
any pixel range. Pure parsers never perform atomic accesses through byte slices.
Native adapters later copy the owned immutable metadata after an acquire state
transition and prove aligned lock-free u32/u64 atomics for x86-64 Linux/Windows.

Geometry is clamped independently to 7680 by 4320, stride rounded to 64 bytes,
capacity rounded to 2 MiB and every addition/multiplication checked before
conversion. Zero dimensions are rejected. Audio ring offsets are computed
only for indices 0..7 and validate the complete header+padding+PCM region.
The allocator owns only a caller-held 1022-page bitmap and fixed reservation
metadata, uses first fit, and returns exact run identity; allocation failure
leaves it unchanged. Free validates the complete owned run and rejects stale,
overlapping or duplicate release without partial mutation. Runtime slot pins
and deferred growth releases are a subsequent subtask, not claimed by a plain
bitmap allocator. Public APIs retain no input/output pointers and document
nullability, output-on-error and single-owner-thread rules.

### 8.2 Configuration and export boundaries (TODO #8/#9)

The first #8 change adds all §3.5 keys to daemon_config_t, defaults,
transactional validation/INI parsing/edit-reset and focused tests. Enums use
documented PascalCase constants and fixed-width C fields. Boolean values
accept the parser's existing 0/1 convention plus true/false for the new keys.
USB auto-attach contains at most 16 hexadecimal VID:PID pairs; reject malformed
or duplicate pairs and retain prior config on failure. Qualified section.key
editor names disambiguate repeated names such as backend/enabled; legacy
unqualified editor keys retain their old meaning. Config changes alone do not
change VM devices or claim native support. QEMU args/build features and config
CLI serialization/get are separately integrated and tested before #8 closes.

The previously unspecified `audio.backend=none` selects the silent QEMU
`none,id=waddle_audio` backend with one HDA controller/duplex endpoint. This
preserves a guest render endpoint for legacy AV process loopback without host
playback/capture; audio.microphone has no effect on that silent backend. The
PipeWire backend uses one HDA controller/endpoint and disables capture voices
with in.voices=0 when microphone=false. No duplicate legacy silent HDA is added.
PipeWire routing/discard-sink and per-PID duplication prevention remain #23
integration requirements; argv tests alone cannot qualify audible behavior.
The named QEMU builder/spawn APIs require the registry's 1..63-byte ASCII
letter/digit/hyphen/underscore identifier, and derive only the fixed sink name
waddle-discard.<device> from it. The daemon passes the canonical registered
runtime/socket identity; legacy/unmanaged callers use the compatibility identity
default. Microphone input omits in.name so PipeWire selects its default source;
an empty string is not used because QEMU forwards it as target.object. Tests
inject absent, regular, symlink and socket lstat results for the fixed CUPS path;
only an actual socket qualifies for forwarding. Runtime CUPS disappearance can
still fail QEMU startup and must follow normal start rollback.

For #9, translation must reflect actual configured exports: canonical longest
component-prefix match, no fallback root export when none is configured, and
zero configured mounts means no host path is shared. Existing explicit root
exports remain valid. Every mount gets one owned virtiofs process/socket/log,
bounded by WaddleMaxMounts, with deterministic QEMU IDs. Partial start unwinds
only newly acquired resources in reverse order. Restart and readiness retries
reuse still-owned resources; teardown stops/reaps every process before clearing
its state. Guest drive/tag association must be implemented and proven, not
inferred from a host argv fixture.

### 8.3 Manifest codec and lock/transaction boundaries (TODO #33)

The first manifest module parses schema-1 JSON only; it performs no filesystem,
policy or guest writes. Input is at most 4 MiB, UTF-8 without embedded NUL,
with at most 4096 records and bounded 4096-byte strings. Reject duplicate JSON
keys/record IDs, invalid types, unknown schema/kinds, malformed SHA-256,
unbounded nesting and unsupported fields. Preserve schema-1 fields in §3.12,
including mime for MIME records and prior string/integer/null values as
appropriate to kind. The parser uses an explicit caller-supplied allocator;
success returns one owned parsed document with a symmetric deinit; every error
frees all partial allocations. Returned slices live until deinit, never longer.
Deinit invalidates owned state. Validation is pure and reentrant for independent
documents. Path/scope authorization and durable journal actions are subsequent
milestones; successful JSON parsing alone does not authorize an artifact path.

Lock ordering for the later transaction engine is session.lock, then the
per-device AV command lock, then manifest.lock; never acquire an earlier lock
while holding a later one. A CLI installer uses manifest.lock only and releases
it before spawning/waiting for a session or issuing LocalStop. Session stop
drains local resources without waiting for an installer holding manifest.lock.
Each bounded artifact transaction journals intended manifest plus recoverable
actions and prior-state backups, fsyncs before mutation, publishes artifacts,
atomically replaces/fsyncs manifest, then removes/fsyncs journal. Startup
recovers under manifest.lock before accepting new mutations. Installation
commits deployment and host base artifacts, releases the lock, starts the
session, then catalogs through separate journaled transactions. Failure to
start the session leaves recorded repairable installation, never unrecorded
guest policy/file changes. Tests must inject crashes at every durable boundary
and run concurrent install/start/stop to prove the lock graph.

Incoming/outgoing transfer managers later enforce both the 16-transfer count
and a per-direction reservation budget equal to configured clipboard.max_mb
(including icons/cursors); over-budget admission returns Busy, individual
over-limit payloads return TooLarge. Cancellation/EOF returns each reservation
once. This is a local resource bound, not a wire/schema change.

### 8.4 Pure layout C boundary and persisted settings refinement

The allocation-free C boundary is src/interop/interop_layout.h with four
interop_layout_* functions: private header snapshot validation, private Ready
slot snapshot validation, checked capture geometry and fixed audio bounds.
Its fixed-width output structs and every offset/size are asserted for the
supported x86_64 Linux/Windows ABIs. Return values are Ok=0,
InvalidArgument=1 (null/wrong-sized/unaligned/overflowing/aliased pointer ranges)
and InvalidLayout=2 (invalid mapping facts/contents, geometry or ring). The
wrappers reuse the pure Zig validators, stage results locally and publish output
only on success. They check pointer alignment/range arithmetic before creating
slices or loading typed inputs; callers still supply accessible memory of the
stated size. They own no memory and do not retain pointers. Private snapshot
bytes must never alias live atomic fields; these APIs do not claim native
acquire/release or mapping initialization. Header input is a previously
validated immutable value, and outputs are disjoint from both input ranges.
The header validator also checks the actual mapping range without dereferencing
it: address+length overflow or either private input/output overlapping the
mapping returns InvalidArgument. Descriptor calls have no mapping pointer;
callers guarantee private copied storage and a validated immutable header.

The #5 allocation is refined before implementation: pure C ABI wrappers/tests
5% and native atomic adapters/proof 5% replace the combined 10% ABI/atomic
subtask; totals and accepted progress are unchanged. Live capture/compositor
pins, growth/reconnect and qualification remain separate.

Device backup schema version 1 accepts the new seamless settings as optional
additive keys with exactly the frozen §3.5 defaults when absent. Export writes
all of them; import validates enum strings, booleans, limits and the at-most-16
unique USB selectors before any published device mutation. This preserves old
schema-1 backups while round-tripping new settings. Clone retains host mounts;
import continues to suppress host mounts and unrelated host-local paths. The
backup manifest is distinct from the integration ownership manifest in §3.12.

### 8.5 Resumed source-build and many-mount boundaries

The explicit qemu-source build target uses the pinned submodule commit as an
expected identity and a caller-selected out-of-tree build directory. The build
helper requires libpipewire-0.3>=0.3.60, slirp and libusb-1.0>=1.0.13 from base
system development packages. It configures x86_64-softmmu/KVM/headless plus
--enable-pipewire --enable-slirp --enable-libusb --disable-download. Missing
features fail rather than silently changing the default audio/network/devices.
No configure fallback downloads are allowed. The existing system-QEMU packaging
path remains distinct; system smoke cannot qualify the pinned source build.

Before reuse, a signature includes source commit/clean tracked state, exact
configure options, helper revision, compiler/build-tool identity and selected
dependency versions/compile/link inputs. Invalid/missing config outputs or
required macros force reconfiguration. Invalidate prior success before an
attempt; write an atomic cache/provenance stamp only after required macros,
Ninja, output binary hash and a post-build source-identity check succeed.
A stamp is cache metadata, not proof of Windows/audio/peripheral acceptance.
Build directories cannot overlap source and are never recursively cleaned.

The additive QEMU many-mount APIs borrow an ordered bounded array of fixed
socket-path/drive values. Count zero requires NULL, count1..WaddleMaxMounts
requires non-NULL, and larger counts fail. Validate all inputs before retaining
argv allocations: NUL-terminated absolute paths, exact letter:slash/backslash
root drives, no comma/control bytes in keyval paths, duplicate socket paths or
case-insensitive duplicate drive letters. Failure returns EINVAL/E2BIG/ENOMEM,
frees every partial argv string, and leaves argv[0] NULL. No input is retained.

For drive D, lowercase d gives chardev waddle_fs_char_d, device waddle_fs_dev_d,
and tag waddle_fs_d. Emit exactly one four-argument chardev/device group per
mount in input order. Guest binding of that tag to D:\ is a separate required
implementation/native gate. Existing single-socket APIs retain char0 and
waddle_fs for valid inputs. New many-mount process records leave the legacy
single-socket field empty; the supervisor owns all per-mount lifecycle objects.
No QEMU builder owns, stops or unlinks a mount listener.

Refine #9's40% all-mount allocation into QEMU many-mount API10% and owned
supervisor lifecycle30%; the sum and current accepted10% root-fix credit do not
change. Native drive binding and failure/restart qualification stay separate.

### 8.6 Native atomic adapter boundary

The C11 adapter in src/interop/interop_native_atomic.h is x86_64-only and
borrows an accessible, initialized, 64-byte-aligned 2 GiB mapping. Compile-time
width/lock-free assertions preserve layout v3; runtime checks require the exact
header base and descriptor at 0x1000 + index*64, index0..191. Address arithmetic
must not wrap. The caller validates immutable layout facts first; pointer checks
cannot establish mapping accessibility, initialization or ownership.

Only Free->Writing, Writing->Ready, Ready->Consuming, Consuming->Free and
Writing->Free cancellation are legal. One strong compare-exchange uses acquire-
release on success and relaxed on failure; mismatch changes no bytes. Ordinary
metadata is accessible only to the current Writing or Consuming owner. Ready
publishes prior writes; acquiring Consuming observes them; releasing Free and
reacquiring Writing orders reuse. These helpers do not pin heap allocations,
initialize atomics, manage reconnect, or authorize a forged ownership claim.

Each heartbeat field has exactly one writer and any number of readers. Relaxed
loads do not synchronize metadata. The writer supplies a nonzero strictly
increasing value; UINT64_MAX requires reconnect rather than wrap. A load output
must be aligned, accessible caller-owned storage disjoint from the entire
mapping and remains unchanged on failure. No API allocates or retains storage.
The caller owns mapping teardown after all participants have stopped.

The CPU fixture reserves 2 GiB of virtual address space, commits only the fixed
header/table pages, and releases the reservation on completion. It checks every
state pair, malformed pointers, cancellation handoff and 100,000 concurrent
producer/consumer publications with exact sequence and payload checks. Linux
ASan/LSan/UBSan and separate production-only gcov line/branch gates are required;
concurrent gcov counters use atomic profile updates. Native Windows execution
is also required before this adapter receives tracker credit. Neither CPU proof
nor a cross-linked executable establishes integrated IVSHMEM/guest acceptance.

QEMU process teardown preserves the PID, live-state flag, QMP connection and log
ownership when kill or waitpid fails unexpectedly. EINTR retries; ESRCH permits
reaping, and ECHILD confirms no owned child remains. Only confirmed termination
allows clearing the PID and closing QMP. Generic cleanup first polls and refuses
to clear a running or unconfirmed process; callers request kill explicitly.
Mount-array callers retain all backend ownership until QEMU is confirmed dead.

### 8.7 Owned filesystem backend lifecycle

Each mount backend is an independent virtiofs_process_t initialized with zero
fields and log_fd=-1. A record retaining any PID, descriptor or path cannot be
reused. Spawn validates absolute bounded paths and the exact boolean read_only
before filesystem/process side effects; read_only=1 emits --readonly and has no
read-write fallback. A missing directory returns its stat error; a non-directory
returns ENOTDIR. Logs use O_NOFOLLOW. A fresh listener path must not exist:
pre-existing sockets return EADDRINUSE and other objects EEXIST without deletion.
Orphan recovery needs separate ownership evidence and is not inferred here.

Readiness is bounded and requires a socket inode without following symlinks.
On startup failure, stop/reap runs before cleanup. SIGTERM with a bounded wait
falls back to SIGKILL; EINTR retries, ESRCH permits wait, and ECHILD confirms the
owned child is gone. Unexpected kill/wait errors retain every ownership field,
log FD and listener path for retry. Socket-removal errors likewise retain the
record after reaping. Cleanup closes/clears only after successful stop and socket
removal; it is idempotent on a fully released initialized record. Callers serialize
one record/path and never discard a failed teardown record. These APIs do not
prove guest mount binding or guest read-only write denial.

Coverage recorder metadata schema2 keeps one32-bit atomic edge mask and one
byte arity per site, plus sorted (site_index, extra_word_offset) pairs and packed
extra32-bit atomic masks only for arity>32. Supported arities are1..255;
unsupported shapes fail before execution. Total statically allocated storage is
align4(5*site_count)+4*extra_words+8*wide_sites and must remain <=512KiB. Generated
metadata binds schema, exact IR and exact ordered records by SHA256. Startup
checks complete sparse order/offset/count correspondence before instrumentation
runs. Invalid hits abort; edge recording is relaxed atomic OR, and dumping after
worker joins preserves all recorded edges. No compiler sites are omitted or
regrouped to meet capacity or coverage. Synthetic255-edge and76,512-site stress
fixtures supplement each module's actual native compiler output.

Vendor packaging uses scripts/vendor_copy.py to record source kind/path/version,
source/package SHA256 and, for pinned QEMU, checked source commit/build signature
and configuration digests. System identities cannot carry pinned-only claims.
virtiofsd packaging requires bounded --help advertisement of --readonly and
records the help digest; this is capability evidence, not guest write-denial.

Publisher and verifier share a nonblocking regular no-follow identity lock.
Preflight errors preserve the previous package. Executable and identity are
fsynced owned sibling temporaries, replaced sequentially, then hash-verified;
the pair is not an atomic transaction. A crash between replacements is detected
by verification. Readers must verify before relying on provenance. Make verifies
existing packaged binaries, and explicit qemu-vendor-from-source replaces the
system package only with a validated pinned build. The obsolete source sidecar
is removed only after successful packaging. No directory tree is cleaned.

### 8.8 All-mount supervisor ownership

The event-loop-owned daemon_state_t retains the legacy first virtiofs process
for wire status and seven additional records, one contiguous owned prefix and
an immutable mount-identity snapshot for the active VM. Mount0 keeps the legacy
socket/log names; indices1..7 use virtiofsd-<index>.sock/.log. Named-device lock
handoff regenerates every log path after resolving the persistent state root.

Validate the whole mount set and unique drive letters, locate required binaries,
then start all backends in config order with exact read-only flags before AV
preparation and QEMU. Pass the ordered backend/drive array to the QEMU API. A
partial startup unwinds in reverse order; teardown errors override the initial
error and retain the failed record plus every unvisited lower index for retry.
Live QEMU always stops/reaps before any backend or AV mapping is released.

A readiness timeout retains the VM and all backends. A retry polls QEMU first;
a live VM requires identical mount identities and no replacement children. A
dead VM permits cleanup and changed config. Stop, kill, natural reap and restart
close QEMU log/QMP descriptors only after confirmed child death; zero-mount,
AV-disabled restarts follow the same rule. Unexpected kill/wait failures retain
resources. Idle supervisor shutdown refuses any remaining mount owner, including
additional records invisible to the legacy first-PID status field. Kill responses
report actual errors/state rather than unconditional success. All mutations are
serialized by the daemon event loop; there are no independent mount threads.
This milestone does not install guest drive bindings or prove canonical-path,
restart-stress, compositor or native guest filesystem acceptance.

### 8.9 Continuation contracts: slot lifetime, filesystem paths and durable store

The following decisions resolve the saved proposals before implementation.
They do not change protocolv2, layoutv3 or manifest-schema1. Pure bookkeeping,
filesystem authorization and actual host/guest integration receive distinct
acceptance; no isolated module implies integrated functionality.

**Slot runtime.** New interop_slot_runtime.zig owns fixed heap/window/slot
bookkeeping, no OS/mapping pointer or allocation. One guest event-loop thread
serializes calls. Geometry uses compute_frame_layout's existing clamp. Same-
capacity/shrink changes desired geometry for future publications only; existing
pins retain their exact prior run and descriptor snapshot. Growth reserves a
complete replacement triple transactionally. Only an unclaimed, unpinned slot
may switch; a slot still too small for desired geometry cannot publish. Keep
all old runs until the complete prior triple is unpinned and switched. A second
pending growth returns Busy rather than discarding either triple.

Generations are nonzero strictly increasing per agent session, as already
specified by InteropTagGeneration; admission rejects reused or older generations.
Frame sequence is nonzero strictly increasing per window incarnation, consumed
on a successful provisional claim even if later cancelled; never wrap or reuse.
At most one provisional writer per window is admitted at a time. The local
runtime claim precedes Free->Writing CAS; failed CAS cancels only that unentered
claim. After successful CAS, cancellation requires confirmed Writing->Free;
publication pin commit requires successful Writing->Ready. Writer prepare,
entry, publication and cancellation also receive the captured session epoch;
mismatches reject before mutation and cannot fail-close a different session.
A post-publication
local inconsistency fails the session closed and retains reservations.

The host performs Consuming->Free only after buffer use has ended, then sends
an exact FrameRelease. The guest clears its runtime pin only on the matching
session/window/generation/slot/sequence release. Observing shared Free alone
never authorizes reuse. Window retirement blocks new claims and quarantines
its descriptors until every writer/pin is gone. Session drain blocks admission
and new claims. Producer shutdown plus explicit caller proof of host buffer
teardown precedes final drain; no new wire acknowledgement is invented here.
Transport loss/heartbeat timeout alone is not proof of host drain. Reconnect
uses a fresh epoch/runtime only after the prior session has safely ended.

**Canonical filesystem boundary.** Preserve the existing pure lexical mapper
and its compatibility API. Add a bounded filesystem-aware C/POSIX boundary for
configured exports and CLI explicit path rules. Export roots must exist as
directories and contain no symlink component; traversal opens components with
O_DIRECTORY|O_NOFOLLOW. Normalize separators and dot components, reject dotdot,
and reject duplicate canonical roots. Return ENOENT/ENOTDIR/ELOOP/EACCES from
root validation, EINVAL for malformed/duplicate rules, EXDEV for an absolute
host path outside all exports, and ENAMETOOLONG for bounded capacity failures.
Zero exports share no absolute host path. Relative and Windows-native paths
pass through unchanged; no filesystem lookup is applied to them.

Existing absolute host inputs use realpath. Missing output paths resolve their
deepest existing ancestor, then append only normalized genuinely absent suffix
components. Broken symlinks and loops remain errors, never absent suffixes.
Longest canonical component-prefix wins, allowing nested exports. A file symlink
inside a root may resolve to another configured root; a target outside all
roots is rejected. Validate every configured root before starting any backend
and pass canonical spellings to virtiofsd. Mount roots must remain stable during
a VM lifetime; these path conversions do not pin host pathnames against external
rename/replacement and must not be described as an OS authorization primitive.
Future live session resolution must bind to the running export identity.
Exec keeps local-error exit125; the future open command maps EXDEV to126 as
already frozen. No guest command is launched after translation failure.

**Manifest store.** New manifest_store.zig keeps the pure manifest codec API
unchanged. The store owns allocator-backed paths and dirfd/lock handles with
idempotent close; requests borrow candidate bytes and bounded artifact actions
through commit only. User state resolves XDG_STATE_HOME then HOME fallback;
system state is exactly /var/lib/waddle/integration and requires euid0. No
user/system fallback. Private state/lock/journal directories and regular files
use no-follow traversal and owner/mode checks. Nonblocking exclusive manifest.lock
spans recover/commit/close; existing session->AV->manifest lock order remains.

Journal schema1 is private, strict JSON with at most32 journals,4096 actions,
4MiB encoded bytes, codec string limits, unique IDs/targets and unknown-field/
key rejection. Only allowlisted kind/root-relative targets are actionable.
Guest kinds and unimplemented host policy kinds fail before any mutation.
DesktopEntry/HandlerEntry target applications, Icon targets hicolor size/apps,
Menu targets config/menus/applications-merged or data/desktop-directories,
Autostart targets config/autostart, and
DesktopShortcut requires an explicit caller-supplied validated desktop root.
MIME state/system binfmt/polkit producers remain disabled until their dedicated
policy boundary is implemented. No arbitrary journal absolute path is trusted.

Commit validates old/candidate manifests, expected hashes, record/action identity,
root/parent paths and current targets. Stage exact new bytes and prior bytes/modes
in owned files and fsync files/directories before publishing a prepared journal.
Artifacts use same-directory temporary files and atomic rename/unlink; recheck
hash/identity before applying each action. Publish/fsync manifest last, then
publish/fsync committed journal phase before cleanup. Prepared recovery restores
old artifacts and manifest in reverse order; committed recovery verifies new
state and only cleans owned temporaries. Unknown third-party state is a retained
RecoveryRequired conflict, never overwritten. Backups are retained, including
force-authorized modified-file backups. Force never bypasses path/schema checks.
Crash between manifest replacement and committed journal rolls back; after
committed journal it finishes cleanup. Failed recovery blocks further commits.
Every durable boundary receives crash/I/O failure testing and allocator-failure
sweeps; all FD/arena/path ownership must be released or retained explicitly.

### 8.10 Opaque slot lifetime C adapter

The new interop_slot_runtime.h and interop_slot_runtime_abi.zig wrap the pure
runtime without exposing its Zig layout, heap tickets or mutable pointers. A
create(epoch, out_handle) call allocates one runtime using the C allocator;
only that opaque handle owns the allocation. Failure leaves out_handle unchanged.
One guest event-loop thread serializes all access. The wrapper never maps memory,
performs shared atomics or checks whether external teardown assertions are true.
Callers must supply truthful transition and producer/host-drain proof as in8.9.

Destroy takes a pointer to the caller-owned handle and releases memory only once
the underlying runtime has completed confirm_host_drained; otherwise it fails
without freeing or NULLing the handle. A NULL handle is an idempotent no-op.
There is no force-free path. Successful destruction NULLs the handle. Creation
with epoch zero fails. All other calls take the captured session epoch and
reject mismatches before underlying mutation, including admission/resize/retire
and drain; release retains its exact sequence/window/generation checks.

Expose admission, request_geometry, prepare_publish, mark_writer_entered,
commit_publish, cancel_unentered, cancel_entered, release_frame, retire_window,
begin_drain and confirm_host_drained with their underlying scalar arguments.
Admission copies exactly three scalar view values to an output array;
prepare_publish copies one. Each view contains descriptor_index, window,
generation, width, height, stride, pixel_bytes, data_offset and data_bytes.
No reservation token or allocator identity crosses this API. The caller owns
outputs; failures leave them byte-for-byte unchanged. NULL/misaligned/overflowing
arguments or outputs overlapping runtime storage are rejected before mutation.
Booleans use fixed uint32 values0/1 and reject all others.

The C status enum has explicit fixed values: Ok0, InvalidArgument1,
OutOfMemory2, followed by every pure runtime error in its declaration order,
starting InvalidSessionEpoch3. Zig maps its exhaustive error set; no catch-all
can silently hide a new error. Static assertions cover enum and scalar-view
size/offsets on both platforms. Header docs specify lifetime, ownership, null
handling, errors and serialization for every entry point. Tests cover output
preservation, stale epochs, publication/release ordering, proof-gated destroy,
growth/retirement and repeated create/drain/destroy under leak-enabled C
sanitizers. This adapter alone adds no integrated capture/session credit.

### 8.11 Quiescent guest mapping metadata initialization

New interop_mapping_init.c/.h provides the missing native initialization step
before a fresh guest Hello. Its single C function interop_mapping_initialize
borrows mapping_base(uintptr_t), mapping_length(uint64_t), fresh epoch(uint64_t)
and exclusive_owner_proof(uint32_t). The caller must own an actual2GiB,
64-byte-aligned writable mapping, with all prior producers stopped and all old
host buffers detached. This is a caller precondition; a boolean cannot prove
external ownership. No OS driver/mapping operation or session authentication is
performed here. The guest serializes the call before allowing any peer reads.

Status enum interop_mapping_init_status_t has Ok0, InvalidArgument1,
MissingProof2 and NotLockFree3. Reject NULL/misalignment, nonexact mapping size,
uintptr_t range wrap, zero epoch and proof values other than0/1 before touching
mapping storage. Proof0 returns MissingProof with all mapping bytes unchanged.
Validate compile-time x86_64 C11 always-lock-free32/64-bit atomics and lock-free
aligned local atomic probes before modifying bytes; unsupported targets fail
compilation or return NotLockFree without mapping mutation. Caller guarantees
epoch freshness; the function does not infer it from stale mapping contents.

Write only the metadata interval [0,0x4000). Zero its4096-byte header and192
64-byte descriptors, initialize each atomic explicitly with atomic_init, and
set fixed SMW3 magic/layout3/epoch/2GiB length/192slots/8audio rings/heap offset
0x200000/heap length0x7FC00000/page bytes0x200000. Heartbeats start0; descriptor
states startFree, all other descriptor metadata and reserved storage start0.
Descriptors receive their BGRA format only when a producer owns Writing. This
routine neither scrubs heap pixels nor initializes audio rings; every byte at
or beyond0x4000 stays unchanged. Audio capability must remain clear until its
separate ring lifecycle initializes those regions. No memory barrier creates a
handshake; send Hello only after this serialized initialization returns Ok.

No allocation or retained pointer exists. Tests reserve sparse mapping address
space, make the written metadata window accessible, poison the whole window,
check every required byte/atomic and an untouched sentinel beyond0x4000, and
assert invalid calls leave storage unchanged. Linux ASan/LSan/UBSan and Windows
CPU execution qualify only this memory primitive; signed-driver access,
exclusive ownership, agent startup and real mapping integration remain open.

### 8.12 Bounded protocol stream framing for the session endpoints

New stream_reader.zig is an allocation-free, caller-owned ingress accumulator
for both future session transports. It calls the frozen codec C ABI (linked
separately) for header decoding and complete-payload validation; it implements
no second TLV/header parser. One serialized event-loop thread owns a reader.
Initialization accepts only producer directions1/2/3 and clears state. The
reader owns a fixed65536-byte frame buffer and private decoded header, with no
retained input pointer, OS handle, outbound queue or session policy.

feed borrows an accessible input byte slice and returns consumed bytes and a
frame-ready flag. It accepts at most one frame per call. Gather exactly16 header
bytes, decode/validate them before trusting payload length, then gather exactly
the declared payload. Stop at that frame boundary even when input contains
coalesced frames; the caller feeds the unconsumed suffix after consume. An empty
slice makes no progress. Feeding a complete unconsumed frame returns zero
consumed and ready=true. Reject input overlapping any reader storage before
mutation; no buffer alias permits input to overwrite reader metadata. A header
codec failure makes the reader failed/closed; later feed/peek/consume cannot
resume it. Save the exact codec error for diagnostics. Reconnect creates a
fresh reader; it does not clear old framing errors mid-connection.

peek succeeds only for one complete frame and returns a copied decoded header,
borrowed complete-frame/payload slices and the codec payload-validation result.
No consumer may interpret fields unless validation is InteropErrorNone.
UnsupportedType/UnsupportedField results remain visible for the caller's frozen
Error/close policy; framing alone does not negotiate capabilities, correlate
request IDs, authorize a message or decide connection lifetime. Malformed
payload bytes are never described as a validated application message.

consume invalidates every borrowed view and resets only the current frame
accumulator. Calling it before a complete frame is an error without mutation.
finish_input records a clean closed stream only when no partial bytes remain;
a partial frame returns TruncatedFrame and fails closed. If a complete frame
is still pending, return FramePending without mutation so the caller can process
it first. EOF never implies producer stop, slot release or compositor drain.

Tests use the real codec C ABI: bytewise/every-split/max-size input, coalesced
frames and exact consumed counts, zero-length payloads, malformed headers,
oversize/unaligned lengths, schema and direction errors, alias rejection,
truncated header/body, pending-frame EOF and fresh-reader recovery. A deterministic
mutation/stress corpus must keep all accesses bounded. Testing allocator checks
apply to fixtures; the production reader allocates nothing. Linux coverage must
retain the unchanged90% line/branch thresholds; actual Windows CPU execution
qualifies parsing only, with host/guest transport integration separately open.

### 8.13 Owned bounded outbound frame queue

New stream_send_queue.zig supplies one serialized direction's FIFO before OS
transport integration. queue_t.init borrows a std.mem.Allocator and validates
producer direction1/2/3; the allocator must outlive deinit. The queue stores512
optional frame records by value, with a ring head/count, front write offset,
pending-byte count and live flag. Each accepted enqueue owns exactly one copied
allocation containing a complete validated frame; no other production allocation
exists. Its maximum resident frame storage is512*65536 bytes plus fixed records.

Enqueue borrows accessible frame bytes and a local class Data or Control. Require
exactly one complete frame, validated through the existing codec C ABI for header,
length and direction/schema, before allocating. Malformed/unsupported input is
rejected without allocation or queue mutation. Data admission requires total
frame count<504; Control admission requires count<512. This reserves eight
admission slots but does not reorder frames or promise control latency. The
caller classifies required control events and must fail its session closed if a
required event cannot enqueue; no required event is silently dropped. There is
no transport, capability negotiation, request correlation or coalescing policy
inside this queue. Queue-full and allocator failures preserve existing frames,
front offset, counters and caller input. Copy input before publishing the record;
input may borrow an existing queued frame because enqueue frees no prior frame.

peek borrows only the remaining bytes of the front frame, or reports Empty.
Every mutation invalidates borrowed views; input and output pointers are never
retained by reference. consume_written(n) accepts only0..remaining front bytes;
zero is a no-op for a nonempty queue. Over-consumption returns InvalidCount
without mutation. A partial acknowledgement advances the offset and subtracts
pending bytes; only full acknowledgement frees/NULLs the owned allocation and
advances the head. pending_bytes means unsent bytes, not resident allocation
size: a partially sent frame retains its whole allocation until completion.
No frame is interleaved with the next. The OS writer must acknowledge only a
confirmed successful write count and use peek again after every mutation.

Deinit frees every live allocation, NULLs records, clears counters and marks the
queue closed; it is idempotent. Closed queues reject further calls; replacing
one with init requires prior deinit. No pointer/handle/OS object is owned other
than frame allocations. All calls require the same event-loop serialization;
there are no atomics or locks. Tests use std.testing.allocator and allocation-
failure sweeps, exact FIFO byte/partial-write oracles,512-frame wrap/capacity and
reserved-slot boundaries, max-size/schema/direction rejection, closed lifecycle,
repeated deinit and deterministic stress. Qualify unchanged90% production line
and branch coverage plus Linux/Windows CPU execution; actual socket backpressure
and endpoint teardown remain separate integration gates.

### 8.14 Pure connected-peer handshake core

New session_handshake.zig implements only the pre-Ready negotiation and explicit
local drain/close states. Stream framing, SemVer string parsing, Ping/Pong,
request allocation, OS mapping, send queues and transport cleanup remain separate
owners. This is an allocation-free, single-event-loop value machine. Init takes
Host/Guest role, nonzero fresh connection_token, local version major, known local
advertised capability bits and accepted_at_ms. Host begins AwaitHello; guest
begins InitializingMapping. A new connection uses a new instance/token; never
reuse the previous instance to bypass its mapping lifetime obligations.

Every event takes connection_token and monotonic now_ms. Wrong token or backwards
time is a local error with the complete state unchanged, checked before any
connection mutation. Local wrong-phase/send-confirmation errors also preserve
state. Use elapsed subtraction, never overflowing absolute deadline addition.
Until Ready, every event first checks the overall5000ms deadline from accepted
transport (including mapping initialization and partial sends). At exactly5000ms
enter Draining with reason HelloTimeout and no invented wire timeout error.
Ready has no handshake timer. Tick updates accepted time and returns no action
unless it expires negotiation. Liveness is specified separately.

Hello facts contain protocol_version, optional parsed version_major, layout_version,
capabilities, session_epoch and request_id; HelloAck facts omit epoch. The adapter
must parse the entire agent_version with SemVer2.0 grammar (prerelease/build are
allowed), reject major beyond uint32 and pass null for malformed versions. A
valid major0 is not an absent value. Frames must already pass the codec; these
fact guards defend the adapter boundary too. Endpoints use nonzero Hello request
IDs and echo exactly in HelloAck. This is explicit endpoint policy above the
codec, which deliberately does not enforce header request-ID/session policy.

Host receive_hello accepts only Host/AwaitHello. Header version other than2,
zero request ID/epoch or unknown capabilities cause ProtocolViolation and
ErrorAndDrain. Malformed/mismatched agent major or layout other than3 causes
VersionMismatch and ErrorAndDrain. Save validated Hello facts, keep public
negotiated capabilities/epoch zero, enter AwaitMapping, and emit no Ack yet.
mapping_validated accepts only the exact staged epoch, explicit successful
full-layout validation and Host/AwaitMapping. Failure emits ProtocolViolation
and drains; success stages intersection=guest_offer & initialized_host_offer,
enters AwaitHelloAckSent and returns SendHelloAck with echoed ID, local major,
layout3 and intersection. Ack carries no epoch tag. Only matching complete
ack_sent confirmation commits epoch/capabilities and enters Ready. Queuing or
partially writing an Ack never means Ready.

Guest mapping_initialized accepts only Guest/InitializingMapping with successful
external layout initialization, a nonzero fresh epoch and caller-allocated
nonzero Hello request ID. It stages local Hello facts, enters AwaitHelloSent and
returns SendHello. The external owner must already prove prior producers stopped
and old host buffers detached; this machine cannot prove it. Matching complete
hello_sent confirmation enters AwaitHelloAck without extending the overall
deadline. receive_ack accepts only Guest/AwaitHelloAck, checks protocol/layout/
major and exact request ID, and requires only known capabilities that are a
subset of the guest offer. The guest cannot recompute the exact intersection:
HelloAck contains no separate host offer. A valid Ack commits staged epoch and
returned subset and enters Ready; violations ErrorAndDrain as above.

Wire Hello/Ack received in the wrong role/phase is fatal ProtocolViolation,
except terminal Closed: after validating token/time, reject with local
WrongPhase and preserve every byte of state. A stale wire callback must never
reopen Closed as Draining. Tick, begin_drain and transport_lost on Closed return
Closed without updating time or any other field; invalid token/backwards time
still report their local errors first. Other local callbacks on Closed retain
their WrongPhase behavior.
Transport send confirmations with wrong local ID/phase are local errors with no
mutation. ErrorAndDrain clears uncommitted staged negotiation, preserves no
facility readiness, and asks the owner to send the given frozen Error code best
effort before teardown. It never replies to a received Error (dispatch belongs
to the owner). begin_drain and transport_lost block further negotiation; repeated
drain is idempotent. teardown_complete accepts only Draining and enters terminal
Closed. These events return lifecycle actions only, never a FrameRelease or
host-drained proof. Even Closed does not authorize reinitializing shared memory.
Goodbye/EOF/timeout are not mapping-detachment evidence.

Actions are copied tagged values: None, SendHello, SendHelloAck, ErrorAndDrain,
Drain and Closed; no borrowed string or heap ownership crosses the boundary.
Local endpoint config supplies the full version string for serialization; typed
outbound Hello and Ack must retain their distinct frozen field sets. Tests cover
both complete roles, zero/subset/intersection negotiation, every wrong token/
time/phase/ID/epoch/version/capability boundary, exact deadline and uint64 end
boundaries, pending-send readiness, transport loss, repeated drain and explicit
teardown. No reconnect/socket/signed-driver acceptance is implied.

### 8.15 Bounded Ready-session control liveness

New session_liveness.zig starts only after handshake Ready. It is a pure,
allocation-free, serialized value machine initialized with Host/Guest role,
nonzero connection_token and ready_at_ms. Store last accepted event, last valid
receive and last registered host Ping times, initially ready_at_ms, plus four
optional Ping records(request_id, issued_at_ms) and a closed flag. No socket,
frame allocation, mapping, request counter or shared heartbeat is owned.

Every call checks connection token, nondecreasing monotonic time and live state
before mutation. Local invalid role/ID/due/capacity errors preserve the entire
state; use a staged copy for checks that can fail after time processing. Elapsed
subtraction avoids deadline overflow. If now-last_valid_rx>=6000, close before
processing even a newly arrived frame at that exact deadline. Return a copied
Disconnect action and release every outstanding local Ping ID. EOF/transport
loss likewise closes and releases local Ping tracking only; no action releases
a frame pin or proves producer/host drain. A new connection uses a new instance.

Only complete codec-valid inbound frames count as traffic. observe_valid_frame
refreshes receive time without completing Ping requests. Guest receive_ping
requires a nonzero host request ID and returns SendPong echoing it immediately;
host receive_pong requires a nonzero ID, completes a matching pending Ping, or
ignores an unmatched/late ID while still counting the valid inbound frame.
This is explicit endpoint policy: do not invent all-time replay tracking or
terminate an otherwise live session solely for a late Pong. Wrong-direction
message dispatch remains a fatal owner/handshake policy; local wrong-role API
calls preserve state and return an error.

Host tick reports PingDue whenever now-last_registered_ping>=2000. It does not
allocate an ID or pretend a queued frame was sent. The owner obtains an unused
nonzero ID from its shared per-direction request allocator, calls register_ping
before enqueuing, then queues the Ping; a required enqueue failure closes the
session. Registration requires Host, PingDue, no matching pending ID and a free
record. A matching ID still present in any record is a duplicate even when its
deadline elapsed. The owner must first receive that record's explicit release
through another event before reusing the ID. Thus a successful registration
never returns its newly registered ID in the released-ID vector. Failed duplicate
registration preserves state and does not consume the pending expiry notification.
Registration records issued_at_ms=now and updates last_registered_ping=now.
There is no catch-up burst after missed ticks: one registration resets cadence.
Repeated ticks can report the same due advice until registration succeeds.

Each Ping record independently expires when now-issued_at_ms>=6000, even if
other valid traffic keeps the connection alive. Expire records before admission
or Pong matching. Returned actions include a fixed vector of up to four released
IDs (expiry, matched response or disconnect); the owner retires those IDs from
its allocator. With registration no faster than2s, expiry before admission and
four records, normal traffic cannot exhaust this table. An unmatched response
after expiry is harmless. This solves unbounded outstanding requests when a
peer sends valid non-Pong traffic indefinitely. The caller's common allocator
must still avoid IDs outstanding in other feature modules and implement frozen
nonzero wrap semantics; this module does not mint a separate ID namespace.

Actions contain copied flags(PingDue, Disconnect), optional echoed Pong ID,
matched-response information and released-ID vector/count. Guest tick never
requests Ping. Valid ordinary traffic does not postpone host Ping cadence.
Closed calls, backwards clocks, wrong tokens, duplicate IDs and early Ping
registration have no state mutation. Tests cover exact1999/2000 and5999/6000
boundaries, both roles, queued registration before response, independent expiry
under continuous other traffic, out-of-order/late/unknown Pongs, shared allocator
handoff outputs, disconnect, stale callback tokens, uint64 end boundaries and
long deterministic traffic without allocation. Shared-memory500ms heartbeats
remain independent and are never interpreted as detach/drain evidence.

### 8.16 Shared bounded agent-version parser

The codec's initial digits-only version check contradicts the Semantic Versioning
contract in §8.14: it rejects valid prerelease/build labels and accepts leading
zeroes. Replace that private check with a shared allocation-free Zig parser in
agent_version.zig, imported by the codec and future peer core. No wire tag,
protocol version, field width or public C codec signature changes. Parse all
5..32 input bytes using [SemVer2.0 grammar](https://semver.org/spec/v2.0.0.html),
with no NUL/prefix/whitespace/trailing bytes. Core major/minor/patch are nonempty
ASCII decimal components with no leading zero except the scalar zero. Only major
is converted, with checked uint32 arithmetic; overflow is InvalidVersion. Minor
and patch are grammar-checked strings bounded by the entire32-byte field, not an
undocumented nine-digit restriction. Optional hyphen prerelease and plus build
are nonempty dot-separated ASCII alphanumeric/hyphen identifiers; prerelease
numeric-only identifiers reject leading zeroes, build identifiers allow them.
A hyphen within an identifier is ordinary content. No version ordering is needed.

Public parse_major takes a borrowed accessible byte slice and returns copied
uint32 major or InvalidVersion. It owns no allocation, retains no pointer and is
thread-safe for independent calls; a valid major0 is distinct from error. Codec
value_valid returns false when this parser fails, preserving existing malformed
writer/payload behavior. The handshake still defends its typed adapter boundary.
Tests cover each grammar production, separator/character/boundary/overflow error,
0/UINT32_MAX majors, metadata digits with leading zeroes, long valid minor/patch,
exact32 bytes and repeated parsing without allocation. Codec tests exercise
Hello/HelloAck through writer and receive validation, not only the helper. Both
module and whole codec retain >=90% production line and branch gates; actual
Windows CPU execution remains required. No extra #4 progress is awarded for
correcting its existing semantic-version contract.

### 8.17 Shared per-direction request identifier owner

New request_ids.zig implements the header request sequence from §3.1 as one
allocation-free value per connected endpoint direction. This is the common
owner for Hello, Ping and later feature requests expecting a response. Replies
echo peer IDs and unsolicited frames use0; neither reserves a local request ID.
Input serial/window generation counters are separate namespaces. All callers on
one connection serialize operations and keep the value alive until its request
users have stopped. The new connection starts a new allocator at1.

Use a fixed4096-element uint32 slot array (zero means free), bounded occupied
count and next_candidate initialized1. The4096 cap is a local memory/admission
policy, not a wire field, peer limit or permission to drop required control
traffic. No socket, response object, queue or timer is owned. reserve returns
one nonzero copied ID and inserts it only on success. At capacity return Full
without any mutation. Starting at next_candidate, skip only currently occupied
IDs, wrapping UINT32_MAX to1. At most count+1 candidates need checking because
there are only count occupied distinct IDs. Preserve global sequence order
while skipping occupied IDs at wrap. After insertion set next_candidate to the
following nonzero ID. Use bounded loops and no overflowing ordinary arithmetic.

release takes a nonzero ID and removes exactly its occupied record; zero returns
InvalidRequestId, an absent ID returns UnknownRequestId, both unchanged. contains
returns false for0 or absent IDs and otherwise true, without mutation. retire_all
clears all occupied slots/count after the connection owner stopped every request
user, preserving next_candidate; it does not release feature resources or imply
mapping drain. Reinitializing an allocator to force ID reuse while old callbacks
remain possible is forbidden. Correlation to message type, response payload and
connection token remains the peer/feature dispatcher's responsibility. Local
queue admission failure must explicitly release its reservation or close and
retire the whole connection; no silent leak or second Ping sequence is allowed.

Public APIs are init/reserve/release/contains/retire_all with explicit ownership,
errors and serialized-thread documentation. Tests cover initial1, sequential
allocation, release/reuse distinction, UINT32_MAX wrap with live1 and UINT32_MAX,
full transactional failure, unknown/zero/double release, holes, retire_all without
sequence reset and deterministic bounded reserve/release stress. Direct test
seeding of next_candidate is only a wrap fixture; production init always starts1.
Retain >=90% production line/branch and actual Windows CPU test gates. No extra
progress is credited beyond the endpoint allocations already frozen in §8.14.

### 8.18 Raw Windows IVSHMEM lease owner

The authoritative public local API is src/interop/interop_mapping_windows.h.
It supersedes the caller-visible struct/zero-on-error suggestion in the saved
mapping_windows_contract proposal: the implementation owns an opaque C record;
all error results preserve caller output values. This is a raw SDK lease owner,
not a session agent, metadata initializer, signed-driver installer or proof of
host detachment. Do not reuse the old AV/vGPU validator or ignore cleanup errors.

One calloc-owned opaque record stores device set, device handle, raw map facts,
explicit ownership flags, published-view latch and primary/cleanup Win32 errors.
Create allocates no SDK resource. Open rejects a resource-owning record unchanged;
otherwise resets diagnostics for the new attempt. Acquire and record the SetupAPI
set, enumerate the exact index and probe detail length: the probe must return
FALSE/ERROR_INSUFFICIENT_BUFFER and a byte count at least sizeof detail and at
most65536. Allocate exactly that count, initialize cbSize and fetch detail. Require
a bounded NUL in the returned DevicePath before CreateFileW. Free/NULL detail
on every return. Use GENERIC_READ|GENERIC_WRITE, shared read/write, OPEN_EXISTING
and FILE_ATTRIBUTE_NORMAL. Record the driver handle before any map call.

Include the actual pinned looking_glass vendor IVSHMEM header through one guarded
SDK wrapper, never copy vendor source. Assert x64, config sizeof1, mmap sizeof32
and peerID/size/ptr/vectors offsets0/8/16/24, plus IOCTL0x222008/0x22200c. REQUEST
uses exactly one cached-config byte and32 output bytes. Once the IOCTL succeeds,
record mapping ownership even if returned byte count or fields are invalid.
Accept only returned32, length0x80000000, nonnull64-aligned address with representable
one-past end. Do not read the mapping. Publish view only after temporary device-set
destruction succeeds; a malformed successful reply still needs RELEASE on rollback.

Acquisition failures record the first operation error (ERROR_NOT_ENOUGH_MEMORY for
allocation, ERROR_INVALID_DATA for malformed SDK data). If an SDK returns failure
with zero last-error, normalize to ERROR_GEN_FAILURE. Rollback releases map before
closing device, and independently destroys the enumeration set. A failed release
retains map/handle and forbids CloseHandle; successful release clears map ownership
even if CloseHandle then fails. Destroy-device-set failure retains that handle too.
Keep the first cleanup failure of each attempt separately from the primary error.
Return CleanupPending if anything remains; a later close retries only retained
resources. Failed-open views were never published, so rollback/close may use proof0.
Published leases require explicit proof1 on every close attempt while the driver
map remains owned, including after a failed release. Successful release ends that
requirement: retries of only a retained device handle/set may use proof0. Keep
the publication-history latch until map release succeeds. No EOF/Goodbye/handshake/timer
provides proof. The caller stops every local user and satisfies external detach
obligations before a valid close attempt, after which the old borrowed view must
never be used even if cleanup is pending. Destroy rejects any owned resource.

No native call/allocator runs on invalid argument, Busy or MissingProof paths.
All calls are serialized; no callbacks or mapping pointers outlive the owner.
Test the same production C lifecycle against narrow handwritten Linux Win32 API
doubles and actual Windows SDK declarations with substituted calls. Include the
actual vendor header in both; a test-only initguid adapter is a double, not copied
SDK source. Fault every acquisition, malformed detail/map field, allocation and
cleanup stage; verify output preservation, diagnostic precedence, independent
cleanup, retry, one-time release/close and zero owned resources/bytes at test end.
C ASan/LSan/UBSan, >=90% production line/branch and actual Windows CPU fixture
execution remain gates. These doubles do not qualify a live signed-driver lease
or any part of full host/guest/compositor acceptance by themselves.

Malformed detail size, empty/unterminated path and malformed successful map reply
use InvalidMapping/ERROR_INVALID_DATA when rollback succeeds; probe/fetch/Open/IOCTL
API failures use OsError with the normalized acquisition error. A failed final
temporary-set destruction is a cleanup-only failure: primary_error remains0,
cleanup_error records that failure, map/device rollback is attempted and the set
remains retained for a later close. Fake SDK resource counts must independently
track successful sets/handles/maps and successful cleanup, not merely inspect
the owner's cached flags or count call attempts.

The reused Looking Glass gitlink is27fe47cbe2a3a8da986d310ab866f0b646ed68f5 at
public https://github.com/gnif/LookingGlass.git. Its project LICENSE is GPLv2;
this existing dependency supplies only the signed-driver interface declarations
and GUID at this boundary, following the existing AV/vGPU include boundary.
No Looking Glass runtime is linked, no vendor source is copied and no gitlink
is changed. CI explicitly initializes that exact existing submodule before
building the Linux double or cross-compiling the SDK translation unit.


### 8.19 Bounded host frame-lifetime registry (2026-10-24)

Accepted lead contract for next pure Zig host_buffer_registry.zig (2026-10-24).
Protocol remains v2/layout3. No native compositor or epoch proof is implemented.
Use allocation-free bounded Zig, fixed192 records. Serialized caller owns struct.
Public types: frame_key_t {session_epoch:u64,window:u64,generation:u64,slot:u32,frame_sequence:u64}; ticket_t=u64; registry_t; counts_t; state_t; registry_error_t. Constants PascalCase, funcs/fields snake_case, all type/module aliases _t. Every public symbol documented ownership/error/thread safety.
registry_t.init(epoch:u64) !registry_t: reject0. No reset/deinit clearing ownership method. State Open initially. Each record ticket immutable monotonically increasing 1..UINT64_MAX, never wraps or reuses (exhausted permanent admission error). Caller cannot reinit a live registry as cleanup. Tickets scoped to exact registry instance/epoch; callers pass captured epoch on EVERY mutating method/callback plus ticket. No pointers/callback/native resources retained.
begin_import(epoch,key) !ticket: validate epoch equality/key epoch, all window/generation/sequence nonzero,slot<3; only Open; reject duplicate occupied (window,slot) regardless gen/seq while record remains, max192; all errors leave complete state unchanged. Precondition: caller has acquired the exact validated descriptor Ready->Consuming before begin_import, with no import/native callback started; on admission error caller retains that native claim and must handle it separately. Record ImportPending, slot_free=false. Ticket assigned only successful admission, cursor unchanged on error. Bound selected from frozen SlotCount. Key is copied.
mark_attached(epoch,ticket)!void: only ImportPending -> Attached. Caller assertion: async import resolved, callback context immutable/no later import callback, live wl_buffer listener installed, adapter commits attach atomically on same serialized loop; adapter must not dispatch reentrant callback between state transition and actual attach. Production adapter not yet implemented. Repeated attached/error unchanged.
complete_never_attached(epoch,ticket,callbacks_quiesced:bool)!void: only ImportPending; require true indicating async callback impossible and no attach ever issued. -> Released, completion_kind NeverAttached. False leaves unchanged. No timeout/EOF/proxy destruction/retire shortcut. Failed/cancelled import sends same exact wire FrameRelease after explicit non-compositor proof; kind local only.
buffer_released(epoch,ticket)!void: only Attached -> Released, kind CompositorRelease. Called ONLY actual wl_buffer.release. Exact epoch/ticket guards reject stale/duplicate. A value-only API cannot authenticate a caller's native fact.
get_completion(epoch,ticket)!completion_t: only Released; copied key + kind + slot_free bool; read-only; no state mutation.
mark_slot_free(epoch,ticket)!void: only Released,slot_free false; caller asserts successful exact descriptor CAS Consuming->Free release ordering , identity checked immediately before transition. ->slot_free true. repeated error unchanged. Do not enqueue FrameRelease before this succeeds. Failed CAS escalates outer quarantine; no retirement.
notify_committed(epoch,ticket)!void: only Released+slot_free; caller asserts bounded send queue OWNS exact encoded FrameRelease, no future native callback holds this ticket. Clears record; never resets monotonic cursor. Queue full does not invoke this and retains key. Queue ACK/socket send not native lifetime proof. Reused slot gets new ticket; late callbacks must fail.
begin_drain(epoch)!void: Open->Draining, idempotent Draining, forbids new admission, preserves records; existing mutations continue. IMPORTANT adapter invokes only after fully processing FIFO prefix before guest producer-ready, not immediately on local drain request, or prior publications would be dropped.
quarantine(epoch)!void: ->Quarantined (idempotent), forbids admission; retains all records and allows exact completion callbacks/slot_free/notify to progress. Never auto-open; no free-all.
counts() counts_t: counts import_pending,attached,released,total,admission state; sums192. zero records necessary not sufficient for unmap; no API returns can_unmap/drained proof. No allocation, mapping access, wire parsing/encoding, Wayland operations, callback storage, timeline or proof generation.
Tests production bothline/branch>=90; no exclusions/loweredgate. Full192, invalidfields, wrongphase, wrong/staleepoch/ticket, UINT64_MAX exhaustion, stale ticket after reuse, callback sequences, duplicate events, neverattached false proof, queuebackpressure, drain/quarantine whilepending and attached, stress192 records variedcompletion. Tests should assert full-state unchanged on errors. Lead separately implements build/Windows/coverage gates. Worker test output/cache /tmp, no sharedmake/build/Git/VM/README/Claude reserved paths.


### 8.20 Connected control-peer composition slice (2026-10-24)

Implement `src/interop/control_peer.zig`, pure serialized Zig composition of the
existing reader/owned queue/handshake/liveness/request_ids/SemVer/C codec. This
is protocol2/layout3, not the proposed drain amendment. No socket, mapping,
compositor, epoch reset or native resource ownership is implemented. Capabilities
advertise implemented facilities only; core Ready means negotiated connection,
not native surface readiness. This slice deliberately has no feature TX/request
API yet: the host/guest OS control loop and unsolicited feature TX are later
integration work. Incoming codec-valid feature frames can be copied to the
external capability/feature dispatcher, which MUST authorize before side effects.

## API and ownership

Module types/constants snake_case_t/PascalCase, methods snake_case; every public
symbol/field documented. `peer_t.init(allocator, config)` copies local version
(5..32 bytes, bounded SemVer2, checked u32 major), borrows allocator until
idempotent deinit, owns reader/queue/one request-ID namespace and fixed scalar
state; no other allocation until a frame is queued. Config: role Host/Guest,
nonzero connection_token, accepted_at_ms, local_version slice, known capability
mask. Initialize existing handshake and incoming reader direction (opposite local
role), outgoing queue direction (local role). No callbacks or borrowed input
survive calls. Caller owns peer value; all methods on it serialized.

Required methods (names fixed; exact result structs/errors documented in source):
- `feed(token, bytes)` returns exact consumed prefix; no timestamp and no internal
  dispatch. Copies into reader; one pending frame backpressures subsequent feed.
  Invalid header is terminal: enter Draining and best-effort unsolicited Error
  ProtocolViolation (ID0). Empty input is no-op, never EOF. Every successful call
  reports phase so a framing failure is visible without a later tick.
- `next_event(token, now_ms, output)` handles exactly one pending complete frame.
  Returns copied scalar event: None, ValidateMapping{epoch,layout_version}, Ready
  {epoch,capabilities}, FeatureFrame{message_type,request_id,copied_bytes},
  PeerError{code,request_id,copied_bytes}, or Draining{reason}. Ready may also be
  indicated by ack_tx (Host readiness boundary). Error/Goodbye can copy full
  frames to output; all feature frames copy complete header+payload. Short output
  returns OutputTooSmall with required_bytes available through `pending_bytes()`;
  leaves reader, clock, peer and output unchanged. No borrowed frame escapes.
  All controls except valid received Error are internal. No pending frame returns
  None. Validate payload fully before extracting fields. Events copy no strings
  apart from the complete caller-owned output frame.
- `pending_bytes()` read-only pending full-frame size or0; no phase mutation.
- `host_mapping_validated(token, now_ms, epoch, layout_version, succeeded)` feeds
  existing handshake result and enqueues returned Ack/Error, reports phase/event.
- `guest_mapping_initialized(token, now_ms, epoch, succeeded)` reserves one
  shared Guest-direction Hello ID, feeds successful initialization to handshake,
  queues Hello using copied full configured version and original capabilities.
  Local invalid args/wrong phase preserve state incl ID cursor; stage fixed
  handshake/ID values before commit. Mandatory queue allocation failure is a
  terminal connection failure (Draining), not a successful Hello or API no-op.
- `peek_tx(token)` borrows current remaining bytes until ANY mutating peer call;
  null when no queue data. No byte may be retained across ack/deinit/mutation.
- `ack_tx(token, now_ms, written)` acknowledges only exact successful OS send
  bytes, at most current remainder. Bad counts/wrong token/backwards-time errors
  preserve all state. Zero is no progress. Only final byte of original Hello or
  HelloAck calls existing hello_sent/ack_sent and can change readiness. Preserve
  original control kind/ID separately from front remainder so partial writes
  cannot misparse header. All internal enqueues share a fixed512-entry completion
  metadata ring synchronized with owned queue success/full-pop, no parallel
  heap ownership. No completion hook on queue admission or partial ACK.
- `tick(token, now_ms)` enforces overall5s handshake deadline; once Ready starts
  existing liveness at exact Ready time/epoch. On Host Ping due reserve ID from
  same request_ids, register with liveness, queue Ping; full/alloc failure drains.
  Apply ReleasedIds exactly once. Normal pending Ping expiry/receive timeout uses
  existing helper behavior. Tick never initializes/retires mappings.
- `begin_drain(token, now_ms)` ends protocol admission and best-effort queues
  unsolicited Goodbye(status Cancelled, ID0), tells owner Draining; not safe-unmap.
- `transport_lost(token)` called only after native I/O callbacks quiesced: mark
  Closed, retire IDs, retain queued allocations until deinit; no fake peer/native
  proof. Repeated same-token terminal callbacks do not reopen state.
- `phase()` read-only phase Handshaking/Ready/Draining/Closed.
- `deinit()` idempotently frees only queue allocations and invalidates core.

All stateful methods check original connection token and live flag first. Local
argument errors (wrong token, backwards time, wrong role/phase, bad byte count,
output too small) leave all state and outputs unchanged. now_ms uses one core
last_now_ms; successful timed processing commits nondecreasing time. A valid wire
violation is a terminal state transition, not a local argument error. Exact
5s handshake and6s Ready boundaries expire BEFORE processing a frame/completion.
Draining is terminal for negotiation (Closed cannot reopen); this v2 core does
not authorize automatic epoch reconnect or replacement. In Draining existing TX
can be flushed bounded by caller; no Ping/Hello/facility dispatch restarts. No
special drain-protocol claims. Feature/native owners retain external resources.

## Dispatch/error policy frozen for this slice

Before Ready accept only expected Hello(Host) or HelloAck(Guest); wrong message,
wrong ID/version/layout/major/epoch/caps is Error and Draining, using existing
handshake actions. Host receives Hello -> ValidateMapping; it cannot Ack/Ready
until explicit matching successful external validation. Guest cannot consume Ack
as Ready until full Hello send. Handshake ID released only on successful Guest
Ready or terminal quiescence. No incoming Error is ever answered with another
Error; pre-Ready Error causes Draining without reply.

In Ready: Hello/Ack again is protocol violation. Ping Host->Guest requires
nonzero request_id; queue Pong echo internally, no feature event. Pong Guest->Host
requires nonzero ID; matched/unknown/expired handling uses existing liveness;
unknown late Pong is ignored according to helper. Every valid complete frame
counts for receive-liveness BEFORE normal handling; deadline wins at equality.
Valid Error is copied as PeerError/scoped diagnostic, never answered and does not
automatically close. Valid Goodbye requiresID0, is copied then immediately
Draining, never replied to; status/message do not grant resource proof. Other
codec-valid feature frames copied unchanged as FeatureFrame only in Ready;
external dispatcher owns negotiated capability checks, type-specific request
correlation and native effects. No facility side effect runs in core.

Malformed/unsupported payload or wrong-direction message -> best-effort Error
with codec's MalformedFrame/UnsupportedField/UnsupportedType code, then Draining.
Locally generated Error echoes triggering decoded request_id (including0), and
sets RefusedType only for a valid decoded header; bad header usesID0/noRefusedType.
All fields use canonical ascending tags and codec writer; never hand-encode known
fields. No recursive error reply for invalid received Error payload: drain only.
Mandatory Hello/Ack/Pong/Ping enqueue failure drains; Error/Goodbye allocation or
queue failure leaves Draining with terminal-send-unavailable result. No dropped
mandatory response may report Ready. Queue deinit remains the sole free owner.

## Required verification

Isolated worker tests/cache/output under /tmp; production both line+branch>=90,
no exclusions/lowered gates, std.testing.allocator allocation failures and zero
leaks. Builtin tests plus separate independent named-module suite: two real core
values exchange arbitrarily fragmented Hello/Ack/Ping/Pong; queued/partial writes
cannot grant Ready; mapping callback gates; all exact time boundaries; bad/stale
callbacks leave state; short output doesn't consume or refreshclock; scoped Error
no reply; malformed received Error no recursive reply; header/body/direction/ID
violations; allocation failure for each mandatory response; FIFO512/controlcapacity;
deinit/repeated terminal events; unknown/latePong; no epoch/native proof generated.
Lead owns shared builds, coverage harness, Windows/C ABI integration and Git.

The following wrapper preflight and terminal rules are normative corrections to
this appendix (the existing helper APIs remain unchanged):

- A valid ack_tx describes bytes already sent by the OS. After local token,
  monotonic-time and count validation, it ALWAYS consumes those bytes and pops
  matching completion metadata on a full frame, including at/after a deadline.
  Expiration then enters Draining and suppresses hello_sent/ack_sent/Ready; it
  must not expose already-sent bytes for resend. Invalid local ACK arguments
  preserve state; the caller must repair and retry that same acknowledgment
  before any further send.
- next_event preflights required copy-out capacity before any clock, deadline,
  liveness, reader or output mutation. Short output is a local error even if a
  deadline has elapsed; a subsequent valid call/tick enforces that deadline.
- The peer owns an authoritative terminal phase latch. Untimed feed header
  failure can latch Draining; untimed transport_lost can latch Closed. After
  either, negotiation/liveness helper state is never consulted for advancement.
  Existing TX can be acknowledged/flushed while Draining; Closed permits no
  further I/O work. Quiescent ID retirement and deinit never grant native proof.
- Preflight mapping-callback local role/phase errors before forwarding to the
  timed handshake helper, whose own phase-check ordering is intentionally
  different. A wrong-role/phase callback remains unchanged even at a deadline;
  a valid callback reaches the deadline and drains. Guest initialization false
  returns unchanged MappingInitializationFailed; the native caller begins drain
  on a real initialization failure. Guest zero epoch is an unchanged local
  argument error. Host succeeded=false or mismatched validated epoch/layout is
  valid failed native proof, so it reaches the helper's ErrorAndDrain path.

Terminal negotiation TX rule: the core exposes read-only
`must_abort_transport()`. When entering Draining with any unfinished queued
Hello or HelloAck, set this latch and stop offering new bytes through peek_tx
(return null). The platform owner aborts the transport; it must not complete a
readiness-causing control frame after local admission closed. Terminal
Error/Goodbye enqueue is unavailable in this case. Already OS-sent bytes must
still be accounted by valid ack_tx (including queue/metadata pop), with every
handshake completion hook suppressed while Draining or Closed; Closed itself
permits no new I/O. For normal Ready-session draining without unfinished
negotiation controls, existing TX/terminal frames may be flushed with a caller
budget, still without facility dispatch. A peer may transiently become Ready
from bytes already in flight; no wire exchange can recall them. This grants
no local facility admission, drain proof or safe reconnect, and every native
owner must keep the old epoch retained until its own accepted proof path.

### 8.21 Pure connected peer C ABI (2026-10-24)

`include/waddle/interop_peer.h` is the exact stable C surface for the appendix8.20
control-only peer; `src/interop/control_peer_abi.zig` is its implementation. It
adds no facility TX, wire field/version, native socket/mapping or drain proof.
It exposes only copied output bytes and scalar events, never borrowed TX pointers.
The C platform loop owns its send buffer and reports actual OS byte counts once.
Two-call size/copy operations are serialized with no intervening peer mutation.

The supported ABI is x86_64 Linux/Windows with 8-byte size_t/pointers. Config is
48 bytes aligned8, offsets role0/reserved4/token8/time16/version_pointer24/
version_size32/capabilities40. Event is56 bytes aligned8: kind0/phase4/flags8/
error12/epoch16/capabilities24/layout32/type36/request40/reserved44/copied_size48.
Enums/constants have explicit header values; convert phase/event kinds explicitly
rather than relying on Zig enum representation. Unused event fields/reserved are
zero. Flags reflect persistent abort/terminal-send-unavailable state on EVERY
successful event, including state snapshots and transport Closed.

The opaque owner is one C-allocator allocation holding peer_t plus any internal
version/queue allocations, all released through destroy after I/O quiesces.
Create requires a NULL owner slot and preserves it on failure; failure destroys
any partial allocation in reverse order. Destroy(NULL-valued slot) is idempotent;
success deinitializes core, frees owner and writes NULL. It has no native proof
precondition because the pure peer owns no external/native resources. Native
resource owners must survive separately; destroying core is never their cleanup.

All pointer parameters enter Zig as raw usize ABI words, following the existing
slot-runtime wrapper's supported64-bit GP register boundary. Before ANY dereference
validate nonnull required objects, natural alignment, nonwrapping address+size,
and disjoint ranges. Byte NULL+size0 is allowed and converted to an empty slice;
nonempty bytes require nonzero/nonwrapping storage. Caller still guarantees
accessible live allocations; arithmetic guards cannot authenticate forged or
freed addresses. Owner addresses must identify a live object from create.
For existing owners, reject every input/output range overlapping the opaque owner,
version allocation or ANY queue frame's FULL owned allocation (including already
ACKed prefixes). Reject overlap among outputs and between output and caller input.
Validate every output before invoking a mutating core method; on every API error
leave all output bytes/scalars unchanged. Local argument errors preserve core
state. InternalInvariant means a violated owned invariant: output remains unchanged,
but the owner must abort/quiesce and destroy rather than assume core rollback.

get_state/pending_bytes/tx_bytes validate the captured token before copied
observation. get_state succeeds after transport Closed, returning kindNone and
terminal phase/flags; pending_bytes can observe retained RX length. tx_bytes and
copy_tx obey the core Closed rejection. copy_tx returns length0 without writing
buffer for empty/abort-required TX. Short output leaves buffer/length/state
unchanged; tx_bytes supplies required length before retry. next_event uses
pending_bytes for retry; short output preserves clocks and pending frame. No
wrapper may consume bytes merely to discover size. Core failure results map to
explicit C statuses in the header; mandatory queue failure is Success with
Draining flags/event, not an unchanged API failure.

mapping_result selects Host/Guest by configured role. succeeded must be exactly0
or1. Guest layout must equal current InteropLayoutVersion before invoking core;
wrong guest version is unchanged InvalidArgument. Host actual epoch/layout and
succeeded are forwarded as validation evidence; mismatch/false follows core
ErrorAndDrain. Guest succeeded0 maps MappingFailed unchanged; native loop must
begin drain on real initialization failure. No mapping pointer is accepted.

Required gates: C static ABI offset/size assertions; real C client round-trip
between two opaque peers with partial copies/sends, mapping callbacks, Ping/Pong,
short RX/TX copy preservation, stale tokens/time/count errors, terminal flags,
transport Closed and idempotent destruction. Null/alignment/overflow/overlap
probes must run at runtime so compiler folding cannot hide guards. Tests execute
under ASan/LSan/UBSan with detect_leaks1 and actual Windows CPU CI. Zig builtin
allocator tests inject creation/queue allocation failures with zero bytes leaked.
Production wrapper line AND branch coverage remain>=90%; no native endpoint or
parent progress credit follows a standalone pure C ABI pass.


### 8.22 Window graph review and acceptance allocation (2026-10-24)

TODO #10 is allocated before integration credit: pure bounded graph25%, opaque
C ABI10%, portable observer15%, native Win32 adapter15%, coordinator/capture/
slot/wire integration20%, installed-session native qualification15%. These sum
to100% of #10. Local Claude92.25% is never copied into the global tracker.
Pure graph/C ABI credit requires independent review, >=90% actual production
line and branch coverage, zero-leak tests, and native Windows CPU execution.
Observer credit additionally requires the delivered-destroy overflow gap and
cached descendant discontinuity rules to be resolved. Session/capture/wire and
installed-session qualification retain their separate allocations.

Claude's human-released source is3714cf2 in the isolated feature worktree.
Codex review fixes graph-create alias mutation, lifecycle singleton release,
failed-hook ownership and namespace conformance through1a179b0. Exact1a179b0
full gates pass with detect_leaks1: graph136/ABI150 each in Debug+ReleaseSafe,
C27/portable observer65, exact21 exports; graph764/765 lines505/514 branches,
ABI145/146 lines60/62 branches, portable observer624/645 lines358/382 branches.
Windows binaries/native SDK objects cross-build; no Windows execution is
inferred. Durable isolated receipt is claude_window_graph/review_qualification_1a179b0.json
once the reviewed paths are imported. It is provisional source evidence.

Further review requires staged deferred-subtree invalidation after accepted
parent destruction, exclusion or identity replacement, including each retiring
Active cascade member and changed Deferred parent. Same-incarnation Reclassified
keeps parent-first flush; rejected stale hints and QueueFull cannot apply the
mask. The exact pending correction is isolated IMPL_DESC9.6, with independent
incarnation_review.md. O5 is separate: discarding a delivered destroy during
hint overflow is not unavoidable OS uncertainty. Full refresh of identical
identity tuples is not proof of continuity. No observer acceptance while this
known loss path remains. Native omission of a callback cannot be detected by
this tuple alone and must remain distinct from loss introduced by our queues.

A dedicated native Windows smoke owns one temporary HWND and observer, uses real
hooks and native queries, verifies ownerless relations, visibility/Gone and
complete teardown, and fails rather than skipping an unavailable desktop. This
is an actual adapter test, distinct from API doubles, but grants no foreign-
process, UAC, secure-desktop, driver, capture or installed-session qualification.
The isolated branch now has dedicated Linux/Windows CI; it is allowed to be
pushed for evidence, never merged into the standard branch before the global
tracker is100% and all required checks pass. No source is imported at this
contract commit, and global completion stays16.40%.


#### 8.21.1 Caller errors versus internal helper disagreement

This supersedes global tag-only error translation at C operation boundaries.
After raw address/range/alias validation, each stateful export preflights caller
conditions read-only in the same order as the core: allocated live owner, nonzero
matching token, then monotonic time when applicable. Feed rejects Draining/Closed
with WrongPhase; next_event, TX query/copy and ACK reject Closed with WrongPhase.
Tick/begin_drain/transport_lost preserve successful idempotent terminal events.
Guest mapping retains its early layout argument check, then token/time and
InitializingMapping preflight, false initialization -> MappingFailed, zero epoch
-> InvalidArgument. Host requires AwaitMapping but false/mismatched proof remains
Success with Draining. No preflight claims native mapping or detach proof.

ACK additionally preflights the actual front queue: Empty -> NoQueuedFrame,
count above remainder -> InvalidWriteCount, QueueClosed while owner is live ->
InternalInvariant. It takes no ownership and changes no queue/time before the
core call. After all these public conditions pass, any escaped core/helper error
is InternalInvariant, including helper token/time/phase disagreement; it must
never be mislabeled a caller error permitting an unsafe retry. The one legitimate
post-preflight next_event error is OutputTooSmall from its pending-frame-aware
copy preflight. Wrapper alias validation already covers its entire owner,
version and every queued allocation. Short RX output still preserves all bytes,
reader state, clocks and liveness.

TX query/copy calls peek only after public token/phase preflight; unexpected
queue failure is InternalInvariant. Mapping's exhausted shared IDs and any
unexpected codec/helper failure are also InternalInvariant. Feed's sticky reader
failure/InputClosed and any escaped peer alias check after wrapper preflight
are InternalInvariant. Creation alone keeps config/version/capability argument
and allocation mappings. No broad global error-tag allowlist is used after
stateful operation preflight. Outputs stay unchanged on every C error; an
InternalInvariant may follow internal mutation and requires caller abort,
quiescence and destruction, not retry. Stable numeric statuses/struct layouts
and protocol versions remain unchanged.

Tests retain all legitimate public statuses and add safe scalar-only helper
phase/token/time disagreements with unchanged C outputs and InternalInvariant.
No owning allocation pointer/count corruption, coverage exclusion, gate change
or production noinline workaround is authorized. The proposal in
resume_proposals/peer_contextual_errors.md is advisory; this paragraph is the
normative wrapper ordering and recovery contract.


#### 8.20.1 Portable composed core qualification

The production control_peer.zig implementation and independent named-module
suite satisfy the frozen8.20 contract locally:47 scenarios plus safe private
helper/fault probes,497/513 production lines and303/336 conditional/switch edges.
Compiler panic/stack-canary handling uses the existing recorder policy; no new
exclusion or threshold change. A real full-queue regression found a mandatory
host HelloAck enqueue failure missing its terminal_send_failed flag; that latch
is now symmetric with guest mandatory-send failure. No I/O or native memory is
owned. Native C wrapper qualification, Windows execution and integrated OS loops
remain separate requirements. Exact hashes/logs are in
control_peer_core_qualification_2026_10_24.json. Internal/helper errors can
follow mutation (especially accounting confirmed OS bytes); only the documented
caller preflight errors promise unchanged core state. An unexpected internal
failure requires abort/quiescence, never replay of already sent bytes.


#### 8.21.2 Contextual C peer qualification

The13-export wrapper now implements8.21.1. Its independent C consumer and84
Zig tests pass, with199/201 production lines and140/150 branches covered under
the unchanged90% gates. The complete interop unit, leak-enabled C sanitizer
and coverage suites pass. Exact source hashes, symbol equality, commands and
logs are retained in control_peer_abi_qualification_2026_10_24.json. C clients
use AddressSanitizer, LeakSanitizer and UndefinedBehaviorSanitizer with leak
detection enabled; the Zig implementation uses ReleaseSafe and its testing
allocator. Three Windows peer executables cross-build; actual Windows execution
for the containing source remains pending CI. No OS socket/mapping ownership or
native session acceptance follows from this portable boundary qualification.


#### 8.22.1 Reviewed graph import qualification

This supersedes the provisional source status in8.22. The explicit owned-file
snapshot at isolated73bc833 is now integrated in the parent; every qualified
source hash matches ci_qualification_73bc833.json. The main standalone gates
pass with145 graph and159 ABI tests in Debug and ReleaseSafe, C27, doubles65,
leak-enabled C sanitizers, exact21 exports and the unchanged90% production
line/branch thresholds. Exact coverage is graph826/827 lines574/586 branches
and ABI145/146 lines60/62 branches. Linux/actual Windows CI40126247004 passes
for those same sources, including the target-correlated own-window native
smoke. Main import evidence is
claude_window_graph/main_import_qualification_2026_10_24.json.

The accepted parent #10 allocation is pure graph25% plus opaque C ABI10%.
O25 cached-descendant pruning is resolved by2310f17 and its regressions. O5
delivered-destroy loss/startup ordering remains unresolved. Independent native
review additionally requires rechecking PID/TID across a fact snapshot and
preserving prior state when monitor information is indeterminate. These block
observer/native credit; CPU graph acceptance grants no installed-session,
UIAccess/UAC, secure-desktop, capture, slot/wire or host-drain proof.


#### 8.22.2 Native fact consistency correction contract

Before reading HWND relations, styles or other properties, the native query
captures nonzero process/thread IDs with GetWindowThreadProcessId. After all
property/process reads and final liveness probing, it reads those IDs again.
Any zero/error or changed pair discards the partial snapshot; a still-existing
window returns QueryFailed, never Exists or an invented Gone. A demonstrably
gone window may retain the existing Gone probe semantics. This detects
cross-process/thread replacement across the read; it does not promise an atomic
Win32 snapshot or detect identical-tuple reuse/ABA. No old facts are offered
after a failed query. Every native scan still closes at the existing scope.

The fullscreen helper has three private results: known false0, known true1,
indeterminate-1. Iconic/captioned windows are known false. MonitorFromWindow
with MONITOR_DEFAULTTONULL returning NULL means no intersecting monitor and
therefore known false. A non-NULL monitor followed by failed GetMonitorInfoW
is indeterminate, so the outer query returns QueryFailed while the HWND still
exists. Successful monitor geometry uses the existing coverage comparison.
No failure may silently clear a prior fullscreen state.

A separate Windows-only white-box fault fixture intercepts only PID/TID and
monitor-info API returns while using real temporary HWNDs and normal adapter
cleanup. It tests changed PID, changed TID, final zero/failure, monitor failure,
known fullscreen/nonfullscreen helpers and unmodified successful queries. Its
synthetic failures are distinct from the existing real native smoke, which
continues without API doubles. Both compile with Zig/MinGW and execute in
Windows CI. Linux portable gates/coverage remain required and do not claim
coverage of preprocessed-out native branches. Source authority: Microsoft
[GetWindowThreadProcessId](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowthreadprocessid),
[GetMonitorInfoW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmonitorinfow),
[MonitorFromWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-monitorfromwindow),
verified2026-10-24. Observer/native acceptance still waits on O5 and its own
full qualification; this correction alone earns no progress.


#### 8.22.3 Callback-free tracked graph snapshot

The first bounded O5 prerequisite is additive C export
`interop_window_graph_list_tracked_windows(const interop_window_graph_t *graph,
uint64_t session_epoch, uint64_t *windows, uint32_t capacity,
uint32_t *count, uint32_t *free_entry_count)`. No existing enum, struct layout
or wire version changes. It lists unique nonzero numeric HWND values from all
Active and Retiring entries, sorted ascending; equal HWNDs across generations
produce one value. Deferred records are excluded. The exact Free-slot count
counts the64 entry states, never64 minus unique HWND count.

Capacity must be at least InteropMaxWindows64 and windows must be non-NULL even
when the graph is empty. Only count elements are written; the remainder of the
caller array stays unchanged. Count and free_entry_count are mandatory aligned
u32 outputs. Validate the complete claimed capacity span, alignment, no address
wrap, pairwise array/scalar disjointness and disjointness from the full graph
allocation before forming slices or writing outputs. Common result precedence:
invalid graph pointer, then WrongThread, Reentrant, StaleEpoch, then argument
errors. StaleEpoch on this read-only API changes no rejection counter. Every
non-Ok result leaves all caller bytes and graph state/counters unchanged.

The function uses fixed64-element stack staging and bounded sort/deduplication,
no allocation, callback, clock, native window query, locks or graph busy mutation
(owner-thread validation retains the existing thread-identity guard). Engine
and C ABI tests cover empty/mixed/full states, duplicate HWND generations,
Deferred exclusion, exact free slots before/after proof completion, output
tails, null/short/alias/misalignment/wrap errors, epoch/thread/reentrancy
precedence and unchanged state/counters. Production line and branch gates
remain90%, sanitizer/testing-allocator checks remain leak-free, and the exact
export allowlist grows from21 to22. This snapshot alone fixes no delivered-
destroy loss and earns no new #10 progress. Observer binding/mailbox/lifecycle
proposal remains advisory until separately frozen and implemented.


### 8.23 Bounded Linux connected socket owner

Implement the exact public local-C interface in include/waddle/interop_linux_socket.h
and the normative ownership, polling, timing, buffering, ACK, error and cleanup
contract in [linux_socket_owner.md](linux_socket_owner.md). The lead-correction
section of that document supersedes its general statements. This is a C OS
adapter around the accepted Zig peer/parser boundary: it never parses untrusted
wire headers in C. All protocol validation remains in the Zig peer.

Source files are src/interop/linux_socket_owner.c and
tests/interop/linux_socket_owner.c. One already-connected nonblocking CLOEXEC
stream fd transfers only on successful create. Fixed8KiB RX carry and64KiB TX
copy, bounded syscalls/helper steps, immediate mandatory ACK of positive sends,
single-use poll cookies, copied one-event results, mapping-wait ticks, finite
nonextending drain deadline, and one-shot Linux close govern every path.
Creation/destroy are symmetric; no caller pointer/callback is retained.
Host starts Active; Guest starts MappingWait and requires explicit initialization.
EOF/stop/destroy/Goodbye never proves mapping release or epoch reuse.

The existing #7 host-transport25% allocation is refined before credit into
connected-byte-owner5%, connection/VSOCK/singleton10%, daemon/local-control10%.
The portable owner earns no credit until real socketpair, adversarial coverage,
zero-leak sanitizers and full shared regression gates pass. Other #7 allocations
and all Windows/native session qualification stay separate.


#### 8.23.1 Contextual mapping/drain result classification

The socket owner is the sole serialized caller of its private peer, retains
immutable configured role/token, and updates its phase/wait latch immediately
after every successful peer operation. Host MappingWait exists only after
ValidateMapping/AwaitMapping; Guest initial MappingWait corresponds exactly to
InitializingMapping. Caller mapping-result checks are read-only before the ABI
call: owner/thread/token/time, closed/state/pending phase, boolean success, then
Guest layout, failed initialization and zero epoch in the existing peer order.
Host false/mismatched evidence is forwarded unchanged and can validly return
Success with Draining. These preflights expose all legitimate caller errors
without I/O or output changes.

After these checks, every non-Success peer mapping result is an unexpected
owner/peer disagreement, including WrongPhase, InvalidArgument or MappingFailed.
It returns socket PeerFailure, closes transport once, retains the first cause,
quiesces synchronous I/O, reports transport loss and preserves caller event
bytes. It must not be advertised as retryable bad caller input. Likewise the
pure peer begin_drain accepts every phase (including idempotent terminal
handling); the live socket owner has already checked token/time/closed status.
Every escaped non-Success from begin_drain is therefore PeerFailure with the
same close/output contract, never a new local WrongPhase allowance.

Creation alone maps documented peer InvalidArgument/OutOfMemory/Success; all
other create results become PeerFailure before descriptor transfer, with owner
output NULL and caller retaining fd. Safe link-wrapper regressions distinguish
real bad-caller mapping preflights from unexpected post-preflight ABI statuses.
No blanket error-tag allowlist is permitted after operation preflight. This
corrects retry-versus-abort semantics independently of coverage and supersedes
more general wording in linux_socket_owner.md.


#### 8.23.2 Explicit poll snapshot reserved bytes

The local poll struct's32-byte layout now names the four alignment bytes at
offset20 as uint32_t reserved_zero_2. Cookie remains offset24, size32/align8;
fd/events/timeout/immediate/reserved offsets and every existing struct/wire
layout remain unchanged. Both reserved fields are zero on successful output.
This removes unnamed padding so deterministic complete output bytes do not
depend on C structure-assignment padding semantics. The owner already zeroes
staged output before setting fields. Guard snapshots must use memcpy for exact
object representations. Header static size assertions and explicit offsets,
byte-preservation tests and fresh source hashes qualify this local amendment.
No public owner exists outside this unmerged feature and no socket credit is
awarded by this header correction.


### 8.23.3 Connected-owner phase, event and final qualification containment

`linux_socket_owner.md` final containment rules are normative. A successful live
peer result must contain a frozen phase from AwaitHello through Draining;
TransportClosed or an unknown value is an invariant failure, not a new live
owner state. Validate before outward event publication/mapping bookkeeping;
invalid scalar results cannot surface FeatureFrame actions. Non-drive caller
outputs remain unchanged on PeerFailure, while drive commits WorkPeerFailure
and terminalizes once. Positive OS-send counts remain counted and unreplayable.
A tick AbortTransport terminalizes before SO_ERROR diagnosis/data I/O. Receive
abort statuses cannot be overwritten by Progress. Explicit destroy may close
transport before a peer-cleanup failure, retains its allocation for retry, and
never retries close or implies external resource release.

Final acceptance builds the ordinary production C object and verifies exactly
eight frozen exports in addition to the exact-source private unit fixture,
128 repeated real socketpair lifecycles, injected cleanup/scalar-output faults,
>=90% production line/branch coverage, leak-enabled ASan/LSan/UBSan and the full
interop regression gates. Linux alias errno checks compile once; no runtime
branch exclusions or coverage threshold reductions are permitted. These gates
accept only the5% connected-byte-owner portion of TODO#7 after the lead records
their exact evidence. VSOCK/singleton, daemon control and native release remain
separate unfinished tasks.


### 8.24 O5 observer preservation and runtime admission freeze

The normative observer extension is in `observer_destroy_preservation.md`; it
uses the accepted graph snapshot ABI and appends observer lifecycle/status
values without renumbering. `linux_session_admission.md` freezes the bounded
lease/connector owner; real runtime wiring is required before credit. The
isolated native harness boundary is `native_runner.md`. These contracts
supersede older advisory proposals and grant no progress by themselves.

### 8.25 GuestReady daemon event-loop integration

[Exact readiness contract](daemon_guest_ready.md) freezes bounded incremental
framing, correlated client transactions and per-client nonblocking configured
guest-exec probes. This supplies no mapping or native acceptance authority.

### 8.26 O5 observer implementation qualification

`observer_destroy_preservation.md` specifies the implemented immutable binding,
129-key mailbox, candidate transition, lifecycle and quarantine rules. Public
stats remain176 bytes and graph ABI remains22 exports. The observer produces no
capture, pin-drain or backing-release proof. The local all-gates receipt is
`o5_evidence_2026_10_24/qualification.json`; Windows cross-build is distinguished
from native execution. Coordinator staging and the installed session remain
separate required work. Failed observers reject tracked-snapshot synchronization
and preedit changes before graph/time/configuration mutation; sticky faults also
reject preedit changes. Only cleanup and diagnostics remain available there.

### 8.27 Bounded daemon status admission decoding (2026-10-25)

The additive `daemon_wire_decode_status_response` consumes exactly8560
little-endian bytes into the legacy C status value. C ABI sizes/offsets are
asserted independently by tests/daemon/test_status_wire.c. State is0..6,
CID3..UINT32_MAX-1, guest-exec port nonzero, RAM512..65536MiB, vCPU1..128,
mount_count0..8, reserved0. Every active1064-byte mount has nonempty terminated
1024-byte host/32-byte drive arrays, boolean read_only and reserved0. Inactive
mounts are zeroed. Returned paths are copied diagnostic values, with no path
opening/authorization. No allocation or retained pointer occurs. Inputs and
aligned outputs are caller-accessible and disjoint; errors preserve the full
output. Header type/sequence and selected-device CID/port equality remain the
runtime caller's responsibility. This is an additive local C API, no wire
version change. Tests cover scalar edges, unterminated/empty strings, all8
mounts, malformed lengths and output preservation, plus an independent C ABI
consumer under ASan/LSan/UBSan. Current production metadata coverage58/58lines
and36/36branches (existing compiler guard exclusions only). The parser alone
earns no session-entry credit.

### 8.28 Fresh native test supervisor (2026-10-25)

The test-only install supervisor follows
[fresh_native_install_contract.md](fresh_native_install_contract.md). Root
review accepted exact child/log/fd ownership, finite30-minute installation
observation plus fixed cleanup allowances, QMP greeting/ID/absolute deadline
and event bounds, swtpm readiness before QEMU, ordinary UID/KVM, source
vhost-vsock descriptor preservation and a borrowed stop-pipe. Failed QMP
transactions poison the stream and cleanup reconnects before ACPI. Every
child receives its own bounded cleanup attempt even after sibling errors.
No process names/groups are signalled. The source test suites16+16 are
hardware-free and part of interop-test. Test-run creation/launch is authorized
by the user's existing native test-device instruction; all generated secrets
stay inside the new private run. No preserved VM data or legacy secrets are
consumed. Preparation succeeded at fresh-0ef763355e3b44cbac3c0fa3414f95fa
with CID52943, private files0600 and directory0700. Preparation is not boot,
listener, interop, or installed-session acceptance. The live run must retain
its own exact result; no tracker credit is assigned to this test tooling.

### 8.29 Linux raw mapping owner (2026-10-25)

[The mapper contract](mapping_linux_contract.md) freezes the new local C API.
The opaque owner is confined to its creating pthread. It duplicates an explicit
borrowed regular/private-file or KVMFR fd CLOEXEC, validates exact2GiB capacity
and type/ownership before shared RW mmap, never creates/truncates/unlinks backing
and never initializes mapping bytes. Guest's Hello selects nonzero epoch and
ordinary header immutability before snapshots; live atomics are individually
loaded after lock-free checks, ordinary ranges copied privately, and the
existing Zig layout ABI validates all untrusted header facts. Only success
publishes a borrowed view. A valid close expires that view; munmap failure keeps
map+fd and a closing state blocks all re-publication until retry succeeds.
Linux fd close is attempted exactly once on every outcome, with CloseDiagnostic
and relinquished fd even on error. The reuse regression prevents closing an
unrelated file after an injected close error. Local user quiescence is asserted
only by a truthful caller and grants no guest/host-drain or backing-reuse
permission. No protocol or frozen layout changes. Normal make gate and exact
production169/173lines,176/188taken arcs pass with no source exclusions;
ASan/LSan/UBSan fail-fast with detect_leaks=1 pass outside the restricted tracer.
Evidence is in mapping_linux_evidence_2026_10_25. The raw adapter is a prerequisite;
live session integration and native KVMFR/Windows acceptance remain separate.

### 8.30 Bounded host admission orchestration (2026-10-25)

[session_host_contract.md](session_host_contract.md) freezes the additive C
admission API. It composes the existing singleton/AV lease owner, authenticated
same-euid AF_UNIX control channel, one bounded correlated Status/GuestReady
exchange and exactly one interop VSOCK attempt. Guest-exec port remains the
selected profile's configured port; interop stays5002. A shared absolute
CLOCK_MONOTONIC deadline and borrowed cancellation fd cover every partial
nonblocking transfer, poll, decoder boundary and post-connect publication.
All untrusted responses use Zig decoders. Incomplete/invalid transactions flag
the borrowed control stream unsafe for reuse. A late failure tears down its
real connected peer; unexpected cleanup failure publishes the authentic owner
with CleanupRequired rather than losing ownership. Caller closes only its own
control/cancel fds, and later native/mapping teardown remains separate. No
session restart, epoch reset, mapping proof or Wayland ownership is inferred.
Public/private pure-guard and real socket fixtures, fail-fast leak-enabled
sanitizers and exact99.17%lines/91.18%taken-arcs pass. The source is CI-gated;
actual entry/mapping/Hello integration is the next milestone, earning no
premature runtime credit from this component.

### 8.31 Native preparation specialize bootstrap correction (2026-10-25)

RunSynchronousCommand/Path has a maximum259-character command value. The
private preparation routine emits a239-character PowerShell command and rejects
any future value exceeding259 before serializing answer XML. It enumerates
CD-ROM logical disks only, locates the first waddle-guest-setup.ps1, executes it
and propagates its process result; absence returns31. Full driver/bootstrap
logic remains on the private answer medium. Existing account/secret ownership
and isolation rules apply. The failed real installation and bounded cleanup are
recorded in native_install_evidence_2026_10_25/specialize_path_attempt. No native
feature acceptance follows from a successful preparation or setup boot.

### 8.32 Windows connected byte transport (2026-10-25)

windows_socket_owner_contract.md is the normative bounded implementation
contract for the connected WinSock owner API, budgets, native SOCKET/thread
identity, actual send acknowledgements, deferred notification and explicit
close retry/quarantine. Only successful native close produces TransportClosed.
WSAEWOULDBLOCK retains a documented live socket for explicit retry; all other
close failures retain indeterminate ownership, never blind close/reuse. Repeat
abort returns the existing pending/indeterminate classification without I/O.
Quarantine transfers unresolved native identity to a caller-owned zeroed record
only after synchronous users stop; it establishes no detach or epoch proof.
The exact source/fixture qualification is windows_socket_owner_evidence_2026_10_25.
The native loopback fixture is executed by Windows CI separately from Linux
double/sanitizer coverage. Guest listener/mapping/agent lifecycle is separate.

### 8.33 Installed foreign-window observer fixture (2026-10-25)

installed_observer_acceptance_contract.md specifies exact native fixture
process/event/GUI ownership, bounded observation, event-order checks and cleanup.
The fixture admits real windows of its exact child into the real graph through
the unmodified native observer; it verifies title, hide, retirement and new
generation behavior. It creates no capture or pin users, so completion of its
local empty-resource retirements cannot imply production detach. Both Windows
SDK toolchains compile it; actual Windows execution is a separate gate.
