/** @file venus_rpc_internal.h @brief Private runtime framing helpers. */
#ifndef WaddleVenusRpcInternalH
/** @brief Include guard, no storage/ownership. */
#define WaddleVenusRpcInternalH
#include "waddle/venus_rpc.h"
/** @brief Validate role/live owner and start one bounded operation.
 * @param[in,out] rpc Borrowed record. @param[in] role Required host/guest role.
 * @param[in] timeout_ms Operation deadline, 1..60000ms.
 * @return RingOk, local RingInvalid, or terminal channel/protocol status.
 * @note Session-thread-only, no allocation; sequence exhaustion is terminal.
 */
venus_ring_status_t venus_rpc_begin(venus_rpc_t *rpc, venus_session_role_t role,
                                    uint32_t timeout_ms);
/** @brief Transfer private bytes in bounded chunks under existing deadline.
 * @param[in,out] rpc Borrowed live owner. @param[in,out] bytes Private buffer[length].
 * @param[in] length Valid accessible extent; zero is a no-op.
 * @param[in] writing One to publish, zero to receive.
 * @param[in] payload One when a header already declared pending payload.
 * @return RingOk or terminal failure (closes session); partial EOF is Corrupt.
 * @note Sole session thread, no allocation; caller validates buffer bounds first.
 */
venus_ring_status_t venus_rpc_transfer(venus_rpc_t *rpc, void *bytes, size_t length, int writing,
                                       int payload);
/** @brief Close both directions and poison exchange identity.
 * @param[in,out] rpc Nonnull live framing record. @param[in] status Terminal status.
 * @return status unchanged; stop reason mapped from status, default Protocol.
 * @note Sole session thread, no allocation or release of borrowed resources.
 */
venus_ring_status_t venus_rpc_fail(venus_rpc_t *rpc, venus_ring_status_t status);
#endif
