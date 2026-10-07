/** @file venus_command.h @brief Exclusive private serialized guest command/reply staging. */
#ifndef WaddleVenusCommandH
/** @brief Include guard, no storage or ownership. */
#define WaddleVenusCommandH
#include "venus_request.h"
/** @brief Borrow a negotiated frontend for one request; no retained payload.
 * @param[in,out] context Nonnull borrowed frontend, exclusive through take/free.
 * @param[in] request Nonnull immutable sequence-zero envelope.
 * @param[in] input Nullable for length zero, otherwise private input[length].
 * @param[in] length Exact request payload extent.
 * @param[out] response Nonnull decoded result, exact successful response shape.
 * @param[out] output Nullable for capacity zero, otherwise private output[capacity].
 * @param[in] capacity Exact expected successful reply extent.
 * @return Frontend ring status, including decoded operation status.
 * @note Sole session thread; callback must not reenter owner or retain pointers.
 */
typedef venus_ring_status_t (*venus_command_exchange_t)(void *context,
                                                        const venus_request_t *request,
                                                        const void *input, size_t length,
                                                        venus_request_t *response, void *output,
                                                        size_t capacity);
/** @brief Exclusive owner phases, no allocation or ownership transfer. */
typedef enum venus_command_state_t {
    CommandIdle = 0,      /**< No outstanding submission; start permitted. */
    CommandSubmitted = 1, /**< CPU submission accepted; poll permitted. */
    CommandReady = 2,     /**< Private reply available; take permitted once. */
    CommandLost = 3,      /**< Sticky failure; abandon old receiver session. */
    CommandReading = 4    /**< Completed CPU reply, assembling bounded byte ranges. */
} venus_command_state_t;
/** @brief Caller-owned sole-thread owner, never copy or mutate live fields.
 * @note Allocation-free; all pointers borrowed until free. No GPU completion.
 * Rebuild every consumer together: this native owner ABI adds a byte cursor;
 * no owner record is serialized or shared with another process. Reading owns
 * the accepted CPU fence and private partial reply until take/abandonment.
 */
typedef struct venus_command_t {
    venus_command_exchange_t exchange; /**< Nonnull initialized frontend callback. */
    void *context;                     /**< Borrowed exclusive frontend context. */
    unsigned char *tx;                 /**< Borrowed exclusive private submission staging. */
    unsigned char *rx;                 /**< Borrowed exclusive private reply staging. */
    size_t tx_bytes;                   /**< Actual accessible tx extent. */
    size_t rx_bytes;                   /**< Actual accessible rx/host reply extent. */
    size_t reply_offset;               /**< Validated byte prefix,0..rx_bytes; reset by take. */
    uint64_t cpu_fence;                /**< Accepted nonzero CPU submission identity. */
    uint32_t command_id;               /**< Pinned command reply identity. */
    uint32_t state;                    /**< One of venus_command_state_t. */
    venus_ring_status_t lost;          /**< Sticky terminal failure, otherwise RingOk. */
} venus_command_t;
/** @brief Borrow disjoint private staging and an already negotiated frontend.
 * @param[out] owner Nonnull zero record, unchanged on failure.
 * @param[in] exchange Nonnull frontend callback, borrowed until free.
 * @param[in,out] context Nonnull exclusive frontend, borrowed until free.
 * @param[in,out] tx Nonnull private tx[tx_bytes], at least44 bytes, multiple of4.
 * @param[in] tx_bytes Actual extent, at most16MiB.
 * @param[in,out] rx Nonnull private rx[rx_bytes], at least4 bytes, multiple of4.
 * @param[in] rx_bytes Actual extent, at most16MiB and within host reply allocation.
 * @return RingOk or RingInvalid; no allocation, pairwise overlap rejected.
 * @note Sole thread; buffers also disjoint from frontend scratch/shared mapping.
 */
venus_ring_status_t venus_command_init(venus_command_t *owner, venus_command_exchange_t exchange,
                                       void *context, void *tx, size_t tx_bytes, void *rx,
                                       size_t rx_bytes);
/** @brief Prefix and publish one serializer-validated command with reply flag1.
 * @param[in,out] owner Nonnull live exclusive owner, sole submission thread.
 * @param[in] bytes Nonnull private immutable encoded command[length], disjoint
 * from owner and both staging buffers; at least8 bytes, multiple of4.
 * @param[in] length Actual command extent, must fit tx after36-byte prefix.
 * @return RingOk when accepted, Again when busy or callback pending, Invalid/Limit
 * before acceptance, or sticky terminal status. No allocation or retained input.
 * @note No other Venus submission/reply-stream mutation until take or abandonment.
 */
venus_ring_status_t venus_command_start(venus_command_t *owner, const void *bytes, size_t length);
/** @brief Poll CPU completion and assemble one bounded private reply byte range.
 * @param[in,out] owner Nonnull live Submitted/Reading owner, sole thread; no
 * external mutation of its cursor, buffers, fence or phase is permitted.
 * @return RingOk only when every expected byte and command identity validate;
 * RingAgain while CPU/reply pending or after a successful nonfinal range;
 * RingInvalid for local state; transport/protocol failure becomes sticky Lost.
 * @note Each call performs at most one successful reply read, at most4096 bytes
 * at reply_offset, after CPU completion. Only a shape-validated successful read
 * advances the cursor. Callback RingAgain may have written staging but retains
 * the same range for retry. No partial reply is published; Reading never polls
 * or submits the CPU command again. Caller enforces an overall elapsed deadline
 * including callback time. CPU completion is not GPU completion. No allocation.
 */
venus_ring_status_t venus_command_poll(venus_command_t *owner);
/** @brief Consume one ready reply view and allow another submission.
 * @param[in,out] owner Nonnull live exclusive owner, sole thread.
 * @param[out] bytes Nonnull disjoint pointer output, NULL on failure; borrowed
 * private reply view valid until next successful start or free.
 * @param[out] length Nonnull disjoint extent output, zero on failure.
 * @return RingOk once for Ready; Again while Submitted/Reading (no partial view); Invalid when Idle;
 * sticky terminal status when Lost. No allocation; command-specific decoding required.
 */
venus_ring_status_t venus_command_take(venus_command_t *owner, const void **bytes, size_t *length);
/** @brief Reset borrowed owner; frees no storage and never calls transport.
 * @param[in,out] owner Nullable zero/live record, sole thread after calls stop.
 * @note Pending/ready/lost free requires abandoning old receiver session first.
 * Idempotent; buffers/frontend cleanup belongs to caller. No GPU cancellation.
 */
void venus_command_free(venus_command_t *owner);
#endif
