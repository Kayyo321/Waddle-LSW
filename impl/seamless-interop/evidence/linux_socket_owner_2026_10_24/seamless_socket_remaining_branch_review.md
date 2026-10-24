# Linux socket-owner remaining branch reachability review

This is a read-only audit of coverage attempt `build/coverage/interop/linux_socket_owner/run-20261024T204037-a92c345b/owner.gcov.json.gz`, using the source and fixtures whose hashes are recorded in its `inputs.json`.

- Covered production source SHA-256: `58b2750c05e97a16072e6ba0d008db515111d8fc213ec4c5c13fb282d2ba3f61`
- Current shared source SHA-256 at report time: `8e7328aa25a939b3e8bf474f7e62a63b1606c0b6c831ee4e1d562880ccbf94bb` (different; findings below apply to the recorded attempt only)
- Coverage input record: `build/coverage/interop/linux_socket_owner/run-20261024T204037-a92c345b/inputs.json`
- Attempt result: 667/732 lines (91.12%), 478/590 branches (81.02%)

## (a) Genuine public paths worth additional tests

The existing guard fixture already exercises unconnected and wrong-type sockets, blocking streams, and missing `FD_CLOEXEC`; those cases should not be duplicated. The uncovered arcs on the fcntl validation expressions at source lines 491 and 494 are specifically the negative `fcntl(F_GETFL)` and `fcntl(F_GETFD)` syscall-result arms, since flag-missing cases already run. Inject those two errors separately and assert the owner slot remains NULL and the caller retains the fd.

Other focused reachability opportunities:

- `recv` EAGAIN at lines 819-827. Existing tests cover recv EINTR followed by data, hard error, and EOF. Inject a single EAGAIN while passing POLLIN; verify one syscall, no RX bytes/event, live owner, and retry on a fresh cookie.
- `getsockopt(SO_ERROR)` succeeds with zero at lines 413-425. Existing tests cover nonzero errors and EINTR/hard failure. Inject successful zero SO_ERROR with buffered valid input; verify POLLERR itself does not close the owner and input is processed once.
- `compact_rx` at lines 444-450. Current tests exercise fragmentation and the pending-frame/internal-step boundary, but do not appear to leave an unconsumed suffix after a consumed event. Send two valid coalesced frames in a protocol phase that accepts both; verify the second call compacts `rx_start != 0 && rx_length != 0` and reports events exactly once. A valid post-Ready Ping/Pong exchange is preferable to arbitrary bytes.
- `create` version pointer short-circuit at lines 478-482: the guard fixture already tests `agent_version=NULL, bytes=0`. Add the distinct non-NULL pointer with zero-byte-length invalid config to enter the zero-length `ranges_overlap` case at lines 81-83 before peer validation rejects it.
- Poll-cookie maximum at lines 597-601 is a real API boundary but cannot be reached by normal iteration. A safe private scalar seam can set `next_poll_cookie=UINT64_MAX` while leaving `cookies_exhausted=false`, then verify the max cookie is issued once and later polls fail without wrapping or modifying output. The later `cookie==0` branch at lines 592-595 is separately shadowed by the prior exhausted flag and should not be reached via fabricated state.
- A malformed protocol header delivered to an active owner can exercise feed-produced Draining event handling (`accept_peer_event`, lines 295-319), including preserving that event and the first terminal diagnostic. Also exercise peer handshake timeout on `tick` if not already present; it can reach terminal/abort event handling through a real valid peer and elapsed time.
- HUP with bytes queued: send a complete valid frame, half-close the peer write side, drive with `POLLIN|POLLHUP`, verify the frame is surfaced before a later drive records generic EOF. Current EOF case closes the peer before sending payload.

## (b) Safe syscall/scalar fault seams

- Wrap `calloc` and fail only the owner allocation at lines 497-499; require `OutOfMemory`, unchanged NULL output, and no fd transfer.
- Wrap `fcntl` to return `-1` with a chosen errno for `F_GETFL` and `F_GETFD` independently (lines 490-495). These are syscall failures distinct from already-covered missing-flag paths.
- Existing wrappers cover send/recv, close, SO_ERROR, and peer helper failures. Avoid replaying existing send EAGAIN/EINTR/zero/hard-error, recv EINTR/hard-error/EOF, nonzero SO_ERROR, and peer helper failure matrices unless a new state precondition is needed to reach a missed arc.
- A cookie-max seed is the only scalar seam suggested here; do not set internal step counters or owning pointers to arbitrary values to force branches.

## (c) Conditions not meaningfully reachable from an authentic owner

- `range_valid`’s `alignment == 0` arm (line 64) has no call site with zero alignment; all alignments are compile-time `alignof` values. Public APIs also reject NULL before calling it. Do not create fake pointers/owners to exercise these internal-only arguments.
- `has_time == false` at `check_time` (line 112) is unreachable after successful create: create initializes it true and every commit preserves true.
- `next_poll_cookie == 0` while not exhausted (line 592) violates the owner invariant; production sets zero and `cookies_exhausted=true` together, and poll checks the flag first.
- The first `drive` step-exhaustion branch (line 714) is unreachable: work is zeroed immediately before `take_internal_step`. Later helper-budget exhaustion is public/reachable and already has a boundary test.
- `close_transport` with `fd < 0` on a first close is unreachable for a live owner: successful create transfers a nonnegative fd and first close immediately sets `closed`; later calls return before examining fd.
- `close_transport`’s work-step allowance condition (lines 218-224) cannot be forced false by normal accounting: ordinary helper steps reserve the terminal notification step; `take_internal_step` refuses at the ceiling. Do not seed a private counter to its maximum.
- `take_internal_step` failure immediately after a positive send (line 906) and the reserved phase query failure at line 920 are precluded by the explicit two-step reservation at line 889. Keep this invariant and test the boundary that stops before send rather than fabricating a send with exhausted budget.
- `amount == 0` inside the TX loop (line 895) is ruled out by the loop’s positive remaining TX and byte-budget predicates. Likewise, a fresh zero-length write cannot occur after a nonempty peer TX frame is copied and checked at lines 879-882.
- `service_carry`’s non-Active rejection (line 354) is not reached by its current callers: both call sites require Active state. Pending-frame service on empty carry remains a real path and should stay tested.
- Several already-covered source lines still have missed short-circuit arcs, not missing whole scenarios. In particular create’s blocking/missing-CLOEXEC branches are covered; only syscall-error terms are missing. The coverage JSON supplies GCC arc IDs, not semantic labels, so classifications are based on the recorded source and fixture rather than treating each missed line as an untested path.

## Limits

No edits, builds, or tests were run. The working source changed during the audit, so all advice is scoped to the immutable coverage-attempt hashes above; re-map line numbers and arc outcomes against the next frozen coverage attempt.
