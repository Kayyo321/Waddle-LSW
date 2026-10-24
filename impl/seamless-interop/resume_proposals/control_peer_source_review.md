# Control-peer source review against §8.20

Read-only review of `src/interop/control_peer.zig` against the frozen §8.20
contract and terminal-TX corrections in `impl/seamless-interop/IMPL_DESC.md`.
No files were changed; no tests or builds were run. Existing tests were not used
as proof for the observations below.

## Findings

1. **Header-error terminal-send failure is not retained by the peer.**
   `control_peer.zig:292–296` stores the result of `enqueue_error` only in the
   returned `feed_result_t.terminal_send_failed`. It does not assign
   `self.terminal_send_failed`, while the public `terminal_send_unavailable()`
   at lines 639–642 reports that persistent member. After a malformed header
   whose best-effort Error cannot be queued (for example, allocator failure),
   the immediate feed result reports failure but the peer accessor reports
   success/no failure. Persist the flag on this path as well.

2. **An invalid Goodbye can be blocked by an irrelevant short output buffer.**
   `control_peer.zig:327–331` marks every codec-valid Goodbye as requiring
   copy-out capacity. Later, `dispatch_ready` at lines 926–933 treats
   `Goodbye` with nonzero `request_id` as a protocol violation, queues a
   ProtocolViolation Error, and does not copy the frame. Thus a short output
   buffer returns `OutputTooSmall` before the violation can be handled, even
   though this invalid Goodbye does not require output space. Determine the
   Goodbye request-ID validity before copy-capacity preflight; preserve the
   unchanged-output guarantee for valid Goodbye frames that really are copied.

## Paths checked

The reviewed ACK path accounts valid OS-confirmed bytes and completion metadata
before deadline/terminal returns; it suppresses handshake hooks after the
terminal latch and expires at the exact deadline. `enter_draining_at` records
unfinished Hello/Ack work, `peek_tx` stops offering bytes when abort is required,
and late ACK callbacks can still account bytes already accepted by the OS.
Hello/Ack completion is the only path to a Ready transition, while later Ping,
Pong, and timer events do not repeat Ready. Guest Hello IDs are staged before
commit, released on Guest Ready or transport quiescence, and host Ping IDs share
the same namespace as liveness records. Queue allocation remains owned by the
queue until full acknowledgement or deinit. Output capacity and alias checks
precede frame dispatch mutations, and valid received Error frames do not trigger
recursive Error responses.

Limits: this was source inspection only. No runtime, allocation-failure, native
transport, or independent test-suite execution was performed.
