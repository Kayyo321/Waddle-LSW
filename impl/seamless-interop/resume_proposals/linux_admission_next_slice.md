Status: advisory investigation only; no implementation or completion credit.

# Bounded Linux admission/singleton slice for seamless interop (#7 10%)

## Current seams and gaps

- Device selection already has the needed source of identity/configuration:
  `resolve_target_device()` in `src/cli/host.c:127-180` resolves the named or
  saved-default profile through `daemon_device_default_get()`, `daemon_device_list()`
  and copies `device_info_t`. The profile carries its `vsock_cid`, `vsock_port`,
  `config_path`, `state_dir` and per-device daemon socket. `daemon_device_get_state_dir()`
  and `daemon_device_get_socket_path()` in `src/daemon/daemon_device.c:93-135`
  construct profile-scoped locations.
- The daemon already owns VM start and guest-exec reachability. In
  `daemon_state_start_subsystem()` (`src/daemon/daemon_state.c:416-675`),
  `DaemonStartFlagWaitGuest` probes `(config.vsock_cid, config.vsock_port)` and only
  then publishes `SubsystemStateRunning`. `daemon_state_get_status()` reports the
  configured CID/port. This probe is the existing arbitrary-command/guest-exec
  service, normally port 5242, not interop readiness.
- The frozen daemon extension in IMPL_DESC §3.4 (`:766-791`) specifies additive
  GuestReady request/response types 0x10/0x11 and a bounded per-client wait in the
  daemon poll loop. It explicitly says status 0 means guest-exec reachability only;
  it grants no interop or mapping authority. The current `waddle_daemon_msg_type_t`
  in `include/waddle/daemon_protocol.h:66-103` has no GuestReady types, and
  `daemon_server.c:40-168` has no such dispatcher case. The daemon accepts clients
  nonblocking and services them in its poll loop (`daemon_server.c:183 onward`),
  so add the probe there without a worker thread or unbounded blocking connect.
- The CLI's `connect_peer()` at `src/cli/host.c:198-282` is useful as a behavioral
  reference: nonblocking `SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC`, `EINPROGRESS`,
  `poll(POLLOUT)`, then `SO_ERROR`, with fd closure on every failed attempt. It is
  `static` and serves `cmd_exec()` (`:287 onward`) for guest command/stdin/stdout
  transport; it should not be reused as the interop protocol owner or put the
  seamless loop in `src/cli/session.c`. That file's `waddle_session()` is the
  unrelated guest-exec byte-stream multiplexer (`src/cli/session.c:26`).
- The connected protocol peer and Linux socket owner now exist:
  `interop_peer_create()` / `interop_peer_destroy()` in
  `src/interop/control_peer_abi.zig:189-223`; and
  `interop_linux_socket_create()` in `src/interop/linux_socket_owner.c:462-524`.
  The latter accepts only an already-connected stream fd with nonblocking and
  CLOEXEC flags and transfers fd ownership only on success. The owner handles
  one serialized peer, bounded poll/drive, mapping callback, drain, and close;
  it does not connect, listen, select a device, authorize a manifest, map IVSHMEM,
  or supply release proof. The ABI header documents token, fd and mapping limits
  in `include/waddle/interop_linux_socket.h`.
- There is no `waddle-session` host runtime or session socket implementation in
  `src/`; `src/cli/session.c` is the unrelated command stream. IMPL_DESC §4.2
  (`:1720-1732`) specifies `session.lock`, `session.sock`, GuestReady and VSOCK
  port 5002 in order. §3.12.5 (`:1496-1520`) specifies LocalStop and same-user
  credential enforcement, while its final line says runtime dispatch/credentials
  remain TODO #7. Thus admission must be a new host-side executable/library seam,
  not a claim that existing daemon/CLI code already supplies the seamless loop.
