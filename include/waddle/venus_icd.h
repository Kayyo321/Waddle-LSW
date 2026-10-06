/** @file venus_icd.h @brief Experimental standalone Vulkan loader and bounded dispatch interface. */
#ifndef WaddleVenusIcdH
/** @brief Include guard; no ownership or storage. */
#define WaddleVenusIcdH
#include "venus_command.h"
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
