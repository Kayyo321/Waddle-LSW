/** @file venus_dispatch.h @brief Bounded Linux host receiver service. */
#ifndef WaddleVenusDispatchH
/** @brief Include guard, no storage/ownership. */
#define WaddleVenusDispatchH
#include "venus_receiver.h"
#include "venus_rpc.h"
/** @brief Consume one request and publish its nonterminal response.
 * @param[in,out] rpc Nonnull live host record with ready borrowed
 * channel/buffer.
 * @param[in,out] receiver Nonnull borrowed live singleton, session-thread-only,
 * retained for call. Must not be concurrently used/destroyed.
 * @param[in] timeout_ms One whole request/dispatch/response
 * deadline, 1..60000ms.
 * @return RingOk for delivered nonterminal response, whose wire status may be
 * Invalid/Again/Limit. Local argument error returns RingInvalid before
 * consuming; terminal transfer/sequence/renderer errors close both rings
 * without a response.
 * @note No allocation. Only Capabilities/Negotiate are accepted until pinned
 * host/guest compatibility succeeds; other operations return Invalid without
 * SDK calls. Oversized payloads drained with bounded scratch before Limit
 * response; no renderer call. All commands privately copied, quota policy
 * unchanged. Stop calls then destroy receiver before releasing borrowed
 * storage.
 */
venus_ring_status_t venus_dispatch_serve(venus_rpc_t *rpc, venus_receiver_t *receiver,
                                         uint32_t timeout_ms);
#endif
