# Host Feature Runtime Contract (session_control_host)

Scope: the optional feature ops table of the synchronous Linux host control runtime
(`src/interop/session_control_host.c`, `src/interop/session_control_host.h`). It lets a
caller run feature dispatch on the same single pump thread that already owns the
mapping, the peer and the socket. Parent contracts: `session_control_host_contract.md`
(runtime, drain and cleanup ownership), `peer_feature_send_contract.md` (admission,
capability map and request identifiers), IMPL_DESC 3.2.7, 8.45.

In scope: the ops table; capability offer and negotiated delivery; inbound feature
delivery; outbound sends through a sender valid only inside callbacks; request
identifiers; extra descriptor multiplexing; deadlines and ticks; closing notification;
re-entrancy rules; the new result values.

Out of scope: any particular feature's behaviour; `session_main` feature behaviour
(it passes NULL ops); guest-side features; mapping or heartbeat changes.

## 1. Ops table

```c
typedef struct interop_host_feature_sender_t interop_host_feature_sender_t; /* opaque */

typedef struct interop_host_feature_ops_t {
    void *context;                 /* passed unchanged to every callback; may be NULL */
    uint64_t local_capabilities;   /* offered in Hello; must be within InteropCapabilityMask */
    int (*on_ready)(void *context, uint64_t negotiated_capabilities, interop_host_feature_sender_t *sender);
    int (*on_frame)(void *context, uint32_t message_type, uint32_t request_id,
                    const uint8_t *frame, size_t length, interop_host_feature_sender_t *sender);
    int (*poll_fds)(void *context, struct pollfd *fds, size_t capacity, size_t *count);
    int (*on_fds)(void *context, const struct pollfd *fds, size_t count, uint64_t now_ms,
                  interop_host_feature_sender_t *sender);
    int (*on_tick)(void *context, uint64_t now_ms, interop_host_feature_sender_t *sender);
    uint64_t (*next_deadline_ms)(void *context, uint64_t now_ms);
    void (*on_closing)(void *context, uint32_t reason);
} interop_host_feature_ops_t;
```

Every member is optional except as stated in section 2. A NULL callback is never called.
`config->feature_ops` is a new last member of `interop_host_control_config_t`; NULL keeps
the capability-zero behaviour exactly.

## 2. Validation (in `host_control_config_valid`, before any device or socket use)

| Condition | Result |
|---|---|
| `feature_ops == NULL` | Unchanged: `admission->local_capabilities == 0`, `run_for_ms` in 1..120000. |
| `feature_ops != NULL`: `local_capabilities` has bits outside `InteropCapabilityMask` | InvalidArgument |
| `feature_ops != NULL`: `admission->local_capabilities != feature_ops->local_capabilities` | InvalidArgument |
| `feature_ops != NULL`: `run_for_ms > 120000` | InvalidArgument; `0` is allowed and means run until stop or loss |
| `feature_ops != NULL`: `poll_fds` and `on_fds` are not both NULL or both non-NULL | InvalidArgument |
| `feature_ops` misaligned for `interop_host_feature_ops_t` | InvalidArgument |
| `run` or `interop_host_control_retry_cleanup` called from inside a callback | InvalidArgument (re-entrancy guard, section 7) |

`run_for_ms == 0` with `feature_ops == NULL` remains InvalidArgument.

## 3. Admission and Ready

- The admission copy receives `local_capabilities` as configured (equal to the ops value).
- Ready with `feature_ops == NULL`: negotiated capabilities must be zero (unchanged).
- Ready with ops: the peer's negotiated intersection must be a subset of the local offer.
  Otherwise PeerFailed and drain.
- After `ready` is committed, `on_ready(negotiated, sender)` is called exactly once. A
  nonzero return is `HostControlFeatureFailed` and begins drain.

## 4. Inbound frames

Feature frames reach the runtime only after the peer has checked them against the
negotiated facilities (`peer_feature_send_contract.md` section 10). A frame that is not
admitted never reaches the runtime; the peer answers it with Error(UnsupportedType) and
drains, and the runtime reports `HostControlUnexpectedFeature`.

For each admitted frame, `on_frame(context, type, request_id, frame, length, sender)` is
called with the peer's copied bytes. The bytes are valid only during the call. A nonzero
return is `HostControlFeatureFailed` and begins drain. If `on_frame` is NULL, every
feature frame is treated as `HostControlUnexpectedFeature`, as with the capability-zero path.

## 5. Sender

`sender` is an opaque pointer valid only while the callback that received it runs.
Using it afterwards, or passing another callback's pointer, yields InvalidArgument.

