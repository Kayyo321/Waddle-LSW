# Peer Feature Send Contract (control_peer, C ABI)

Scope: the outbound feature admission path of the connected Ready control peer
(`src/interop/control_peer.zig`, `src/interop/control_peer_abi.zig`,
`include/waddle/interop_peer.h`). Parent specification: IMPL_DESC 8.20 and 8.21
and the frozen framing in IMPL_DESC 3.2.

In scope: admitting one complete caller-built feature frame (header plus payload)
to the bounded outbound FIFO of a Ready connection, with role, direction,
negotiated-capability and queue checks; reporting the result through the copied
event; the interaction with drain, deadline expiry and liveness traffic.

Out of scope: socket I/O, guest or host dispatch of received features,
feature request-ID allocation and correlation, native resource teardown,
mapping validation, and any effect the feature causes on the platform. Ready is
connection negotiation only; it grants no facility admission by itself.

## 1. Entry points

Zig (serialized method on `peer_t`):

```
pub fn enqueue_feature(self: *peer_t, connection_token: u64, now_ms: u64,
    frame: []const u8) (control_peer_error_t || feature_send_error_t)!event_t
```

C (`include/waddle/interop_peer.h`):

```
int interop_peer_enqueue_feature(interop_peer_t *peer, uint64_t token,
    uint64_t now_ms, const uint8_t *bytes, size_t length, interop_peer_event_t *event);
```

Exported from `control_peer_abi.zig` as `interop_peer_enqueue_feature(owner_address,
token, now_ms, bytes_address, length, event_address)` with the same pointer
rules as the neighbouring exports.

## 2. Validation order

Each step runs only if every earlier step passed. Steps 1-5 are pure: they
change no state. Step 6 is the only timer transition.

| Step | Check | Failure result (Zig / C) |
|---|---|---|
| 0 (C only) | owner resolves; `bytes`/`length` valid and not overlapping peer storage; `event` aligned, non-NULL, disjoint from bytes; NULL bytes only with length 0 | InvalidArgument |
| 1 | peer live; token nonzero; token matches | PeerClosed / InvalidConnectionToken / WrongConnectionToken (WrongToken) |
| 2 | `now_ms >= last_now_ms` | TimeWentBackwards |
| 3 | phase is exactly Ready | WrongPhase (Draining, Closed and every handshake phase included) |
| 4 | `16 <= len <= MaxFrameBytes`; header magic, version, payload length (<= MaxPayloadBytes, multiple of 4) decode; `16 + header.length == len` | InvalidFrame |
| 5a | type is not control (0x0001-0x0006) and not local-only (0x7F00-0x7FFF) | UnsupportedFrame |
| 5b | type is in the facility table (section 4) | UnsupportedFrame |
| 5c | codec payload validation with this role's producer direction: wrong direction, unknown type, unknown critical tag -> UnsupportedFrame; malformed TLV, missing or duplicate required fields, bad values -> InvalidFrame | UnsupportedFrame / InvalidFrame |
| 5d | the type's facility is in the negotiated Ready intersection | FeatureNotNegotiated |
| 6 | Ready deadline: if `now_ms - last_valid_rx_ms >= 6000` the peer enters Draining (reason TransportLost) and the call returns a Draining event; the frame is NOT queued | (success, event kind Draining) |
| 7 | queue admission in the Data class (section 5) | QueueFull / OutOfMemory |

Step 6 comes after the pure checks so that a malformed or refused frame never
drains a connection. Expiry still wins over admission, matching `ack_tx` and
`tick`: an expired deadline is a timer event, not a frame decision.

## 3. Results and state changes

- Success: the frame bytes are copied into one queue allocation (Data class),
  a `Feature` send record is appended with `request_id` from the header and
  `total_bytes = remaining_bytes = len`, `send_count` increments, `last_now_ms`
  becomes `now_ms`. The returned event is `None` with the current phase.
- Any error (steps 1-5, 7): no field changes. Queue, send records, `last_now_ms`,
  request IDs, phase, `terminal_send_failed` and `abort_transport` are unchanged.
  The C ABI copies no event on error.
