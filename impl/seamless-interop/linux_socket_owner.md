# Linux connected-socket owner: API and behavioral contract

This normative contract pairs with `include/waddle/interop_linux_socket.h` and IMPL_DESC8.23. It is a host-side transport adapter for the frozen `interop_peer_t` ABI. It does not alter protocol-v2, mapping policy, feature authorization, buffer ownership, producer quiescence, or epoch-reset rules. A transport-close result is never proof that shared memory is safe to reuse.

## Ownership and thread model

`interop_linux_socket_create` accepts only a caller-owned, connected `SOCK_STREAM` descriptor that already has `O_NONBLOCK` and `FD_CLOEXEC`. It inspects but never changes flags. It verifies `SO_TYPE`, `getpeername`, `F_GETFL`, and `F_GETFD`. The config must contain a valid peer config, `reserved_zero == 0`, and `drain_timeout_ms` in `[1, InteropLinuxSocketMaxDrainTimeoutMs]`. The output owner slot must be aligned, accessible, NULL-valued, and disjoint from config. The inputs must be readable, nonwrapping, and disjoint. On any create failure the descriptor remains caller-owned, `*out_owner` stays NULL, and `errno` is set for an underlying OS error; on success the owner takes the fd and peer allocations. The implementation allocates/initializes owner first, creates the peer, and publishes the owner slot only after all steps succeed.

An owner is authentic only if returned by successful create, and may only be used on the creating pthread. It starts Active for a host peer. A guest peer starts in MappingWait because the guest initializes its mapping before it can send Hello; create emits no synthetic ValidateMapping event. The caller knows this from the role it supplied and must invoke `mapping_result` after its guest initialization attempt. A host enters MappingWait only after the real ValidateMapping peer event. Callers serialize every operation including destroy; the owner stores no callback and no caller pointer. Supplying a fabricated, freed, or concurrently accessed opaque pointer violates the API precondition; functions do not attempt unsafe validation by dereferencing arbitrary addresses. Wrong-thread calls on an authentic live owner return `WrongThread` without changing state or outputs. Tokens must equal the nonzero token copied from peer config. A token mismatch returns `WrongToken` before reading owner phase/time or changing any output.

For all APIs, pointer inputs must refer to live accessible storage of the documented size/alignment. Every pointer range must be nonwrapping and pairwise disjoint where the header says disjoint; caller ranges must also not overlap owner storage. The implementation checks wrap, required alignment, nullability, sizes, and overlaps among supplied ranges and its own accessible owner/buffer storage before mutation. It delegates peer-internal alias checks to the frozen peer ABI. It does not probe arbitrary memory readability/writability. On local argument, alias, thread, token, phase, time, stale-cookie, or budget error, all caller outputs and frame bytes remain unchanged and no socket or peer operation occurs. `frame_out` is written only on success, for exactly `event.copied_bytes`; bytes after that length remain unchanged. Successful operations initialize every output field including reserved zeros.

The drive API has no duplicate event output: the sole newly surfaced peer event is `out_work->event`. `out_work->diagnostic_event` repeats only a terminal scalar diagnostic in TransportClosed state. It is never a repeated FeatureFrame, Error payload, Goodbye payload, or other action to execute. The owner never returns a borrowed pointer.

## Results, time, and saturation

The function return value describes API-call validity. `InteropLinuxSocketSuccess` means the call itself completed; inspect `interop_linux_socket_work_t.status` for I/O progress, terminal I/O, or peer-invariant failure. `MappingFailed` is a dedicated expected result when guest mapping initialization returns false: owner/peer state and output remain unchanged, MappingWait remains active, and caller may retry or abort. For a host, `succeeded=0` is an expected completed validation failure; forward it to the peer and expose its Draining event. Non-boolean `succeeded`, Guest invalid layout/zero epoch, and genuine wrong owner phase are local preflight errors, preserve state/output, and do not invalidate a poll cookie. Host false/mismatched epoch/layout validation is forwarded evidence and may successfully enter Draining. A closed/timed-out owner returns Closed/WrongPhase and cannot be revived. EOF, socket errors, peer `InternalInvariant`, abort, drain expiry, and orderly completion are committed drive outcomes returned as `Success` plus work status/state/diagnostics, not local API errors. For non-drive operations, an unexpected peer ABI invariant result returns `PeerFailure`, preserves their output, but terminalizes the owner and closes transport; this is the sole operation-error result that commits owner state. The first terminal cause is retained in `terminal_reason` and never overwritten. `system_errno` is the first preserved nonzero OS errno for a terminal system error; it is zero when none exists. A peer invariant failure has `WorkPeerFailure` and a terminal diagnostic; it does not fabricate errno. Later valid closed-state calls return stable `WorkTransportClosed` snapshots, zero syscall/byte counts, no new event, and the stored terminal diagnostic. They do not re-close, re-notify `transport_lost`, or repeat external actions.

