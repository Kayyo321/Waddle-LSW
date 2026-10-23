/** @file graphics_dynamic_wire_oracle.c @brief Independent dynamic command encoders. */
#include "vn_cs.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#define vn_ring_submit_command venus_graphics_dynamic_unused_ring_submit_command
#include "vn_protocol_driver_command_buffer.h"
#undef vn_ring_submit_command
#pragma GCC diagnostic pop
/** Encode fixed documented dynamic fixtures using generated protocol routines.
 * @param[in] opcode Pinned dynamic graphics selector; invalid returns0.
 * @param[in] records Nullable borrowed viewport/scissor array, count accessible records.
 * @param[in] count0..16 used only by array selectors.
 * @param[out] output Nonnull exclusive8192-byte caller storage.
 * @return Complete byte count, no allocation/retention; disjoint calls thread-safe.
 */
size_t venus_graphics_dynamic_test_encode(uint32_t opcode, const void *records,
    uint32_t count, unsigned char *output) {
    struct instance_encoder_t encoder={.bytes=output,.capacity=8192};
    VkCommandBuffer command=(VkCommandBuffer)(uintptr_t)8;
    VkBuffer buffers[2]={(VkBuffer)(uintptr_t)42,(VkBuffer)(uintptr_t)43};
    VkDeviceSize offsets[2]={16,32},sizes[2]={64,128},strides[2]={4,16};
    float constants[4]={0.5f,0.25f,0.125f,1.0f};
    switch(opcode) {
    case 94: vn_encode_vkCmdSetViewport(&encoder,1,command,2,count,records); break;
    case 95: vn_encode_vkCmdSetScissor(&encoder,1,command,2,count,records); break;
    case 97: vn_encode_vkCmdSetDepthBias(&encoder,1,command,0.5f,0.25f,0.125f); break;
    case 98: vn_encode_vkCmdSetBlendConstants(&encoder,1,command,constants); break;
    case 99: vn_encode_vkCmdSetDepthBounds(&encoder,1,command,0.25f,0.75f); break;
    case 100: vn_encode_vkCmdSetStencilCompareMask(&encoder,1,command,3,0x87654321); break;
    case 101: vn_encode_vkCmdSetStencilWriteMask(&encoder,1,command,3,0x87654321); break;
    case 102: vn_encode_vkCmdSetStencilReference(&encoder,1,command,3,0x87654321); break;
    case 104: vn_encode_vkCmdBindIndexBuffer(&encoder,1,command,buffers[0],16,VK_INDEX_TYPE_UINT32); break;
    case 105: vn_encode_vkCmdBindVertexBuffers(&encoder,1,command,2,2,buffers,offsets); break;
    case 107: vn_encode_vkCmdDrawIndexed(&encoder,1,command,6,2,3,-17,19); break;
    case 108: vn_encode_vkCmdDrawIndirect(&encoder,1,command,buffers[0],16,2,32); break;
    case 109: vn_encode_vkCmdDrawIndexedIndirect(&encoder,1,command,buffers[0],16,2,32); break;
    case 215: vn_encode_vkCmdSetCullMode(&encoder,1,command,1); break;
    case 216: vn_encode_vkCmdSetFrontFace(&encoder,1,command,1); break;
    case 217: vn_encode_vkCmdSetPrimitiveTopology(&encoder,1,command,1); break;
    case 218: vn_encode_vkCmdSetViewportWithCount(&encoder,1,command,count,records); break;
    case 219: vn_encode_vkCmdSetScissorWithCount(&encoder,1,command,count,records); break;
    case 220: vn_encode_vkCmdBindVertexBuffers2(&encoder,1,command,2,2,buffers,offsets,sizes,strides); break;
    case 221: vn_encode_vkCmdSetDepthTestEnable(&encoder,1,command,1); break;
    case 222: vn_encode_vkCmdSetDepthWriteEnable(&encoder,1,command,1); break;
    case 223: vn_encode_vkCmdSetDepthCompareOp(&encoder,1,command,1); break;
    case 224: vn_encode_vkCmdSetDepthBoundsTestEnable(&encoder,1,command,1); break;
    case 225: vn_encode_vkCmdSetStencilTestEnable(&encoder,1,command,1); break;
    case 226: vn_encode_vkCmdSetStencilOp(&encoder,1,command,3,1,2,3,4); break;
    case 227: vn_encode_vkCmdSetRasterizerDiscardEnable(&encoder,1,command,1); break;
    case 228: vn_encode_vkCmdSetDepthBiasEnable(&encoder,1,command,1); break;
    case 229: vn_encode_vkCmdSetPrimitiveRestartEnable(&encoder,1,command,1); break;
    case 279: vn_encode_vkCmdBindIndexBuffer2(&encoder,1,command,buffers[0],16,128,VK_INDEX_TYPE_UINT32); break;
    default:return 0;
    }
    return encoder.used;
}
