#ifndef WaddleInteropPeerH
#define WaddleInteropPeerH
/**
 * @file interop_peer.h
 * @brief Opaque C boundary for one connected protocol-v2 control peer.
 *
 * One serialized event-loop owner calls every operation. The peer owns copied
 * version/queue bytes only, never a socket, mapping, callback or detach proof.
 * Caller supplies accessible buffers; nonempty ranges must not wrap uintptr_t.
 * All arguments/output ranges must be disjoint from each other and peer-owned
 * storage. NULL byte buffers are allowed only with zero length/capacity. Scalar
 * and structure outputs are non-NULL and naturally aligned. Local argument
 * errors preserve state and all outputs. InternalInvariant preserves outputs
 * but requires the caller to abort/quiesce I/O and destroy the peer.
 *
 * A successful call can report Draining/Closed: these describe the connection,
 * never native memory release. Ready grants negotiated connection status only;
 * copied feature frames still need external capability/operation authorization.
 * Actual OS send counts must be acknowledged before any next send. No borrowed
 * pointer crosses this interface. All returned bytes belong to the caller.
 */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque allocation owned until destroy; never copy or forge its address. */
typedef struct interop_peer interop_peer_t;
/** @brief Stable C operation result; protocol errors instead appear in events. */
typedef enum interop_peer_result_t {
    InteropPeerSuccess = 0, /**< Operation completed; inspect event phase/flags. */
    InteropPeerInvalidArgument = 1, /**< Null/alignment/range/config/alias error. */
    InteropPeerOutOfMemory = 2, /**< Creation could not allocate; no owner returned. */
    InteropPeerWrongToken = 3, /**< Delayed callback belongs to another connection. */
    InteropPeerTimeWentBackwards = 4, /**< Caller must repair timestamp before retry. */
    InteropPeerWrongPhase = 5, /**< Operation is invalid for current role/phase. */
    InteropPeerOutputTooSmall = 6, /**< Query size then retry; nothing consumed/copied. */
    InteropPeerInvalidWriteCount = 7, /**< Count exceeds actual front remainder. */
    InteropPeerNoQueuedFrame = 8, /**< Cannot acknowledge an empty TX queue. */
    InteropPeerInternalInvariant = 9, /**< Owned invariant failed; abort/quiesce. */
    InteropPeerMappingFailed = 10, /**< Guest initialization false; begin drain. */
    InteropPeerClosed = 11 /**< Destroyed core; no further operation is valid. */
} interop_peer_result_t;
/** @brief Local role, fixed at create; not a wire enum. */
typedef enum interop_peer_role_t {
    InteropPeerHost = 1, /**< Receives Hello; validates mapping before Ack. */
    InteropPeerGuest = 2 /**< Initializes mapping then sends Hello. */
} interop_peer_role_t;
/** @brief Stable C phase values; no phase is a native teardown proof. */
typedef enum interop_peer_phase_t {
    InteropPeerAwaitHello = 0, /**< Host awaits complete Hello. */
    InteropPeerAwaitMapping = 1, /**< Host awaits external validation callback. */
    InteropPeerAwaitHelloAckSent = 2, /**< Host awaits final Ack send byte. */
    InteropPeerInitializingMapping = 3, /**< Guest awaits initialization callback. */
    InteropPeerAwaitHelloSent = 4, /**< Guest awaits final Hello send byte. */
    InteropPeerAwaitHelloAck = 5, /**< Guest awaits matching complete Ack. */
    InteropPeerReady = 6, /**< Negotiated connection; facilities remain external. */
    InteropPeerDraining = 7, /**< Admission ended; retain uncertain resources. */
    InteropPeerTransportClosed = 8 /**< I/O quiesced; peer allocations still owned. */
} interop_peer_phase_t;
/** @brief Copied event kind with fixed meanings in IMPL_DESC section8.20. */
typedef enum interop_peer_event_kind_t {
    InteropPeerEventNone = 0, /**< No outward operation required. */
    InteropPeerEventValidateMapping = 1, /**< Validate copied epoch/layout externally. */
    InteropPeerEventReady = 2, /**< This call first committed negotiation readiness. */
    InteropPeerEventFeatureFrame = 3, /**< Full frame copied; authorize externally. */
    InteropPeerEventError = 4, /**< Scoped remote Error copied; never auto-replied. */
    InteropPeerEventDraining = 5 /**< Connection terminal; no resource proof. */
} interop_peer_event_kind_t;
/** @brief Event flag requiring immediate transport abort, not fresh TX. */
#define InteropPeerAbortTransport 1u
/** @brief Event flag indicating a best-effort Error/Goodbye could not be queued. */
#define InteropPeerTerminalSendUnavailable 2u
/** @brief Valid event flag bits. */
#define InteropPeerEventFlagsMask 3u

