# Session local command handler contract (TODO #7 socket side, TODO #29 29c/29f)

Scope: the `waddle-session` feature handler that serves the local `session.sock` command
socket. It accepts `LocalLaunch` (0x7F00) from `waddle open` and `LocalStop` (0x7F01) from
`waddle session stop`, forwards launches to the guest as `LaunchRequest` (0x0700, H->G),
correlates the guest `LaunchResult` (0x0701) with the waiting client, stores the host-owned
activation token, and answers `LocalStop` with `LocalStopResult` (0x7F02) before requesting a
graceful drain. Parent contracts: `host_feature_runtime_contract.md` (ops table, sender,
callback rules), `open_command_contract.md` (client side), `activation_store_contract.md`,
IMPL_DESC 3.9.1, 3.9.3, 3.12.5, 6.5, 8.44.

In scope: directory and socket creation, stale socket policy, peer credential decision, client
slots, frame reading, the correlation table, the token policy, launch forwarding and reply
mapping, LocalStop ordering, timeouts, teardown.

Out of scope: the session application runtime (`session_app`, `session_main.c`) that admits the
session lock, sets `lock_held_proof`, composes the handler into `interop_host_feature_ops_t`, and
maps `SessionHandlerRequestStop` to a graceful stop; the guest side of launches; the window and
catalog features; `waddle session stop` client behaviour (already specified by 3.12.5).

## 1. Files

| Path | Role |
|---|---|
| `src/interop/session_local_handler.h` | Public API: config, handler struct, stats, table export, pure decision helpers. |
| `src/interop/session_local_handler.c` | Implementation. Calls only the codec, the activation store, the feature sender and libc. |
| `tests/interop/session_local_handler_test.c` | Unit, socket, and fault-injection tests; includes the handler `.c` for private rules. |
| `tests/interop/session_local_harness.c` | Harness binary: real `interop_host_control_run` with a fake daemon and fake guest. |
| `tests/interop/session_local_coverage.py` | gcov line and branch gate for the handler `.c`. |
| `scripts/interop_lane_launch.mk` | Lane targets. |

The handler is compiled separately (`build/session_local_handler.o`) so the harness links the
production object. The test file includes the `.c` so private rules are testable.

## 2. Public API

```c
typedef struct session_local_config_t {
    const char *runtime_dir;   /* in: absolute XDG_RUNTIME_DIR, NUL-terminated, borrowed until destroy */
    const char *device_name;   /* in: device name, 1..63 bytes of [A-Za-z0-9_-], borrowed until destroy */
    uint32_t lock_held_proof;  /* in: nonzero only when this process holds session.lock (set after admission) */
    uint32_t reserved_zero;    /* in: must be zero */
} session_local_config_t;

typedef struct session_local_handler_t session_local_handler_t;  /* complete type in the header */

int  session_local_handler_init(session_local_handler_t *handler, const session_local_config_t *config);
void session_local_handler_table(session_local_handler_t *handler, session_handler_t *out_table);
void session_local_handler_destroy(session_local_handler_t *handler);
void session_local_handler_stats(const session_local_handler_t *handler, session_local_stats_t *out_stats);
```

- `init` performs no I/O and no allocation. The handler struct is caller-owned (about 80 KiB) and
  contains every buffer, the epoll set bookkeeping and the activation store by value. It may be
  heap or static storage. `init` returns SessionLocalSuccess (0), SessionLocalInvalidArgument (1) for NULL arguments, a bad device name, a
  runtime directory that is not absolute, or a socket path of 107 bytes or more.
- `table` fills `out_table` with: `name = "session_local"`, `context = handler`,
  `capabilities = InteropCapLaunch` (no other capability), and the callbacks in section 9.
- `destroy` is safe after `on_closing`, before any run, and twice. It closes any descriptor still
  open and unlinks the socket only if this handler created it and it still exists. It never
  sends frames.
- `stats` copies the counters of section 12 into `out_stats`.

## 3. Ownership and lifetime

- `config` strings are borrowed; the handler copies the derived socket and directory paths.
- The handler owns: the listening socket, the epoll descriptor, every accepted client
  descriptor, the socket file it created, and the outstanding guest request identifiers.
