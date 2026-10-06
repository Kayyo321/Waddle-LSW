/** @file features_query_oracle.c @brief Independent pinned partial query
 * encoding fixture. */
#include "features_oracle.h"
#include "vn_cs.h"
/** @brief Isolate immutable vendor submit stub from other oracle translation
 * units. */
#define vn_ring_submit_command venus_features_unused_submit
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "vn_protocol_driver_device.h"
#pragma GCC diagnostic pop
/** @brief Encode actual typed native query with the pinned guest generator.
 * @param[in] tags Nonnull borrowed array[count], recognized unique tags,
 * count0..4.
 * @param[in] count Actual accessible tag count.
 * @param[out] bytes Nonnull private4096-byte output, no pointer retention.
 * @return Initialized prefix extent; allocation-free, sole test thread.
 */
size_t venus_features_test_query(const uint32_t *tags, size_t count,
                                 unsigned char *bytes) {
  feature_node_t nodes[4];
  feature_nodes_init(nodes, tags, count);
  VkPhysicalDeviceFeatures2 value = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = count ? nodes : NULL};
  struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 4096};
  vn_encode_vkGetPhysicalDeviceFeatures2(
      &encoder, 1, (VkPhysicalDevice)(uintptr_t)7, &value);
  return encoder.used;
}
