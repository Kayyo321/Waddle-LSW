# Host Wayland Windows Contract

Owner: TODO #11 subtasks 11b, 11d, 11e, 11f, 11g and 11h. The module realizes interop window messages as
xdg-shell objects, using the window table (`host_window_map.h`), the popup geometry (`host_popup_positioner.h`), the
codec (`include/waddle/interop_protocol.h`), and the rules of IMPL_DESC: 2.6 (decorations), 3.2.5 (window
messages), 3.6.2 (press table), 4.3 (appearance), 4.5 (configure, window geometry), 4.7 and 8.44 (teardown and
popup positioning).

Files: `src/interop/host_wayland_windows.h` (public API), `src/interop/host_wayland_windows.c` (implementation),
`tests/interop/host_wayland_windows_test.c` with the double (`host_wayland_windows_fake.h`), the codec fixtures
(`host_wayland_windows_fixtures.h`), the randomized run (`host_wayland_windows_stress.h`) and the defensive paths
(`host_wayland_windows_defensive.h`), `tests/interop/host_wayland_windows_coverage.py`, and the lane rules in
`scripts/interop_lane_window_map.mk`.

Out of scope: real compositor acceptance (11i, 11j), frame and buffer attachment (the frames layer, which calls
`host_wayland_windows_note_mapped`), the input path that records presses, the guest's state and dpi requests, and
the guest-side deferral of announcements (3.2.8). `src/av/*` is read only.

## Public API

Every function is documented in `host_wayland_windows.h` with In/Out directions and its complete status list.

| Function | Purpose |
|---|---|
| `host_wayland_windows_init(ctx, config)` | Copy the configuration; validate it. |
| `host_wayland_windows_reset(ctx)` | Epoch reset: destroy every window in teardown order; clear presses, seats and grabs. |
| `host_wayland_windows_handle_frame(ctx, frame, length, now_ms)` | Apply one WindowAnnounce, WindowUpdate or WindowRetire frame. |
| `host_wayland_windows_is_mapped(ctx, window, generation)` | True after the first acknowledged configure. |
| `host_wayland_windows_record_press(ctx, input)` | Record a press (lease, kind, button, seat, trigger window) for a later grab. |
| `host_wayland_windows_release_press(ctx, T)` | Remove the press with protocol serial T. |
| `host_wayland_windows_drop_presses(ctx, lease_epoch)` | Keep only the presses of one input lease. |
| `host_wayland_windows_forget_seat(ctx, seat)` | A seat was destroyed: drop its presses and release its grabs. |
| `host_wayland_windows_note_mapped(ctx, window, generation)` | Frames layer, on a window's first buffer commit: re-send child dialog parents. |
| `host_wayland_windows_set_scale(ctx, numerator, denominator)` | Scale for windows realized from now on. |
| `host_wayland_windows_window_count(ctx)` | Realized windows. |

`host_wayland_press_input_t` carries the protocol serial T, the Wayland serial S, the seat, the trigger window and
generation, the lease epoch, the press kind (pointer, touch, key), the button code, and the monotonic clock.

## Ownership, threading and clock

- The caller owns the `host_wayland_t` storage. It must not move while initialized, and it must be reset before its
  storage is released. Callbacks receive the `user` pointer unchanged.
- Borrowed: the compositor, the `xdg_wm_base`, the decoration manager, and the seats of the press table. A seat is
  borrowed from `record_press` until `forget_seat` names it or the context is reset.
- Single-thread only: the Wayland dispatch thread calls every function and every callback. Callbacks run
  synchronously inside the call that caused them, and only after the state they report is stored.
- The module allocates nothing. Listener bindings live inside the context, so each proxy's listener data address
  is stable while that object exists.
- The clock is the caller's monotonic clock in milliseconds (for example CLOCK_MONOTONIC). Every frame and every
  press advances it. A value below the last one seen clears the press table before the new input is recorded.

