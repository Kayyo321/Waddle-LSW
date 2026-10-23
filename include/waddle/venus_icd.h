/** @file venus_icd.h @brief Experimental standalone Vulkan loader and bounded dispatch interface. */
#ifndef WaddleVenusIcdH
/** @brief Include guard; no ownership or storage. */
#define WaddleVenusIcdH
#include "venus_command.h"
#include "venus_capabilities.h"
#include "venus_instance_wire.h"
/** @brief Bind the experimental ICD to one exclusive already negotiated frontend.
 * @param[in] exchange Nonnull borrowed callback, retained until unbind/abandon;
 * must support65620-byte command requests,4096-byte command replies,
 * RequestGpuFence/RequestGpuPoll and RequestCreate/Free/Read/Write with
 *4096-byte bounded copy payloads for host-coherent mapped resources,
 * bounded deadlines, and never reenter the ICD.
 * @param[in,out] context Nonnull borrowed callback state, remains live/exclusive.
 * @return RingOk; Invalid null/occupied binding; Limit namespace exhaustion.
 * @note Binding allocates no storage; mapping owns bounded shadows until unmap/
 * free/abandon. A process-local mutex serializes all calls. Bind before loader
 * discovery. Caller owns frontend, channel, mapping and worker teardown.
 */
venus_ring_status_t venus_icd_bind(venus_command_exchange_t exchange, void *context);
/** @brief Bind one exclusive frontend with its actual negotiated capability profile.
 * @param[in] exchange Nonnull borrowed callback, retained with the existing bind contract.
 * @param[in,out] context Nonnull exclusive callback owner, live until unbind/abandon.
 * @param[in] capabilities Nonnull initialized immutable private160-byte snapshot,
 * borrowed only until return; compatible integers are copied, no pointer retained.
 * @return RingOk; RingInvalid for null/incompatible/occupied; RingLimit namespace exhaustion.
 * @note Allocation-free, mutex serialized. Rejection preserves existing session and
 * snapshot; successful unbind/receiver-retired abandon scrub the copy. Negotiation
 * does not advertise implemented Vulkan API/extensions or actual hardware features.
 */
venus_ring_status_t venus_icd_bind_capabilities(venus_command_exchange_t exchange,
                                              void *context,
                                              const venus_capabilities_t *capabilities);
/** @brief Borrowed sequential exchange using the frontend local absolute deadline.
 * @param[in,out] context Nonnull exclusive negotiated frontend, retained until release.
 * @param[in] request Nonnull decoded request, borrowed for this call.
 * @param[in] input Nullable only when length is zero; borrowed input[length].
 * @param[in] length Exact input byte extent.
 * @param[out] response Nonnull disjoint response, zero on transport failure.
 * @param[out] output Nullable only for no payload, borrowed output[capacity].
 * @param[in] capacity Actual accessible output extent.
 * @param[in] deadline_ms Nonzero frontend monotonic absolute deadline; never renewed.
 * @return RingOk or ordinary/terminal ring status; failed payload remains unchanged.
 * @note ICD mutex thread only; no reentry or retained request/output pointers.
 */
typedef venus_ring_status_t (*venus_icd_exchange_until_t)(
    void *context, const venus_request_t *request, const void *input, size_t length,
    venus_request_t *response, void *output, size_t capacity, uint64_t deadline_ms);
/** @brief Read the same local monotonic millisecond clock used by timed exchange.
 * @param[in,out] context Nonnull borrowed live exclusive frontend.
 * @return Nonzero monotonic milliseconds, zero on clock failure.
 * @note ICD mutex thread only; no allocation, reentry or ownership transfer.
 */
typedef uint64_t (*venus_icd_clock_t)(void *context);
/** @brief Bind a timed frontend for complete bounded native capability queries.
 * @param[in] exchange_until Nonnull borrowed absolute-deadline callback.
 * @param[in] clock_ms Nonnull borrowed matching frontend clock.
 * @param[in,out] context Nonnull exclusive negotiated frontend, live until release.
 * @param[in] capabilities Nonnull compatible immutable snapshot, copied on success.
 * @param[in] reply_bytes Trusted actual receiver reply extent, exactly524288 bytes.
 * @return RingOk; Invalid incompatible profile/extent/occupied binding;
 * Limit namespace exhaustion; transport status on failed allocation proof.
 * @note Mutex serialized. Before publication, a single5000ms deadline covers
 * successful last-byte and rejected first-outside-byte receiver range probes.
 * Rejection preserves any incumbent binding. Frontend/session ownership stays
 * with caller; uncertain transport requires receiver retirement before release.
 */
venus_ring_status_t venus_icd_bind_timed(venus_icd_exchange_until_t exchange_until,
    venus_icd_clock_t clock_ms, void *context, const venus_capabilities_t *capabilities,
    uint32_t reply_bytes);
/** @brief Release a quiescent binding after all Vulkan objects are destroyed.
 * @return RingOk, including already-unbound; Again while objects/submission remain.
 * @note No frontend teardown/storage free; mutex serialized, borrowed pointers expire.
 */
venus_ring_status_t venus_icd_unbind(void);
/** @brief Forget poisoned objects after the caller retires the receiver session.
 * @note Caller must first stop application calls and abandon/retire receiver; no
 * transport cancellation occurs here. No allocation; mutex serialized, idempotent.
 */
void venus_icd_abandon(void);
/** @brief Negotiate external loader interface, exported with the Vulkan ABI alias.
 * @param[in,out] version Nonnull private version; accepts2..5, clamps higher to5;
 * unchanged for unsupported0/1. No ownership transfer.
 * @return VK_SUCCESS or VK_ERROR_INITIALIZATION_FAILED for null/unsupported input.
 * @note Allocation-free, thread-safe; does not advertise complete Vulkan API support.
 */
VkResult venus_icd_negotiate_loader(uint32_t *version);
/** @brief Resolve global/implemented instance and device functions with exact Vulkan calling ABI.
 * @param[in] instance NULL for globals, otherwise a live ICD instance; not dereferenced.
 * @param[in] name Nullable, accessible NUL-terminated byte string, at most256 bytes.
 * @return Borrowed static function pointer or NULL for unsupported/invalid lookup.
 * @note Mutex serialized; allocation-free; function pointers outlive bindings.
 */
PFN_vkVoidFunction venus_icd_get_instance_proc_addr(VkInstance instance, const char *name);
/** @brief Resolve implemented physical queries for a live instance.
 * @param[in] instance Nonnull live ICD instance, validated without dereference.
 * @param[in] name Nullable accessible NUL-terminated bytes, maximum256 bytes.
 * @return Static borrowed query pointer or NULL for unsupported/invalid lookup.
 * @note Mutex serialized; allocation-free; no object ownership transfer.
 */
PFN_vkVoidFunction venus_icd_get_physical_proc_addr(VkInstance instance, const char *name);
#endif