- Each guest request identifier is reserved by the runtime; the handler owns it between a
  successful `interop_host_feature_send_request` and the matching release (section 8).
- No heap memory is allocated by the handler. Every released buffer is caller storage.

## 4. Directory and socket layout (bind time: `on_ready`)

The socket path is `<runtime_dir>/waddle/<device_name>/session.sock`. The bind happens in
`on_ready`, after the peer is Ready and the runtime has admitted the session, which implies
the singleton lock is held. `waddle open` retries the connection while the lock is held and the
socket is missing, so the socket appearing late is expected (IMPL 8.44).

1. For `<runtime_dir>/waddle` and then `<runtime_dir>/waddle/<device_name>`:
   - `lstat`. ENOENT: `mkdir(path, 0700)` then `chmod(path, 0700)`, then `lstat` again.
   - The entry must be a directory and not a symbolic link, owned by `geteuid()`, with mode
     exactly `0700`. Anything else refuses the bind (nothing is unlinked).
   - Decision function: `session_local_directory_decision(is_dir, is_link, owner, mode, uid)`.
2. Stale socket policy for `session.sock`:
   - `lstat` ENOENT: no stale socket.
   - Exists but is not a socket: refuse. The handler never unlinks a non-socket.
   - Exists and is a socket: connect probe with a nonblocking `AF_UNIX` stream connect.
     - Connect succeeds, or fails with EAGAIN or EINPROGRESS: the socket is live. Refuse.
     - ECONNREFUSED: stale. Unlink only when `lock_held_proof != 0`. With proof zero, refuse.
     - Any other errno: refuse.
   - Decision function: `session_local_stale_decision(probe_errno, lock_held_proof)`.
3. Bind: `socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC)`, then `umask(0177)`, `bind`,
   restore the previous umask. The umask is process-wide and the runtime's pump thread is the
   only thread that creates files, so the window is serialized. After bind, `lstat` must show a
   socket with mode `0600`. Otherwise the new socket is unlinked (this handler created it) and
   the bind is refused.
4. `listen(fd, 8)`. `epoll_create1(EPOLL_CLOEXEC)`, then add the listening socket with EPOLLIN.
5. Failures after any step release what was created in this call only, in reverse order.
   `on_ready` returns SessionLocalCallbackFailure (2), which fails the run (`HostControlFeatureFailed`).

## 5. Peer credentials and client slots

- At most four clients (`SessionLocalMaxClients = 4`). A connection accepted when all four slots
  are occupied is closed at once; `refused_capacity` is incremented.
- `SO_PEERCRED` is read on accept. Decision function
  `session_local_peer_decision(peer_uid, expected_uid)`: `Allowed` only when equal. Otherwise the
  connection is closed at once; `refused_peer` is incremented. A failed `getsockopt` closes the
  connection with `refused_peer` incremented.
- Accepted sockets are nonblocking and `SOCK_CLOEXEC`.
- Each client has a serial number (monotonic per accept, never zero). Correlation entries store
  the slot index and serial; a reply goes to a slot only when its serial still matches, so a
  reused slot never receives an older client's reply.
- Accepts stop while stopping (section 10): new connections are closed at once.

## 6. Frame reading (local direction)

- Each client has a 4096-byte receive buffer. Frames larger than 4096 bytes (header included)
  close the client. This is stricter than the codec limit (65536); the open client's frames are
  far smaller.
- The header is decoded by the codec (`interop_header_decode`) inside `session_local_frame_scan`,
  which adds the 4096-byte bound. The scanner lives in C: a Zig module for these few lines could
  not be coverage-gated (its branch edges were not attributed; 75 percent measured), so it was
  folded into the handler. Header and payload validation remain Zig codec functions. Results:
  `NeedMore` (wait for bytes), `Ready` (a complete frame is at the start of the buffer),
  `Malformed` (bad magic, version, length or alignment: close), `TooLarge` (close).
- Before dispatch the payload is validated with `interop_payload_validate(type,
  InteropFromLocal, ...)`. Any nonzero result closes the client.
- Only `LocalLaunch` (0x7F00) and `LocalStop` (0x7F01) are accepted. Any other type closes the
  client, including LaunchResult, Error and every guest-only type.
