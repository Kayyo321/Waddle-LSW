# Linux socket owner draft review

Review was read-only. The reviewed source/header/contract hashes were:

- `src/interop/linux_socket_owner.c`: `91651a13f998f8015e582595d936231573e1bc7e4e776ebb66b205c901d1ef04`
- `include/waddle/interop_linux_socket.h`: `2f70728292defaafd576e16f8a145005164847349160376b6a4e0deddd032cdf`
- `impl/seamless-interop/linux_socket_owner.md`: `70c353845da417225a8851adcfd924aa069903e3ebe2773c73b2079a6a34f67a`

## Actionable findings

1. **A completed RX frame can become permanently undispatchable when the per-drive helper budget expires.** In `service_carry`, feed consumes bytes first (lines 357-377), then separately charges `pending_length` and `next_event` (379-394). With two internal steps left, feed and pending query can consume the frame into the peer and discover it is pending, while the `next_event` step is refused. On the next call, `drive` only invokes `service_carry` for nonempty `owner->rx_length` (723-737); if feed consumed the carry, it never queries/dispatches the peer's pending frame. `poll_events` continues to set `immediate_work` for `pending_bytes != 0` (source lines 559-562), so repeated calls can spin without progress. Add a pending-frame service path independent of carry length, with bounded helper accounting, before new receive work. The trigger is feasible through the existing internal-step budget; no state fabrication is needed.

2. **POLLERR with an interrupted SO_ERROR probe does not defer application dispatch.** `handle_fatal_revents` returns “not terminal” on `getsockopt(SO_ERROR)` EINTR (lines 408-427), after which `drive` proceeds into buffered carry servicing and receive work (715-794). The contract says EINTR defers POLLERR diagnosis to the next drive and places fatal poll evidence before buffered application dispatch. Consider ending the current drive without RX/TX dispatch on probe EINTR so the next poll/drive resolves POLLERR first; otherwise clarify that dispatch during deferred diagnosis is permitted.

## Review limits

No files were edited, no build/tests were run, and no native endpoint or VM was used. The source hash above identifies the exact draft reviewed; re-check if the author changes it. Other reviewed paths (positive-send ACK reservation, per-drive step cap with close-notification reserve, mapping-result guest/host distinction, first terminal cause, close-once handling, and destroy retry ownership) had no additional actionable issue found in this pass.
