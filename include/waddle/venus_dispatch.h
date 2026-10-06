/** @file venus_dispatch.h @brief Bounded Linux host receiver service. */
#ifndef WaddleVenusDispatchH
/** @brief Include guard, no storage/ownership. */
#define WaddleVenusDispatchH
#include "venus_receiver.h"
#include "venus_rpc.h"
/** @brief Trusted worker callback for fence-ordered frame publication.
 * @param[in,out] context Nonnull borrowed sole-thread state retained for serve.
 * @param[in] bytes Nonnull immutable private frame bytes[length], call-scoped.
 * @param[in] length Exactly1216 bytes; callback must decode via bounded Zig codec.
 * @param[in] timeline GPU timeline1..63.
 * @param[in] fence Nonzero issued GPU fence.
 * @return RingOk queued; Again pressure/order pending; Invalid local frame;
 * Limit quota; terminal native/receiver status otherwise.
 * @note No caller storage retained; callback owns its worker leases/temporary FDs.
 */
typedef venus_ring_status_t (*venus_dispatch_submit_t)(void *context, const void *bytes,
                                                       size_t length, uint32_t timeline,
                                                       uint64_t fence);
/** @brief Trusted callback consuming one completion encoded through bounded Zig.
 * @param[in,out] context Nonnull borrowed sole-thread worker state.
 * @param[in] frame Nonzero outstanding frame ID.
 * @param[out] bytes Nonnull private disjoint bytes[length], call-scoped.
 * @param[in] length Exactly32 bytes; success must fill exact release packet.
 * @return RingOk consumed; Again awaiting; Invalid absent; terminal loss otherwise.
 * @note No retained pointers/FD transfer; worker must guard pending allocations.
 */
typedef venus_ring_status_t (*venus_dispatch_take_t)(void *context, uint64_t frame, void *bytes,
                                                     size_t length);
/** @brief Trusted read-only allocation guard.
 * @param[in] context Nonnull borrowed private sole-thread worker state.
 * @param[in] resource_id Registered ID2..65.
 * @return Nonzero prevents resource free; zero unreferenced.
 * @note No allocation/mutation/retained pointer; includes unconsumed completions.
 */
typedef int (*venus_dispatch_busy_t)(const void *context, uint32_t resource_id);
/** @brief Trusted bounded reverse-channel health pump.
 * @param[in,out] context Nonnull borrowed sole-thread worker state.
 * @return RingOk healthy including no packets, terminal native/protocol status otherwise.
 * @note Must process at most three packets per call, no blocking/retained pointers.
 */
typedef venus_ring_status_t (*venus_dispatch_pump_t)(void *context);
/** @brief Call-scoped immutable trusted binding, naturally pointer-aligned.
 * @note Caller-owned; all callbacks refer to dispatch's receiver/session. Sole
 * worker thread. Owns no pointers/resources, never decoded from guest storage.
 */
typedef struct venus_dispatch_presentation_t {
    void *context;                       /**< Nonnull borrowed worker owner retained for serve. */
    venus_dispatch_submit_t submit;      /**< Nonnull ordered publication callback. */
    venus_dispatch_take_t take;          /**< Nonnull exact release encoding callback. */
    venus_dispatch_busy_t resource_busy; /**< Nonnull outstanding allocation guard. */
    venus_dispatch_pump_t pump;          /**< Nonnull bounded native health pump. */
} venus_dispatch_presentation_t;
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
/** @brief Consume one mapped request with an optional trusted presentation binding.
 * @param[in,out] rpc Nonnull ready host RPC borrowing private channel/buffer.
 * @param[in,out] receiver Nonnull sole-thread receiver retained for call.
 * @param[in] presentation Nullable immutable complete trusted binding retained for call.
 * @param[in] timeout_ms Whole request/dispatch/response budget1..60000ms.
 * @return Same status/terminal policy as venus_dispatch_serve; incomplete bindings
 * return Invalid before consumption. Unbound presentation requests return Invalid.
 * @note Sole worker thread, no allocation. Guards resource frees and pumps releases
 * during health sampling; clears borrowed monitor context before every return.
 * Caller owns receiver/binding/leases and stops service before freeing them.
 */
venus_ring_status_t
venus_dispatch_serve_presented(venus_rpc_t *rpc, venus_receiver_t *receiver,
                               const venus_dispatch_presentation_t *presentation,
                               uint32_t timeout_ms);
#endif
