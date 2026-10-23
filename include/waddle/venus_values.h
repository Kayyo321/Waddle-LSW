/** @file venus_values.h @brief Bounded native Vulkan core query reply
 * conversion. */
#ifndef WaddleVenusValuesH
/** @brief Include guard, no storage or ownership. */
#define WaddleVenusValuesH
#include "venus_ring.h"
#include <vulkan/vulkan.h>
/** @brief Native external Vulkan property ABI; caller owns storage, no
 * allocation. */
typedef VkPhysicalDeviceProperties venus_vk_properties_t;
/** @brief Native external Vulkan feature ABI; caller owns storage, no
 * allocation. */
typedef VkPhysicalDeviceFeatures venus_vk_features_t;
/** @brief Native external Vulkan memory property ABI; caller owns storage, no
 * allocation. */
typedef VkPhysicalDeviceMemoryProperties venus_vk_memory_t;
/** @brief Decode pinned command6 reply to native property fields after
 * validation.
 * @param[out] output Nonnull disjoint private native record, unchanged on
 * failure.
 * @param[in] bytes Nonnull immutable private bytes[length], borrowed only for
 * call.
 * @param[in] length Actual accessible input, at most16MiB; trailing resource
 * capacity ignored.
 * @return RingOk, Invalid for null/overlap/overflow/extent, Corrupt for
 * malformed data.
 * @note No allocation or wire-pointer dereference; thread-safe on disjoint
 * records.
 */
venus_ring_status_t venus_values_properties_decode(venus_vk_properties_t *output, const void *bytes,
                                                   size_t length);
/** @brief Decode pinned command3 reply with strict VkBool32 validation.
 * @param[out] output Nonnull disjoint private native record, unchanged on
 * failure.
 * @param[in] bytes Nonnull immutable private bytes[length], borrowed for call.
 * @param[in] length Actual accessible input, at most16MiB; trailing capacity
 * ignored.
 * @return RingOk, Invalid for local null/alias/extent, Corrupt for wire/boolean
 * errors.
 * @note Allocation-free; no wire pointers; thread-safe on disjoint records.
 */
venus_ring_status_t venus_values_features_decode(venus_vk_features_t *output, const void *bytes,
                                                 size_t length);
/** @brief Decode pinned command8 reply with bounded memory counts/heap indices.
 * @param[out] output Nonnull disjoint private native record, unchanged on
 * failure.
 * @param[in] bytes Nonnull immutable private bytes[length], borrowed for call.
 * @param[in] length Actual accessible input, at most16MiB; trailing capacity
 * ignored.
 * @return RingOk, Invalid for local null/alias/extent, Corrupt for
 * wire/count/index errors.
 * @note Allocation-free; no wire pointers; thread-safe on disjoint records.
 */
venus_ring_status_t venus_values_memory_decode(venus_vk_memory_t *output, const void *bytes,
                                               size_t length);
#endif