All timestamps are caller-supplied monotonic milliseconds in `uint64_t`. Successful calls that consume a timestamp update the last-time value; equal timestamps are accepted, a smaller timestamp returns `TimeWentBackwards` without consuming a poll cookie or changing outputs/state. `poll_events` also consumes its supplied time to reject time regression consistently. `drive` must be called at least once every 100 ms in Active, MappingWait, and Draining. The adapter calls `interop_peer_tick` with the supplied time and does not reimplement peer deadlines. `timeout_ms` from poll guidance is at most 100; local pending work forces zero. Deadline equality is expired because the peer ABI owns that comparison.

The first transition into peer Draining from any source (begin_drain, peer tick, received frame, mapping result, or positive-send ACK) records `drain_deadline_ms = min(UINT64_MAX, now_ms + configured_drain_timeout_ms)` with checked saturating addition. A repeated begin_drain or any later Draining observation never extends or recomputes that deadline. At `now_ms >= drain_deadline_ms`, the graceful flush deadline is expired; unsent transport bytes are abandoned and the transport closes. Saturation is defined even at the clock limit: when the sum overflows, deadline is `UINT64_MAX`, and a call at that exact timestamp expires it. No wraparound comparison is used. Mapping wait does not pause time. `mapping_result` must pass the caller's actual result to the peer; it cannot revive a timed-out/closed peer.

Drive budgets are independent caller inputs with hard ceilings: RX bytes `0..8192`, TX bytes `0..65536`, and a combined recv/send attempt budget `0..64`. Zero means perform no recv/send syscall, while timers, already-complete carry dispatch, and terminal revent handling can still proceed. Each recv/send attempt, including EINTR/EAGAIN/zero returns, charges one syscall. A read/write request is additionally capped by its remaining byte budget and available fixed buffer space. Pure peer/helper steps also have a fixed combined ceiling of `InteropLinuxSocketMaxInternalStepsPerDrive` (64) per drive; tick, feed, pending/tx queries, event extraction, ACK, and phase-state queries each charge at least one step. `out_work.syscalls` and `internal_steps` never exceed their ceilings. Implementations must stop at either limit; no helper/feed loop may be unbounded. A single SO_ERROR probe has a separate maximum of one attempt per drive so fatal POLLERR can be diagnosed even with zero data-I/O budget.

## Poll snapshots and bounded progress

`interop_linux_socket_poll_events(owner, token, now_ms, out_poll)` returns the borrowed poll fd (or -1 when closed), requested bits, recommended timeout, `immediate_work`, and a nonzero cookie. The fd is owner-owned and borrowed only for the snapshot: caller must not close/store it, and must discard it on any intervening owner operation. The caller invokes `poll(2)` on that fd only while the matching cookie is current. The adapter never invokes `poll(2)` itself. Requested bits are numeric Linux `POLLIN`, `POLLOUT`, `POLLERR`, `POLLHUP`, and `POLLNVAL`; callers must pass that snapshot's OS-returned `revents` unchanged to the matching drive. `POLLERR`, `POLLHUP`, and `POLLNVAL` are always requested/observed. `POLLIN` is requested only when bounded carry has room and the state may read; `POLLOUT` only when the peer has queued TX and state permits send.

The poll result exposes work that readiness alone cannot signal. `immediate_work=1` and `timeout_ms=0` only for internal work that can advance without socket readiness: nonempty un-fed carry that can be passed to the peer assembler, a complete peer frame pending, a due terminal transition (including expired drain), or a Draining state with no TX remaining that can close immediately. Queued TX alone does not set immediate_work; it requests POLLOUT and otherwise waits for writability, preventing a zero-timeout spin when the peer send buffer is full/EAGAIN. Once all carry bytes have been fed and only the peer-private partial-frame assembler remains, more bytes are required; request POLLIN rather than immediate work. Otherwise timeout is at most 100 ms and, in Draining, no greater than the remaining drain time. During MappingWait the adapter may read into available carry space to detect EOF/fatal I/O, but cannot feed/dispatch; buffered bytes alone do not spin and timeout remains at most 100 ms so `tick` continues. Caller must not block when it already holds an unconsumed successful poll snapshot or knows a cookie-free immediate API action is pending.

