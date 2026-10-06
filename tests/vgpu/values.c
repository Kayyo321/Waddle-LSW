/** @file values.c @brief Independent pinned C encoder oracle and native reply
 * decoder fixture. */
#include "vn_protocol_renderer_device.h"
#include "waddle/venus_values.h"
#include <assert.h>
#include <string.h>
_Static_assert(VK_COMMAND_TYPE_vkGetPhysicalDeviceProperties_EXT == 6, "Pinned properties");
_Static_assert(VK_COMMAND_TYPE_vkGetPhysicalDeviceFeatures_EXT == 3, "Pinned features");
_Static_assert(VK_COMMAND_TYPE_vkGetPhysicalDeviceMemoryProperties_EXT == 8, "Pinned memory");
/** @brief Encode one deterministic pinned core query reply into borrowed
 * storage.
 * @param[in] kind Exactly3/4/5/6/7/8/33; other values return zero.
 * @param[out] bytes Nonnull exclusive private bytes[capacity].
 * @param[in] capacity Actual accessible extent, at least4096; otherwise zero
 * result.
 * @return Initialized reply prefix length; zero invalid local argument/kind.
 * @note Test-only, allocation-free, sole test thread. No guest/driver
 * implementation.
 */
size_t venus_values_test_encode(uint32_t kind, void *bytes, size_t capacity) {
    if (!bytes || capacity < 4096 ||
        (kind != 3 && kind != 4 && kind != 5 && kind != 6 && kind != 7 && kind != 8 && kind != 33))
        return 0;
    struct fixture_encoder_t encoder = {.bytes = bytes, .capacity = capacity};
    uint64_t pointer_tag = 1;
    vn_encode_uint32_t(&encoder, &kind);
    if (kind == 5) {
        int32_t result = VK_SUCCESS;
        vn_encode_int32_t(&encoder, &result);
    }
    vn_encode_uint64_t(&encoder, &pointer_tag);
    if (kind == 6) {
        venus_vk_properties_t value = {.apiVersion = VK_API_VERSION_1_3,
                                       .vendorID = 42,
                                       .deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU};
        memcpy(value.deviceName, "pinned-oracle", sizeof("pinned-oracle"));
        value.limits.minMemoryMapAlignment = 64;
        value.limits.timestampPeriod = 123.25f;
        value.sparseProperties.residencyNonResidentStrict = 1;
        vn_encode_VkPhysicalDeviceProperties(&encoder, &value);
    } else if (kind == 8) {
        venus_vk_memory_t value = {.memoryTypeCount = 1, .memoryHeapCount = 1};
        value.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        value.memoryHeaps[0].size = 1048576;
        value.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
        vn_encode_VkPhysicalDeviceMemoryProperties(&encoder, &value);
    } else if (kind == 4) {
        VkFormatProperties value = {.optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT};
        vn_encode_VkFormatProperties(&encoder, &value);
    } else if (kind == 5) {
        VkImageFormatProperties value = {.maxExtent = {4096, 4096, 1},
                                         .maxMipLevels = 12,
                                         .maxArrayLayers = 256,
                                         .sampleCounts = 1,
                                         .maxResourceSize = 1048576};
        vn_encode_VkImageFormatProperties(&encoder, &value);
    } else if (kind == 7 || kind == 33) {
        uint32_t count = 1;
        uint64_t array_count = 1;
        vn_encode_uint32_t(&encoder, &count);
        vn_encode_uint64_t(&encoder, &array_count);
        if (kind == 7) {
            VkQueueFamilyProperties value = {.queueFlags = VK_QUEUE_GRAPHICS_BIT,
                                             .queueCount = 1,
                                             .timestampValidBits = 64,
                                             .minImageTransferGranularity = {1, 1, 1}};
            vn_encode_VkQueueFamilyProperties(&encoder, &value);
        } else {
            VkSparseImageFormatProperties value = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                   .imageGranularity = {64, 64, 1}};
            vn_encode_VkSparseImageFormatProperties(&encoder, &value);
        }
    } else {
        venus_vk_features_t value = {.robustBufferAccess = 1, .tessellationShader = 1};
        vn_encode_VkPhysicalDeviceFeatures(&encoder, &value);
    }
    return encoder.used;
}
/** @brief Encode an exact caller-provided core physical properties reply independently.
 * @param[in] properties Nonnull borrowed native value; no input pointers retained.
 * @param[out] bytes Nonnull exclusive writable bytes[capacity], initialized prefix on success.
 * @param[in] capacity Actual accessible extent, at least4096; otherwise zero result.
 * @return Encoded reply byte extent, zero for invalid local arguments.
 * @note Test-only renderer oracle, allocation-free, thread-safe on disjoint output storage.
 */
