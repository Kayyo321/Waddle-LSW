# Composed VSOCK peer core contract proposal

## Scope and composition

Add `src/interop/peer_core.zig` as the allocation-owning composition layer for one already-connected VSOCK peer. It composes `stream_reader.reader_t`, `stream_send_queue.queue_t`, `session_handshake.session_handshake_t`, `session_liveness.session_liveness_t`, one shared `request_ids.request_ids_t` for this sender direction, `agent_version.parse_major`, and the frozen C codec. It owns framing buffers, queued frame copies, the copied local version, handshake/liveness state, outstanding IDs, and bounded control-frame completion metadata.

It owns no socket, timer thread, mapping, AV/device handle, guest-window state, feature authorization, process, or OS detach proof. A serialized platform loop owns the socket and supplies monotonic milliseconds and exact successful send counts. The caller/feature adapter owns external mapping initialization/validation and facility authorization. Handshake-ready means only that the protocol handshake completed; it does not claim feature readiness or safe shared-memory release.

All peer calls are serialized on the connection's event-loop thread. The connection token is supplied on every stateful call; a stale token returns `StaleConnection` without changing state. Core deinit is permitted after I/O callbacks are quiesced. Teardown of external mappings remains with their owner.

## Proposed Zig surface

The exact type names below are proposed private-module API, not wire/API additions. Public Zig functions use snake_case; all exported symbols carry structured `///` docs. `peer_config_t` owns no input slices; `peer_t.init` copies the version bytes.

```zig
pub const peer_role_t = enum { host, guest };
pub const peer_phase_t = enum { handshaking, ready, draining, closed };
pub const peer_config_t = struct {
    role: peer_role_t,
    token: u64,
    accepted_ms: u64,
    local_version: []const u8,
    local_capabilities: u64,
};
pub const peer_t = struct {
    pub fn init(allocator: std.mem.Allocator, config: peer_config_t) InitError!peer_t;
    pub fn deinit(self: *peer_t) void;
    pub fn phase(self: *const peer_t) peer_phase_t;

    // External mapping boundary. These methods only report results from the
    // caller-owned mapping operation; they do not map/unmap or prove detachment.
    pub fn host_mapping_validated(self: *peer_t, token: u64, now_ms: u64,
        epoch: u64, layout_version: u32, succeeded: bool) StateError!peer_action_t;
    pub fn guest_mapping_initialized(self: *peer_t, token: u64, now_ms: u64,
        succeeded: bool, epoch: u64) StateError!peer_action_t;

    // One complete reader frame is processed/consumed at a time. `feed` returns
    // exact bytes accepted; it never retains the caller's input slice.
    pub fn feed(self: *peer_t, token: u64, now_ms: u64,
        input: []const u8) FeedError!feed_result_t;
    pub fn next_event(self: *peer_t, token: u64, output: []u8) EventError!event_result_t;

    // Queue copies complete frames. TX view borrows only until ack_tx or another
    // queue mutation; callers must send no more than returned.len bytes.
    pub fn enqueue_application(self: *peer_t, token: u64,
        frame: []const u8, facility: facility_id_t) QueueError!void;
    pub fn peek_tx(self: *peer_t, token: u64) TxError!?[]const u8;
    pub fn ack_tx(self: *peer_t, token: u64, now_ms: u64,
        bytes_written: usize) TxError!peer_action_t;
    pub fn tick(self: *peer_t, token: u64, now_ms: u64) StateError!peer_action_t;
    pub fn begin_drain(self: *peer_t, token: u64, now_ms: u64,
        reason: drain_reason_t) StateError!peer_action_t;
    pub fn transport_lost(self: *peer_t, token: u64) StateError!peer_action_t;
};
```

`facility_id_t` is supplied by the feature dispatcher and is checked against the handshake's committed capability intersection before enqueue. Incoming feature frames are delivered with their message type and facility tag to the same policy boundary; the core does not invent a type-to-capability map. The caller cannot enqueue Hello/Ack/Ping/Pong/Error through `enqueue_application`; those are internal control messages. `facility_id_t` must represent only frozen `InteropCapability*` values, with a distinct `none` for common connection control.

`feed_result_t` contains `{ consumed: usize, frame_pending: bool, action: peer_action_t }`. `consumed` is the exact prefix accepted by the reader. Since the reader stores one frame, when `frame_pending` is true the caller must call `next_event` before feeding more data. A zero-consumption result with a pending frame is backpressure, not EOF. EOF is separately reported through `finish_input`/`transport_lost`; it must not be simulated by an empty feed.

`next_event` has a two-call-safe copy-out contract: it reports `required_bytes` and leaves the decoded frame pending if output is short; it consumes the frame only after the full frame/event has been copied or internally handled. No returned pointer aliases the reader. A control frame that is handled internally may return `event_kind = none`, zero copied bytes, and its resulting action. For application frames, the event contains the full encoded frame (including header), message type, request ID, and facility classification; no payload slice escapes.