- Singleton primitives exist only for adjacent concerns. The daemon owns
  `$XDG_RUNTIME_DIR/waddle/<device>/waddle.lock` and its daemon/QMP/virtiofsd
  sockets. AV CLI commands acquire `<state>/av/command.lock` using `open(O_NOFOLLOW,
  0600)`, `fstat` type/owner/mode checks and `flock(LOCK_EX|LOCK_NB)` in
  `src/cli/av_managed.inc:376-390`. IMPL_DESC §6.10 (`:2002-2007`) says
  `waddle-session` must take that same AV command lock for its entire ownership
  interval. The separate per-session `session.lock` from §4.2 does not exist in
  code. Do not treat the daemon lock or AV lock as the session singleton.
- `src/interop/manifest.zig` is the schema parser only (IMPL_DESC §8.3,
  `:2183-2204`); it does not do filesystem policy, transactions, guest policy,
  or establish that an interop connection is authorized. Session startup's
  manifest recovery/migration and later integration transactions are larger,
  separate work. Parser success must not be used as a connection credential.

## Minimal implementation boundary

Add a dedicated host admission owner, preferably `src/interop/linux_session_owner.c`
and `.h`, plus a small `src/interop/waddle_session_main.c` only if the current
build has no executable entry point for `waddle-session`. Keep `src/cli/host.c`
responsible for the `waddle open`/session-start command routing and selected
profile handoff; do not expand `cmd_exec()` or its legacy guest-exec protocol.
Use a new per-device `session.lock` lease helper in the owner rather than
overloading daemon `waddle.lock` or `command.lock` semantics.

The owner should expose a small staged API, with exact names to be settled by the
lead before implementation:

1. `session_owner_acquire(device_info, out_owner)` resolves only the existing
   registered profile. It creates/opens `$XDG_RUNTIME_DIR/waddle/<device>/session.lock`
   with no-follow semantics, validates regular file, same uid and private mode,
   then takes a nonblocking exclusive flock. It next obtains the existing
   `<state>/av/command.lock` under the documented lock order: session lock, AV
   command lock, then (only for separate installer operations) manifest lock.
   Hold both lease fds until session teardown has quiesced all callbacks and
   relinquished safe-to-release resources. An occupied/invalid lock fails closed;
   never delete a lock file, unlink an active socket, or signal a PID to recover.
2. `session_owner_wait_guest_exec(...)` sends the new GuestReady request to the
   selected device daemon, using the §3.4 timeout (session startup asks for 120 s).
   This confirms only the legacy guest-exec endpoint on configured `vsock_port`.
   Enforce `vsock_port != InteropControlPort(5002)` locally before daemon ensure
   or any start request, as required by §4.2/§3.4; don't silently substitute
   5242 or let an arbitrary guest-exec request collide with interop. A timeout,
   malformed response or daemon disconnect reports failure without initiating
   interop mapping or changing epoch state.
3. `session_owner_connect_control(...)` uses registered `vsock_cid` and reserved
   `InteropControlPort=5002`, independent of `vsock_port`. Use a nonblocking
   CLOEXEC AF_VSOCK SOCK_STREAM connector with 2-second per-attempt deadline,
   SO_ERROR verification and close-on-failure; retry 250 ms to 4 s only while
   daemon Status says Running (IMPL_DESC §3.2.7 and §4.2). Do not use a grace
   period to infer whether a connection is a probe. A successful connector
   transfers its connected fd to `interop_linux_socket_create()` with Host role,
   fresh nonzero token, monotonic acceptance time, bounded version/capabilities,
   and finite drain interval. Failed create leaves connector responsible for
   closing the still-owned fd; successful create transfers sole I/O ownership
   to the socket owner. The connection token is a stale-callback generation,
   not an authentication secret.
4. The host loop calls the existing socket owner's `poll_events` / `drive`, then
   routes exactly one copied peer event at a time. On `ValidateMapping`, the
   external host mapper opens configured `[av] shm_path`, validates the actual
   mapping length and layout-v3 header/epoch with the existing mapping/layout
   APIs, then calls `mapping_result` with the observed success, epoch and layout.
   Mapping success is the required HelloAck gate. The socket owner never maps or
   claims IVSHMEM authority. Only negotiated capability bits may be handed to
   future feature dispatch, and every copied feature frame still requires
   operation-specific authorization before side effects.
