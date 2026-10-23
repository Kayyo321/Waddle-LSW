/** @file device_wire_oracle.c @brief Independent pinned device-create encoder oracle. */
#include "vn_cs.h"
/** @brief Isolate the immutable vendor test submit symbol; no storage or ownership. */
#define vn_ring_submit_command venus_device_oracle_unused_submit
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "vn_protocol_driver_device.h"
#pragma GCC diagnostic pop
/** @brief Encode one canonical device request using the pinned native generator.
 * @param[in] info Nonnull initialized borrowed native record, nodes, priorities
 * and terminated names; exclusively immutable for this synchronous call.
 * @param[in] physical_id Nonzero translated physical identity, never dereferenced.
 * @param[in] device_id Nonzero distinct translated device identity, never dereferenced.
 * @param[out] bytes Nonnull exclusive output[8192]; canonical packet must fit.
 * @return Initialized prefix bytes; no allocation, retention or ownership transfer.
 * @note Sole fixture thread; native x86_64 little-endian ABI. Bounds enforced by
 * the independent writer. This oracle implements no transport or feature policy.
 */
size_t venus_device_test_encode(const VkDeviceCreateInfo *info, uint64_t physical_id,
                               uint64_t device_id, unsigned char *bytes) {
    _Static_assert(sizeof(uintptr_t) == sizeof(uint64_t), "x64 oracle required");
    _Static_assert(sizeof(float) == 4, "32-bit native priorities required");
    assert(info && bytes && physical_id && device_id && physical_id != device_id);
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 8192};
    VkDevice device = (VkDevice)(uintptr_t)device_id;
    vn_encode_vkCreateDevice(&encoder, 1, (VkPhysicalDevice)(uintptr_t)physical_id,
                            info, NULL, &device);
    return encoder.used;
}
#ifdef VgpuDeviceWireOracleSelfTest
/** @brief Validate the oracle's simplest canonical packet against literal fields.
 * @return Zero when native bounds/header/identity/priority/trailer assertions pass.
 * @note Allocation-free, sole main thread, all borrowed inputs are stack owned.
 */
int main(void) {
    unsigned char bytes[8192];
    const float priority = 0.5f;
    const VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 3, .queueCount = 1, .pQueuePriorities = &priority};
    const VkDeviceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
    for (unsigned cycle = 0; cycle < 128; ++cycle) {
        memset(bytes, 0xa5, sizeof(bytes));
        assert(venus_device_test_encode(&info, 7, 42, bytes) == 144);
        const unsigned char prefix[] = {11, 0, 0, 0, 1, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0};
        assert(memcmp(bytes, prefix, sizeof(prefix)) == 0);
        const unsigned char priority_word[] = {0, 0, 0, 0x3f};
        assert(memcmp(bytes + 84, priority_word, sizeof(priority_word)) == 0);
        const unsigned char device_word[] = {42, 0, 0, 0, 0, 0, 0, 0};
        assert(memcmp(bytes + 136, device_word, sizeof(device_word)) == 0);
        assert(bytes[144] == 0xa5);
    }
    return 0;
}
#endif
