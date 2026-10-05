/** @file venus_session.h @brief Bounded control codec and readiness handoff. */
#ifndef WaddleVenusSessionH
/** @brief Compile-time include guard; no storage or lifetime. */
#define WaddleVenusSessionH
#include "venus_region.h"

/** @brief Exact lifecycle frame length, encoded little-endian field-by-field. */
#define VenusControlBytes 64u
/** @brief Lifecycle wire identity; distinct from CLI, AV, and region identities. */
#define VenusControlMagic 0x57564331u

/** @brief Session-thread role; no ownership or shared representation. */
typedef enum venus_session_role_t {
    SessionHost = 1, /**< Initializes the region and initiates Offer. */
    SessionGuest = 2 /**< Validates the mapped region and acknowledges Offer. */
} venus_session_role_t;
/** @brief Control message kinds; wire u32, only listed values are valid. */
typedef enum venus_control_kind_t {
    ControlOffer = 1, /**< Host advertises initialized region and session ID. */
    ControlAck = 2,   /**< Guest confirms the exact advertised layout. */
    ControlReady = 3, /**< Host commits readiness after acknowledgement. */
    ControlStop = 4   /**< Either peer closes both rings with a nonzero reason. */
} venus_control_kind_t;
/** @brief Terminal reason; first close reason is retained, encoded u32. */
typedef enum venus_stop_reason_t {
    StopNone = 0,       /**< Required on non-Stop frames; local session still open. */
    StopDisconnect = 1, /**< Control stream EOF or orderly shutdown. */
    StopCancel = 2,     /**< Local or peer cancellation. */
    StopDeadline = 3,   /**< Deadline expired without completing the operation. */
    StopProtocol = 4    /**< Malformed, stale, or out-of-order control message. */
} venus_stop_reason_t;
/** @brief Session-thread-only local state; never placed in shared memory. */
typedef enum venus_session_state_t {
    SessionNone = 0,         /**< Zero record, not attached or initialized. */
    SessionInitialized = 1,  /**< Both rings validated; payload workers stopped. */
    SessionOffering = 2,     /**< Host encoded Offer; awaiting Ack. */
    SessionAccepted = 3,     /**< Guest accepted Offer; must encode Ack. */
    SessionWaiting = 4,      /**< Guest encoded Ack; awaiting Ready. */
    SessionAcknowledged = 5, /**< Host accepted Ack; must encode Ready. */
    SessionReady = 6,        /**< Valid handoff complete; payload traffic permitted. */
    SessionClosed = 7        /**< Terminal; rings closed, workers must be joined. */
} venus_session_state_t;

/** @brief Private decoded fields, not a native wire/shared-memory struct.
 * @note Caller-owned, no allocation or handles. Encode/decode are pure and
 * thread-safe on disjoint outputs; all integers have fixed-width C ABI types.
 */
typedef struct venus_control_t {
    uint32_t kind;          /**< Valid venus_control_kind_t value. */
    uint32_t reason;        /**< Zero except Stop; valid venus_stop_reason_t value. */
    uint64_t session_id;    /**< Nonzero host-generated session identity. */
    uint64_t mapping_bytes; /**< Validated region extent, 4096..one GiB. */
    uint32_t capacity;      /**< Validated ring capacity; both rings fit extent. */
} venus_control_t;

/** @brief Lifecycle-owned local handoff record with a borrowed region view.
 * @note Caller-owned zero record, no heap/handle ownership. Session-thread-only:
 * all fields are read-only to callers. Do not copy or concurrently mutate/use.
 * Mapping outlives the record and all payload workers; close before joining.
 */
typedef struct venus_session_t {
    venus_region_view_t region;  /**< Borrowed attached rings/resource arena. */
    uint64_t mapping_bytes;      /**< Validated local declared extent snapshot. */
    uint64_t session_id;         /**< Host identity or zero before guest receives Offer. */
    venus_session_role_t role;   /**< Immutable local role. */
    venus_session_state_t state; /**< Session-thread transition state. */
    venus_stop_reason_t reason;  /**< First terminal reason, otherwise StopNone. */
} venus_session_t;