- `request_id` must be nonzero, or the client is closed.
- Several frames may arrive in one read; they are consumed in order. Consumed bytes are removed
  by moving the remainder to the front of the buffer.
- End of stream (recv returns 0) or a receive error closes the client. Input that stops
  mid-frame is discarded with the client.
- Receive is attempted while the client is not stopping. Once a `LocalStop` is accepted, no
  further input is read from any client (section 10).

## 7. Client replies and transmit buffer

- Each client has an 8192-byte transmit buffer. Replies are appended as complete codec frames
  built with `interop_writer_finish`. A reply that does not fit closes that client (the reply is
  dropped, `replies_dropped` is incremented). This bound is reached only by a client that
  pipelines more than the outstanding limit below.
- Send is nonblocking (`MSG_NOSIGNAL | MSG_DONTWAIT`). A partial send keeps `tx_offset`. The
  epoll set is switched to EPOLLIN|EPOLLOUT while bytes are pending and back to EPOLLIN when
  empty.
- A send error (EPIPE, ECONNRESET, other) closes the client.
- Frame directions used: `LaunchResult` built with `InteropFromGuest` (the codec table allows
  only this direction for it), `Error` built with `InteropFromHost` (allowed for both
  directions), `LocalStopResult` built with `InteropFromLocal`.
- Each client may have at most four outstanding launches (`SessionLocalMaxOutstanding = 4`). A
  fifth `LocalLaunch` is answered immediately with `LaunchResult{Busy}` and is not forwarded.

## 8. Launch forwarding and correlation

### 8.1 Forwarding

On a valid `LocalLaunch` (request id R, client slot C) and when not stopping:

1. Build `LaunchRequest` (type 0x0700, direction `InteropFromHost`, request id 0) with the codec,
   fields in ascending tag order: `app_key` (0x0008) when present, `target_path` (0x002F) when
   present, `arguments` (0x0030) when present, `working_dir` (0x0031) when present,
   `launch_kind` (0x0036, u8, required), `activation_token` (0x0037) when present. Values are
   copied from the validated local frame. `working_dir` is passed through when present, though
   the open client does not send it (IMPL 3.9.3). The scratch frame is 4096 bytes in the handler.
2. `interop_host_feature_send_request(sender, frame, length, &guest_id)`.
3. Result mapping (pure function `session_local_send_outcome`):

| Send result | Client reply (LaunchResult status, or Error) |
|---|---|
| HostFeatureSuccess | none yet; a correlation entry is created |
| QueueFull | LaunchResult, status Busy (5); no retry |
| RequestIdsFull | LaunchResult, status Busy (5) |
| NotNegotiated | LaunchResult, status Unsupported (7) |
| Draining | LaunchResult, status Cancelled (3) |
| any other result | Error, error_code InteropErrorUnexpected (8), message `launch not forwarded` |

Replies from this table carry `process_id = 0`, the echoed local request id, and no message
except the Error message. The codec requires `process_id` in LaunchResult; zero is the documented
"no process" value. A forwarding failure creates no correlation entry and holds no identifier.

### 8.2 Correlation table

- Capacity 32 (`SessionLocalMaxPending = 32`). Key: the guest request identifier. Value: client
  slot, client serial, local request id, the client's activation token (copied, NUL-terminated,
  at most 255 bytes, valid or absent), and `deadline_ms = now + 30000`.
- A full table cannot happen while the runtime's identifier pool has room: the handler refuses
  before sending when 32 entries are live, replying Busy.
- Lookup is a linear scan of 32 entries by guest identifier. Duplicate keys cannot be created:
  the runtime never reissues a live identifier.

### 8.3 Guest LaunchResult (`owns_type` = 0x0701)

`on_frame` receives the admitted frame; `now` is the last `now_ms` the handler saw (section 11).

1. Find the entry by `header.request_id`. None: the frame is ignored and `ignored_results` is
   incremented. A release is not attempted (the identifier is not live from this handler).
2. Validate the payload with `interop_payload_validate(LaunchResult, InteropFromGuest)`.
   Malformed: the callback returns SessionLocalCallbackFailure (run fails, drain). This is a protocol violation.
