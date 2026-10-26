# `waddle session stop` client contract

Implements the client half of TODO #7 (local command control, 5 percent) for IMPL_DESC 3.12.5.
Frozen grammar and wire rules are in IMPL_DESC 3.12.5 and 3.2; this file records the decisions the
text leaves open and the code that enforces them.

## 1. Command

    waddle session stop [--device|-d NAME]

- `session` must be followed by `stop`. Any other first word, a missing or repeated device option,
  an empty device name, or any other argument is a usage error: one usage line on stderr, exit 2.
- The device is resolved with `resolve_target_device` in `src/cli/host.c`, the resolver `waddle open`
  uses. Unknown or ambiguous devices print the resolver's own diagnostic and exit with its code, which
  is the same code `waddle open -d NAME` returns for that device.
- The dispatch is one branch in `src/cli/host.c`: `session` calls `waddle_session_command`
  (`src/cli/session_stop_command.h`, implemented in `src/interop/session_stop_command.zig`).

## 2. Outcomes and exit codes

| Condition | Exit | stderr |
|---|---|---|
| LocalStopResult with status Ok | 0 | `waddle session stop: session stopped` |
| No socket accepts a connection and session.lock is unlocked (or absent) | 0 | `... no session is running for NAME (already stopped)` |
| LocalStopResult with a non-Ok status | 1 | `... the session did not stop: MESSAGE` or `status N` |
| Error frame carrying this request id | 1 | `... the session refused the stop: MESSAGE` |
| Session lock held, no connectable socket | 1 | `...; the stop is unconfirmed` |
| session.lock cannot be inspected safely | 1 | unconfirmed |
| Runtime directory not private to this user | 1 | unconfirmed |
| Connect failure other than missing or refused | 1 | unconfirmed |
| Send failure | 1 | unconfirmed |
| EOF, or EOF inside the reply, before a complete reply | 1 | `the session closed without a result` |
| No complete reply within 5000 ms | 1 | `no result within 5 seconds` |
| Reply echoes another request id, another frame type, or a bad frame | 1 | unconfirmed |
| Usage error | 2 | `usage: waddle session stop [--device|-d NAME]` |

## 3. Decisions the frozen text does not state

1. **Stale socket.** `ECONNREFUSED` means a socket file exists but no process listens on it. It is
   handled like a missing socket: session.lock decides. An unlocked lock is "already stopped". The
   socket file is left in place; the stop client never unlinks it. The session's own stale-socket
   rule (`session_local_stale_decision`) removes it on the next start.
2. **When the lock is probed.** session.lock is probed only when no session accepted a connection
   (the socket is missing or refused). It is never probed while a live session is connected.
3. **How the lock is probed.** `lock_state` in `src/interop/local_session_client.zig` opens session.lock
   read-only with `O_NOFOLLOW` and takes a shared, nonblocking `flock`, then releases it before
   returning. A symbolic link, a non-regular file, or a file owned by another user is `unsafe`.
   The probe holds the lock for the duration of one syscall. A session starting at the same moment
   may see its exclusive acquire fail with `Busy` for that instant; the stop then reports what it
   saw at the probe. Nothing is ever acquired and kept, and nothing is unlinked or signalled.
4. **Peer credentials.** After connect the client checks `SO_PEERCRED`: the peer's effective uid must
   equal the client's effective uid. A mismatch is a connect failure (exit 1 unconfirmed).
5. **Request id.** The LocalStop frame uses request id 1 (`StopRequestId`). The protocol requires a
   nonzero id, and each stop uses a fresh connection, so one fixed id is sufficient.
6. **Reply validation.** A reply with a different request id is a protocol violation: the client
   stops reading at once and exits 1 (it does not skip frames until the deadline). The reply payload is
   checked with `interop_payload_validate` (direction `InteropFromLocal`); a missing status is
   rejected. Payloads above 4096 bytes are refused without being read.
7. **Deadline.** The 5 s budget starts after the send. Every read re-checks the deadline, so a reply
   that trickles in cannot extend the wait.
8. **Connection lifetime.** The client closes its descriptor after one request. It never sends a
   second frame.

## 4. Session side (interaction)

The session handler (`src/interop/session_local_handler.c`) answers LocalStop with LocalStopResult
(status Ok, the request id echoed) and then requests a graceful stop. The reply is sent before the
runtime tears down, so the client normally sees Ok. The post-cleanup ordering required by IMPL 3.12.5
(reply after draining and releasing both locks) is the session lane's work and is tracked there; until
it lands, a client that reads the reply can still observe the session's own shutdown afterwards.
`waddle stop` is unchanged and still shuts down the whole device; session cleanup follows its disconnect.

## 5. Code and tests

- `src/interop/local_session_client.zig`: directory classification, connect with peer check, lock probe,
  deadline-aware reads and writes, diagnostic builder. Shared with `waddle open`.
- `src/interop/session_stop_command.zig`: argument parsing, decision logic, reply handling, production hooks.
- Unit tests: `make interop-session-stop-test` (fake clock, fake socket, scripted replies, production hooks
  on real files and sockets). Coverage: `make interop-session-stop-coverage` (gate 90 percent line and
  branch); `make interop-session-shared-coverage` for the shared module.
- End-to-end: `make interop-session-stop-e2e-test` runs `tests/integration/session_stop_e2e.py` against
  the fake server (every abnormal reply) and, with the session command harness, against a live session
  (graceful stop, socket removed, lock released, second stop reports already stopped).
