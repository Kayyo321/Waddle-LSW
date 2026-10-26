# Pending review TODOs (45 percent milestone)

The user paused the session on 2026-10-26 and asked that the remaining reviews be
left as TODOs. Every credited subtask below passed its **first-pass** independent
(Opus) review: each confirmed High/Critical finding was fixed with a
failing-before test and the gates were re-run on the merged head. The
**second-pass verification** reviews below were started and then stopped
unfinished; they are the first actions of the next session. If a second pass finds a
defect in a credited slice, remove that slice's credit in `TRACKER.md` honestly.

Method for every review: Opus agent, read-only (no `make`, no edits), scratch
builds under the session scratchpad only, verify each claimed fix by reading the
code and a targeted experiment, run three mutation experiments, finish with an
ACCEPT / ACCEPT-WITH-NOTES / REJECT verdict per sub-allocation. The three
prompts were:

## R1. Wayland layer and windows handler (TODO #11)

Scope: `host_window_map.zig`, `host_popup_positioner.zig`, `host_wayland_windows.c`,
`host_wayland_core.c`, `session_windows_handler.c` and their tests/contracts.
Verify the first-pass findings are fixed: popup grab topmost rules
(`invalid_grab`), all-or-nothing `WindowUpdate`, popup reposition, press-table
bounds (lease epoch, seat removal, duplicate serials, clock), dialog
`set_parent` before parent mapping, anchor rect inside the parent (floor), IMPL
2.6 decoration handling, configure after ack, naming. Review the **unreviewed**
`session_windows_handler.c` / `host_wayland_core.c` (hostile frames, ownership,
coalescing, `FrameReady` -> immediate `FrameRelease`, move/resize serial
validation, reducer use, teardown). Verdicts for 11a, 11b, 11c, 11d, 11e, 11f,
11g, 11h. **11g (handler wiring) is NOT credited until this review accepts it.**

## R2. Session runtime and the `waddle open` / `waddle session stop` path (TODO #7, #29)

Scope: `session_control_host.c`, `session_app.c`, `session_supervisor.c`,
`session_handlers_builtin.c`, `session_main.c`, `session_local_handler.c`,
`session_exit_codes.h`, `linux_socket_owner.c`, `control_peer.zig` request-id
classes, `open_command.zig`, `open_request.zig`, `activation_store.zig`,
`session_stop_command.zig`, `local_session_client.zig`, the `host.c` hunks, the
e2e suites. Verify the two first-pass findings lists (runtime: drain on
`poll_fds` failure, scoped remote Error, request-id classes, re-entrancy, early
`on_closing`, descriptor faults, table bounds; open path: cold-start argv,
lock-contention handover with exit 75 on the **client** side, SIGPIPE/ECONNRESET,
terminal escapes, LocalStop reply ordering, handler re-arm, socket path agreement
and client peer-credential check, slot exhaustion, notifications, reserved names,
fd leak). **Known partial items from the workers**: server runtime path derivation
from the daemon fallback helper (item 7), disconnect-drops-pending-entry (item 8),
and `open`'s handling of exit 75 as "handed over" were not confirmed done.
Verdicts for #7 local control 5% and reconnect 10%, and #29 29a-29f, 29h.

## R3. Manifest commands, keyboard/pointer handlers (TODO #33, #31, #15, #16)

Scope: `manifest_store.zig` additions (`status`, `remove`, `forget`,
`register_record`, `migrate_record`, icon path `icons/hicolor/...` with legacy
handling), `integration_commands.zig`, `integration_mime.zig`,
`integration_producers.zig`, `session_input_handler.c`, `session_input_wayland.c`,
`keymap_names.zig`, `guest_input.c`, keyboard modules (spot-check >= 15 KLID rows
against Microsoft), the AV pointer tranche (`b4d437c`, `371cd3d`, `d361b8f`),
`guest_launch.c`. Verdicts for 33a, 31a, 31b (is the `exe_handler` route into
`waddle open` implemented?), #15 typed injection / live repeat / pure and live
halves, #16 16a, and #29i (`guest_launch`, CI-run on a real desktop).

## Reviews that never ran at all

* `catalog_runtime` fixed version (lane branch `seamless-45-lanes/catalog`).
* `window_coordinator` fixed version (lane branch `seamless-45-lanes/coordinator`).
* `guest_launch.c` (first pass), `guest_input.c` (first pass).
* The `session_main` default-supervised mode and `session_handlers_builtin`.

## Credit that depends on these reviews

See the "Credited" and "Implemented but not credited" tables in
[HANDOFF_RETURN.md](HANDOFF_RETURN.md).