The peer C ABI has no deadline getter. The adapter therefore reports `timeout_ms <= 100` even when it cannot know whether a peer deadline is closer. Callers must schedule drive on timeout with a fresh monotonic `now_ms`; they must not copy protocol timeout constants into a second timer. If local work is immediately actionable, use timeout zero.

Poll cookies start at 1. Every successful `poll_events` invalidates any earlier cookie and issues the next value. Cookie `UINT64_MAX` may be issued once; after that, no further cookie can be issued and `poll_events` returns `PollCookieExhausted` with output unchanged. Cookies never wrap/reuse. A successful drive consumes its cookie before performing work. Every successful mapping_result, begin_drain (including a repeat) and abort invalidates the outstanding cookie. Local-invalid mapping_result or begin_drain calls do not invalidate it. Any terminal closure also invalidates it. Stale/replayed cookies return `StalePoll` with no syscalls or mutation. If cookie space is exhausted while transport is live, `poll_events` fails without changing time/state/output. The caller must use cookie-free `abort` or `destroy`; it must not resume polling by resetting/reusing the generation.

Closed owners retain their allocation until explicit destroy. `poll_events` on a valid closed owner returns a fresh cookie, `fd=-1`, `events=0`, `immediate_work=0`, and `timeout_ms=0`; it does not carry the terminal diagnostic. A subsequent `drive` with that cookie returns a closed snapshot without syscalls and consumes the cookie. This distinct snapshot path does not re-surface the terminal peer event as a new actionable event; `out_work.event` is None and only `diagnostic_event` contains the saved terminal scalar. `interop_linux_socket_get_closed_snapshot(owner, token, out_work)` provides terminal diagnostics even after cookie exhaustion. It requires the authentic owner thread, exact token, and TransportClosed state; it has no cookie/time input, does no I/O or peer call, does not replay an event (`event` is None), and leaves output unchanged on error.

The caller may invoke poll only after completing the previous successful drive or handling the event it returned. An outstanding cookie invalidated by an intervening state change cannot be used. `revents` must be a subset of the Linux poll result bits accepted by the header; unsupported bits are local argument errors. A zero `revents` value is valid and still permits the timer tick and local carry/TX progress.

## Drive order and event accounting

Before mutation, drive validates authentic live owner/thread, token, current nonzero cookie, timestamp, revents mask, all budgets, non-NULL/aligned/disjoint outputs, and `frame_capacity >= InteropLinuxSocketFrameCapacity`. The frame capacity rule applies even if this particular call may not produce a frame, so an event can never be consumed without enough destination space. Failure preserves the cookie and every output. On passing preflight, the cookie is consumed and `now_ms` becomes current.

Drive then follows this bounded order:

1. Call `interop_peer_tick` before newly readable bytes; timeout at equality wins. If it returns a non-None event, preserve and return it without later work that could hide it. Apply AbortTransport immediately.
2. Handle fatal poll evidence before dispatching buffered application data: POLLNVAL is terminal; for POLLERR, make at most one bounded `getsockopt(SO_ERROR)` probe. A nonzero result is terminal. EINTR on the probe defers diagnosis to the next drive; a zero SO_ERROR is not itself terminal. POLLHUP alone is not EOF.
3. Service existing carry before new recv. Feed only a bounded prefix via `interop_peer_feed`, retaining exactly `length - consumed` bytes. Capture and return any non-None event produced directly by feed (for example malformed-header Draining); do not then dispatch another event or send. Otherwise, if `pending_bytes` is nonzero, call `interop_peer_next_event` once and return its event. Any receive-side event means no TX in this call.
4. If no event is ready, read in Active or MappingWait only, bounded by remaining byte/syscall budgets and carry room. HUP with readable data is drained before a subsequent `recv == 0` is recorded as generic EOF. In Active, feed newly read carry bytes and return any feed/dispatch event as above. MappingWait only stores the bytes; it never feeds. At full carry stop reading and leave suffix bytes in the kernel.
5. If no receive-side event or terminal condition was returned, and state permits TX, copy the exact current front-frame remainder into the fixed TX copy and send bounded chunks. Every positive send is immediately ACKed to the peer with exactly that count and the same timestamp before another send attempt. EINTR/EAGAIN acknowledge zero. Zero-byte send for nonempty input is terminal error.
6. If `ack_tx` yields a non-None event, preserve and return it immediately; no later event can overwrite it. Thus the HelloAck final-byte Ready event is returned exactly once after its byte is accepted by the kernel. Feature events are returned exactly once to the caller.