size_t venus_values_test_properties(const VkPhysicalDeviceProperties *properties, void *bytes, size_t capacity) {
    if (!properties || !bytes || capacity < 4096) return 0;
    struct fixture_encoder_t encoder = {.bytes = bytes, .capacity = capacity};
    const uint32_t kind = 6; const uint64_t pointer_tag = 1;
    vn_encode_uint32_t(&encoder, &kind); vn_encode_uint64_t(&encoder, &pointer_tag);
    vn_encode_VkPhysicalDeviceProperties(&encoder, properties);
    return encoder.used;
}
#ifndef VgpuValuesOracle
int main(void) {
    unsigned char bytes[4096];
    venus_vk_properties_t properties = {0};
    venus_vk_features_t features = {0};
    venus_vk_memory_t memory = {0};
    size_t count = venus_values_test_encode(6, bytes, sizeof(bytes));
    assert(count && venus_values_properties_decode(&properties, bytes, count) == RingOk);
    assert(properties.vendorID == 42 && properties.limits.minMemoryMapAlignment == 64);
    assert(properties.limits.timestampPeriod == 123.25f);
    assert(!strcmp(properties.deviceName, "pinned-oracle"));
    count = venus_values_test_encode(8, bytes, sizeof(bytes));
    assert(count && venus_values_memory_decode(&memory, bytes, count) == RingOk);
    assert(memory.memoryTypeCount == 1 && memory.memoryHeapCount == 1);
    assert(memory.memoryHeaps[0].size == 1048576);
    count = venus_values_test_encode(3, bytes, sizeof(bytes));
    assert(count && venus_values_features_decode(&features, bytes, count) == RingOk);
    assert(features.robustBufferAccess == 1 && features.tessellationShader == 1);
    for (size_t prefix = 0; prefix < count; prefix++) {
        venus_vk_features_t before = features;
        assert(venus_values_features_decode(&features, bytes, prefix) == RingCorrupt);
        assert(!memcmp(&features, &before, sizeof(features)));
    }
    VkPhysicalDeviceProperties native = {.apiVersion = VK_API_VERSION_1_0};
    native.limits.minUniformBufferOffsetAlignment = 16;
    native.limits.minStorageBufferOffsetAlignment = 32;
    native.limits.maxUniformBufferRange = 256;
    native.limits.maxStorageBufferRange = 4096;
    native.limits.maxComputeWorkGroupCount[0] = 8;
    native.limits.maxComputeWorkGroupCount[1] = 9;
    native.limits.maxComputeWorkGroupCount[2] = 10;
    count = venus_values_test_properties(&native, bytes, sizeof(bytes));
    assert(count && venus_values_properties_decode(&properties, bytes, count) == RingOk);
    assert(properties.limits.minUniformBufferOffsetAlignment == 16 && properties.limits.minStorageBufferOffsetAlignment == 32);
    assert(properties.limits.maxUniformBufferRange == 256 && properties.limits.maxStorageBufferRange == 4096);
    assert(properties.limits.maxComputeWorkGroupCount[0] == 8 && properties.limits.maxComputeWorkGroupCount[2] == 10);
    assert(!venus_values_test_properties(NULL, bytes, sizeof(bytes)));
    assert(!venus_values_test_properties(&native, NULL, sizeof(bytes)));
    assert(!venus_values_test_properties(&native, bytes, sizeof(bytes) - 1));
    return 0;
}
#endif
