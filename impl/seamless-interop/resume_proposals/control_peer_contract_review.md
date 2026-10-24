# §8.20 control-peer contract review

Review type: read-only contract/source consistency review. No repository files,
builds, or tests were changed/run. Reviewed `impl/seamless-interop/IMPL_DESC.md`
§8.20 (including its normative corrections at lines 3114–3139), helper behavior
in `session_handshake.zig` and `stream_send_queue.zig`, and the currently present
`control_peer.zig` implementation.

The normative additions settle the earlier contract questions around OS-confirmed
ACK accounting, output-capacity preflight, callback role/phase preflight, and
untimed terminal transitions. The implementation still has three material
contract violations that should block acceptance:

1. **Draining send acknowledgements can strand bytes already sent.** In
   `src/interop/control_peer.zig:427-433`, a completed Hello/HelloAck always calls
   the handshake helper before consuming queue bytes. If `begin_drain` already
   moved the peer to Draining, `hello_sent`/`ack_sent` returns `WrongPhase`; the
   OS write has already happened but the queue and completion ring remain
   unchanged. This contradicts §8.20 lines 3127–3131, which allow existing TX to
   flush while Draining, and lines 3117–3123, which require valid ACKs to account
   bytes already sent. Account bytes and pop metadata, but suppress readiness
   hooks when negotiation is terminal.

2. **ACK callbacks do not enforce deadlines consistently.** At
   `control_peer.zig:427-448`, deadline handling is reached only for a *completed*
   Hello or HelloAck. A partial write, or a completed ordinary control frame,
   advances `last_now_ms` without expiring a handshake/Ready timeout. For a
   partial Hello/Ack, the helper-mutated `next_handshake` is also not committed.
   The contract requires expiration before every frame/completion at the exact
   deadline (§8.20 lines 3061–3065), while still consuming valid OS-reported
   bytes (§8.20 lines 3117–3123). Every valid ACK needs a deadline decision;
   timeout must enter Draining and suppress Hello/Ack readiness without
   re-exposing acknowledged bytes.

3. **Transport-loss API and phase source do not implement the frozen terminal
   latch.** §8.20 fixes `transport_lost(token)` as untimed and says an
   authoritative peer terminal phase latch makes it Closed (§8.20 lines
   3127–3131). The current source instead declares
   `transport_lost(self, connection_token, now_ms)` at `control_peer.zig:527`,
   has no separate latch, and derives `phase()` from `handshake.phase` at
   `control_peer.zig:569–570`. The helper's timed `transport_lost` can return
   Drain at the exact handshake deadline before teardown completes, so transport
   loss cannot reliably produce the required Closed state. Make terminal state
   authoritative and untimed as specified; do not consult helper advancement
   after transport loss.

No additional contract blocker was found in the received-Error rule or fixed
completion-ring design: the spec clearly forbids recursive Error responses, and
metadata can remain synchronized if insertion follows successful enqueue and
removal follows full queue-frame acknowledgement.