The combined caller `syscall_budget` bounds recv/send attempts. The optional SO_ERROR probe has a separate fixed ceiling of one attempt per drive, because fatal POLLERR must be diagnosable even when the caller supplies zero data-I/O budget. Close/shutdown calls happen only on terminalization and are not retry loops.
Successful peer API outcomes that return an event are not repeated on later drives. Only a terminal scalar is retained for diagnostics. An unexpected peer ABI error after preflight triggers the fail-closed terminal path; the drive itself still returns Success with `WorkPeerFailure`, closed state, and diagnostic in work output, because the transport mutation has already committed.

## Mapping wait, drain, and terminal transport

Receiving `ValidateMapping` enters MappingWait. The owner returns that event once. Until `mapping_result`, it continues `interop_peer_tick` on every drive, monitors fatal poll events, and can read bounded bytes into the 8 KiB carry without feeding the peer. It does not suppress HUP/ERR/NVAL and does not suppress deadline processing. When carry fills it suppresses further POLLIN but continues polling fatal conditions and ticking at the 100 ms cadence. Caller mapping work must itself have a finite bound. On timeout, the owner follows the peer event, its AbortTransport flag, and the finite Draining policy; a fatal socket error closes immediately. Once the peer has left its mapping phase, later mapping_result returns WrongPhase/Closed and cannot restore Active/Ready.

`mapping_result` is legal in MappingWait for a guest initial initialization attempt or a host ValidateMapping event. It forwards the caller-supplied `succeeded`, epoch, layout, and time to the pure peer ABI. Successful state change clears MappingWait and invalidates the poll cookie. A guest `succeeded=0` maps to expected `MappingFailed` with no state/output mutation; it does not become an invariant failure. A host `succeeded=0` is a valid failure result that follows the peer’s Draining transition. Guest invalid layout/zero epoch, non-boolean success, and wrong owner phase are local caller errors with state/output unchanged. Host mismatched epoch/layout is forwarded validation evidence. Every escaped non-Success from the peer after those preflights is an invariant failure and closes transport. Buffered bytes are then fed before new socket reads. Mapping objects remain owned by the caller/native mapping manager.

`begin_drain` is legal for every authentic live owner phase accepted by the peer ABI. A repeat is successful, invalidates the poll cookie and never extends the first deadline. Closed/thread/token/time errors are rejected locally; any escaped peer non-Success after preflight closes transport and returns PeerFailure. On a successful first transition it invalidates the poll cookie and records the saturating finite drain deadline; repeated calls never extend it. It follows the returned `InteropPeerAbortTransport` flag. If the flag requests abort, stop both directions, close exactly once, and call `interop_peer_transport_lost` after no further syscalls can occur. In Ready Draining without abort, stop reading/feature admission and flush only existing permitted TX within budgets. Close when queue empties or `now_ms >= deadline`; on deadline, discard unsent bytes. `interop_linux_socket_abort` is a separate cookie-free immediate operation: after authentic owner/thread/token validation it invalidates the cookie, stops all I/O, closes once, calls `transport_lost`, and reports `TerminalLocalAbort`; it does not call a second drain operation or flush TX. Repeated abort on a closed owner returns its stable snapshot. Never claim a discarded or flushed control frame proves producer/mapping release.

Orderly `recv == 0` is always reported as generic socket EOF. The socket carry may be empty while the peer's private assembler owns a partial header/body, so the owner must not label EOF clean/truncated based on carry length. It does not parse untrusted protocol headers. Close on EOF/error once and call `interop_peer_transport_lost` after I/O is quiescent. Store the first diagnostic and errno. Linux `close(fd)` is attempted exactly once; invalidate fd ownership before the attempt and never retry on EINTR. Repeated terminal calls do not close again or call transport_lost again.

`destroy` must run on the owner pthread after the caller stops all operations. It aborts any remaining transport, closes an owned descriptor once, calls `transport_lost` if needed, destroys the peer, frees owner memory, and NULLs the slot. A local preflight error preserves slot/owner. Return `Success` when complete cleanup succeeds; return `CloseDiagnostic` when cleanup succeeds but the one `close` attempt failed, with `errno` set to that close error and the slot still NULL. Never retry close after EINTR or restore fd ownership. Destroying a transport never unmaps external resources.

## Test acceptance plan

