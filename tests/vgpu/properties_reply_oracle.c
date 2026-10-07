/** @file properties_reply_oracle.c @brief Independent pinned receiver typed Properties2 encoder. */
#include "properties_oracle.h"
#include "vn_protocol_renderer_device.h"
/** @brief Fill independent expected native SDK values.
 * @param[in] tags Nonnull borrowed recognized unique array[count]. @param[in] count Extent0..5.
 * @param[out] core Nonnull exclusive native core. @param[out] nodes Nonnull private five-node storage.
 * @note Allocation-free, no retained pointer, disjoint owners are thread-safe. */
void venus_properties_test_fixture(const uint32_t *tags, size_t count, VkPhysicalDeviceProperties *core, property_data_t *nodes) {
  property_fixture_init(core, nodes, tags, count);
}
/** @brief Encode supplied native payloads through independent receiver generator.
 * @param[in] tags Nonnull borrowed recognized unique array[count]. @param[in] count Extent0..5.
 * @param[in] core Nonnull initialized borrowed core. @param[in] input Nonnull initialized five-node storage.
 * @param[out] bytes Nonnull exclusive4096-byte output. @return Initialized prefix extent.
 * @note Inputs copied before private link construction; no mutation/allocation/retention. Thread-safe for disjoint outputs. */
size_t venus_properties_test_encode(const uint32_t *tags, size_t count, const VkPhysicalDeviceProperties *core, const property_data_t *input, unsigned char *bytes) {
  assert(tags && core && input && bytes && count <= 5);
  property_data_t nodes[5];
  memcpy(nodes, input, sizeof(nodes));
  property_nodes_link(nodes, tags, count);
  VkPhysicalDeviceProperties2 value = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext=count ? nodes : NULL, .properties=*core};
  struct fixture_encoder_t encoder = {.bytes=bytes, .capacity=4096};
  uint32_t command=148;
  uint64_t present=1;
  vn_encode_uint32_t(&encoder, &command);
  vn_encode_uint64_t(&encoder, &present);
  vn_encode_VkPhysicalDeviceProperties2(&encoder, &value);
  return encoder.used;
}