```c
typedef enum interop_host_feature_result_t {
    HostFeatureSuccess = 0,
    HostFeatureInvalidArgument = 1,  /* NULL or invalid sender, NULL frame with length, NULL out pointer, bad range */
    HostFeatureNotReady = 2,         /* peer not Ready (other than draining) */
    HostFeatureDraining = 3,         /* closing has begun, or the Ready deadline expired at this send; nothing queued */
    HostFeatureInvalidFrame = 4,
    HostFeatureUnsupportedFrame = 5,
    HostFeatureNotNegotiated = 6,
    HostFeatureQueueFull = 7,        /* retry after the pump has transmitted queued bytes */
    HostFeatureRequestIdsFull = 8,
    HostFeatureUnknownRequestId = 9,
    HostFeatureRequestRetired = 10,  /* benign: transport loss retired every identifier */
    HostFeatureOutOfMemory = 11,
    HostFeatureClosed = 12,          /* transport closed */
    HostFeatureTransportFailed = 13  /* internal failure; the run ends with PeerFailed */
} interop_host_feature_result_t;

int interop_host_feature_send(interop_host_feature_sender_t *sender, const uint8_t *frame, size_t length);
int interop_host_feature_send_request(interop_host_feature_sender_t *sender, const uint8_t *frame, size_t length, uint32_t *out_request_id);
int interop_host_feature_release_request(interop_host_feature_sender_t *sender, uint32_t request_id);
```

Results are positive enum values, following the project's result-enum convention. This
deviates from the earlier brief's "negative errno-like" wording; the values are still a
closed documented set, and 0 is success.

- `send` admits unsolicited and response frames (`peer` `enqueue_feature`). A Request type
  here returns UnsupportedFrame; a Response with a zero identifier returns InvalidFrame.
- `send_request` admits Request types through `enqueue_request`; `*out_request_id` is
  written on success (zero when the deadline drained the peer, with Draining returned).
- `release_request` releases a feature identifier. It is allowed while closing, because
  dispatchers abandon requests during drain.
- Every send error leaves the peer unchanged. A Draining result sets an internal flag so the
  run begins drain even if the caller ignores the result.

## 6. Descriptors, deadlines and ticks

Per loop iteration, while Ready and not draining (in this order):
1. `on_tick(now_ms, sender)` runs before the socket poll. Sends it makes are transmitted by
   the same iteration's poll and drive.
2. `poll_fds(fds, 8, &count)` supplies up to eight extra descriptors. They are multiplexed
   with the socket, daemon and stop descriptors. A count above 8 is FeatureFailed.
3. `next_deadline_ms(now_ms)` caps the poll wait. A returned deadline at or before `now_ms`
   produces a zero-timeout poll. The module must advance its state in `on_tick`.

The poll wait is still clamped to 100 ms and to the run deadline. `on_tick` therefore runs
at least every 100 ms while Ready.

After the socket drive of an iteration, when any extra descriptor has revents and the
runtime is still Ready and not draining, `on_fds(fds, count, now_ms, sender)` runs with the
polled `revents`. The runtime never closes these descriptors.

Socket events are not replayed after a callback. The socket owner's poll snapshot is
invalidated by any admission, so the next iteration polls again.

## 7. Callback rules

- Callbacks run only on the creating pthread, only from the pump, and never nested. A
  callback that calls `interop_host_control_run` or `interop_host_control_retry_cleanup`
  gets InvalidArgument (a `_Thread_local` guard).
- No callback runs after drain has begun, except `on_closing`.
- `on_closing(context, reason)` runs exactly once. It runs at the first iteration boundary
  after drain begins, or at the end of the run, whichever is first, and always before owner
  cleanup. `reason` is the `interop_host_control_result_t` value recorded at that moment;
  the returned outcome may be more specific. Sends from `on_closing` return Draining.
- If the run ends without any callback having run, `on_closing` still runs once.

## 8. Results

Appended to `interop_host_control_result_t`:

| Value | Name | Meaning |
|---|---|---|
| 11 | `HostControlFeatureFailed` | An ops callback returned nonzero, or `poll_fds` failed or reported more than eight descriptors. Drain begins. |

Existing outcomes keep their meaning. An unadmitted inbound feature frame is still
`HostControlUnexpectedFeature`. An error or drain from the peer is still `HostControlPeerFailed`.

## 9. Unchanged behaviour

Heartbeat (500 ms, `interop_heartbeat_load`/`_store_next`), mapping validation before
Ack, drain ordering, cancellation, daemon-loss handling and owner cleanup are unchanged.
The capability-zero path with `feature_ops == NULL` is bit-for-bit the same behaviour as
before this contract, except that a refused unnegotiated frame now yields
`HostControlUnexpectedFeature` through the peer's Draining event (`peer_feature_send_contract.md`).

## 10. Verification

Tests in `tests/interop/session_control_host.c` cover: exact type, request identifier and
bytes for both directions; FeatureFailed and on_closing exactly once; an unnegotiated
inbound frame; extra descriptor readiness through on_fds; deadline and tick firing;
`run_for_ms == 0` with ops and the NULL-ops rejection; send after closing; cancellation;
guest disconnect mid-run; the re-entrancy guard; configuration rejections; and a stress run of
2000 frames in each direction interleaved with heartbeats. The runtime coverage gate is
`tests/interop/session_control_host_coverage.py` (90% lines and branches), run under ASan,
LSan and UBSan.