Use real `socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC)` and the actual peer ABI in isolated `/tmp` binaries. Do not run a VM, AF_VSOCK daemon, device mapping, compositor, or shared build as part of this owner test.

Tests should prove: successful/failed descriptor transfer; fd flags are inspected not changed; partial/coalesced frames across 8 KiB carry and peer assembler; one event per call and exact copied-byte bounds; Ready appears only after final positive send ACK; short writes/EINTR/EAGAIN/hard errors and output preservation; HUP with queued bytes then EOF; generic EOF diagnostics for empty and partial-wire cases; mapping-wait ticks and fatal-error processing; carry-full mapping wait does not spin; timeout at exact peer boundary; independent budget ceilings; zero-budget calls; all local pointer/range/alias/token/time/phase/cookie failures preserve outputs and cookie; cookie replay and near-exhaustion without wrap; immediate-work/timeout correctness with private carry/pending frames; graceful drain flush, deadline saturation/equality, explicit abort, single close, first-error retention, stable closed snapshots, and no repeated feature side effect; allocator/syscall-fault cleanup with no leak.

Socketpair tests establish only Linux connected stream-owner behavior against the frozen pure peer ABI. They do not establish production VSOCK admission, mapping correctness, compositor release, guest/host process shutdown proof, epoch reset, reconnect safety, or native runtime acceptance.

## Lead corrections and exact acceptance

These rules supersede any more general wording above. Only Guest invalid
layout/zero epoch and false initialization are unchanged local errors; Host
false/mismatched epoch/layout evidence must be forwarded exactly to the peer,
which returns Success with Draining as specified in8.21.1. The adapter must
not relabel failed Host validation as a retryable caller error. Non-boolean
success and genuine wrong role/phase remain preflight errors. Every successful
non-drive mapping/drain/abort call invalidates an outstanding poll cookie,
including repeat drain; the first drain deadline is never extended. Time is
updated only on successful operations, except documented terminal internal
failures. Creation also rejects overlap between the borrowed agent-version
range and the output owner slot before publication. The fd has one exclusive
I/O owner; callers cannot close, change flags, duplicate I/O or reuse it.

Before each send, reserve enough internal-step budget for its mandatory ACK
and state/flag processing. Budget exhaustion may never leave positive OS send
bytes unacknowledged. Every positive send count contributes to tx_bytes even
if its following ACK detects an invariant and closes transport. No such bytes
may be retried. A pure-helper step cap may stop other work while retaining all
bytes/events for the next drive. No extra frame/event can be consumed merely
to inspect whether it was interesting; a consumed None handshake event may
return a completed work result without inventing a caller action.

The initial public ABI uses64-bit pointers/size_t: config56bytes, poll32bytes,
work152bytes, natural8-byte alignment. These are local C layouts, never wire
formats. Zero entire successful output structs including padding/reserved
bytes before assigning fields. All fixed buffers are embedded in the one owner;
peer allocations are destroyed by interop_peer_destroy. No syscall function
pointer table or test fault configuration is public. Tests can use link-time
wrappers for deterministic syscall/allocator failures and a private white-box
seam for safe scalar cookie/clock containment checks. They must restore scalar
faults before deterministic cleanup and must not forge owning pointers/counts.

Acceptance requires real Linux socketpair negotiation/fragmentation/short-write
scenarios, bounds/rollback/timeout/drain/stress/fault cases, production line
and branch coverage>=90%, leak-enabled ASan/LSan/UBSan with zero leaks and
full interop regression checks. Socketpair proves this byte owner only. Within
#7's existing25% host-transport allocation, this slice is5%; connection setup/
VSOCK admission/singleton is10% and daemon/local-control integration is10%.
No completion is awarded until implemented and qualified. No #6 Windows-owner
or native mapping/capture/reconnect/epoch-drain credit is granted.

## Contextual result amendment

IMPL_DESC8.23.1 supersedes broad post-call status mapping. Once local mapping/
drain preflights pass, every escaped peer non-Success is an invariant failure
requiring close and PeerFailure with unchanged caller event output. Guest
false/layout/epoch errors and owner wrong phase remain local preflights; Host
failed validation remains forwarded evidence. Creation alone maps its documented
argument/allocation results before fd transfer.

The poll snapshot explicitly names offset20..23 as reserved_zero_2, per
IMPL_DESC8.23.2. All32 bytes are covered by named fields; cookie offset24 is
unchanged. Both reserved words are zero. Other local structs already contain
no unnamed alignment holes on the supported64-bit ABI.

