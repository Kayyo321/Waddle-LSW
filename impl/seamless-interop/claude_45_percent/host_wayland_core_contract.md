# Host Wayland Core Contract

Owner: TODO #11g (connection, globals and seat table) and TODO #13 (poll and dispatch loop) of the seamless
interop host. The core owns the single `wl_display` connection of one `waddle-session` run. The windows layer
(`host_wayland_windows.h`) and the session handler (`session_windows_handler.h`) read the bound globals through
the accessors below. The core never creates windows.

Files: `src/interop/host_wayland_core.h` (public API, frozen except for the changes listed under Deviations),
`src/interop/host_wayland_core.c` (implementation), `tests/interop/host_wayland_core_test.c` (tests),
`tests/interop/host_wayland_core_fake.h` (recording double), `tests/interop/host_wayland_core_coverage.py` (gate).

Out of scope: window creation and configure handling (windows layer), wl_seat event handling such as capabilities,
keyboard and pointer objects (input lane), frame and buffer work on wl_shm (frames layer), and real compositor
acceptance. `src/av/*` is not touched.

## Public API

All status values come from `host_wayland_core_status_t`. A NULL core pointer is `InvalidArgument` where a status is
returned, `NULL` or `0` or `-1` where a value is returned, and never a crash.

| Function | Returns |
|---|---|
| `host_wayland_core_init(core, config)` | `Ok`; `InvalidArgument` for NULL core or config, or an already initialized core; `ConnectFailed`; `MissingGlobal`; `RoundtripFailed`. |
| `host_wayland_core_close(core)` | nothing. NULL or uninitialized is a no-op. Afterwards the storage is zeroed. |
| `host_wayland_core_poll_events(core)` | `POLLIN`, plus `POLLOUT` while output is pending. `0` for NULL, uninitialized or failed. |
| `host_wayland_core_fd(core)` | `wl_display_get_fd`. `-1` for NULL, uninitialized or failed. |
| `host_wayland_core_dispatch(core, revents)` | `Ok`; `WouldBlock` when a flush hit EAGAIN; `DisplayError`; `NotInitialized`; `InvalidArgument`. |
| `host_wayland_core_flush(core)` | `Ok`; `WouldBlock`; `DisplayError`; `NotInitialized`; `InvalidArgument`. |
| `host_wayland_core_roundtrip(core)` | `Ok`; `RoundtripFailed`; `DisplayError`; `NotInitialized`; `InvalidArgument`. |
| `host_wayland_core_set_seat_listener(core, listener, user)` | `Ok`; `NotInitialized`; `InvalidArgument`. |
| `host_wayland_core_globals(core)` | pointer to the globals, or `NULL` for NULL or uninitialized. |
| `host_wayland_core_seat_count(core)` | count in 0 .. `HostWaylandCoreMaxSeats`; `0` for NULL or uninitialized. |
| `host_wayland_core_seat_at(core, index)` | the seat in slot `index`; `NULL` for a free slot, an index at or above `HostWaylandCoreMaxSeats`, or NULL or uninitialized core. |

Status meanings: `DisplayError` is sticky until close. `RoundtripFailed` is not sticky: the connection is still
usable, and only the caller decides what to do. `WouldBlock` is not an error: the caller waits for `POLLOUT`.

## Ownership

- The caller owns the `host_wayland_core_t` storage. It must be zero-filled or closed before init, because init
  reads the `initialized` byte to refuse a second init. The storage must not move between init and close, because the
  registry, ping and seat callbacks carry its address.
- An injected display (`config.display` non-NULL) is borrowed and never disconnected. A display from
  `wl_display_connect` (`config.display` NULL) is owned and disconnected by close, and only then.
- Every wl_* object the core binds is owned by the core and destroyed by close. Pointers returned by `globals` and
  `seat_at` are valid until close.
- A seat is borrowed from the core through the listener. The listener must not destroy it. The core registers no
  wl_seat listener of its own, so the input lane may attach one (libwayland allows one listener per proxy).
- The core never allocates. Seats live in a fixed table of `HostWaylandCoreMaxSeats` (8) entries.

## Threading

Single thread. The thread that calls init also calls every other function and receives every callback. Listener
callbacks run synchronously inside `dispatch`, `roundtrip` (startup included), and `set_seat_listener` (the replay).
A listener must not call `close`, `init`, `dispatch`, `flush` or `roundtrip`, and must not change the seat listener
during a removal. The core does not check for this; it is a rule for the callers.

## Startup sequence (`init`)

1. Reject NULL arguments and an already initialized core (`InvalidArgument`, nothing changed).
2. If `config.display` is NULL, call `wl_display_connect(config.display_name)`. Failure is `ConnectFailed`, and the
   storage is left unchanged.
3. Zero the storage, record the display and the ownership flag, set `initialized`.
4. `wl_display_get_registry`, then add the registry listener. A NULL registry or a refused listener closes the core
   and returns `RoundtripFailed`.
