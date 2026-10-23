/** @file extensions_reply_oracle.c @brief Pinned command14 renderer reply oracle. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "vn_protocol_renderer_device.h"
/** @brief Encode actual receiver count/fill results with the pinned generator.
 * @param[in] result Signed native VkResult copied exactly.
 * @param[in] count Returned count0..1024; may be nonzero with absent properties.
 * @param[in] properties Nullable borrowed immutable native array[count]; NULL=count mode.
 * @param[out] bytes Nonnull exclusive writable output[capacity], borrowed for call.
 * @param[in] capacity Extent at least28 or28+268*count with properties.
 * @return Initialized reply prefix; no allocation, retention or ownership transfer.
 * @note Sole synchronous writer; disjoint outputs thread safe, generator checks bounds.
 */
size_t venus_extensions_test_reply(int32_t result,uint32_t count,
    const VkExtensionProperties *properties,unsigned char *bytes,size_t capacity) {
    assert(count<=1024 && bytes && capacity>=28+(properties?(size_t)count*268:0));
    struct fixture_encoder_t encoder={.bytes=bytes,.capacity=capacity};
    const struct vn_command_vkEnumerateDeviceExtensionProperties args={
        .ret=(VkResult)result,.pPropertyCount=&count,
        .pProperties=(VkExtensionProperties *)properties};
    vn_encode_vkEnumerateDeviceExtensionProperties_reply(&encoder,&args);
    return encoder.used;
}
