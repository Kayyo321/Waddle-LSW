/** @file venus_instance_wire.h @brief Bounded initial core Vulkan instance wire boundary. */
#ifndef WaddleVenusInstanceWireH
/** @brief Include guard; no storage or ownership. */
#define WaddleVenusInstanceWireH
#include "venus_values.h"
/** @brief Imported native Vulkan input ABI; caller owns its referenced strings. */
typedef VkInstanceCreateInfo venus_vk_instance_info_t;
/** @brief Imported native Vulkan result ABI; caller owns output storage. */
typedef VkResult venus_vk_result_t;
/** @brief Encode pinned initial core create command into private storage.
 * @param[in] info Nonnull native input; borrowed strings accessible through NUL,
 * at most1024 bytes including NUL, UTF8. Null application info/names permitted.
 * Only core Vulkan1.0/1.1, zero flags, null pNext and no layers/extensions supported.
 * @param[in] host_id Nonzero existing reserved instance identity, never a native pointer.
 * @param[out] bytes Nonnull private output[capacity], disjoint from inputs/written.
 * @param[in] capacity Actual accessible extent8..16MiB.
 * @param[out] written Nonnull private prefix length, unchanged on failure.
 * @return RingOk; Invalid unsupported/null/alias/overflow; Limit short capacity/name.
 * @note Allocation-free; outputs preserved on failure; thread-safe on disjoint storage.
 */
venus_ring_status_t venus_instance_wire_create(const venus_vk_instance_info_t *info,
                                               uint64_t host_id, void *bytes, size_t capacity,
                                               size_t *written);
/** @brief Decode a private completed create reply before publishing its reservation.
 * @param[out] result Nonnull private native result, unchanged on failure.
 * @param[in] bytes Nonnull immutable private bytes[length], disjoint from result.
 * @param[in] length Actual accessible input, at most16MiB; trailing capacity ignored.
 * @param[in] host_id Nonzero already reserved instance identity.
 * @return RingOk for complete validated transaction (including negative Vulkan result),
 * Invalid local/null/alias/overflow, Corrupt truncated/invalid tag/result/identity.
 * @note No allocation or wire pointers; caller handles Vulkan failure rollback.
 * Thread-safe on disjoint records; command owner serializes transport.
 */
venus_ring_status_t venus_instance_wire_create_reply(venus_vk_result_t *result, const void *bytes,
                                                     size_t length, uint64_t host_id);
/** @brief Encode pinned destroy command for an existing instance.
 * @param[in] host_id Nonzero live host instance ID.
 * @param[out] bytes Nonnull private output[capacity], disjoint from written.
 * @param[in] capacity Actual accessible extent8..16MiB; at least24 to succeed.
 * @param[out] written Nonnull initialized prefix length, unchanged on failure.
 * @return RingOk; Invalid null/alias/overflow/ID; Limit insufficient capacity.
 * @note Allocation-free; outputs preserved on failure; sole lifecycle owner thread.
 * Command owner validates reply command1; local retirement follows host success.
 */
venus_ring_status_t venus_instance_wire_destroy(uint64_t host_id, void *bytes, size_t capacity,
                                                size_t *written);
#endif
