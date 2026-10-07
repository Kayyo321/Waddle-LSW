/** @file requirements2_query_oracle.c @brief Pinned requirement/format query encoder. */
#include "vn_cs.h"
/** @brief Isolate imported vendor compile-only submit hook. */
#define vn_ring_submit_command venus_requirements2_unused_submit
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "vn_protocol_driver_buffer.h"
#include "vn_protocol_driver_image.h"
#include "vn_protocol_driver_device.h"
#pragma GCC diagnostic pop
/** @brief Encode actual typed query topology with immutable pinned code.
 * @param[in] opcode144/145/149, optional node flag0/1.
 * @param[out] bytes Nonnull exclusive4096 bytes. @return Initialized extent.
 * @note Allocation-free, no retained inputs; independent test owners safe.
 */
size_t venus_requirements2_test_query(uint32_t opcode, uint32_t optional, unsigned char *bytes) {
    struct instance_encoder_t encoder={.bytes=bytes,.capacity=4096};
    VkMemoryDedicatedRequirements dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 result={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=optional ? &dedicated : NULL};
    VkDevice device=(VkDevice)(uintptr_t)7;
    if(opcode==145) {
        VkBufferMemoryRequirementsInfo2 info={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,.buffer=(VkBuffer)(uintptr_t)11};
        vn_encode_vkGetBufferMemoryRequirements2(&encoder,1,device,&info,&result);
    } else if(opcode==144) {
        VkImageMemoryRequirementsInfo2 info={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=(VkImage)(uintptr_t)11};
        vn_encode_vkGetImageMemoryRequirements2(&encoder,1,device,&info,&result);
    } else {
        assert(opcode==149);
        VkFormatProperties3 extended={.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 format={.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,.pNext=optional ? &extended : NULL};
        vn_encode_vkGetPhysicalDeviceFormatProperties2(&encoder,1,(VkPhysicalDevice)(uintptr_t)7,VK_FORMAT_R8G8B8A8_UNORM,&format);
    }
    return encoder.used;
}
