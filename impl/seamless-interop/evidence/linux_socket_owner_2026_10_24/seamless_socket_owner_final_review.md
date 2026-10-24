# Final read-only review: Linux socket owner

Reviewed `src/interop/linux_socket_owner.c` (SHA-256 `46a66725ca8d08870161f6290c761df1defe6b0c4f3b5de420cd61d3e066ac54`), `include/waddle/interop_linux_socket.h`, the corresponding `linux_socket_owner.md` / IMPL_DESC §8.23 contract, and the main, guard, and private fixtures.

## Findings

No actionable correctness defect found in the requested areas.

The reviewed paths stage caller outputs until preflight succeeds; consume poll cookies and commit caller time only after the corresponding local checks; count positive `send` bytes before ACK so an ACK invariant failure cannot cause a resend; reserve helper steps for ACK and post-ACK phase observation before sending; and bound RX/TX/syscall and peer-helper work. Unexpected peer results and unrecognized phases fail closed. Close invalidates fd ownership before the single Linux `close` attempt, records the first close diagnostic, and does not retry `close` after `EINTR`. Peer notification/destruction failures retain the allocated owner and preserve the caller slot for retry. The closed snapshot behavior matches the contract: later snapshots report `WorkTransportClosed` with the stored terminal scalar diagnostic, while the original failing drive can report `WorkPeerFailure`.

## Limits

This was source/spec inspection only. I did not run tests, build, or native workloads. The review does not establish AF_VSOCK behavior, daemon integration, production scheduling discipline, or peer/native mapping teardown safety. The source hash was the supplied review identity at inspection time; it was not edited.

## Follow-up: repeated abort on an already closed owner

Reviewed the follow-up implementation and the `0x212` regression. `abort` now returns the same initialized closed snapshot as `get_closed_snapshot` when called on an already-closed owner, after normal output/thread/token preflight. The branch performs no close or peer notification; it still invalidates any cookie as documented for successful abort. The regression verifies byte-for-byte snapshot equality, no repeat close or transport-lost call, and that `destroy` alone retries a failed peer notification while preserving the owner slot on failure. No issue found in this change.

Updated source SHA-256: `2d041eb6d8557fe79201231fca926022ab073d91fafb2489e213317a0574d808`. This follow-up was reviewed read-only; no tests/builds were run.

## Final follow-up: create overlap ordering and close accounting

Read-only review of the final frozen source and main/guard test changes found the create preflight now rejects overlapping config/output ranges before dereferencing `*out_owner`; short-circuit evaluation preserves that ordering after both spans are validated. The guard fixture exercises the overlapping slot inside config and confirms config bytes and owner output remain unchanged. The new total-close counter is separate from the existing injected-failure counter, so the `0x212` assertions now distinguish all close attempts from failed-close modeling without changing older expectations. No issue found.

Final reviewed SHA-256 values: source `74907d6f6fa2c5d95d210ad4552a28abef207b3dcbc6fc796b8f55c11dd2c581`; main fixture `8e8b61851a7e24e858c7d2a11f7742994bd57c3b0b1b88a43170c3b60e2640d8`. No tests/builds were run for this read-only pass.