## Status values

`host_wayland_status_t`: `Ok`; `InvalidArgument` (NULL pointer or a bad argument, nothing changed); `MalformedFrame`
(codec or field validation failed, nothing changed); `UnsupportedType` (an unknown type or a host-to-guest type; the
codec validates direction before dispatch); `Refused` (the table or the positioner refused, or a retire was invalid;
`on_error` reported, nothing changed); `RealizeFailed` (an object, a listener, or the positioner for a popup could
not be built; the window was rolled back and `on_error` reported); `PressTableFull`; `NotInitialized`;
`DuplicateSerial` (a live press holds the same T); `SeatTableFull` (all `HostWaylandMaxSeats` seats are registered).

## Error codes and refused types

`on_error(refused_type, error_code, window, generation)`. `refused_type` is the message that caused the report:
`0x0100` WindowAnnounce, `0x0101` WindowUpdate, `0x0102` WindowRetire, `0x0103` WindowConfigure. The C names are
`InteropMsgWindowAnnounce`, `InteropMsgWindowUpdate`, `InteropMsgWindowRetire` and `InteropMsgWindowConfigure`.

| Event | refused_type | Error code | Status |
|---|---|---|---|
| Announce refused: `RefuseDuplicate` | Announce | `ProtocolViolation` | Refused |
| Announce refused: `RefuseStaleParent` or `RefuseWindowConflict` | Announce | `StaleWindow` | Refused |
| Announce refused: `RefuseExhausted` | Announce | `QuotaExceeded` | Refused |
| Announce refused: `RefuseInvalid` | Announce | `MalformedFrame` | Refused |
| Popup announced under a parent that is not realized (defensive) | Announce | `StaleWindow` | RealizeFailed |
| Popup positioner refused the geometry | Announce | `MalformedFrame` | RealizeFailed |
| Object or listener creation failed | Announce | `Unexpected` | RealizeFailed |
| Decoration mode other than client or server (event) | Announce | `ProtocolViolation` | window rolled back |
| First surface configure with no decoration mode (2.6) | Announce | `ProtocolViolation` | window rolled back |
| Update of a window that is not realized | Update | `StaleWindow` | Refused |
| Update parent refused: `Stale` or `RefuseStaleParent` | Update | `StaleWindow` | Refused |
| Update parent refused: `RefuseCycle` or `RefuseRole` | Update | `ProtocolViolation` | Refused |
| Update parent refused: `RefuseInvalid` | Update | `MalformedFrame` | Refused |
| Popup parent change (a popup never changes parent) | Update | `ProtocolViolation` | Refused |
| Popup rectangle change: positioner request refused | Update | `MalformedFrame` | Refused |
| Popup rectangle change: xdg_wm_base below version 3 | Update | `VersionMismatch` | Refused |
| Popup rectangle change: positioner could not be created | Update | `Unexpected` | Refused |
| Popup configure the positioner cannot convert | Configure | `Unexpected` | (event) |
| Retire the table rejects as invalid (`RefuseInvalid`) | Retire | `MalformedFrame` | Refused |

A Retire of a window the table no longer holds (`Stale`) is `Ok` and reports nothing.

## Announce and realization

Roles (`interop_window_role_t`): Toplevel, Dialog and UacConsent are `xdg_toplevel`; Menu, Tooltip and ImePreedit are
`xdg_popup`.

**Toplevels** are realized in this order: `wl_compositor.create_surface`, `xdg_wm_base.get_xdg_surface`,
`xdg_surface.get_toplevel`, `set_app_id` (`waddle.<device>.<app_key>`, or `waddle.<device>.uac` for UacConsent),
`set_title` (when present), `set_parent` (a Dialog whose parent is a realized toplevel or dialog), `set_min_size`
(`ceil(value / scale)`, when present), the decoration request (below), then the buffer-less commit.