3. Read `status` (u32) and `process_id` (u32). Read `message` when present.
4. Token policy: when `status == Ok` and the entry holds a token, call
   `interop_activation_put(store, token, length, process_id, last_now)`. The token is the
   client's `activation_token` from the LocalLaunch. A token echoed by the guest in its
   `activation_token` field is never read. Store refusals (Duplicate, TokenSpent,
   SpentRingFull, InvalidProcess when `process_id` is zero, InvalidToken) do not change the
   reply; they increment `token_refused`. Acceptance increments `token_stored`.
5. If the client slot still holds the entry's serial, queue a LaunchResult with the guest status,
   the guest `process_id`, and the guest `message` when present. Otherwise the reply is dropped
   and `replies_dropped` is incremented.
6. `interop_host_feature_release_request(sender, guest_id)`. The return value is ignored (it is
   UnknownRequestId only if the identifier was already released; RequestRetired is benign).
7. Clear the entry.

### 8.4 Timeout

On `on_tick`, an entry whose `deadline_ms <= now` is removed: the client (if still attached)
receives LaunchResult status TimedOut (4), the identifier is released, `timeouts` is
incremented. A guest result that arrives later for that identifier is ignored (section 8.3,
step 1), counted in `ignored_results`.

### 8.5 Duplicate and unknown results

Once an entry is released, its identifier may be reissued by the runtime. A later result with
the same identifier is therefore indistinguishable from an unknown result. Both are ignored and
counted in `ignored_results`. A duplicate is never delivered twice to a client.

### 8.6 Disconnect before the result

A client that disconnects keeps its entries. The result (or the timeout) still releases the
identifier, and the reply is dropped (section 8.3 step 5).

## 9. Callback table and behaviour

| Callback | Behaviour |
|---|---|
| `owns_type` | Nonzero only for 0x0701. |
| `on_ready` | Bind (section 4). Returns 0 or 1. |
| `on_frame` | Section 8.3. Returns 0, or SessionLocalCallbackFailure for a malformed LaunchResult. |
| `poll_fds` | Reports exactly one descriptor: the handler's epoll instance, events POLLIN, `revents` zero, count 1. |
| `on_fds` | Calls `epoll_wait(0)`, accepts, reads and dispatches frames, flushes output, closes failed clients. Returns SessionHandlerRequestStop when a stop flush has completed (section 10); otherwise 0; SessionLocalCallbackFailure on an internal failure. |
| `on_tick` | Enforces the idle timeout (section 13), the launch timeout (section 8.4), the stop flush bound, and flushes pending output. Returns SessionHandlerRequestStop when the stop is ready. |
| `next_deadline_ms` | Earliest of: pending launch deadlines, idle deadlines of clients with no complete frame, the stop flush deadline. UINT64_MAX when none. |
| `on_closing` | Section 12. Exactly once. |

Why one epoll descriptor: `SessionHandlerMaxDescriptors` is 4, but the listener plus four clients
need five. The handler multiplexes them in an epoll set and reports one descriptor. This is a
deviation from the brief's "poll_fds reports the listening socket and client sockets"; it keeps
the four-client limit.

## 10. LocalStop ordering

1. A complete `LocalStop` (no fields, nonzero request id) from client C queues
   `LocalStopResult{status = Ok}` with the same request id, and sets `stopping = 1` with
   `stop_client = (C, serial)` and `stop_deadline = now + 1000 ms`.
2. While stopping: no further frames are read from any client; accepts are closed at once; a
   client's `LocalLaunch` already buffered is not dispatched. Output continues to flush.
3. The stop is ready, and the callback returns `SessionHandlerRequestStop`, when one of:
   a. the stop client's transmit buffer is fully sent;
   b. the stop client is gone (closed or reset);
   c. `now >= stop_deadline` (bounded wait; the reply may be unsent, which the client reports as
      unconfirmed). The bound is 1000 ms.
4. The runtime begins drain only after the callback returns, so drain never starts before the
   reply bytes are written or the client has gone. The reply is never sent by a closing connection
   after cleanup.
5. The handler does not release locks; the runtime's teardown releases `session.lock` and the AV
   lock.
6. Other clients with pending launches receive Cancelled from `on_closing`.

