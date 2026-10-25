# Host Wayland Windows Contract

Owner: TODO #11 subtasks 11b, 11d, 11e, 11f, 11g and 11h. Realizes the interop
window messages as xdg-shell objects, using the window table
(`host_window_map.h`), the popup geometry (`host_popup_positioner.h`), the codec
(`include/waddle/interop_protocol.h`), and the decoration lifecycle of
IMPL_DESC 2.6, the window messages of 3.2.5, the appearance flow of 4.3, teardown
of 4.7 (per window, the per-window order of 8.44), the press table of 3.6.2, and
8.44 and 8.45.

Files: `src/interop/host_wayland_windows.h` (public API), `src/interop/host_wayland_windows.c`
(implementation), `tests/interop/host_wayland_windows_test.c` with the fake
(`host_wayland_windows_fake.h`), the codec fixtures (`host_wayland_windows_fixtures.h`),
the randomized stress run (`host_wayland_windows_stress.h`) and the defensive
paths (`host_wayland_windows_defensive.h`), `tests/interop/host_wayland_windows_coverage.py`,
and the lane rules in `scripts/interop_lane_window_map.mk`.

Out of scope: real compositor acceptance (11i, 11j, GNOME/KDE/Sway), FrameReady
and buffer attachment (the next layer), the input path that records presses,
move, resize and state requests from the guest, and the guest-side deferral of
announcements (3.2.8). `src/av/*` is read only and is not changed.

## Public API

Every function is in `host_wayland_windows.h` with full Doxygen comments:

| Function | Purpose |
|---|---|
| `host_wayland_windows_init(ctx, config)` | Copy the configuration; validate it. |
| `host_wayland_windows_reset(ctx)` | Epoch reset: destroy every window in teardown order; uninitialize. |
| `host_wayland_windows_handle_frame(ctx, frame, length, now_ms)` | Apply one WindowAnnounce, WindowUpdate or WindowRetire frame. |
| `host_wayland_windows_is_mapped(ctx, window, generation)` | True after the first acknowledged configure. |
| `host_wayland_windows_record_press(ctx, T, S, seat, window, generation, now_ms)` | Record a press for a later grab. |
| `host_wayland_windows_release_press(ctx, T)` | Remove a press on release or cancel. |
| `host_wayland_windows_set_scale(ctx, numerator, denominator)` | Change the scale for windows realized from now on. |
| `host_wayland_windows_window_count(ctx)` | Realized windows. |

The configuration (`host_wayland_config_t`) carries the compositor, the
`xdg_wm_base` and its bound version, the optional decoration manager, the device
name (1..64 bytes), the scale fraction (zero means 1/1), and the callbacks.

## Ownership and threading

- The caller owns the `host_wayland_t` storage. The context records the address
  of its own fields, so the storage must not move while initialized. Reset before
  the storage is released or reused.
- Borrowed: the compositor, the `xdg_wm_base`, the decoration manager, the seats in
  the press table, and the callback user pointer. They must outlive the windows
  that use them, or the context reset.
- Single-thread only: the Wayland dispatch thread calls every function and every
  callback. Callbacks run synchronously inside the call that caused them.
- The module allocates nothing. Listener bindings live inside the context, so
  their addresses are stable for the lifetime of each proxy.

## Status values

`host_wayland_status_t`: `Ok`, `InvalidArgument`, `MalformedFrame`,
`UnsupportedType`, `Refused` (the table or the positioner refused the frame),
`RealizeFailed` (a Wayland object or listener could not be created; the window
was rolled back), `PressTableFull`, and `NotInitialized`. A refused or failed
frame also reports `on_error` with its protocol error code.

Frames that the codec rejects as malformed (bad header, length beyond the buffer,
bad payload) are `MalformedFrame`. A type the codec does not know, and a
host-to-guest type sent by the guest, are `UnsupportedType`. Any other message
type that is valid but not window state (input, state, icon) is also
`UnsupportedType` here.

## Error codes

`on_error(refused_type, error_code, window, generation)` reports the protocol
`interop_error_t` value. The mapping is the table's, from `host_window_map.h`:

`refused_type` is the message type of the frame that caused the report: `0x0100`
(WindowAnnounce) for announce and realization, `0x0101` (WindowUpdate) for update,
and `0x0103` (WindowConfigure) for a popup conversion that does not fit.

