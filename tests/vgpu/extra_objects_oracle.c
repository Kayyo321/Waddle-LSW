/** @file extra_objects_oracle.c @brief Pinned independent core object packet encoder. */
#include "vn_cs.h"
/** @brief Isolate imported compile-only vendor submit hook. */
#define vn_ring_submit_command venus_extra_unused_submit
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#include "vn_protocol_driver_device_memory.h"
#include "vn_protocol_driver_descriptor_set_layout.h"
#include "vn_protocol_driver_image.h"
#include "vn_protocol_driver_render_pass.h"
#include "vn_protocol_driver_sampler.h"
#include "vn_protocol_driver_buffer_view.h"
#include "vn_protocol_driver_query_pool.h"
#include "vn_protocol_driver_pipeline_cache.h"
#include "vn_protocol_driver_event.h"
#include "vn_protocol_driver_command_buffer.h"
#pragma GCC diagnostic pop
/** @brief Encode supplied native create or fixed fixture operation using pinned code.
 * @param[in] opcode Supported private selector. @param[in] info Nullable canonical
 * native creation input, borrowed for call. @param[out] bytes Exclusive8192 bytes.
 * @return Initialized extent. @note No allocation/retention, disjoint test owners safe.
 */
size_t venus_extra_objects_test_encode(uint32_t opcode, const void *info, unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes=bytes,.capacity=8192};
    VkDevice device=(VkDevice)(uintptr_t)7;
    VkCommandBuffer command=(VkCommandBuffer)(uintptr_t)9;
    VkQueryPool pool=(VkQueryPool)(uintptr_t)11;
    VkEvent event=(VkEvent)(uintptr_t)11;
    VkPipelineCache cache=(VkPipelineCache)(uintptr_t)11;
    switch(opcode) {
    case 164: {VkDescriptorSetLayoutSupport support={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};vn_encode_vkGetDescriptorSetLayoutSupport(&encoder,1,device,info,&support);break;}
    case 21: {VkDeviceMemory memory=(VkDeviceMemory)(uintptr_t)11;vn_encode_vkAllocateMemory(&encoder,1,device,info,NULL,&memory);break;}
    case 84: {VkExtent2D extent;vn_encode_vkGetRenderAreaGranularity(&encoder,1,device,(VkRenderPass)(uintptr_t)11,&extent);break;}
    case 280: {VkExtent2D extent;vn_encode_vkGetRenderingAreaGranularity(&encoder,1,device,info,&extent);break;}
    case 56: {VkSubresourceLayout layout;vn_encode_vkGetImageSubresourceLayout(&encoder,1,device,(VkImage)(uintptr_t)11,info,&layout);break;}
    case 42: vn_encode_vkCreateEvent(&encoder,1,device,info,NULL,&event);break;
    case 47: vn_encode_vkCreateQueryPool(&encoder,1,device,info,NULL,&pool);break;
    case 52: {VkBufferView view=(VkBufferView)(uintptr_t)11;vn_encode_vkCreateBufferView(&encoder,1,device,info,NULL,&view);break;}
    case 61: vn_encode_vkCreatePipelineCache(&encoder,1,device,info,NULL,&cache);break;
    case 70: {VkSampler sampler=(VkSampler)(uintptr_t)11;vn_encode_vkCreateSampler(&encoder,1,device,info,NULL,&sampler);break;}
    case 43: vn_encode_vkDestroyEvent(&encoder,1,device,event,NULL);break;
    case 48: vn_encode_vkDestroyQueryPool(&encoder,1,device,pool,NULL);break;
    case 53: vn_encode_vkDestroyBufferView(&encoder,1,device,(VkBufferView)(uintptr_t)11,NULL);break;
    case 62: vn_encode_vkDestroyPipelineCache(&encoder,1,device,cache,NULL);break;
    case 71: vn_encode_vkDestroySampler(&encoder,1,device,(VkSampler)(uintptr_t)11,NULL);break;
    case 44: vn_encode_vkGetEventStatus(&encoder,1,device,event);break;
    case 45: vn_encode_vkSetEvent(&encoder,1,device,event);break;
    case 46: vn_encode_vkResetEvent(&encoder,1,device,event);break;
    case 49: {unsigned char output[32];vn_encode_vkGetQueryPoolResults(&encoder,1,device,pool,3,4,sizeof output,output,8,VK_QUERY_RESULT_64_BIT);break;}
    case 63: {size_t size=32;unsigned char data[32];vn_encode_vkGetPipelineCacheData(&encoder,1,device,cache,&size,data);break;}
    case 64: {VkPipelineCache sources[2]={(VkPipelineCache)(uintptr_t)13,(VkPipelineCache)(uintptr_t)15};vn_encode_vkMergePipelineCaches(&encoder,1,device,cache,2,sources);break;}
    case 127:vn_encode_vkCmdBeginQuery(&encoder,1,command,pool,3,VK_QUERY_CONTROL_PRECISE_BIT);break;
    case 128:vn_encode_vkCmdEndQuery(&encoder,1,command,pool,3);break;
    case 129:vn_encode_vkCmdResetQueryPool(&encoder,1,command,pool,3,4);break;
    case 130:vn_encode_vkCmdWriteTimestamp(&encoder,1,command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool,3);break;
    case 205:vn_encode_vkCmdWriteTimestamp2(&encoder,1,command,UINT64_C(1)<<40,pool,3);break;
    default:assert(0);
    }
    return encoder.used;
}