## Final implementation containment rules

Each successful live peer observation must report one of the eight frozen phases
AwaitHello, AwaitMapping, AwaitHelloAckSent, InitializingMapping, AwaitHelloSent,
AwaitHelloAck, Ready or Draining. TransportClosed cannot originate from a live
owned peer: the adapter alone calls transport_lost after closing its fd. An
unexpected closed/unknown phase is therefore a peer invariant failure. Validate
phase before copying any newly actionable event or changing mapping-callback
bookkeeping. Poll/mapping/drain preserve caller output on PeerFailure; drive
commits a closed WorkPeerFailure result with event None. Already-positive send
bytes remain counted and are never replayed. This checks the frozen phase set,
not a second implementation of the peer transition machine.

A timer-produced AbortTransport closes before any fatal-revent socket probe.
No SO_ERROR probe or data syscall is made on the closed descriptor. Feed and
next_event abort outcomes retain WorkTransportClosed or WorkPeerFailure instead
of being overwritten with WorkProgress. Failed transport_lost preserves the
first OS/terminal cause, surfaces PeerFailure on that operation and is retried
only by explicit destroy; closed snapshots remain diagnostic-only.

Destroy failure retains the owner allocation and slot for a serialized retry,
but it may already have terminalized transport. Peer notification/destruction
failures never reopen or retry close. Once notification and peer destruction
succeed, owner storage is zeroed/freed and the slot becomes NULL; an earlier close
error then returns CloseDiagnostic. Callers retain any external mappings and
resource-release obligations throughout every result.

Tests use an exact-source private translation unit for pure range/compaction
helpers and one authentic owner's seeded UINT64_MAX cookie. They forge no owning
pointer or dynamic allocation count. The ordinary production C object is built
separately and must export exactly the eight frozen functions, with no test
entry point. Coverage counts only the original production C path, hashes the
source/fixtures/headers/contract/helper/real peer objects before and after each
attempt, retains every attempt and enforces >=90% lines and branches without
exclusions. Linux's identical EAGAIN/EWOULDBLOCK values are checked once by a
shared helper; the alternate comparison is compiled only when the constants
differ. The 128 repeated fragmented socketpair sessions test local lifecycle
cleanup; they do not establish VSOCK admission, reconnect continuity or VM proof.

## Feature pass-through admission and request identifiers

The socket owner forwards admission to the protocol peer through three entry points. Each takes the owner, the owner's token, a monotonic time, and caller buffers, and each checks owner, token, time, and buffer ranges before the peer is called.

- `interop_linux_socket_enqueue_feature(owner, token, now_ms, bytes, length, out_event)` admits one complete feature frame whose identifier is zero or already matches the class rules. It returns no identifier.
- `interop_linux_socket_enqueue_request(owner, token, now_ms, bytes, length, out_request_id, out_event)` admits one request frame with identifier zero. The peer reserves an identifier, stamps it into the queued copy, and returns it through `out_request_id` only on success. The caller's buffer is not modified.
- `interop_linux_socket_release_request(owner, token, request_id)` releases one identifier previously returned by enqueue_request. Liveness and Hello identifiers are not releasable through this entry.

Peer results cross the boundary through the table `PeerToSocketResult`, indexed by the raw peer result. A value outside `InteropPeerSuccess..InteropPeerRequestRetired` is an internal peer fault: the owner closes the transport and returns `InteropLinuxSocketPeerFailure`. Table entries marked -1 are the same fault. Local refusals leave owner state unchanged.

The socket result values for this path are: `InteropLinuxSocketInvalidFrame` (13) for a malformed frame or an identifier that violates its class, `InteropLinuxSocketUnsupportedFrame` (14) for a type not legal for this entry or producer, `InteropLinuxSocketFeatureNotNegotiated` (15) for a facility absent from the negotiated set, `InteropLinuxSocketQueueFull` (16) when the Data class has no slot, `InteropLinuxSocketRequestIdsFull` (17) when 4032 feature identifiers are live, `InteropLinuxSocketUnknownRequestId` (18) for a release target that is not live, and `InteropLinuxSocketRequestRetired` (19) for a release after transport loss. Every one of them except the release results leaves nothing queued and no identifier reserved.

Inbound feature frames are not checked here. The peer checks each received identifier against its class (see `session_control_host_contract.md`, Feature table). The socket owner passes the peer's `Draining` event through unchanged, so a received class violation reaches the caller as a Draining event with ProtocolViolation.