**Popups** are realized in this order: `wl_surface`, `xdg_surface`, `xdg_wm_base.create_positioner`, the positioner
setters (size, anchor rectangle, anchor, gravity, offset, constraint adjustment, and `set_reactive` at xdg_wm_base
version 3 or later), `xdg_surface.get_popup` under the direct parent's `xdg_surface`, `xdg_positioner.destroy`, the
grab (below), then the buffer-less commit. The positioner values are the pure positioner's, computed from the parent's
logical size (decision 10) before any object is created.

## Grab rules (3.6.2, 8.44)

A Menu requests `xdg_popup.grab(seat, S)` before its first commit only when all of these hold:

1. Its `Announce.serial` equals a live press's T, and the press is no older than 300 ms.
2. The press's seat is registered (`host_wayland_windows_forget_seat` has not named it since).
3. The press's trigger window and the popup share the same root toplevel.
4. The grab is legal for the parent:
   - parent is a toplevel (or a dialog): no grab is active on that seat;
   - parent is a popup: the parent holds a grab on that seat, is not dismissed, and is the seat's topmost grab.

Otherwise the popup is created ungrabbed. A grab from a non-topmost parent is `invalid_grab` at the compositor and
ends the session, so the module never sends one. Tooltips and ImePreedits never grab.

The per-seat stack is represented by `grab_sequence`: each granted grab takes the next number from the context, and
the topmost grab of a seat is the live grabbed record with the highest number on that seat. A dismissed popup
(`popup_done`), or a retired one, drops its grab and every grab above it on the same seat. A popup above a dismissed
one therefore loses its eligibility to grab; the popup below it becomes topmost again. `forget_seat` clears the grabs
on a destroyed seat.

## Press table (3.6.2)

- Capacity 64 entries. Each entry holds T, S, the seat, the trigger window and generation, the record time, the
  lease epoch, the press kind and the button.
- Expiry: 300 ms after the record time. Expired entries are removed on every record and lookup.
- A live entry is never evicted. A full table refuses the new press with `PressTableFull`; the caller then fails
  the session.
- A duplicate live T is refused with `DuplicateSerial`.
- The seat of a press is registered in a table of `HostWaylandMaxSeats` (8) seats. A full seat table refuses the
  press with `SeatTableFull`. A press on an unregistered seat never grants a grab.
- Pointer presses need a nonzero button. Touch and key presses ignore it. Unknown kinds are `InvalidArgument`.
- `drop_presses(epoch)` keeps the presses of one lease. `release_press(T)` removes one. `forget_seat(seat)` removes
  the seat's presses and releases its grabs.

## Decorations (2.6, mirroring av_wayland.c)

Toplevels, Dialogs and UacConsent request `client_side` through `zxdg_toplevel_decoration_v1` after the toplevel
exists, before the first commit, with the listener registered first. Popups never get a decoration. Without a
decoration manager no window gets one.

- A decoration event stages its mode. The next `xdg_surface.configure` applies it, then acknowledges the serial.
- The first `xdg_surface.configure` of a decorated surface with no staged mode is **not acknowledged**. Delivery ends:
  the window is retired and its objects destroyed, and the error is reported (ProtocolViolation, Announce).
- A decoration mode other than client or server ends delivery in the same way.
- Server-side mode is acknowledged first, then the decoration is destroyed. The surface keeps its guest frame.
- A destroyed decoration's events are no-ops. Their objects were destroyed by the rollback.

## Configure, close and popup events

- `xdg_toplevel.configure` **latches** width, height and the state bits on the record. It does not call back.
- `xdg_surface.configure` acknowledges its serial first, then, if a state is latched, reports it through
  `on_configure` and clears the latch (4.5). A state the surface never acknowledged is never delivered. Several
  toplevel configures before one surface configure report only the last. Minimized is never reported.
