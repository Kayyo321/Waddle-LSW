# Peer Feature Send Contract (control_peer, C ABI)

Scope: outbound feature admission, request-identifier allocation and the receive-side
facility gate of the connected Ready control peer (`src/interop/control_peer.zig`,
`src/interop/control_peer_abi.zig`, `include/waddle/interop_peer.h`). Parent
specifications: IMPL_DESC 3.2 (frozen framing and message table), 8.20 and 8.21
(peer composition and C ABI), and 8.45 (frozen decisions for the capability map,
transfer admission and request identifiers).

In scope: admitting one complete caller-built feature frame to the bounded outbound
FIFO of a Ready connection; allocating and releasing feature request identifiers;
the facility table shared by both directions; the interaction with drain, deadline
expiry and liveness traffic.

Out of scope: socket I/O, guest or host dispatch of feature payloads, transfer
record lookup, mapping validation, native resource teardown, and any effect a
feature has on the platform. Ready is connection negotiation only.

## 1. Entry points

Zig (serialized methods on `peer_t`):

```
pub fn enqueue_feature(self, token, now_ms, frame) (control_peer_error_t || feature_send_error_t)!event_t
pub fn enqueue_request(self, token, now_ms, frame) (control_peer_error_t || feature_send_error_t)!request_admission_t
pub fn release_request(self, token, request_id) (control_peer_error_t || request_release_error_t)!void
pub fn feature_requirement(message_type, producer, payload) error{MalformedField}!?feature_requirement_t
pub fn check_facility(message_type, producer, payload, negotiated) feature_send_error_t!void
pub fn feature_class(message_type) feature_class_t
```

C (`include/waddle/interop_peer.h`), each exported by `control_peer_abi.zig` with the
same pointer rules as its neighbours:

```
int interop_peer_enqueue_feature(peer, token, now_ms, bytes, length, event);
int interop_peer_enqueue_request(peer, token, now_ms, bytes, length, uint32_t *out_request_id, event);
int interop_peer_release_request(peer, token, request_id);
```

## 2. Facility table (8.45 capability map)

`feature_requirement` is the single table. The receive path calls `check_facility`
with the same arguments, so the two directions cannot drift.

| Requirement | Message types |
|---|---|
| all Windows | WindowAnnounce, WindowUpdate, WindowRetire, WindowConfigure, WindowClose, WindowMoveRequest, WindowResizeRequest, WindowStateRequest, FrameReady, FrameRelease, WindowIconChanged, PopupRepositioned, PopupDismissed, ElevationNotice, DesktopSwitch |
| all Input | InputFocus, InputKey, InputPointerMotion, InputPointerButton, InputPointerAxis, InputRelease, InputLockState, InputRepeatInfo, LeaseRevoked, LeaseAck, LeaseReady, CaretRect |
| all Input + RelativePointer | InputPointerRelative |
| all Input + Touch | InputTouch, InputTouchFrame, InputTouchCancel |
| all Input + Tablet | InputTablet, InputTabletProximity |
| all Input + Gestures | InputGesturePinch |
| all Input + TextInput | InputTextCommit, InputTextPreedit |
| all KeyboardLayout | KeyboardLayout |
| all Cursor | CursorShape |
| all Cursor, plus RelativePointer when `pressed` is 1 | CursorConstraint |
| all Clipboard | ClipboardOffer, ClipboardRequest, ClipboardClear |
| all DragDrop | DndEnter, DndMotion, DndLeave, DndDrop, DndResult, DndSourceStart, DndSourceRequest, DndSourceFinished |
| all Catalog | CatalogBegin, CatalogAdd, CatalogUpdate, CatalogRemove, CatalogEnd, IconRequest |
| all Launch | LaunchRequest, LaunchResult |
| all UacNative | UacPrompt, UacDecision |
| all Audio | AudioStreamOpen, AudioStreamClose |
| all Printers | PrinterSync, PrinterResult |
| all Display | DisplayConfig, DisplayApplied |
| transfers, producer Host: any of Clipboard, DragDrop | TransferBegin, TransferChunk, TransferEnd, TransferCancel |
| transfers, producer Guest: any of Clipboard, DragDrop, Catalog, Cursor | TransferBegin, TransferChunk, TransferEnd, TransferCancel |

