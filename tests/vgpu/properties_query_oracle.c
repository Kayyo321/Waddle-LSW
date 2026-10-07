/** @file properties_query_oracle.c @brief Independent pinned guest Properties2 encoder. */
#include "properties_oracle.h"
#include "vn_cs.h"
/** @brief Isolate unused imported vendor submit symbol in this translation unit. */
#define vn_ring_submit_command venus_properties_unused_submit
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "vn_protocol_driver_device.h"
#pragma GCC diagnostic pop
/** @brief Encode an actual typed native query with immutable guest code.
 * @param[in] tags Nonnull borrowed recognized unique array[count]. @param[in] count Extent0..5.
 * @param[out] bytes Nonnull exclusive4096-byte output. @return Initialized prefix extent.
 * @note No allocation/retention; disjoint owners are thread-safe. */
size_t venus_properties_test_query(const uint32_t *tags, size_t count, unsigned char *bytes) {
  property_data_t nodes[5];
  VkPhysicalDeviceProperties core;
  property_fixture_init(&core, nodes, tags, count);
  property_nodes_link(nodes, tags, count);
  VkPhysicalDeviceProperties2 value = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext=count ? nodes : NULL};
  struct instance_encoder_t encoder = {.bytes=bytes, .capacity=4096};
  vn_encode_vkGetPhysicalDeviceProperties2(&encoder, 1, (VkPhysicalDevice)(uintptr_t)7, &value);
  return encoder.used;
}