- Step 6: phase becomes Draining, `handshake.drain_reason` records TransportLost
  if no earlier reason exists, `last_now_ms` becomes `max(last, now_ms)`. The frame
  is not queued. The C event kind is Draining and the call returns Success.

Error code mapping (`map_feature_error`, C result values):

| Zig error | C result |
|---|---|
| InvalidFrame | 12 InteropPeerInvalidFrame |
| UnsupportedFrame | 13 InteropPeerUnsupportedFrame |
| FeatureNotNegotiated | 14 InteropPeerFeatureNotNegotiated |
| QueueFull | 15 InteropPeerQueueFull |
| OutOfMemory | 2 InteropPeerOutOfMemory |
| WrongPhase | 5 InteropPeerWrongPhase |
| WrongConnectionToken | 3 InteropPeerWrongToken |
| InvalidConnectionToken | 1 InteropPeerInvalidArgument |
| TimeWentBackwards | 4 InteropPeerTimeWentBackwards |
| PeerClosed | 11 InteropPeerClosed |

Values 12-15 were appended to `interop_peer_result_t`; no existing value changed.

## 4. Facility table

Derived from the capability names of IMPL_DESC 3.2.4 and the message table of
3.2.5. Requirement kinds: `all` (every listed bit) or `any` (at least one of the
listed bits).

| Facility bits required | Message types |
|---|---|
| all Windows | WindowAnnounce, WindowUpdate, WindowRetire, WindowConfigure, WindowClose, WindowMoveRequest, WindowResizeRequest, WindowStateRequest, FrameReady, FrameRelease, WindowIconChanged, PopupRepositioned, PopupDismissed |
| all Input | InputFocus, InputKey, InputPointerMotion, InputPointerButton, InputPointerAxis, InputRelease, InputLockState, InputRepeatInfo, LeaseRevoked, LeaseAck, LeaseReady |
| all Input + RelativePointer | InputPointerRelative |
| all Input + Touch | InputTouch, InputTouchFrame, InputTouchCancel |
| all Input + Tablet | InputTablet, InputTabletProximity |
| all Input + Gestures | InputGesturePinch |
| all Input + TextInput | InputTextCommit, InputTextPreedit |
| all TextInput | CaretRect |
| all KeyboardLayout | KeyboardLayout |
| all Cursor | CursorShape |
| all Cursor + RelativePointer | CursorConstraint |
| all Clipboard | ClipboardOffer, ClipboardRequest, ClipboardClear |
| all DragDrop | DndEnter, DndMotion, DndLeave, DndDrop, DndResult, DndSourceStart, DndSourceRequest, DndSourceFinished |
| all Catalog | CatalogBegin, CatalogAdd, CatalogUpdate, CatalogRemove, CatalogEnd, IconRequest |
| all Launch | LaunchRequest, LaunchResult |
| all UacNative | ElevationNotice, UacPrompt, UacDecision, DesktopSwitch |
| all Audio | AudioStreamOpen, AudioStreamClose |
| all Printers | PrinterSync, PrinterResult |
| all Display | DisplayConfig, DisplayApplied |
| any of Clipboard, DragDrop, Catalog, Cursor, UacNative | TransferBegin, TransferChunk, TransferEnd, TransferCancel |

Control types (0x0001-0x0006) and local-only types (0x7F00-0x7F02) have no
entry; they are refused in step 5a.

Decisions made here, not fixed by the frozen text:
- Window messages require Windows. The brief suggested no bit beyond base for
  window messages; the frozen rule "only negotiated facilities may be used"
  (IMPL_DESC 3.2.7 step 4) needs one.
- Transfers have no own facility. The peer can only require that a facility
  which creates transfers was negotiated; the specific origin check remains with
  the dispatcher, as IMPL_DESC 8.20 assigns it.
- Both `CursorConstraint` and `InputPointerRelative` require RelativePointer,
  because both carry relative-pointer semantics (IMPL_DESC 3.6.5).

## 5. Queue admission