Control types (0x0001-0x0006), local-only types (0x7F00-0x7F02) and unknown types
have no entry. `UacNative` never admits a transfer. The producer direction, not the
local role, selects the transfer row, both when sending and when receiving.

## 3. Request identifier classes

| Class | Types | Header identifier |
|---|---|---|
| Request | LaunchRequest, PrinterSync, DisplayConfig, DndDrop | Allocated by `enqueue_request`; the caller passes zero |
| Response | LaunchResult, PrinterResult, DisplayApplied, DndResult | Must be nonzero and echo the request; never reserved |
| Unsolicited | every other feature type, including all transfers | Must be zero; transfers use their own identifiers in the payload |

Class checks run only after the codec and facility checks pass, so a refused frame
of the wrong class still reports its earlier error first.

## 4. Validation order

Each step runs only if every earlier step passed. Steps 1-5 are pure; step 6 is the
only timer transition. Steps 1-5 apply to both entry points.

| Step | Check | Failure result |
|---|---|---|
| 0 (C only) | owner resolves; `bytes`/`length` valid and disjoint from peer storage; outputs aligned, non-NULL and disjoint; NULL bytes only with length 0 | InvalidArgument |
| 1 | peer live; token nonzero; token matches | PeerClosed / InvalidConnectionToken / WrongConnectionToken |
| 2 | `now_ms >= last_now_ms` | TimeWentBackwards |
| 3 | phase is exactly Ready | WrongPhase |
| 4 | `16 <= len <= MaxFrameBytes`; header magic, version, length decode; `16 + header.length == len` | InvalidFrame |
| 5a | type is not control and not local-only | UnsupportedFrame |
| 5b | codec payload validation with this role's producer direction | UnsupportedFrame (unknown, unsupported, wrong direction) / InvalidFrame (malformed) |
| 5c | facility from the table above, using the payload for CursorConstraint | UnsupportedFrame (no row) / FeatureNotNegotiated / InvalidFrame (malformed pressed) |
| 5d | class: `enqueue_feature` refuses Request types (UnsupportedFrame), requires nonzero identifiers on Response types and zero otherwise (InvalidFrame); `enqueue_request` accepts only Request types (UnsupportedFrame) with zero identifier (InvalidFrame) | as stated |
| 6 | Ready deadline: if `now_ms - last_valid_rx_ms >= 6000` the peer enters Draining (TransportLost); the frame is not queued | (success, event Draining) |
| 7 | (`enqueue_request` only) feature identifiers below the 4032 cap, then a reservation from request_ids | RequestIdsFull |
| 8 | Data-class queue admission | QueueFull / OutOfMemory |

Step 6 comes after the pure checks so that a refused frame never drains a connection.

## 5. Results and state changes

- `enqueue_feature` success: bytes copied into one Data-class allocation, a `Feature`
  send record appended with the header identifier, `last_now_ms = now_ms`, event None.
- `enqueue_request` success: the reserved identifier is stamped into bytes 12..16 of a
  scratch copy, that copy is queued, the identifier is appended to the feature-owned
  set, the send record carries it, `last_now_ms = now_ms`. Returns the identifier and
  event None. The caller's buffer is never modified.
- Any error from steps 1-5 or 7-8: no field changes. For `enqueue_request` the
  identifier cursor is restored as well, so the next successful request receives the
  identifier the failed one would have received.
- Step 6 (either entry point): phase becomes Draining, `last_now_ms = max(last, now)`,
  no frame is queued, and `enqueue_request` returns identifier zero.

Error mapping (C results): InvalidFrame 12, UnsupportedFrame 13, FeatureNotNegotiated 14,
QueueFull 15, RequestIdsFull 16, UnknownRequestId 17, RequestRetired 18; the remaining
errors map as in `map_feature_error`/`map_release_error`. Values 12-18 were appended
to `interop_peer_result_t`; no existing value changed.

## 6. Request identifier ownership

- Feature identifiers come from the same `request_ids` owner as Ping and Hello, so
  one namespace per direction is preserved (IMPL_DESC 3.2.1).
- The peer keeps `feature_request_ids` as the set of identifiers it reserved for
  features. Only identifiers in that set are releasable. Liveness Ping identifiers,
  the Hello identifier and never-reserved values report UnknownRequestId.
