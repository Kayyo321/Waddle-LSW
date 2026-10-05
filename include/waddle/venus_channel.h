/** @file venus_channel.h @brief Deadline-aware native lifecycle byte stream. */
#ifndef WaddleVenusChannelH
/** @brief Compile-time include guard; no storage or ownership. */
#define WaddleVenusChannelH
#include "venus_session.h"
#include "venus_wait.h"

/** @brief Session-thread-owned channel; fields read-only to callers.
 * @note Borrows stream/session/cancellation. Owns only Windows event; zero init,
 * free after calls finish. Never copy or concurrently use. Mapping/session and
 * optional atomic cancellation flag outlive the channel and all operations.
 */
typedef struct venus_channel_t {
    venus_session_t *session; /**< Borrowed initialized lifecycle state. */
    intptr_t stream; /**< Borrowed Linux nonblocking socket or Windows overlapped HANDLE. */
    intptr_t event;  /**< Owned Windows manual-reset completion event; zero on Linux. */
    const _Atomic uint32_t *cancel;      /**< Nullable borrowed atomic release-set cancellation. */
    uint64_t deadline_ms;                /**< Absolute monotonic deadline; configure before I/O. */
    uint8_t incoming[VenusControlBytes]; /**< Private partial control frame, never shared. */
    size_t received;                     /**< Private accumulated bytes, 0..VenusControlBytes. */
    int initialized;                     /**< Native validation/event acquisition completed. */
} venus_channel_t;

/** @brief Borrow a connected native control stream and acquire local event state.
 * @param[out] channel Nonnull zero caller-owned record; failure leaves zero.
 * @param[in,out] session Nonnull initialized quiescent borrowed lifecycle record.
 * @param[in] stream Linux nonblocking SOCK_STREAM fd; Windows valid handle opened
 * with FILE_FLAG_OVERLAPPED. Caller retains and closes it after channel free.
 * @param[in] cancel Nullable borrowed atomic flag, nonzero means cancellation.
 * @return RingOk or RingInvalid for arguments/native validation/event failure.
 * @note Session-thread-only, no stream mutation/ownership transfer. Windows may
 * allocate one kernel event, released by venus_channel_free. No payload workers yet.
 */
venus_ring_status_t venus_channel_init(venus_channel_t *channel, venus_session_t *session,
                                       intptr_t stream, const _Atomic uint32_t *cancel);
/** @brief Set one deadline for a complete handshake/stop/exact ring operation.
 * @param[in,out] channel Nonnull initialized borrowed record, no pending calls.
 * @param[in] timeout_ms Relative 1..60000 milliseconds, monotonic and overflow checked.
 * @return RingOk, RingInvalid for arguments, RingClosed for clock failure.
 * @note Session-thread-only; must not extend a partially received control frame.
 * Does not reset cancellation or reopen a terminal session.
 */
venus_ring_status_t venus_channel_deadline(venus_channel_t *channel, uint32_t timeout_ms);
/** @brief Complete Offer/Ack/Ready delivery according to the local session role.
 * @param[in,out] channel Nonnull initialized record with a configured deadline.
 * @return RingOk only after complete readiness delivery; RingInvalid for local
 * state, RingCorrupt for invalid/partial EOF, RingClosed for disconnect, or
 * RingCancelled/RingTimeout. All transport failures close both rings.
 * @note Sole session thread; no payload workers until this returns RingOk.
 */
venus_ring_status_t venus_channel_handshake(venus_channel_t *channel);
/** @brief Bounded lifecycle wait callback for an exact ring transfer.
 * @param[in,out] context Nonnull initialized ready venus_channel_t, caller-owned.
 * @return RingOk after a bounded readiness slice; terminal protocol/stream/
 * cancellation/deadline status otherwise, closing both rings before return.
 * @note Use only on the same owning session thread; max one-ms poll per tick.
 * Privately accumulates partial control frames without resetting deadline.
 */
venus_ring_status_t venus_channel_wait(void *context);
/** @brief Close rings and deliver a complete Stop frame within the current deadline.
 * @param[in,out] channel Nonnull initialized record with known session identity.
 * @param[in] reason Nonzero valid stop reason, retained even if delivery fails.
 * @return RingOk or state/transport/deadline/cancellation error.
 * @note Sole session thread; closes rings before send, no implicit stream closure.
 */
venus_ring_status_t venus_channel_stop(venus_channel_t *channel, venus_stop_reason_t reason);
/** @brief Close session, release native event, and clear local borrowed state.
 * @param[in,out] channel Nullable record; zero/already freed is a no-op.
 * @note Sole session thread after all calls finish; does not close borrowed stream
 * or join/unmap users. Lifecycle owner releases those in documented order.
 */
void venus_channel_free(venus_channel_t *channel);
#endif