- The frame goes to `stream_send_queue` in the Data class. That class refuses
  when the total queued frame count is at least 504 (`DataCapacity` = 512
  minus `ReservedControlSlots` = 8). Control frames (Ping, Pong, Error,
  Goodbye) use the Control class and keep the reserved slots.
- All 512 ring entries may be used by control traffic. `QueueFull` covers both
  "reserved slots remain" and "ring full" for the Data class.
- Admission never reorders: frames leave in the order they were admitted, and
  a Ping queued after feature frames leaves after them.
- `QueueFull` is retryable. The caller retries after `ack_tx` completes at least
  one front frame, which lowers the total count below 504.

## 6. Ownership and lifetime

- The caller owns `frame` and may reuse or free it immediately after return.
- The queue owns one copy until the frame is fully acknowledged or the peer is
  destroyed. The copy is never aliased by the caller.
- The peer allocates only that copy (allocator borrowed from init). It does not
  own the feature payload semantics.
- No request ID is reserved or released by this path. `header.request_id` is
  copied verbatim; the caller owns feature request correlation (IMPL_DESC 8.20).
  Response-side events arrive as `FeatureFrame` with the echoed ID.

## 7. Threading

All calls must be serialized on the connection's event-loop owner, together
with `feed`, `next_event`, `tick`, `peek_tx`, `ack_tx`, `begin_drain`,
`transport_lost` and `deinit`. Borrowed TX slices from `peek_tx` are invalidated
by `enqueue_feature`; the caller must re-peek after every admission.

## 8. Interaction with drain, deadline and liveness

- Admission is allowed only in Ready. After `begin_drain`, `transport_lost`, a
  handshake failure or any other Draining/Closed transition, every call returns
  WrongPhase with no change.
- Frames already queued at drain remain FIFO. The caller may flush them with its
  budget through `peek_tx` and `ack_tx`. Feature frames never set
  `abort_transport`; only queued Hello or HelloAck can do that, and Ready
  admission cannot queue them.
- The Ready deadline (6000 ms since last valid receive) is checked after the pure
  validation. Sending does not refresh liveness: only valid received frames do.
  A slow or stalled OS sender can therefore starve Pong delivery; the caller must
  keep draining. The peer does not enforce a byte budget beyond the 504-frame
  Data limit.
- Ping is produced only by `tick`. It uses the Control class, so a full Data
  class never blocks liveness traffic.

## 9. Verification

Zig (`make interop-control-peer-test`, 58 tests, all passing): exact bytes and
round trip for both roles; complete 0..65535 facility-table sweep (70 feature
types); refusal in every non-Ready phase; wrong token, time and exact deadline;
malformed header, payload and oversize; unknown optional and unknown critical
TLVs; facility gating and any-of transfers; data-class boundary and recovery;
FIFO around Ping and Pong; drain and transport loss; FailingAllocator admission;
deterministic 3000-step random admission and partial-acknowledgement sequence.

C ABI (`make interop-control-peer-abi-test`, 85 Zig tests plus the C client
test, all passing): pointer, alias and token guards; every mapped error; a real
handshake through the ABI; FeatureNotNegotiated and UnsupportedFrame; the
504-frame QueueFull then recovery; the Ready deadline and WrongPhase afterwards.
The C client was also built with `-fsanitize=address,leak,undefined` and run with
`detect_leaks=1`.

Coverage (`python3 tests/av/coverage.py <mode>`): control_peer line 97.07%
(531/547), branch 91.47% (354/387); control_peer_abi line 98.65% (219/222),
branch 94.08% (159/169). Harness-excluded compiler panic/stack-canary guards:
control_peer 101, control_peer_abi 21.

## 10. Open items

- Feature request-ID namespace: IMPL_DESC 3.2.1 specifies one per-direction
  counter, while the peer's occupied set (Hello and Ping IDs) is internal. This
  path does not check feature request IDs against that set. The dispatcher must
  either avoid IDs the peer occupies or the counter must move into the peer.
  Not resolved in this slice; the lead should decide before dispatcher work.
- Dispatcher-level transfer origin checks are external to this contract.