## 11. Clock and deadlines

- Every callback except `on_frame` receives `now_ms` (CLOCK_MONOTONIC). The handler stores it as
  `last_now_ms`. `on_frame` has no clock argument and uses `last_now_ms`; the runtime ticks at
  least every 100 ms, so the staleness is bounded by about 100 ms.
- Launch deadline: `now_ms_at_send + 30000` (LaunchTimeoutMs). Idle deadline:
  `accepted_ms + 10000` (IdleTimeoutMs). Stop bound: 1000 ms (StopFlushMs).
- Unit tests inject `now_ms` directly through the callbacks; no production override exists.

## 12. Teardown (`on_closing`, exactly once)

1. For each correlation entry whose client is still attached: queue LaunchResult status
   Cancelled (3) with process_id 0 and attempt one nonblocking send. Then release the identifier.
   Clear all entries.
2. For each client: attempt one nonblocking flush of pending bytes, then close the descriptor.
3. Close the epoll descriptor and the listening socket.
4. If this handler created the socket file, unlink it. Ignore errors.
5. Set `closed = 1`. Later calls to `on_closing` return without effect.
6. `destroy` repeats steps 2-4 for anything still open (for example a run that never reached
   `on_ready`).

## 13. Idle timeout

A client that has sent no complete frame within `IdleTimeoutMs` of its accept is closed
(`idle_closed` is incremented). After its first complete frame the client may stay connected
until it closes or the handler stops. The handler keeps no other per-client timer.

## 14. Errors and failure modes

| Condition | Effect |
|---|---|
| Invalid config at init | init returns SessionLocalInvalidArgument (or Refused); no state change. |
| Directory unsafe, socket live, stale without proof, non-socket path | on_ready returns SessionLocalCallbackFailure; run fails. |
| Probe or bind unexpected errno | on_ready returns SessionLocalCallbackFailure. |
| Epoll or listen failure | on_ready returns SessionLocalCallbackFailure after cleanup. |
| Client malformed frame, wrong type, zero request id, oversize | client closed; no reply. |
| Client send error or reset | client closed. |
| Transmit buffer overflow | client closed; reply dropped. |
| Guest LaunchResult malformed | on_frame returns SessionLocalCallbackFailure; run fails. |
| Guest LaunchResult unknown or duplicate | ignored; counted. |
| Activation store refusal | counted; reply unchanged. |
| Epoll wait error other than EINTR | on_fds returns SessionLocalCallbackFailure. |
| Accept error other than EAGAIN/EINTR/ECONNABORTED | counted as `accept_errors`; the listener stays armed. |

## 15. Threading

All handler calls run on the runtime's pump thread (`session_handler.h`). No internal locks.
Signals are not handled by the handler; the umask change is the only process-wide state and is
restored before `on_ready` returns.

## 16. Stats

`session_local_stats_t` holds these counters (unsigned 64-bit, saturating not required):
`accepted`, `refused_capacity`, `refused_peer`, `closed_malformed`, `closed_unknown_type`,
`closed_oversize`, `closed_zero_request`, `closed_io`, `idle_closed`, `launches_forwarded`,
`launches_busy`, `launches_unsupported`, `launches_cancelled`, `launch_errors`, `results_delivered`,
`ignored_results`, `timeouts`, `replies_dropped`, `token_stored`, `token_refused`,
`stop_requests`, `stop_deadline_expired`, `accept_errors`.

## 17. Verification

Run from the worktree root with `-j4` at most.

- `make interop-session-local-test`: unit rules (decisions, scanner, correlation, token policy,
  send mapping, reply directions, stop readiness, timeouts, teardown), real AF_UNIX tests in a
  temporary `XDG_RUNTIME_DIR` (stale socket with and without proof, live rival refused,
  unsafe directories and links, non-socket path, four-client limit with a fifth refused, idle
  limit, malformed, oversize, unknown-type and zero-identifier frames, split and pipelined frames,
  LocalStop ordering, launch correlation with timeout, duplicate and unknown results, disconnect
  mid-launch), fault injection through linker wrappers (every on_ready step, accept and peer
  errors, receive and send EAGAIN/EINTR/error, epoll failures, scripted epoll events), and a
  descriptor count before and after the run.
