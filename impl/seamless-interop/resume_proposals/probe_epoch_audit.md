# Seamless readiness-probe and session-epoch audit

## Findings

The specification gives the same VSOCK endpoint two incompatible roles. The
host endpoint flow in §3.2.7 connects to guest port 5002 and waits for the
guest's first `Hello` (§3.2.7 steps 1–2). `GuestReady` is also specified to
connect to port 5002 and immediately close (§3.4, lines 782–785), with §6.1
calling close-before-`Hello` normal. In §8.14, however, the guest must complete
`mapping_initialized` and publish the fresh mapping before it can send `Hello`
(lines 2685–2690). On accept, the guest cannot tell whether the connecting host
will send `Hello` (it never sends first) or is the readiness probe: both are
silent until the peer closes. If the guest initializes on accept, the probe can
initialize shared memory and consume a fresh epoch. If it waits for EOF, it also
waits for a real host to close and can never start the handshake. A delay or
grace period only makes this a timing heuristic, not a reliable discriminator.

There is also a source/spec port mismatch. The current startup poll in
`src/daemon/daemon_state.c:652-663` connects to `s->config.vsock_port` and closes
on success. Its default is `WaddleDefaultVsockPortVal` at
`src/daemon/daemon_config.zig:159-163` (5242 in the current tests), while the
new interop endpoint is frozen as port 5002. Thus this code currently probes the
legacy guest service by default; it only collides with the new endpoint if
configuration changes that port. The documented §3.4 `GuestReady` implementation
is not present in the current `src/daemon` tree. Do not silently treat the
existing configurable-port readiness poll as proof that the 5002 session agent
is ready.

The epoch scope is contradictory too. The public tag table calls
`session_epoch` a random nonzero value “per agent start” (§3.1, line 395), but
the agent is a long-lived logon process that listens across host reconnects
(§2.2); §8.1 requires a fresh epoch before `Hello` on each accepted session
connection (lines 2075–2082); §8.11 calls initialization's epoch fresh and
caller-guaranteed fresh (lines 2515–2531); and §8.14 requires a fresh epoch at
`mapping_initialized` and a new connection instance/token (lines 2647–2651,
2685–2690). Reusing one process-start epoch for later reconnects would violate
the explicit no-reinterpretation rule. “Per agent start” should not be used as
the runtime rule.

## Minimal safe choice

Keep the wire protocol and detach proof unchanged. Do not make a readiness-only
connect to port 5002. `GuestReady` should report VM/legacy-service readiness
from its existing distinct endpoint (the current configurable port, normally
5242), or be limited to QEMU/daemon readiness; it must not claim that the new
session agent completed negotiation. The host session owner then makes the one
real connection to 5002 and waits for the actual `Hello`/`HelloAck` handshake.
The handshake itself, rather than a connect-only probe, is the readiness test
for seamless sessions. If `GuestReady` must report new-agent readiness, change
its implementation to observe/forward an actual owned session handshake rather
than opening and closing a probe; do not consume a mapping epoch for a probe.

The guest adapter must not call `mapping_initialized` merely because `accept`
succeeded. It calls it only for the host-owned real session after it has
exclusive local admission and the existing explicit producer-stop/host-buffer-
detach proof. A probe that closes before `Hello` remains harmless only if it
caused no mapping initialization. Connection close, EOF, `Closed`, and timeout
remain insufficient detach evidence as §8.14 already states. No finite delay
can resolve the protocol ambiguity, so do not specify a grace timer as the
distinguishing rule.

Clarify the epoch table entry as “fresh random nonzero `u64` for each successful
mapping/session initialization before `Hello`, not per process start; do not
reuse an epoch within one agent lifetime.” Generate/validate it at the serialized
mapping initialization boundary after the external detach proof. Failed probes
do not allocate epochs. This changes documentation semantics only; it adds no
wire field and makes no new ownership proof. The random generator and policy
for collision avoidance across agent restarts should be specified by the
implementation owner; the existing detach proof remains the authority that
allows reuse of shared memory, not epoch uniqueness alone.

## Decision needed before adapter integration

Choose whether daemon `GuestReady` means legacy VSOCK service reachable or only
VM running. In either case, reserve port 5002 for the real host-owned protocol
connection and make the separate handshake the seamless-agent readiness gate.
Also update the tag-table epoch wording to per mapping/session initialization.
