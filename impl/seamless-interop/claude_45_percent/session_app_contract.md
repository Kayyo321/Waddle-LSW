# Session Application Runtime and Reconnect Supervisor Contract

Scope: the application runtime that composes feature handlers (`src/interop/session_app.c`,
`session_app.h`), the reconnect supervisor (`src/interop/session_supervisor.c`,
`session_supervisor.h`), the built-in handler registry (`session_handlers_builtin.c`, `.h`), the
detail output of the control runtime, and the graceful stop contract. Parent contracts:
`session_control_host_contract.md` (control runtime, drain and cleanup), `session_handler.h` (the
handler interface), `host_feature_runtime_contract.md` (ops table and sender), and
`linux_session_admission.md` / IMPL_DESC 4.2 and 6.1 (retry schedule and daemon state).

## 1. Graceful stop (control runtime)

`InteropHostFeatureStopRequested` (1) returned from `on_ready`, `on_frame`, `on_fds` or `on_tick`
begins drain and ends the run with `HostControlCancelled`. No other handler is failed, and `on_closing`
still runs once. Any other nonzero status ends the run with `HostControlFeatureFailed`. The
`poll_fds` failure path is unchanged: any failure there is `HostControlFeatureFailed`.

## 2. Control detail

`interop_host_control_run_detailed(config, owners, detail)` fills `interop_host_control_detail_t`
(zeroed on entry; `detail` may be NULL):

| Field | Meaning |
|---|---|
| `admission_result` | `interop_host_session_result_t` when the outcome is AdmissionFailed; else 0 |
| `reached_ready` | 1 when Ready was committed in this run |
| `guest_error` | 1 when the guest sent a protocol Error, or a feature frame that was refused as unnegotiated |
| `ready_at_ms`, `ended_at_ms` | Monotonic milliseconds at Ready and at the end of the pump; 0 when unavailable |

`interop_host_control_run` is `interop_host_control_run_detailed(config, owners, NULL)`.

## 3. Application runtime

```c
typedef struct session_app_config_t {
    interop_host_control_config_t control;  /* feature_ops must be NULL; admission->local_capabilities must be 0 */
    const session_handler_t *handlers;      /* registration order; NULL only when handler_count is 0 */
    size_t handler_count;                   /* 0 (capability zero) or 1..SessionAppMaxHandlers (4) */
} session_app_config_t;
```

Validation (InvalidArgument before any socket is touched): `feature_ops` non-NULL; nonzero admission
offer; `handler_count` above 4; a NULL table with a nonzero count; a table pointer with count 0;
NULL `name` or `owns_type`; a duplicate name (strcmp); capability bits outside
`InteropCapabilityMask`; `poll_fds` without `on_fds`. The admission offer is the OR of the handler
capabilities; the caller's offer is zero and is replaced.

With `handler_count == 0` the call is the plain control runtime (no feature ops installed), so
the binary's empty built-in registry keeps capability-zero behaviour unchanged.

Routing:

- `on_ready`: every handler with `on_ready`, in registration order, with the negotiated set. The
  first nonzero status stops the fan-out; handlers after it are not readied.
- `on_frame`: exactly one handler must own the type (`owns_type` nonzero). Zero owners or more
  than one owner fails the run (HostControlFeatureFailed). The owner's `on_frame` receives the copied
  bytes.
- `poll_fds`: handlers in registration order each report at most `SessionHandlerMaxDescriptors` (4).
  Their descriptors are concatenated; the total is at most `InteropHostFeatureFdCapacity` (8). A total
  above 8, a handler failure or a count above 4 fails the run. Each handler's slice is recorded.
  Budget note: four handlers at four descriptors each can report 16, so the total budget cannot be
  checked at configuration time from the handler table alone. It is enforced per poll cycle at
  runtime. A per-handler `max_descriptors` field in `session_handler_t` would allow a static check;
  that interface change is requested, not made here.
  The runtime also refuses any reported descriptor equal to the socket, stop, or daemon descriptor,
  and fails the run when a reported descriptor has POLLERR, POLLHUP, or POLLNVAL.
- `on_fds`: the runtime passes the same descriptor array. Each handler whose slice contains a
  nonzero `revents` receives its own slice, with its revents, in the order it reported.
- `on_tick`: every handler with `on_tick`, in registration order; the first nonzero status stops.
- `on_error` (optional handler member): a scoped remote Error (nonzero request identifier) received
  while Ready is broadcast to every handler that defines it, in registration order; the first nonzero
  status stops. Remote Errors with identifier zero are fatal in the runtime and never reach handlers.
- `next_deadline_ms`: the minimum over handlers that provide it; `UINT64_MAX` when none does.
- `on_closing`: reverse registration order, exactly once per handler, through the closing sender
  (every send returns Draining). The runtime also announces closing when a refusal follows table
  validation (device, stop-descriptor flags, zero token), so no validated handler is left unannounced. The runtime calls the application's closing exactly once even if
  the run ends before any handler was readied; handlers that were never readied still receive
  `on_closing`. A NULL `on_closing` is skipped; the others are still called.