`peer_action_t` is a value result with `{ phase, ready_changed, terminal, terminal_error, needs_write, ping_due }`. It is advisory state, not proof of a send or teardown. `event_result_t` additionally contains `{ kind, required_bytes, copied_bytes, message_type, request_id, error_code }`. The implementation should use tagged Zig unions internally and translate only fixed-width scalar fields through C.

## Frame dispatch and readiness rules

1. On input, call `reader.feed` and return its exact consumed count. When a complete frame is pending, decode the header and validate the entire payload before accessing fields. Copy all required scalar/string facts before consuming; only `next_event` exposes a copied full application frame.
2. Invalid magic, protocol version, or declared length is a fatal header error. If the error path can enqueue a terminal Error frame, do so best-effort, then request drain. The terminal send is not required to block transport closure indefinitely.
3. Before Ready, only the handshake role/phase dispatcher accepts Hello or HelloAck as applicable. Parse the exact bounded SemVer 2.0 field with `agent_version.parse_major`; do not retain codec slices. Queueing Hello/Ack does not mark readiness. Record their control kind and request ID, and call `hello_sent` / `ack_sent` only when `ack_tx` acknowledges the final byte of that exact complete frame. Readiness events occur only after all other handshake requirements also pass.
4. In Ready, valid Ping is handled internally: call `receive_ping`, enqueue Pong with the same nonzero request ID, then consume. Valid Pong goes to `receive_pong`; release every ID listed by the liveness action back to the one shared allocator. Other valid frames refresh receive liveness and are copied out to the feature owner.
5. A valid received Error is surfaced as an `error_received` event (including code, optional refused type, and copied full frame) and counts as valid traffic for liveness. It does not itself close the connection and the core never replies to an Error. The feature/session owner may interpret codes such as `StaleWindow` as a scoped refusal. A malformed Error payload follows the malformed-payload policy below.
6. On host tick, if liveness says Ping is due, reserve from the same request-ID allocator used by application requests, register the ID with liveness before queueing Ping, then enqueue. If enqueue fails after registration, fail/drain this connection and retire all outstanding IDs only after work is quiesced; do not silently omit the Ping or reuse its ID. On Pong match, independent Ping expiry, or transport loss, release IDs exactly as liveness reports.
7. `begin_drain` performs only protocol/core state transition and best-effort terminal queueing. It never frees a mapping. `transport_lost` invalidates the token, retires the allocator only after all peer work is quiesced, and tells the outer owner the transport ended; it is not an OS/shared-memory detach proof.

## Backpressure and terminal behavior

The send queue copies every accepted complete frame and preserves FIFO order and its reserved control capacity. Application queue admission failure is returned synchronously and leaves the caller frame and peer state unchanged. The platform loop calls `ack_tx` only for bytes the OS confirmed sent; zero-byte send is no progress, and an error is handled as transport failure. It must not acknowledge requested length or a partial write that the OS did not report.

Failure to enqueue a mandatory handshake response or Pong cannot be treated as success. For Ack failure, move to draining and do not report Ready. For Pong failure, close/drain rather than acknowledge the Ping while omitting its response. Error/Goodbye are best-effort terminal frames: enqueue only from control capacity; if that also fails, report terminal send unavailable and allow bounded caller teardown. A terminal frame's partial send does not grant any cleanup proof. A local Error sender must not recursively answer a received Error.

## Error policy requiring lead freeze

- **Frozen/header-fatal:** malformed header magic/version/length is fatal; best-effort `Error{ProtocolViolation}` then drain. This is explicit in §3.2.1.
- **Body validation is not fully composable today:** codec validation distinguishes `MalformedFrame`, `UnsupportedField`, and `UnsupportedType`; §6.1 says any fatal codec error closes, while §3.2.7 says use `UnsupportedType` for an unnegotiated facility. The codec currently cannot distinguish unknown message type, wrong-direction type, and known type rejected solely for an unnegotiated facility without adding a policy table. Lead must freeze whether (a) all malformed/unsupported payloads send corresponding Error and drain, with a pre-validation capability classifier for known facility frames, or (b) only malformed frames drain while unsupported type/field errors are surfaced/rejected by the feature owner. Do not implement an implicit hybrid.
- **Ready Error:** valid peer Error is a scoped diagnostic/refusal event; never automatic universal disconnect and never answer with Error. Terminal handling of `ProtocolViolation`/`MalformedFrame` codes received from a peer remains owner policy; the core reports code and phase.
- **Goodbye:** no reply should be generated automatically. Event delivery followed by owner-requested drain is the conservative choice, but whether inbound Goodbye must have `request_id=0` and whether it immediately drains needs lead confirmation.
- **Error correlation:** frozen rules do not specify whether Error echoes the triggering request ID or is unsolicited. The core should preserve source request ID in a pending error context only after this is frozen. It must not fabricate a response ID or use zero without an explicit endpoint rule.
- **Capability policy:** use the handshake's negotiated intersection, but the message-to-capability table and compound dependencies remain uncommitted (see `/tmp/seamless_peer_message_policy.md`). Until frozen, external dispatcher authorization is mandatory and `enqueue_application` should require an explicit facility argument.

