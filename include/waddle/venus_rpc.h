/** @file venus_rpc.h @brief Sequential bounded guest/host ring exchanges. */
#ifndef WaddleVenusRpcH
/** @brief Include guard, no ownership or storage. */
#define WaddleVenusRpcH
#include "venus_channel.h"
#include "venus_request.h"
/** @brief Session-thread record; all fields read-only to callers.
 * @note Borrows ready channel and disjoint private buffer; no allocation. Never
 * copy/use concurrently. All borrowed storage outlives calls and free.
 */
typedef struct venus_rpc_t {
    venus_channel_t *channel; /**< Borrowed ready lifecycle and ring owner. */
    unsigned char *buffer;    /**< Borrowed exclusive private scratch. */
    uint32_t buffer_bytes;    /**< Immutable actual private buffer extent. */
    uint64_t next_sequence;   /**< Next exchange ID; zero means terminal. */
} venus_rpc_t;
/** @brief Initialize framing after complete readiness delivery.
 * @param[out] rpc Nonnull zero private record, unchanged on error.
 * @param[in,out] channel Nonnull initialized ready borrowed channel.
 * @param[in,out] buffer Nonnull exclusive private buffer[bytes], disjoint from
 * mapping and all records. Borrowed until free, overwritten during exchanges.
 * @param[in] bytes Actual accessible private extent, 160..16MiB.
 * @return RingOk or RingInvalid; no allocation or ownership transfer.
 * @note Sole session thread; valid for host or guest roles.
 */
venus_ring_status_t venus_rpc_init(venus_rpc_t *rpc, venus_channel_t *channel, void *buffer,
                                   uint32_t bytes);
/** @brief Close the borrowed session and clear local fields.
 * @param[in,out] rpc Nullable zero/live record; already freed is a no-op.
 * @note Sole session thread after calls stop; never frees buffer/receiver/channel,
 * closes a native handle or unmaps. Lifecycle owner then releases those resources.
 */
void venus_rpc_free(venus_rpc_t *rpc);
/** @brief Publish a request and acquire an exact private response.
 * @param[in,out] rpc Nonnull live guest framing record, session-thread-only.
 * @param[in] request Nonnull immutable host-value request with sequence zero.
 * @param[in] input Nullable only when length zero, immutable private input[length].
 * @param[in] length Exactly request payload_bytes; must fit private scratch.
 * @param[out] response Nonnull disjoint record, zeroed on failure; wire status
 * describes operation outcome when return is RingOk.
 * @param[out] output Nullable only when no response payload expected; private
 * output[capacity], unchanged on failure, disjoint from records and mapping.
 * @param[in] capacity Actual output bytes, enough for full successful response.
 * @param[in] timeout_ms One entire exchange deadline, 1..60000 milliseconds.
 * @return RingOk for valid delivered response; RingInvalid for local arguments
 * without publication; terminal protocol/channel statuses close both rings.
 * @note Input/output may alias each other; both disjoint from scratch/records.
 * No allocation. Return does not imply GPU completion; retry uses a new sequence.
 */
venus_ring_status_t venus_rpc_exchange(venus_rpc_t *rpc, const venus_request_t *request,
                                       const void *input, size_t length, venus_request_t *response,
                                       void *output, size_t capacity, uint32_t timeout_ms);
#endif