Status translation: a handler returning 0 continues; returning `SessionHandlerRequestStop` (1)
requests a graceful stop (section 1); returning any other value fails the run. Handlers fail
the run through this same path, so `HostControlFeatureFailed` is the single failure outcome.

Requests and sends: all handlers share one peer. Request identifiers come from the per-connection
namespace, so two handlers receive distinct identifiers. A response is routed to the handler that
owns its type; a handler sends through the sender it was called with.

## 4. Reconnect supervisor

`session_supervisor_run(config, out_owners, out_report)` runs one attempt per admission, from
scratch, through `session_app_run_detailed`. The owners pair is zero on every attempt.

Classification (`session_supervisor_classify`):

| Outcome | Class |
|---|---|
| AdmissionFailed with NotRunning, GuestUnavailable, ConnectError, DaemonError or TimedOut | Retry |
| AdmissionFailed with Busy (contention), AttemptConsumed, PeerCreateError, Cancelled, or any other result | Terminal |
| PeerFailed with `guest_error` 0 (dropped guest, handshake loss, EOF after Ready) | Retry |
| PeerFailed with `guest_error` 1 (guest protocol error or refused feature) | Terminal |
| DaemonLost, TimedOut (before Ready) | Retry |
| Success, Cancelled, InvalidArgument, FeatureFailed, UnexpectedFeature, MappingFailed, OsError, CleanupRequired | Terminal |

Backoff: the first wait is 250 ms; each subsequent wait doubles up to 4000 ms
(250, 500, 1000, 2000, 4000, 4000, ...). A Retry attempt that reached Ready and whose
`ended_at_ms - ready_at_ms` is at least `SessionSupervisorStableMs` (10000 ms) resets the next
wait to 250 ms before waiting. The schedule applies only to Retry outcomes.

Stop: stop is checked before every attempt and again before every wait. A readable stop descriptor
during a wait ends the supervisor with HostControlCancelled, and the wait is never completed. The
stop descriptor is read by the runtime and the caller only; the supervisor polls it without reading.

Cleanup: when an attempt returns CleanupRequired, the supervisor calls
`interop_host_control_retry_cleanup` up to `SessionSupervisorCleanupRetries` (3) times. If cleanup
resolves, the operational result of that attempt is unknown, so the supervisor ends with
HostControlOsError (terminal). If it does not resolve, the supervisor returns CleanupRequired with
the owners still published in `out_owners`.

Limits: `max_retries` (0 means unlimited) bounds the number of waits. When the limit is reached, the
last Retry outcome is returned.

Seams: `session_supervisor_seams_t` supplies `wait_ms`, `stop_requested`, `run` and `retry_cleanup`.
`session_supervisor_system_seams()` returns the real implementations: `poll(2)` on the stop descriptor
for waits and stop checks, the monotonic clock for the wait deadline, `session_app_run_detailed` and
`interop_host_control_retry_cleanup`. No environment variable selects a seam.

Report: `attempts`, `retries` (waits), `ready_sessions`, `cleanup_retries`, `backoff_total_ms` and
`final_outcome`.

## 5. Verification

- `tests/interop/session_supervisor.c` (scripted seams, no sockets): classification table; exact
  backoff schedule and total; the stable-session reset; stop before an attempt, after a failure and
  during a wait; contention and limits; cleanup that resolves and cleanup that does not; invalid inputs;
  2000 seeded random sequences with run/wait counts, schedule values, no run after a requested stop,
  and owners zero on entry.
- `tests/interop/session_app.c` (in-process fixture: fake daemon, VSOCK socketpair seam, guest peer):
  validation rejections; routing by type, disjoint owners and the union offer; ownership conflicts
  (two owners, no owner); descriptor slices and the budget of 8; a failure in each of on_ready, on_frame,
  on_tick, on_fds and poll_fds; a graceful stop from on_tick and from on_ready, with closing still
  running; reverse closing order exactly once with a skipped NULL closing; deadlines and the
  capability-zero path; request identifiers across two handlers; a 2000-frame stress run each way; a
  cancellation; and one integration test in which the real runtime runs under the real supervisor
  seams, the guest drops once after HelloAck, and the second attempt reaches Success.
- `tests/interop/session_app_coverage.py` builds the runner with coverage on `session_app.c` and
  `session_supervisor.c`, gates both at 90% line and branch, and runs the same runner under
  ASan, LSan and UBSan.

## 6. CLI mode and supervisor integration

`session_main` runs the supervisor with `--supervised --device NAME`, with optional `--max-retries N`
(0 means unlimited). Exactly one of `--supervised` and `--control-only` is required with `--device`.
`--control-only` keeps the one-shot path and its messages unchanged, so the existing CLI tests still apply.
`--max-retries` is accepted only with `--supervised`.

