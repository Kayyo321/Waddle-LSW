/** @file features_reply_oracle.c @brief Independent pinned native receiver
 * feature reply fixture. */
#include "features_oracle.h"
#include "vn_protocol_renderer_device.h"
/** @brief Encode actual native receiver feature flags using the pinned
 * generator.
 * @param[in] tags Nonnull borrowed array[count], recognized unique tags,
 * count0..4.
 * @param[in] count Actual accessible tag count.
 * @param[out] bytes Nonnull private4096-byte output, no pointer retention.
 * @return Initialized prefix extent; allocation-free, sole test thread.
 */
size_t venus_features_test_reply(const uint32_t *tags, size_t count,
                                 unsigned char *bytes) {
  feature_node_t nodes[4];
  feature_nodes_init(nodes, tags, count);
  VkPhysicalDeviceFeatures2 value = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
      .pNext = count ? nodes : NULL};
  VkBool32 core[55];
  _Static_assert(sizeof(core) == sizeof(value.features),
                 "Pinned core feature flags");
  for (size_t index = 0; index < 55; index++)
    core[index] = (VkBool32)(index % 2);
  memcpy(&value.features, core, sizeof(core));
  struct fixture_encoder_t encoder = {.bytes = bytes, .capacity = 4096};
  uint32_t command = 147;
  uint64_t pointer = 1;
  vn_encode_uint32_t(&encoder, &command);
  vn_encode_uint64_t(&encoder, &pointer);
  vn_encode_VkPhysicalDeviceFeatures2(&encoder, &value);
  return encoder.used;
}