| Event | refused_type | Error code |
|---|---|---|
| Announce refused: `RefuseDuplicate` | Announce | `InteropErrorProtocolViolation` |
| Announce refused: `RefuseStaleParent` or `RefuseWindowConflict` | Announce | `InteropErrorStaleWindow` |
| Announce refused: `RefuseExhausted` (table full) | Announce | `InteropErrorQuotaExceeded` |
| Announce refused: `RefuseInvalid` (identity or role) | Announce | `InteropErrorMalformedFrame` |
| Realization failed: popup parent not realized (defensive) | Announce | `InteropErrorStaleWindow` |
| Realization failed: positioner refused the geometry | Announce | `InteropErrorMalformedFrame` |
| Realization failed: object creation or listener registration | Announce | `InteropErrorUnexpected` |
| Update of a window never realized (`Stale`) | Update | `InteropErrorStaleWindow` |
| Update parent refused: `Stale` or `RefuseStaleParent` | Update | `InteropErrorStaleWindow` |
| Update parent refused: `RefuseCycle` or `RefuseRole` | Update | `InteropErrorProtocolViolation` |
| Update parent refused: `RefuseInvalid` | Update | `InteropErrorMalformedFrame` |
| Decoration mode other than client or server | Announce | `InteropErrorProtocolViolation` |
| Popup configure the positioner cannot convert | Configure | `InteropErrorUnexpected` |

A popup can never change parent: the table refuses it by role, so every popup
parent change reports `RefuseRole` and keeps the old parent.

Retire is silent when the table reports the window stale: the frame returns `Ok`
and reports nothing. A retire the table rejects as invalid returns
`MalformedFrame` without calling `on_error`.

## Announce and realization

Roles (`interop_window_role_t`): Toplevel, Dialog and UacConsent become
`xdg_toplevel`. Menu, Tooltip and ImePreedit become `xdg_popup`.

**Toplevels** are realized in this order: `wl_compositor.create_surface`,
`xdg_wm_base.get_xdg_surface`, `xdg_surface.get_toplevel`, `set_app_id`, `set_title`
(when present), `set_parent` (Dialog with a realized toplevel or dialog parent
only), `set_min_size` (ceil(value / scale), when present), the decoration request
(below), and the buffer-less `wl_surface.commit`. The app id is
`waddle.<device>.<app_key>`, or `waddle.<device>.uac` for UacConsent. It is set
before the first commit and never again.

**Popups** are realized in this order: `wl_surface`, `xdg_surface`,
`xdg_wm_base.create_positioner`, the positioner setters (size, anchor rectangle,
anchor, gravity, offset, constraint adjustment, and `set_reactive` when the
xdg_wm_base version is at least 3), `xdg_surface.get_popup` under the direct
parent's `xdg_surface`, `xdg_positioner.destroy`, the grab (below), then the
buffer-less commit. The positioner values are the pure positioner's: the parent's
logical size is `ceil(parent guest size / scale)` (at least 1), and the request
is computed before any object is created.

**Mapping.** The first `xdg_surface.configure` is acknowledged with its serial and
marks the window mapped (`host_wayland_windows_is_mapped`). Later configures are
acknowledged and keep it mapped.

## Decorations (2.6)

Toplevels, Dialogs and UacConsent request `client_side` through
`zxdg_toplevel_decoration_v1` after the toplevel exists, before the first commit,
with the listener registered first. Popups never get a decoration.

- A decoration event stages its mode. The next `xdg_surface.configure` applies it,
  then acknowledges the serial.
- A decorated surface's first configure without a mode is acknowledged and stays
  unmapped until a later configure carries one.
- A mode other than client or server terminates delivery: the window is never
  mapped and `Error{ProtocolViolation}` is reported.
- Server-side mode is acknowledged first, then the decoration is destroyed. The
  surface keeps its guest frame, and later configures reuse the last mode. Events
  from a destroyed decoration are no-ops.
- Without a decoration manager no window gets a decoration.

## Grab and the press table (3.6.2)

The press table holds 64 entries keyed by the protocol serial T. Each entry keeps
the Wayland serial S, the seat, the trigger window and generation, and the record
time. An entry is valid for 300 ms. Recording first expires old entries, and a
full table of live entries is refused with `PressTableFull`; the caller must then
fail the session. A live entry is never evicted.

A Menu gets `xdg_popup.grab(seat, S)` before its first commit only when all of
these hold: its `Announce.serial` equals a live entry's T, the entry is no older
than 300 ms, and the entry's trigger window has the same root toplevel as the
popup. Every other case is an ungrabbed popup, including an absent, stale,
expired or out-of-family trigger, and every Tooltip and ImePreedit. The host never
fabricates a serial. Releasing or retiring the trigger window removes its entries.

## Configure, close and popup events

- `xdg_toplevel.configure` becomes `on_configure(window, generation, state_bits,
  width, height)`. Maximized, fullscreen and activated map onto the
  `interop_window_state_t` bits. Minimized is never reported, because Wayland has
  no minimized state. Negative sizes become 0, which means the client decides.