5. `wl_display_roundtrip`. A negative result closes the core and returns `RoundtripFailed`. The roundtrip delivers
   every queued global, so each global is bound before it returns.
6. Each global is bound inside its registry callback (see Globals below).
7. Require `wl_compositor`, `xdg_wm_base` and `wl_shm`. Any one missing closes the core and returns `MissingGlobal`.
   The decoration manager is optional; its absence is not an error.

Every failure after step 3 runs `host_wayland_core_close` before returning, so the connection is never left open.

## Globals and bind versions

| Global | Bound version | Notes |
|---|---|---|
| `wl_compositor` | min(announced, 4) | Bound once; later announcements are ignored. |
| `xdg_wm_base` | min(announced, 5) | Bound once. The version is kept in `globals.wm_base_version`. A listener refused at bind time destroys the object and leaves the slot NULL. |
| `wl_shm` | min(announced, 1) | Bound once. Version 1 has no release request. |
| `zxdg_decoration_manager_v1` | min(announced, 1) | Bound once; optional. |
| `wl_seat` | min(announced, 7) | Recorded in the seat table (below). |

Unknown interfaces are ignored. A bind that returns NULL leaves the slot empty. A required global that stays empty
produces `MissingGlobal` at startup.

## Seat table

- Each recorded seat occupies the first free slot of `seats[]`, and `seat_names[]` keeps its registry name.
- A seat announced while all 8 slots are occupied is refused before it is bound and increments `seats_refused`.
  That is not an error.
- A bind that returns NULL records nothing and does not increment `seats_refused`.
- The seat listener, when set, is called with `added = true` as the seat is recorded. Setting the listener replays
  every recorded seat in slot order with `added = true`. Setting NULL stops notifications and keeps the table.

### Removal (`global_remove`)

Only recorded seats are acted on. A removed name that is not a recorded seat is ignored, and so are removals of the
compositor, shm, xdg_wm_base and decoration globals: the core does not track their registry names. This is a known
limit (see Honest limits).

For a recorded seat, in this order:

1. Call the listener with `added = false`. The seat pointer is still valid during the call.
2. Destroy the seat: `wl_seat_release` when its proxy version is 5 or later (the request exists from version 5),
   otherwise `wl_seat_destroy`, which is a client-side proxy destruction.
3. Clear the slot and its name.

## Poll and dispatch

`poll_events` returns `POLLIN`. It adds `POLLOUT` while `output_pending` is set.

`dispatch(core, revents)`:

1. If `revents` has `POLLERR`, `POLLHUP` or `POLLNVAL`, the core becomes failed and returns `DisplayError`.
2. `wl_display_dispatch_pending`. A negative result is fatal.
3. If `POLLIN` is set, read the socket:
   a. `wl_display_prepare_read`. On failure with `EAGAIN` (events are queued), dispatch the queue and retry. A
      failure with any other errno, or a dispatch that returns zero while the prepare still fails, is fatal.
   b. `wl_display_read_events`. A negative result is fatal unless `wl_display_get_error` is zero and errno is
      `EAGAIN` or `EINTR`. Those two are not errors: nothing was read.
   c. `wl_display_dispatch_pending` again. A negative result is fatal.
4. Flush: success clears `output_pending`. A failure with errno `EAGAIN` and a zero display error sets
   `output_pending` and returns `WouldBlock`. Any other failure, or a nonzero display error, is fatal.

`flush` runs step 4 alone. `roundtrip` runs `wl_display_roundtrip`; success clears `output_pending`, because the
roundtrip flushes everything it sent.

A fatal condition sets `failed`. Afterwards `dispatch`, `flush` and `roundtrip` return `DisplayError`,
`poll_events` returns 0, and `fd` returns -1, until close.

### Ping answer

The xdg_wm_base listener answers each ping with `xdg_wm_base_pong(wm_base, serial)` inside the dispatch that receives
it. The pong is buffered and sent by the flush that ends that dispatch. A ping is never dropped, and each ping
produces exactly one pong with the same serial.

## Teardown (`close`)

Order, checked by the tests:

1. Every recorded seat, in slot order: listener notified with `added = false`, then release or destroy.
2. `zxdg_decoration_manager_v1_destroy`, when bound.
3. `wl_shm_destroy`, when bound. In libwayland 1.23 this is a client-side destruction (`wl_proxy_destroy`). The wl_shm
   protocol has no destroy request at any version; its `release` request, from version 2, is not sent at the bound
   version 1.
4. `xdg_wm_base_destroy`, when bound.
5. `wl_compositor_destroy`, when bound. Also client-side in libwayland 1.23.
6. `wl_registry_destroy`, when the registry exists.
7. `wl_display_disconnect`, only when the display is owned.

Then the storage is zeroed. Close on NULL or an uninitialized core does nothing and calls no libwayland function.

## Failure model

