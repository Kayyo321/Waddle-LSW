#ifndef WaddleVenusWaitH
#define WaddleVenusWaitH
#include "venus_ring.h"

/** @brief Lifecycle-owned wait callback, invoked only for ring backpressure.
 * @param[in,out] context Nullable borrowed callback context, retained until wait ends.
 * @return RingOk to retry after waiting; RingCancelled, RingTimeout, RingClosed,
 * RingCorrupt, or RingInvalid to stop. RingAgain and unknown values are invalid.
 * @note Callback must wait for a bounded interval or real readiness, checking
 * cancellation, deadline, and control-channel EOF. Spurious wakeups are allowed.
 * Runs on the calling ring owner thread; must not detach/unmap or concurrently
 * operate on that thread's cursor. No allocation or ownership transfer implied.
 */
typedef venus_ring_status_t (*venus_wait_callback_t)(void *context);

/** @brief Write exactly length bytes, waiting with lifecycle cancellation checks.
 * @param[in,out] ring Nonnull attached producer endpoint, retained until return.
 * @param[in] data Nonnull immutable borrowed source[length], disjoint from ring.
 * @param[in] length Bytes, 1..validated capacity.
 * @param[in] wait Nonnull borrowed lifecycle callback, retained until return.
 * @param[in,out] context Nullable callback-owned context, retained until return.
 * @return RingOk or ring/callback failure. Cancellation/timeout do not transfer.
 * @note Producer-thread only; no allocation/spinning without callback. A callback
 * RingClosed closes this ring; session owner coordinates the opposite ring.
 * Source must remain stable throughout all retries. Join this call before free.
 */
venus_ring_status_t venus_ring_write_wait(venus_ring_t *ring, const void *data, size_t length,
                                         venus_wait_callback_t wait, void *context);
/** @brief Read exactly length bytes, waiting with lifecycle cancellation checks.
 * @param[in,out] ring Nonnull attached consumer endpoint, retained until return.
 * @param[out] data Nonnull borrowed output[length], disjoint from ring.
 * @param[in] length Bytes, 1..validated capacity.
 * @param[in] wait Nonnull borrowed lifecycle callback, retained until return.
 * @param[in,out] context Nullable callback-owned context, retained until return.
 * @return RingOk or ring/callback failure; failures leave output untouched.
 * @note Consumer-thread only. Callback observes cancellation/timeout/EOF; no
 * allocation or internal OS handle ownership. Join before detach/unmap.
 */
venus_ring_status_t venus_ring_read_wait(venus_ring_t *ring, void *data, size_t length,
                                        venus_wait_callback_t wait, void *context);
#endif