## Future narrow C ABI

Use one opaque `waddle_interop_peer_t *`; no Zig slices, allocator handles, unions, or borrowed reader pointers cross C. All calls are serialized. Each operation receives the captured `uint64_t connection_token`; stale tokens return a fixed `WaddleInteropPeerStaleConnection` result with outputs unchanged. All lengths use `size_t`, timestamps use monotonic `uint64_t` milliseconds, and wire values retain frozen-width integers.

```c
typedef struct waddle_interop_peer waddle_interop_peer_t;
typedef struct waddle_interop_peer_config {
    uint32_t role;
    uint32_t reserved_zero;
    uint64_t connection_token;
    uint64_t accepted_ms;
    const uint8_t *agent_version;
    size_t agent_version_bytes;
    uint64_t local_capabilities;
} waddle_interop_peer_config_t;
typedef struct waddle_interop_mapping_result {
    uint32_t operation; /* host validation or guest initialization */
    uint32_t succeeded;
    uint32_t layout_version;
    uint32_t reserved_zero;
    uint64_t session_epoch;
} waddle_interop_mapping_result_t;
typedef struct waddle_interop_peer_event {
    uint32_t kind;
    uint32_t message_type;
    uint32_t request_id;
    uint32_t error_code;
    uint32_t required_bytes;
    uint32_t copied_bytes;
    uint32_t phase;
    uint32_t flags;
} waddle_interop_peer_event_t;

int waddle_interop_peer_create(const waddle_interop_peer_config_t *config,
    waddle_interop_peer_t **out_peer);
int waddle_interop_peer_mapping_result(waddle_interop_peer_t *peer,
    uint64_t token, uint64_t now_ms, const waddle_interop_mapping_result_t *result);
int waddle_interop_peer_feed(waddle_interop_peer_t *peer, uint64_t token,
    uint64_t now_ms, const uint8_t *input, size_t input_bytes,
    size_t *out_consumed, waddle_interop_peer_event_t *out_event);
int waddle_interop_peer_next_event(waddle_interop_peer_t *peer, uint64_t token,
    uint8_t *output, size_t output_capacity, waddle_interop_peer_event_t *out_event);
int waddle_interop_peer_enqueue(waddle_interop_peer_t *peer, uint64_t token,
    uint32_t facility, const uint8_t *frame, size_t frame_bytes);
int waddle_interop_peer_peek_tx(waddle_interop_peer_t *peer, uint64_t token,
    const uint8_t **out_bytes, size_t *out_length);
int waddle_interop_peer_ack_tx(waddle_interop_peer_t *peer, uint64_t token,
    uint64_t now_ms, size_t bytes_written, waddle_interop_peer_event_t *out_event);
int waddle_interop_peer_tick(waddle_interop_peer_t *peer, uint64_t token,
    uint64_t now_ms, waddle_interop_peer_event_t *out_event);
int waddle_interop_peer_begin_drain(waddle_interop_peer_t *peer, uint64_t token,
    uint64_t now_ms, uint32_t reason, waddle_interop_peer_event_t *out_event);
void waddle_interop_peer_destroy(waddle_interop_peer_t *peer);
```

The C header needs structured Doxygen ownership/nullability/thread/lifetime/error docs. `feed` permits `(input == NULL && input_bytes == 0)` only as a no-op (use a separate EOF method); otherwise input must be non-NULL. Event/output pointers must be non-NULL and disjoint from input. `agent_version` must be non-NULL and 5–32 bytes. Mapping operation must match role; all reserved fields must be zero. These validation errors leave peer state and caller outputs unchanged. `peek_tx` is the sole borrowed pointer and is valid only until the next mutating call or ack; prefer a `copy_tx` ABI if callers cannot guarantee this lifetime. Output fields must remain unchanged on API errors. `create` copies configuration bytes before returning and sets `*out_peer = NULL` on failure. `destroy` frees only peer-core allocations after caller has stopped callbacks; it performs no socket or mapping operation.

## Remaining lead decisions

1. Freeze malformed/unsupported payload and unnegotiated-facility policy, including the classification order needed to emit `UnsupportedType` without retaining malformed data.
2. Freeze Goodbye request-ID/close semantics and Error correlation for locally generated errors.
3. Freeze message-to-capability mapping and multi-capability dependencies, or keep strict explicit facility authorization outside the core until a table is approved.
4. Decide whether `peek_tx` borrowed pointer is acceptable for both C platform loops or expose copy-out only.
5. Define the bounded outstanding request-ID capacity/admission behavior across future application request modules; the core must share, not duplicate, the allocator.

No wire field or message type is added. No OS detach proof, mapping-release rule, GuestReady claim, capability implementation, or native guest acceptance is implied by this composition.