/** @brief Borrowed create inputs; complete version bytes copied before return. */
typedef struct interop_peer_config_t {
    uint32_t role; /**< interop_peer_role_t, Host or Guest only. */
    uint32_t reserved_zero; /**< Must be zero. */
    uint64_t connection_token; /**< Nonzero unique connection callback token. */
    uint64_t accepted_at_ms; /**< Monotonic acceptance timestamp. */
    const uint8_t *agent_version; /**< Non-NULL accessible 5..32 SemVer2 bytes. */
    size_t agent_version_bytes; /**< Exact byte length; no trailing NUL required. */
    uint64_t local_capabilities; /**< Known, actually implemented facility offer. */
} interop_peer_config_t;
/** @brief Copied scalar event; all unused fields are zero on success. */
typedef struct interop_peer_event_t {
    uint32_t kind; /**< interop_peer_event_kind_t. */
    uint32_t phase; /**< interop_peer_phase_t after operation. */
    uint32_t flags; /**< InteropPeerEventFlagsMask bits only. */
    uint32_t error_code; /**< Frozen protocol error or zero; no native OS errno. */
    uint64_t session_epoch; /**< Staged/ready epoch for ValidateMapping/Ready. */
    uint64_t capabilities; /**< Negotiated intersection for Ready only. */
    uint32_t layout_version; /**< Required version on ValidateMapping. */
    uint32_t message_type; /**< Copied frame type or zero. */
    uint32_t request_id; /**< Exact copied frame request ID or zero. */
    uint32_t reserved_zero; /**< Always zero. */
    size_t copied_bytes; /**< Bytes written to caller frame output. */
} interop_peer_event_t;

/** @brief Create one pure connected peer.
 * @param[in] config Non-NULL aligned accessible config; borrowed during call.
 * @param[out] out_peer Non-NULL aligned slot containing NULL; unchanged on failure.
 * @return interop_peer_result_t; Success transfers one opaque owner to caller.
 * @note Thread-safe for distinct instances; destroy releases all allocations.
 */
int interop_peer_create(const interop_peer_config_t *config, interop_peer_t **out_peer);
/** @brief Destroy after all I/O callbacks and calls are quiesced.
 * @param[in,out] peer Non-NULL aligned owner slot; NULL value is an idempotent no-op.
 * @return Success or InvalidArgument; success deinitializes/frees and NULLs owner.
 * @note Serialized. Does not close sockets or infer native/shared-memory drain.
 */
int interop_peer_destroy(interop_peer_t **peer);
/** @brief Snapshot current phase/flags without mutation.
 * @param[in] peer Non-NULL live owner; borrowed during call.
 * @param[in] token Captured nonzero connection token.
 * @param[out] event Non-NULL event; unchanged on error, kindNone on success.
 * @return interop_peer_result_t. Serialized; no memory transfer.
 */
int interop_peer_get_state(const interop_peer_t *peer, uint64_t token, interop_peer_event_t *event);
/** @brief Feed at most one frame; an empty range is a no-op, never EOF.
 * @param[in,out] peer Non-NULL live serialized owner.
 * @param[in] token Captured connection token.
 * @param[in] bytes Borrowed accessible bytes, NULL only for length0.
 * @param[in] length Exact accessible input length, no wrap or peer alias.
 * @param[out] consumed Non-NULL exact copied prefix length; unchanged on API error.
 * @param[out] event Non-NULL framing phase/error/flags; unchanged on API error.
 * @return interop_peer_result_t; header error is Success plus Draining event.
 * @note No pointers retained; outputs disjoint from input and each other.
 */
int interop_peer_feed(interop_peer_t *peer, uint64_t token, const uint8_t *bytes, size_t length, size_t *consumed, interop_peer_event_t *event);
/** @brief Query complete pending RX frame length for copy-out retry.
 * @param[in] peer Non-NULL live serialized owner; no mutation.
 * @param[in] token Captured token.
 * @param[out] length Non-NULL; full pending header+payload bytes or0 on success.
 * @return interop_peer_result_t; output unchanged on error, no ownership transfer.
 */
int interop_peer_pending_bytes(const interop_peer_t *peer, uint64_t token, size_t *length);
/** @brief Dispatch one complete frame, copying feature/Error/Goodbye bytes only.
 * @param[in,out] peer Non-NULL live serialized owner.
 * @param[in] token Captured token. @param[in] now_ms Nondecreasing monotonic time.
 * @param[out] bytes Caller-owned accessible capacity bytes; NULL only for capacity0.
 * @param[in] capacity Output capacity; disjoint from peer and event.
 * @param[out] event Non-NULL scalar event; unchanged on error.
 * @return interop_peer_result_t; OutputTooSmall preserves buffer, clocks and RX.
 * @note Query pending_bytes then retry; caller retains all output memory.
 */
