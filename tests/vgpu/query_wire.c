/** @file query_wire.c @brief Independent pinned query encoder and native wire
 * ABI tests. */
#include "vn_cs.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "vn_protocol_driver_device.h"
#pragma GCC diagnostic pop
#include "waddle/venus_query_wire.h"
#include <assert.h>
#include <string.h>
/** @brief Generate immutable encoder output for production packet comparison.
 * @param[in] command Pinned command2,3,6,8; test-only, no ownership.
 * @param[in] host_id Nonzero preassigned host identity, never dereferenced.
 * @param[in] ids Nullable iff count0; immutable preassigned identities[count].
 * @param[in] count Physical-device fill count0..16.
 * @param[out] bytes Nonnull private output[capacity], borrowed for synchronous
 * call.
 * @param[in] capacity Actual accessible extent, at least4096.
 * @return Initialized prefix length; zero for invalid local input/command.
 * @note Allocation-free, sole test thread; generated decoder helpers never
 * called.
 */
size_t venus_query_test_encode(uint32_t command, uint64_t host_id,
                               const uint64_t *ids, uint32_t count, void *bytes,
                               size_t capacity) {
  if (!host_id || !bytes || capacity < 4096 || count > VenusQueryMaxDevices ||
      (!ids && count))
    return 0;
  struct instance_encoder_t encoder = {.bytes = bytes, .capacity = capacity};
  if (command == 2) {
    VkPhysicalDevice devices[VenusQueryMaxDevices];
    for (uint32_t index = 0; index < count; index++)
      devices[index] = (VkPhysicalDevice)(uintptr_t)ids[index];
    vn_encode_vkEnumeratePhysicalDevices(&encoder, 1,
                                         (VkInstance)(uintptr_t)host_id, &count,
                                         ids ? devices : NULL);
  } else if (command == 3) {
    VkPhysicalDeviceFeatures features = {0};
    vn_encode_vkGetPhysicalDeviceFeatures(
        &encoder, 1, (VkPhysicalDevice)(uintptr_t)host_id, &features);
  } else if (command == 6) {
    VkPhysicalDeviceProperties properties = {0};
    vn_encode_vkGetPhysicalDeviceProperties(
        &encoder, 1, (VkPhysicalDevice)(uintptr_t)host_id, &properties);
  } else if (command == 8) {
    VkPhysicalDeviceMemoryProperties memory = {0};
    vn_encode_vkGetPhysicalDeviceMemoryProperties(
        &encoder, 1, (VkPhysicalDevice)(uintptr_t)host_id, &memory);
  } else
    return 0;
  return encoder.used;
}
#ifndef VgpuQueryOracle
int main(void) {
  unsigned char bytes[4096], expected[4096];
  uint64_t ids[] = {2, 3};
  size_t written = 0;
  for (uint32_t count = 0; count <= 2; count++) {
    assert(venus_query_wire_enumerate(1, count ? ids : NULL, count, bytes,
                                      sizeof(bytes), &written) == RingOk);
    size_t size = venus_query_test_encode(2, 1, count ? ids : NULL, count,
                                          expected, sizeof(expected));
    assert(size == written && !memcmp(bytes, expected, size));
  }
  const uint32_t Commands[] = {3, 6, 8};
  for (unsigned index = 0; index < sizeof(Commands) / sizeof(*Commands);
       index++) {
    assert(venus_query_wire_fixed(Commands[index], 2, bytes, sizeof(bytes),
                                  &written) == RingOk);
    size_t size = venus_query_test_encode(Commands[index], 2, NULL, 0, expected,
                                          sizeof(expected));
    assert(size == written && !memcmp(bytes, expected, size));
  }
  unsigned char reply[44] = {2};
  reply[8] = 1;
  reply[16] = 2;
  reply[20] = 2;
  reply[28] = 2;
  reply[36] = 3;
  venus_vk_result_t result = VK_ERROR_UNKNOWN;
  uint32_t count = 99;
  assert(venus_query_wire_enumerate_reply(&result, &count, ids, 2, reply,
                                          sizeof(reply)) == RingOk);
  assert(result == VK_SUCCESS && count == 2);
  for (size_t length = 0; length < sizeof(reply); length++) {
    result = VK_ERROR_UNKNOWN;
    count = 99;
    assert(venus_query_wire_enumerate_reply(&result, &count, ids, 2, reply,
                                            length) == RingCorrupt);
    assert(result == VK_ERROR_UNKNOWN && count == 99);
  }
  return 0;
}
#endif