- `make interop-session-local-sanitizers`: the same suite under
  `-fsanitize=address,leak,undefined` with `detect_leaks=1`.
- `make interop-session-local-coverage`: `tests/interop/session_local_coverage.py`, a gcov
  line and taken-branch gate at 90 percent for `src/interop/session_local_handler.c`.
- `make interop-session-local-e2e-test`: `build/interop_session_local_harness` serves
  `session.sock` in a temporary runtime directory while `tests/integration/open_e2e.py` runs the
  built `build/waddle` against it (harness mode, 11 cases).

## 18. Open decisions for the lead

1. Composition (outside this lane): the runtime must admit the session, set `lock_held_proof`
   to 1 only after `interop_linux_session_acquire` succeeded for the device (0 when it failed or
   was not attempted), set it before `session_local_handler_init` (the value is copied once), keep
   the owner alive until the handler's `on_closing` has run, and never release the owner before
   that. The handler never acquires, releases, or inspects `session.lock`. The exact text is in the
   `session_local_config_t` comment in `src/interop/session_local_handler.h`. The runtime must also
   build `interop_host_feature_ops_t` from `session_handler_t` (the
   harness contains a reference adapter), and handle `SessionHandlerRequestStop` as a graceful
   stop. `session_control_host.c` has no stop path yet: callbacks can only return zero or nonzero
   failure. The harness uses `pthread_kill(pthread_self(), SIGTERM)` on the pump thread, which the
   signalfd reads as a stop, and the runtime then ends with HostControlCancelled.
2. Local frame bound: 4096 bytes (section 6) instead of the codec's 65536.
3. Guest-supplied `message` text is copied into the client's LaunchResult unchanged. The open
   client prints it to a terminal, so terminal control sequences are not filtered by this lane.
4. The epoll descriptor (section 9) is the only descriptor reported by `poll_fds`.
5. Callback failures return `SessionLocalCallbackFailure` (2). The graceful stop value 1 is
   reserved by `session_handler.h`; a static assertion keeps the two distinct.

## 19. Integration (tranche 3)

Composition. The handler is registered by the builtin registry (`session_handlers_builtin.c`) in the supervised
binary (`waddle-session --device NAME`, the default mode). Open decision 18.1 is resolved as follows:

- The runtime builds `interop_host_feature_ops_t` from `session_handler_t` (session_app). The test-only adapter
  and its `pthread_kill` stand-in are removed; the harness drives the real `waddle_session_main`.
- `lock_held_proof` is set to 1 only at on_ready, which runs after admission has acquired `session.lock`. The
  owner stays alive until the handler's on_closing has run, because the runtime releases owners only after every
  on_closing. The handler is initialised with the proof already set, so its stale-socket decision is correct.
- A graceful stop (SessionHandlerRequestStop from LocalStop) ends the run as HostControlCancelled, which the
  supervised command reports as exit 0 after the owners are released.

Directory. The handler's runtime directory is `$XDG_RUNTIME_DIR`. The supervised command refuses to start unless
the daemon socket is `<runtime>/waddle/<device>/daemon.sock`, the layout the client (`waddle open`) uses to derive
session.sock. session.sock is `<runtime>/waddle/<device>/session.sock`.

Pending on the launch lane (not implemented here): LocalStopResult is still queued to the client before drain.
IMPL 3.12.5 requires the reply after drain and lock release. session_main will call
`session_local_handler_send_stop_reply()` once the launch lane lands it, after `session_app_run` returns and
the owners are released. Until then the reply is sent before the drain begins, as section 10 describes.

Re-arming. The supervisor reuses one handler table across attempts. The builtin wrapper re-initialises the handler
on each Ready, and the harness case `two_consecutive_sessions` runs two guest sessions in a row through the real
command path.

Verification (harness and e2e). `tests/integration/open_e2e.py` in harness mode runs 15 cases: the 12 original ones,
plus `local_stop` (LocalStop reply, exit 0, socket removed), `reconnect_after_guest_drop` (the first guest drops after
HelloAck, the supervisor reconnects, and a later open is served), and `two_consecutive_sessions` (each of two guest
sessions serves one open).