- Feature reservations are capped at 4032 live identifiers (`FeatureRequestCapacity`),
  so at least 64 slots of the 4096-entry namespace remain for liveness.
- `release_request` is valid in every live phase. It is not time-dependent. After
  `transport_lost`, `release_request` returns RequestRetired (benign) and
  `enqueue_request` returns WrongPhase.
- The dispatcher releases an identifier when the response arrives or it abandons the
  request. Releasing does not remove a frame that is still queued.
- Response frames are not reserved; their echoed identifiers belong to the request
  that the dispatcher already holds.

## 7. Ownership and lifetime

- The caller owns `frame` and may reuse or free it immediately after return.
- The queue owns one copy until the frame is fully acknowledged or the peer is
  destroyed. The copy is never aliased by the caller.
- `enqueue_request` uses a stack scratch buffer of `MaxFrameBytes` for the stamp;
  it is not retained.
- The peer allocates only queue copies. Identifier and tracking storage is inline.

## 8. Threading

All calls must be serialized on the connection's event-loop owner, together with
`feed`, `next_event`, `tick`, `peek_tx`, `ack_tx`, `begin_drain`, `transport_lost`
and `deinit`. Borrowed TX slices from `peek_tx` are invalidated by any admission;
re-peek after every admission.

## 9. Interaction with drain, deadline and liveness

- Admission is allowed only in Ready. After `begin_drain`, `transport_lost`, a
  handshake failure or any other Draining/Closed transition, every call returns
  WrongPhase with no change, and release returns RequestRetired only after transport
  loss.
- Frames already queued at drain remain FIFO. Feature frames never set
  `abort_transport`.
- Ping is produced only by `tick`, in the Control class; a full Data class never
  blocks liveness traffic.

## 10. Receive side

`dispatch_ready` checks every received feature frame with `check_facility` using the
reader's producer direction and the negotiated set. A refused frame produces the same
handling as a codec-level UnsupportedType: one Error(UnsupportedType) reply is queued,
the frame is consumed, and the peer enters Draining (WireError). Admitted frames are
copied unchanged to the caller as FeatureFrame events. The receive side does not yet
validate request identifier classes; the dispatcher owns that check.

## 11. Decisions (8.45, plus the points settled while implementing)

- The 8.45 capability map is applied exactly: ElevationNotice and DesktopSwitch need
  Windows only; UacPrompt and UacDecision need UacNative only; CaretRect needs Input;
  CursorConstraint needs RelativePointer only while pressed; KeyboardLayout needs its
  own facility only.
- Transfers are admitted by producer role; UacNative never admits them.
- Request-class violations use InvalidFrame, and a Request type on the feature entry
  point uses UnsupportedFrame, because the type is valid but the wrong entry point was
  used.
- An unadmitted received frame is handled as a codec UnsupportedType (Error reply and
  drain), not delivered for the dispatcher to refuse.
- Release after transport loss is RequestRetired, distinct from UnknownRequestId.
- Feature reservations are capped at 4032 so liveness always has 64 identifiers.

## 12. Verification

- `make interop-control-peer-test` (Zig, named-module root): 65 tests, all passing.
  Covers the complete 0..65535 table sweep with the CursorConstraint payload, facility
  gating on both directions, the request class rules, identifier stamping and echo,
  the 4032 cap with liveness still reserving, wraparound past UINT32_MAX, the data-queue
  boundary, an allocation failure with a full snapshot comparison, the exact Ready
  deadline, and the 3000-step random admission sequence.
- `make interop-control-peer-abi-test` (85 Zig tests plus the C client): pointer, alias,
  token and time guards; every mapped result including RequestIdsFull, UnknownRequestId
  and RequestRetired; the exact Ready deadline boundary; release and stamping through
  the ABI.
- Coverage: see the lead's gate list in the lane report (`tests/av/coverage.py control_peer`
  and `control_peer_abi`).

## 13. Open items

- Dispatcher-level transfer origin checks and feature payload semantics remain
  external (IMPL_DESC 8.20).
- Receive-side request-identifier class checks are not enforced in the peer; the
  dispatcher must reject an unsolicited frame that carries a nonzero identifier.