Each supervised attempt is independent: it connects the daemon, opens the configured mapping, reads a
fresh token, admits, runs the application session and closes its descriptors. Daemon connection failures are
recorded as `HostSessionDaemonError` so the supervisor retries them. Mapping, token, and clock failures
are recorded as terminal admission results. The supervisor's summary (attempts, retries, ready sessions,
backoff total) is written to stderr.

Default-on supervised mode is not implemented. The existing private parse test requires a mode flag
with `--device`, and changing that contract would change an existing test; the lead decides.

The built-in registry is empty, so the supervised binary currently advertises zero capabilities.

## 7. Review fixes applied to this contract

- Poll cookie: a feature poll failure that starts drain refreshes the socket poll cookie before the drive.
- Remote Error: a scoped Error (nonzero identifier) is non-fatal while Ready and reaches `on_peer_error` and
  the handler `on_error`; a zero-identifier Error or any Error before Ready stays fatal.
- Inbound identifiers: classes are enforced on receipt in the peer (see the host contract).
- Re-entry: `poll_fds` and `next_deadline_ms` run under the re-entry guard.
- Closing: refusals after table validation announce closing exactly once.
- `on_fds`: withheld after a same-iteration peer Draining or fatal Error.
- Descriptors: aliases of the socket, stop, or daemon descriptor and POLLHUP/POLLERR/POLLNVAL fail the run.
- Peer results: out-of-range raw results are a peer fault in the socket owner, never a table index.
- Sender results: QueueFull and RequestIdsFull are exercised end to end through the fixture. NotReady,
  Closed, and OutOfMemory are not producible from a feature callback (callbacks run only inside a Ready
  pump, and the runtime never closes the peer during a callback). They are covered at the socket layer.

## 8. Command mode and the builtin registry (tranche 3)

Default mode. `waddle-session --device NAME` with no mode flag runs the supervised session with the
builtin handlers. This is the command `waddle open` spawns when session.sock is absent. `--supervised`
selects the same mode explicitly; `--control-only` keeps the one-shot, capability-zero diagnostic run.
`--max-retries` applies to supervised mode only.

Builtin registry (`session_handlers_builtin.c`). `session_handlers_builtin_make(config, out_handlers,
out_count)` builds one table, `launch`, which forwards to the session command handler
(`session_local_handler`). The table is handed to session_app with the caller-owned storage in the
config; `session_handlers_builtin_destroy` releases the handler state after each run.

- The launch handler is initialised in its on_ready, not before. on_ready runs only after admission
  succeeded, and admission acquires `session.lock` in the session owner that is kept alive until every
  on_closing has run. The handler is therefore initialised with `lock_held_proof = 1` after the lock is
  held, which meets the obligations in `session_local_config_t`.
- Before Ready every forwarder is inert: owns_type is zero, poll_fds reports nothing, next_deadline is
  UINT64_MAX, a frame is refused, and on_closing does nothing. An attempt that never reaches Ready
  therefore never touches the handler.
- After on_closing the handler is marked not ready, so the next attempt initialises it again from
  scratch. Two consecutive guest sessions reuse the same table and storage.
- The runner requires that the daemon socket follows `<XDG_RUNTIME_DIR>/waddle/<device>/daemon.sock`, the
  layout that both the handler and the `waddle open` client derive session.sock from. Otherwise it refuses
  with status 2 before any attempt.
- The catalog handler (`session_catalog_handler.c`) is not registered. Its runtime writes desktop entries
  and icons under the user's XDG data root, its menu and shortcut policies are not derivable from the device
  configuration, and the icon-path allowlist defect in the manifest store is still open. It stays for the lead
  to register once those are decided.
- The keyboard handler (`session_input_handler.c`) is not registered: it needs a Wayland display and the
  windows/core lane. The registration hook is documented in `session_handlers_builtin.h`.

Exit statuses (`session_exit_codes.h`). 0 for a bounded run that completed or for a graceful local stop
(LocalStop or SIGINT/SIGTERM, both Cancelled). 1 for a runtime or OS failure. 2 for usage, profile and
environment errors. 75 (EX_TEMPFAIL) when another session holds the device's `session.lock` or
`command.lock`: admission reports Busy or AV-Busy before any daemon exchange. 75 is a handover: the open
client waits for the other session's socket instead of reporting a failure.

Stop signals. A stop signal that the supervisor only polled (during backoff) stays pending while the signal
descriptor is blocked. The cleanup drains the descriptor before it restores the prior signal mask, so a
pending SIGTERM is consumed and never delivered with its default action after a clean stop.

Harness verification. `tests/interop/session_local_harness.c` calls `waddle_session_main` (compiled with
WaddleSessionNoMain) and serves a fake daemon on the real socket path, with one fake guest per VSOCK connect.
Open e2e in harness mode covers: warm and cold paths, statuses, LocalStop with status 0 and socket removal,
reconnect after a dropped guest, and two consecutive guest sessions each serving one open.

Not done in this tranche: the LocalStop reply is still queued to the client before drain. Moving it after
the owners and locks are released (IMPL 3.12.5) needs `session_local_handler_send_stop_reply()`, which the
launch lane owns; session_main calls it once that function lands.