int interop_peer_next_event(interop_peer_t *peer, uint64_t token, uint64_t now_ms, uint8_t *bytes, size_t capacity, interop_peer_event_t *event);
/** @brief Report external mapping initialization/validation without owning mapping.
 * @param[in,out] peer Non-NULL live serialized owner.
 * @param[in] token Captured token. @param[in] now_ms Nondecreasing monotonic time.
 * @param[in] epoch Exact initialized/validated epoch.
 * @param[in] layout_version Actual validated version (Guest requires current version).
 * @param[in] succeeded Exactly0 or1; false guest result is MappingFailed unchanged.
 * @param[out] event Non-NULL scalar event; unchanged on API error.
 * @return interop_peer_result_t; Host failure is Success plus Draining, not proof.
 * @note Operation chosen by configured role. No mapping pointer is accepted.
 */
int interop_peer_mapping_result(interop_peer_t *peer, uint64_t token, uint64_t now_ms, uint64_t epoch, uint32_t layout_version, uint32_t succeeded, interop_peer_event_t *event);
/** @brief Query remaining bytes offered for next OS send.
 * @param[in] peer Non-NULL live serialized owner; no mutation.
 * @param[in] token Captured token.
 * @param[out] length Non-NULL;0 for empty queue or abort-required Draining.
 * @return interop_peer_result_t; output unchanged on error, no borrowed pointer.
 */
int interop_peer_tx_bytes(const interop_peer_t *peer, uint64_t token, size_t *length);
/** @brief Copy entire remaining front TX frame without acknowledging any byte.
 * @param[in] peer Non-NULL live serialized owner; no mutation.
 * @param[in] token Captured token.
 * @param[out] bytes Caller-owned accessible capacity bytes, NULL only for capacity0.
 * @param[in] capacity Disjoint buffer capacity.
 * @param[out] length Non-NULL copied length,0 if no bytes offered.
 * @return interop_peer_result_t; short output preserves buffer/length/state.
 * @note Query tx_bytes then retry. Caller owns copy; only real send result is ACKed.
 */
int interop_peer_copy_tx(const interop_peer_t *peer, uint64_t token, uint8_t *bytes, size_t capacity, size_t *length);
/** @brief Account bytes already accepted by the OS, even at expiry or local drain.
 * @param[in,out] peer Non-NULL live serialized owner.
 * @param[in] token Captured token. @param[in] now_ms Nondecreasing monotonic time.
 * @param[in] written Actual successful OS byte count, no more than front remainder.
 * @param[out] event Non-NULL event; unchanged on API error.
 * @return interop_peer_result_t; repair invalid local ACK before any next send.
 * @note Full-send readiness hooks suppressed after expiry/drain; no native proof.
 */
int interop_peer_ack_tx(interop_peer_t *peer, uint64_t token, uint64_t now_ms, size_t written, interop_peer_event_t *event);
/** @brief Advance handshake/liveness timers and enqueue due control traffic.
 * @param[in,out] peer Non-NULL live serialized owner.
 * @param[in] token Captured token. @param[in] now_ms Nondecreasing monotonic time.
 * @param[out] event Non-NULL scalar event; unchanged on API error.
 * @return interop_peer_result_t; timeout is Success plus Draining, never detach.
 */
int interop_peer_tick(interop_peer_t *peer, uint64_t token, uint64_t now_ms, interop_peer_event_t *event);
/** @brief Close protocol admission and request bounded transport cleanup.
 * @param[in,out] peer Non-NULL live serialized owner.
 * @param[in] token Captured token. @param[in] now_ms Nondecreasing monotonic time.
 * @param[out] event Non-NULL scalar event; unchanged on API error.
 * @return interop_peer_result_t; inspect AbortTransport before sending more bytes.
 * @note Best-effort Goodbye is not a native/shared-memory drain receipt.
 */
int interop_peer_begin_drain(interop_peer_t *peer, uint64_t token, uint64_t now_ms, interop_peer_event_t *event);
/** @brief Mark transport Closed only after native callbacks are quiesced.
 * @param[in,out] peer Non-NULL live serialized owner; allocations remain owned.
 * @param[in] token Captured token; repeated terminal callback is idempotent.
 * @param[out] event Non-NULL scalar event; unchanged on API error.
 * @return interop_peer_result_t; no socket/mapping operation or detach proof.
 */
int interop_peer_transport_lost(interop_peer_t *peer, uint64_t token, interop_peer_event_t *event);
#ifdef __cplusplus
}
#endif
#endif