5. A separate future local-session listener owns `session.sock`; this bounded
   admission slice can reserve the startup binding and same-user policy, but
   should not implement all LocalLaunch/LocalStop routing unless the lead expands
   the slice. The eventual listener must use a private per-device runtime path,
   mode 0600/0700 as appropriate, verify `SO_PEERCRED` uid against the session
   uid before decoding, enforce `InteropFromLocal` codec direction and max 16
   clients (§5.1), and echo request IDs. It must not trust filesystem mode alone
   as peer identity. `waddle open` is the client; LocalStop is a local-only frame
   and must never be accepted on AF_VSOCK.

## Authority and teardown invariants

- Device registry/config is the source of `(device name, CID, guest-exec port,
  state path)`; the default profile is resolved by the existing selection API.
  Local UID/peer credentials authorize local commands only. `session.lock` and
  AV command lock establish exclusive per-user ownership, not guest trust.
- GuestReady success is not Hello, negotiated Ready, a mapping authorization,
  or a new epoch. An accepted socket/connection token is not proof that the guest
  initialized a mapping. Guest owns the fresh nonzero mapping epoch and must
  initialize its memory and stop/join old producers before sending Hello; host
  validates observed mapping facts before permitting HelloAck (§8.1).
- The guest's one-connection rule is protocol-level: reject a second connection
  while Ready with the specified busy ProtocolViolation; it must not initialize
  shared memory for a rejected peer. Host-side session lock prevents duplicate
  local host instances but cannot replace this guest-side gate.
- On pre-connect failures, close any untransferred fd and release acquired locks
  in reverse order. If connection/handshake or mapping has begun, use the peer's
  drain/abort state and existing native ownership rules. Never increment/reuse an
  epoch because a socket closed, a daemon reports Running/Stopped, or LocalStop
  returned. Retain uncertain mapping/buffer ownership; keep the AV command lease
  until all native users are actually quiescent and the mapping owner permits
  unmap. No automatic reconnect or epoch reset is part of this slice.
- VersionMismatch may trigger the one documented deployment retry only through
  the installation/manifest transaction path after the current connection is
  closed; do not make deployment or manifest parser state part of socket
  authentication. Failed deployment leaves repairable integration state.

## Test plan and scope accounting

- Add isolated unit tests for session lock creation/validation, duplicate lock,
  wrong owner/mode/type, symlink rejection, lock ordering and cleanup on every
  acquisition/connect failure. Test that no socket unlink/PID signal occurs on
  a held or unverifiable lock.
- Add daemon protocol/client tests for GuestReady request shape, correlated
  response/status/CID, per-client deadline, timeout/disconnect, and proof that
  success only indicates the configured legacy guest-exec port. Inject a probe
  backend in tests; do not depend on an actual VSOCK service.
- Add connector tests with wrapped socket/connect/poll/getsockopt/clock calls for
  immediate connect, EINPROGRESS+success, timeout, SO_ERROR failure, retry
  backoff, Running-to-not-Running stop, and fd close/ownership transfer. Keep
  socketpair tests of the existing byte owner separate: they establish stream
  behavior, not AF_VSOCK admission.
- Test port 5002 rejection before daemon ensure/start using call counters. Test
  failed peer creation leaves fd with connector, successful creation transfers
  it once, second singleton returns already-running without side effects, and
  session/AV leases remain held throughout a simulated Draining state.
- Test local listener credentials and direction separately when implemented:
  same-uid accepted; different-uid/credential-query failure rejected before
  frame parse; LocalLaunch/LocalStop accepted only with local direction; >16
  clients rejected. These tests must use wrappers or local sockets and never
  real foreign credentials/VMs.
- This is only the #7 connection/VSOCK/singleton 10% allocation. Existing
  connected-byte-owner 5% is separate. The daemon/local-control 10% remains
  incomplete unless GuestReady dispatch/client, session.sock dispatch and
  credential checks are implemented and qualified. Full host event loop,
  Wayland/IVSHMEM mapping implementation, protocol feature dispatch, manifest
  transactions, guest single-connection admission, native VM and compositor
  acceptance remain outside this bounded slice.
