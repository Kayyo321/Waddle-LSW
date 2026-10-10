/** @file requirements2_reply_oracle.c @brief Independent actual host reply encoder. */
#include "vkr_cs.h"
#include "vn_protocol_renderer_buffer.h"
#include "vn_protocol_renderer_image.h"
#include "vn_protocol_renderer_device.h"
/** @brief Encode real typed host fixture payloads, never a decoder round-trip.
 * @param[in] opcode144/145/149, optional node flag0/1.
 * @param[out] bytes Nonnull exclusive4096 bytes. @return Initialized extent.
 * @note No allocation or retention; distinct private outputs thread safe.
 */
size_t venus_requirements2_test_reply(uint32_t opcode,uint32_t optional,unsigned char *bytes) {
    struct fixture_encoder_t encoder={.bytes=bytes,.capacity=4096};
    vn_encode_uint32_t(&encoder,&opcode);
    uint64_t present=1;
    vn_encode_uint64_t(&encoder,&present);
    if(opcode==144 || opcode==145) {
        VkMemoryDedicatedRequirements dedicated={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,.prefersDedicatedAllocation=1,.requiresDedicatedAllocation=1};
        VkMemoryRequirements2 result={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=optional ? &dedicated : NULL,.memoryRequirements={4096,256,31}};
        vn_encode_VkMemoryRequirements2(&encoder,&result);
    } else {
        assert(opcode==149);
        VkFormatProperties3 extended={.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,.linearTilingFeatures=UINT64_C(1)<<40,.optimalTilingFeatures=UINT64_C(1)<<41,.bufferFeatures=UINT64_C(1)<<42};
        VkFormatProperties2 format={.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,.pNext=optional ? &extended : NULL,.formatProperties={1,2,4}};
        vn_encode_VkFormatProperties2(&encoder,&format);
    }
    return encoder.used;
}