| Condition | Result | Sticky |
|---|---|---|
| NULL pointer, or core not initialized | `InvalidArgument` or `NotInitialized`, or `0` or `-1` | no |
| `wl_display_connect` fails | `ConnectFailed`, storage unchanged | no |
| Registry creation or listener refused, startup roundtrip fails | `RoundtripFailed`, core closed | no |
| Required global missing, or its bind or listener refused | `MissingGlobal`, core closed | no |
| Flush hits EAGAIN with no display error | `WouldBlock`, `POLLOUT` set | no |
| POLLERR, POLLHUP, POLLNVAL | `DisplayError` | yes |
| Dispatch fails; read fails other than EAGAIN or EINTR; flush fails other than EAGAIN | `DisplayError` | yes |
| Any libwayland error with `wl_display_get_error` nonzero | `DisplayError` | yes |
| Roundtrip fails with no display error | `RoundtripFailed` | no |

Close after a failure always runs the full teardown. A compositor protocol error makes the next dispatch or flush
return a negative result with `wl_display_get_error` nonzero, which is fatal here. The core does not try to recover.

## Deviations from the brief

- `wl_display_cancel_read` is not called. A failed `prepare_read` holds no read intention, so there is nothing to
  cancel; the core dispatches the queue and retries. A successful prepare is always followed by `read_events` in the
  same call, so no path leaves a read intention open.
- The brief named `wl_shm_destroy` and `wl_compositor_destroy` as separate steps. Both exist in the installed
  libwayland 1.23 headers, but as client-side proxy destruction. The protocol XML (`/usr/share/wayland/wayland.xml`)
  has no destroy request for wl_compositor or wl_shm, so no request is sent. The core calls them as written.
- The header gained one private field, `output_pending`, and two documentation fixes (POLLNVAL as fatal in dispatch,
  and init refusing an already initialized core). The POLLOUT rule cannot be expressed without the field.

## Verification

Test: `tests/interop/host_wayland_core_test.c`. It includes the module source, so the static helpers are reachable,
and it links the generated xdg-shell and xdg-decoration protocol objects. The recording double replaces libwayland
through `-Wl,--wrap` for the proxy functions and the display functions the core calls.

The tests cover:

- init with an injected display: all globals bound, the bound versions, the bind requests, the accessors, the poll
  descriptor and mask;
- each missing required global, the optional decoration manager, failed binds, a refused xdg_wm_base listener, a
  refused registry listener, a failed registry creation, a failed startup roundtrip (both modes);
- owned and unowned displays, a failed connect (storage unchanged), and invalid init arguments;
- the ping: a queued ping and a direct listener call, each producing exactly one pong with the same serial;
- seats: announcement during and after init, the listener replay, removal with notification and destroy order,
  removal without a listener, removal of unknown names, the 8-slot table with a refused ninth, release versus destroy
  by version, and removal of non-seat globals being ignored;
- dispatch: fatal revents (sticky), read results (EAGAIN, EINTR, EIO, and faults), `dispatch_pending` failures on
  either call, prepare-read retry, a prepare that fails with a stray errno, a prepare that makes no progress, POLLOUT
  only dispatch;
- flush: WouldBlock with POLLOUT, the clear on a later flush and on a roundtrip, fatal errors (EINTR, EIO, faults);
- roundtrip: success, a non-sticky failure, a sticky fault;
- NULL and uninitialized arguments for every function;
- the exact close order, with and without an owned display.

Every run ends with a check that no libwayland request was sent on a dead or unknown proxy, and that no proxy was
destroyed twice.

Sanitizers: the test is built and run with `-fsanitize=address,leak,undefined -Werror` and
`ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.

Coverage: `tests/interop/host_wayland_core_coverage.py` builds an instrumented copy under
`build/coverage/interop/host_wayland_core/`, runs it, and gates `src/interop/host_wayland_core.c` at 90 percent line
and branch with no exclusions. Measured: 169 of 169 lines and 178 of 178 branches.

## Honest limits

- No real compositor is involved. Every event is delivered by the double, and no test checks the wire format.
  Acceptance by a real compositor is the job of a later lane.
- The injected display is a double proxy. Real libwayland behaviour of `prepare_read`, `read_events` and `flush`
  under contention is modelled from the libwayland documentation, not observed.
- A missing decoration manager is tolerated. The core reports it as `decoration_manager == NULL`; the windows layer
  must treat that as "no server-side decoration negotiation" and must not dereference it.
- Removal of the compositor, xdg_wm_base, shm or decoration globals is ignored. A compositor that removes one of them
  mid-session leaves a stale pointer in `globals` until close. The core does not track their registry names.
- Globals announced after init are bound when they arrive, if their slot is empty. The core does not re-validate
  them, and there is no recovery for a required global that disappears after startup.
- The seat listener is called from inside libwayland's dispatch. A listener that does real work there delays the
  whole dispatch.
- A roundtrip failure with no display error is not sticky. The caller decides whether to close. This matches the
  contract in the header and is not a claim that the connection is healthy.
- The seat table holds 8 seats. The ninth is refused silently apart from `seats_refused`.
