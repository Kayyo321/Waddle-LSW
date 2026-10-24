# Read-only review: contextual peer ABI

**Result: no actionable contract defect found** in `src/interop/control_peer_abi.zig` against `include/waddle/interop_peer.h` and IMPL_DESC §8.21.1.

- Every stateful export validates raw owner/output/input ranges and aliasing before dereference or mutation. Token and time preflights precede role/phase/front-queue checks in the prescribed order. `mapping_result` keeps the Guest layout argument check before token/time, then checks phase, false initialization, and zero epoch in the specified order.
- `ack_tx` checks live/token/time, rejects Closed, peeks the actual front frame, distinguishes Empty and QueueClosed, validates `written <= front.len`, then calls the helper. It does not modify the output before success. If a later helper/liveness check fails after the actual ACK, the queue advancement remains (required to account OS-accepted bytes), the event output stays unchanged, and InternalInvariant requires abort/quiescence rather than retrying the send.
- Helper errors are normalized to InternalInvariant after public preflight; the only ordinary post-preflight conversion is `next_event`'s OutputTooSmall. `copy_tx` and `tx_bytes` normalize unexpected peek failures. Host mapping evidence is forwarded; Guest false initialization maps to MappingFailed before the core call.
- Output copies happen after successful helper completion. The event mapper writes reserved/unused fields as zero. Short output is rejected before bytes, queue, reader, clock, or event output are changed by the core's retry-aware path.
- Scalar-only test probes restore every `peer.live = false` and `queue.live = false` mutation before deferred `destroy_owner`; the lifecycle mutations checked in this review do not leave allocator-owned pointers/counts corrupted. The tests that alter reader failure/closed flags restore them before deinit.

This is a source/contract review only. I did not build or run the wrapper, C fixture, coverage, sanitizers, or native Windows tests. Those remain independent qualification gates.
