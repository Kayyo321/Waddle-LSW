/** @file venus_receiver.h @brief Public-ABI Venus bootstrap and CPU reply owner. */
#ifndef WaddleVenusReceiverH
/** @brief Compile-time include guard; no ownership or storage. */
#define WaddleVenusReceiverH
#include "venus_ring.h"
/** @brief Pinned upstream version-zero Venus capability set bytes. */
#define VenusCapabilityBytes 160u
/** @brief Opaque Linux receiver; created/destroyed by one session thread.
 * @note Owns renderer singleton, context, reply resource/map, command scratch,
 * and callback lifetime. No borrowed IVSHMEM or socket ownership. Never copy.
 */
typedef struct venus_receiver_t venus_receiver_t;
/** @brief Create one renderer bootstrap context and CPU reply stream.
 * @param[out] receiver Nonnull pointer to initially-null owner pointer; success
 * transfers ownership, released with venus_receiver_destroy; failure stays null.
 * @param[in] command_capacity Power-of-two private bytes, 64..16777216.
 * @param[in] reply_bytes Power-of-two CPU reply extent, 4096..16777216.
 * @return RingOk; RingInvalid for local arguments; RingAgain if renderer claimed;
 * RingCorrupt for allocation, renderer, capability, context, blob or map failure.
 * @note Linux/session-thread-only; no competing global renderer API calls.
 * Trusted server executable must already be installed or configured through
 * RENDER_SERVER_EXEC_PATH. CPU bootstrap does not select a hardware GPU.
 */
venus_ring_status_t venus_receiver_create(venus_receiver_t **receiver, uint32_t command_capacity,
                                          uint64_t reply_bytes);
/** @brief Release a partial/live owner and null its pointer.
 * @param[in,out] receiver Nullable pointer to nullable owned record.
 * @note Session-thread-only, no concurrent API calls. Context/callbacks stop
 * before resource unmap, renderer cleanup, scratch free and singleton release.
 * Safe with pending fences; no deadline waits or transferred handles remain.
 */
void venus_receiver_destroy(venus_receiver_t **receiver);
/** @brief Copy the retained public Venus capability set for negotiation.
 * @param[in] receiver Nonnull borrowed live owner.
 * @param[out] output Nonnull disjoint private buffer[length], unchanged on error.
 * @param[in] length Exactly VenusCapabilityBytes.
 * @return RingOk or RingInvalid; no allocation or ownership transfer.
 * @note Session-thread-only; bytes are the pinned upstream little-endian capset.
 */
venus_ring_status_t venus_receiver_capabilities(const venus_receiver_t *receiver, void *output,
                                                size_t length);
/** @brief Copy a bounded complete command bundle into private scratch and submit.
 * @param[in,out] receiver Nonnull live session-thread-owned record.
 * @param[in] commands Nonnull immutable private or shared input[length], disjoint
 * from owner; remains stable during copy. Renderer sees only the private copy.
 * @param[in] length Multiple of four, 8..configured command_capacity bytes.
 * @param[out] fence Nonnull disjoint private output, zeroed on failure; successful
 * monotonically increasing nonzero CPU fence, not a GPU completion guarantee.
 * @return RingOk, RingInvalid for local bounds, RingAgain for in-flight bundle,
 * RingCorrupt for poisoned owner/public submit/fence failure or exhausted IDs.
 * @note Session-thread-only, no allocations. Copy replies before next submission.
 * Venus semantics/handles decoded by upstream; own envelope bounds checked in Zig.
 */
venus_ring_status_t venus_receiver_submit(venus_receiver_t *receiver, const void *commands,
                                          size_t length, uint64_t *fence);
/** @brief Acquire completion of the most recent timeline-zero submission.
 * @param[in] receiver Nonnull borrowed live owner.
 * @return RingOk if no pending CPU work; RingAgain if fence not retired;
 * RingInvalid if null, RingCorrupt if poisoned or callback identity failed.
 * @note Session-thread-only, nonblocking. Runtime must enforce deadline and
 * cancellation; CPU completion does not establish GPU queue completion.
 */
venus_ring_status_t venus_receiver_poll(const venus_receiver_t *receiver);
/** @brief Copy a completed CPU reply range into private caller storage.
 * @param[in] receiver Nonnull borrowed live owner; no concurrent submit/destroy.
 * @param[in] offset Byte offset within mapped reply extent.
 * @param[out] output Nonnull disjoint private output[length], unchanged on error.
 * @param[in] length Nonzero bounded reply bytes; caller decodes Venus reply size.
 * @return RingOk, RingAgain while CPU work pending, RingInvalid for bounds/null,
 * or RingCorrupt for poisoned owner; no allocation or ownership transfer.
 * @note Session-thread-only; copy needed replies before reusing command stream.
 */
venus_ring_status_t venus_receiver_reply(const venus_receiver_t *receiver, uint64_t offset,
                                         void *output, size_t length);
#endif