- `xdg_toplevel.close` becomes `on_close`.
- `xdg_popup.configure` is converted by the pure reposition rule
  (`x' = x + (cx - lx) * scale`, with the guest's own size) and reported through
  `on_popup_repositioned`. A conversion that does not fit `int32` is an error
  report and no callback.
- `xdg_popup.popup_done` marks the popup dismissed in the table and reports
  `on_popup_dismissed`. The window stays realized, with its slot, until the guest
  retires it. A dismissed popup accepts no further configure, and no
  acknowledgement is sent for one.
- `xdg_popup.repositioned` (the reactive token) is accepted and ignored.

Every event is delivered only when the record's generation matches the binding,
and when the proxy the compositor names is the object the record owns for that
kind. A destroyed or rolled-back object therefore never reaches a callback.

## Update

WindowUpdate: the guest rectangle is recorded. For a toplevel, `set_title` is sent
only when the title changed. A parent change goes through the table exactly once.
A Dialog moving to a realized parent gets `set_parent` on that parent's toplevel,
or `set_parent(NULL)` when the new parent is a popup. A popup or a toplevel refuses
the change with the table's error and keeps its parent. `min_size` is sent as
ceil(value / scale). A window never realized is `StaleWindow`.

## Retire, rollback and reset

**Retire** asks the table for the subtree, children first. Each window's objects
are destroyed in this order: decoration, then `xdg_toplevel` or `xdg_popup`, then
the positioner if one is still held, then `xdg_surface`, then `wl_surface`. The
window's press entries are removed. Objects are detached from the record before
any destroy request, so an event that arrives during teardown finds no owner.

**Rollback.** Any creation or listener-registration failure while a window is
being realized destroys the objects it created, in the same order, and retires
the window in the table. No partial window stays published, and the window
reports `Error{Unexpected}` (or `MalformedFrame` for a positioner refusal). Object
creation failure is fatal to the window, following 2.6 and the av_wayland.c
pattern. Only a missing decoration manager is tolerated.

**Reset** tears the whole table down in the table's reset order: roots newest
first, and children before parents within each root. If the table is corrupt, its
records are destroyed in descending slot order with the same per-window order.
Afterwards the context is uninitialized and the press table is empty.

## Verification

- `make interop-host-wayland-windows-test`: the module source is included, so the
  static helpers are exercised. The recording Wayland double replaces the
  generated request wrappers through `--wrap`, as `tests/av/wayland_state.c` does.
  The run uses AddressSanitizer, LeakSanitizer and UBSan with `-Werror`.
- Codec fixtures: frames built with the codec writer, fed to the module, and the
  exact request sequence checked as `interface:opcode`, with `!` for a destroy.
- Randomized stress: four fixed seeds, 40 segments of 60 random operations each
  (announce, update, retire, configure, close, popup done, decoration modes,
  press, release, injected creation and listener failures). After every
  operation the records must agree with the table, every success callback must
  name a realized window, no destroy may leave a live dependent behind, and no
  proxy may be destroyed twice. Each segment ends with a reset whose creates and
  destroys balance exactly.
- Defensive paths: decoders refuse each missing required field and each bad
  parent pair (payloads are written by the codec writer and left unfinished);
  callbacks with NULL sinks; uninitialized and NULL contexts; bindings naming
  no live context; reposition overflow; a popup_done the table no longer holds;
  zero-width parents; a dialog moved onto a popup; a corrupt root walk; a window
  the table holds with no record.
- `make interop-host-wayland-windows-coverage`: gcov on the instrumented test
  unit. Gate: at least 90 percent line and branch coverage of
  `host_wayland_windows.c`, with no exclusions. The measured values are recorded in
  the commit that adds the gate.

## Decisions taken where the request or the spec was silent or conflicted

1. Decoration object creation failure rolls the window back, following 2.6 and
   the av_wayland.c pattern. The request described it as tolerated; that conflicts
   with 2.6, so 2.6 is followed. Only a missing decoration manager is tolerated.
2. Popup grab expiry uses the 300 ms rule of 3.6.2, the same as the activating
   press. Section 8.44 names expired serials without giving a period.
3. The grab family is "same root toplevel": the trigger's root, found by following
   direct parents through popups, must equal the popup's root.
4. An unknown frame type, or a host-to-guest type, is `UnsupportedType`, as the
   codec validates direction before the module dispatches.
5. A WindowUpdate that changes a popup's rectangle records the rectangle but sends
   no new positioner, because xdg_popup.reposition is not implemented yet (see Open
   decisions).
7. A refused WindowUpdate is not atomic. The rectangle (and, for a toplevel, the
   title) is recorded before the parent check, so a refused parent change still
   applies those fields and keeps the old parent. The alternative, validating the
   parent before any field, was not chosen here; it is listed under Open decisions.
6. The host integer scale is one value for all windows (set_scale). The per-output
   fractional scale is the adapter's job and is not modelled here.

## Open decisions and limitations

- Popup rectangle changes after realization are not repositioned yet. xdg_popup.reposition
  with a new positioner needs the reactive token, and it belongs with the popup
  lifecycle of the next layer.
- `set_min_size` on an Update with the field absent leaves the minimum unchanged.
- A refused WindowUpdate is not atomic (decision 7). Moving the parent check before
  the field writes would make refusal all-or-nothing; it is a small change and
  needs the lead's choice of semantics.
- Real GNOME, KDE and Sway acceptance (11i, 11j) stays open and earns no credit here.
- The guest's state and dpi on WindowUpdate are validated but not applied: the
  maximize and fullscreen requests belong to the state-request path.