/** @brief Decode exactly one privately accumulated lifecycle frame.
 * @param[out] control Nonnull private output, disjoint from input, zeroed on error.
 * @param[in] frame Nonnull borrowed immutable length-byte private buffer.
 * @param[in] length Exactly VenusControlBytes, otherwise invalid.
 * @return RingOk or RingCorrupt for wire input, RingInvalid for null arguments.
 * @note Bounds-checked Zig, allocation-free; thread-safe on disjoint outputs.
 */
venus_ring_status_t venus_control_decode(venus_control_t *control, const void *frame,
                                         size_t length);
/** @brief Encode validated fields into exactly one lifecycle frame.
 * @param[in] control Nonnull immutable private fields, disjoint from output.
 * @param[out] frame Nonnull caller-owned length-byte output, unchanged on error.
 * @param[in] length Exactly VenusControlBytes; other lengths rejected.
 * @return RingOk or RingInvalid; invalid fields never modify output.
 * @note Bounds-checked Zig, allocation-free; thread-safe on disjoint outputs.
 */
venus_ring_status_t venus_control_encode(const venus_control_t *control, void *frame,
                                         size_t length);
/** @brief Attach a fresh open region without initializing or resetting it.
 * @param[out] session Nonnull zero caller-owned record, disjoint from mapping;
 * failed initialization leaves it zero. Existing live records must not be reused.
 * @param[in] role SessionHost or SessionGuest.
 * @param[in,out] mapping Nonnull borrowed coherent aligned initialized mapping.
 * @param[in] length Actual accessible bytes; declared region must fit.
 * @param[in] session_id Nonzero fresh identity for host, zero for guest.
 * @return RingOk, RingInvalid, or attachment failure. Cursors must all be zero.
 * @note Session-thread-only, both peers quiescent; no allocation or ownership
 * transfer. Payload traffic remains prohibited until handoff completes.
 */
venus_ring_status_t venus_session_init(venus_session_t *session, venus_session_role_t role,
                                       void *mapping, size_t length, uint64_t session_id);
/** @brief Encode a legal local startup/Stop transition and advance its state.
 * @param[in,out] session Nonnull initialized caller-owned handoff record.
 * @param[in] kind Offer/Ack/Ready at its legal phase, or Stop with a known ID.
 * @param[in] reason StopNone except Stop, which requires a terminal reason.
 * @param[out] frame Nonnull disjoint caller-owned private output[length].
 * @param[in] length Exactly VenusControlBytes.
 * @return RingOk or RingInvalid; local errors preserve state and output.
 * @note Session-thread-only. After encoding, send the complete frame before
 * starting workers; send failure MUST close. Stop closes both rings immediately.
 */
venus_ring_status_t venus_session_send(venus_session_t *session, venus_control_kind_t kind,
                                       venus_stop_reason_t reason, void *frame, size_t length);
/** @brief Accept a peer transition after exact identity/layout/state checks.
 * @param[in,out] session Nonnull initialized local record, disjoint from input.
 * @param[in] frame Nonnull privately accumulated immutable frame[length].
 * @param[in] length Exact received bytes; malformed lengths are fatal.
 * @return RingOk; RingClosed for valid Stop/closed session; RingCorrupt for
 * protocol failure (closes both rings); RingInvalid for null/uninitialized args.
 * @note Session-thread-only, no allocations. Duplicate/stale frames are fatal.
 */
venus_ring_status_t venus_session_receive(venus_session_t *session, const void *frame,
                                          size_t length);
/** @brief Close both rings and retain the first terminal reason.
 * @param[in,out] session Nullable local record; null/zero/already closed is a no-op.
 * @param[in] reason Valid nonzero stop reason; invalid value becomes StopProtocol.
 * @note Session-thread-only; rings close atomically but an in-flight transfer may
 * finish. Joins, detach, stream closure, and mapping free belong to lifecycle owner.
 */
void venus_session_close(venus_session_t *session, venus_stop_reason_t reason);
/** @brief Test whether the startup handoff completed successfully.
 * @param[in] session Nullable borrowed record; null/zero/closed returns zero.
 * @return One only in SessionReady; no mutation, allocation, or ownership transfer.
 * @note Session-thread-only, not a synchronization primitive for worker threads.
 */
int venus_session_ready(const venus_session_t *session);
#endif