- `xdg_toplevel.close` becomes `on_close`.
- `xdg_popup.configure` is converted by the pure reposition rule (`x' = x + (adjusted - origin) * scale`, with the
  guest's own size), using the request and rectangle that are currently stored, and reported through
  `on_popup_repositioned`. A conversion that does not fit `int32` is reported as `Unexpected` (Configure).
- `xdg_popup.popup_done` marks the popup dismissed in the table, drops its grab and every grab above it, and reports
  `on_popup_dismissed`. The window stays realized until the guest retires it. A dismissed popup receives no further
  configure.
- `xdg_popup.repositioned(token)` clears `reposition_pending` only when the token is the last one sent. An older
  token is stale and ignored.

Every event is delivered only when the record's generation matches the binding and the proxy the compositor names is
the object the record owns for that kind. A destroyed object therefore never reaches a callback.

## Update (4.3): all-or-nothing

A WindowUpdate is decoded, its exact record found, and then:

- **Popup**: a parent change is refused with `ProtocolViolation` before anything is written. A rectangle change
  builds the new request in temporaries, checks the xdg_wm_base version, creates a fresh positioner, sends
  `xdg_popup.reposition(positioner, token)` with the next token, destroys the positioner, and only then stores the
  rectangle, the request and the token. An unchanged rectangle sends nothing.
- **Toplevel**: the table reparent runs first; it is the only step that can refuse. Only after it succeeds are the
  title (changed titles only), the parent mirror (`set_parent` to a toplevel or dialog parent, or `set_parent(NULL)`
  for a popup parent) and the minimum size applied, and the rectangle stored.

A refused Update changes no field and sends no Wayland request.

## Retire, rollback and reset

**Retire** asks the table for the subtree, children first. Each window's objects are destroyed in this order:
decoration, then `xdg_toplevel` or `xdg_popup`, then the positioner if one is held, then `xdg_surface`, then
`wl_surface`. The window's press entries are dropped and its grab is popped before its objects are destroyed. The
record is detached before any destroy request, so an event that arrives during teardown finds no owner.

**Rollback**: an object creation or listener failure while a window is realized destroys the objects it created,
retires the window in the table, and reports the error. A decoration fault or a missing first mode retires the window
and its subtree the same way. Only a missing decoration manager is tolerated.

**Reset** tears the table down in its reset order (roots newest first, children before parents within each root).
A corrupt table is torn down by descending slot order with the same per-window order. Afterwards the presses, seats,
grab state and records are cleared, and the context is uninitialized.

## Mapped parents (note_mapped)

A dialog's `set_parent` is honoured by the compositor only once the parent has a buffer. The frames layer calls
`host_wayland_windows_note_mapped(window, generation)` on the window's first buffer commit. The module re-sends
`xdg_toplevel.set_parent` for every realized child dialog of that window. The initial request at realization is kept.
A call for a window that is not realized as a toplevel is a no-op.

## Verification

- `make interop-host-wayland-windows-test`: the module source is included, so the static helpers are exercised. The
  recording double replaces the generated request wrappers through `--wrap`. The run uses AddressSanitizer,
  LeakSanitizer and UBSan with `-Werror`.
- The double is strict. It rejects a request on a destroyed or unknown object, an ack of a serial that was never sent
  or was already acknowledged, a second role on one xdg_surface, a second listener on one proxy (-1), and a
  `xdg_popup.grab` the compositor would refuse as invalid. Its storage addresses are reused after destroy, so a stale
  pointer can name a live object. Events are dispatched only to live objects, as libwayland does. The test run
  requires zero protocol errors.
- Named checks, one per reviewed defect: `check_stale_binding_generation` (removing the generation check in
  `resolve` fails it), `check_grab_topmost_rules`, `check_update_is_atomic`, `check_popup_rect_update`,
  `check_press_table_rules`, `check_note_mapped_reparents_children`, `check_parent_size_rounds_down`,
  `check_first_configure_without_mode`, `check_invalid_mode`, `check_configure_is_latched`,
  `check_retire_invalid_reports`.
- Codec fixtures build frames with the codec writer and check the exact request sequence as `interface:opcode`, with
  `!` for a destroy.
- Randomized stress: four fixed seeds, 40 segments of 60 random operations each. After every operation the records
  must agree with the table, every success callback must name a realized window, no destroy may leave a live
  dependent behind, and no proxy may be destroyed twice. Each segment ends with a reset whose creates and destroys
  balance.
- `make interop-host-wayland-windows-coverage`: gcov on the instrumented test unit. The gate is at least 90 percent
  line and branch coverage of `host_wayland_windows.c`, with no exclusions.

## Decisions

Numbered in the order they were taken. Each covers a point where the request or the spec was silent or in conflict.

1. A decoration fault (invalid mode, or a first configure with no mode) retires the window. The request said to
   tolerate a creation failure; 2.6 and av_wayland.c make it fatal, so 2.6 is followed. Only a missing decoration
   manager is tolerated.
2. The first `xdg_surface.configure` of a decorated surface with no mode is not acknowledged, as in
   av_wayland.c `surface_configure`.
3. A press is valid for 300 ms (3.6.2). The grab family is the root toplevel, found by following direct parents
   through popups.
4. The grab stack is the per-seat order of granted grabs, represented by grab sequence numbers. A dismiss or retire
   pops the grab and every grab above it on the seat.
5. A popup never changes parent. The refusal is `RefuseRole` from the table's codes, given before the table is asked.
6. A WindowUpdate is all-or-nothing. The table reparent is the only fallible step, so it runs first.
7. A popup rectangle change sends a fresh positioner and `xdg_popup.reposition`. An xdg_wm_base below version 3 cannot
   reposition, so such a change is refused with `VersionMismatch`, and an unchanged rectangle is not refused.
8. Reposition tokens are nonzero and increase, wrapping from the maximum to 1. Only the current token completes a
   reposition.
9. An unknown type, or a host-to-guest type, is `UnsupportedType`.
10. The parent's logical size for the positioner clamp is its guest size over the scale, rounded down, and at least 1
    (4.5). Minimum sizes keep `ceil(value / scale)`, so a minimum never falls below the guest's requirement.
11. A toplevel state is reported after the acknowledgement that covers it, and a latch survives until one is reported.
12. An invalid retire (`RefuseInvalid`) reports `on_error` with the Retire message type and returns `Refused`, as
    every other refusal does.
13. The seat table holds 8 seats. A seat is registered by its first press and forgotten by `forget_seat`.
14. The lease epoch belongs to each press. `drop_presses` keeps one epoch, which the caller names on focus loss.
15. The caller's clock must be monotonic. A backwards clock clears the press table rather than trusting stale times.
16. Late events are dispatched only to live objects, as libwayland does. A late event delivered to a storage address
    reused by a new object is not distinguishable by the module from that object's own event, because the listener
    data is reused too. The module relies on the compositor layer not dispatching to destroyed proxies. The
    generation check in `resolve` is tested directly with a binding captured before the retire.
17. `note_mapped` re-sends `set_parent` for each child dialog. The initial `set_parent` is kept.
18. `set_scale` affects windows realized after the call. Realized windows keep the scale they were realized with.

## Open items

- Real GNOME, KDE and Sway acceptance (11i, 11j) is not done and earns no credit here.
- A parent's rectangle change does not reposition its child popups. The child keeps its request until it is announced
  again or updated. This belongs with the popup lifecycle of the next layer.
- A `xdg_popup.configure` that arrives after a newer reposition is converted with the newer request. The compositor's
  sequencing is not modelled.
- The guest's state and dpi on WindowUpdate are validated but not applied; the state-request path owns them.
- The frames layer must call `host_wayland_windows_note_mapped` on the first buffer commit. Nothing in this lane calls
  it yet.
- A WindowUpdate that omits the minimum size leaves the minimum unchanged.
