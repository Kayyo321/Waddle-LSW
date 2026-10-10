/** @file graphics_pipeline_wire_oracle.c @brief Pinned generated pipeline encoder oracle. */
#include "vn_cs.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#define vn_ring_submit_command venus_graphics_pipeline_unused_ring_submit_command
#include "vn_protocol_driver_pipeline.h"
#undef vn_ring_submit_command
#pragma GCC diagnostic pop
/** @brief Encode one graphics pipeline with independent translated IDs.
 * @param[in] info Nonnull accessible records and arrays, borrowed for this call.
 * @param[out] output Nonnull exclusive8192-byte caller-owned output storage.
 * @return Encoded byte count. Allocation-free; thread-safe on disjoint storage.
 */
size_t venus_graphics_pipeline_test_create(const VkGraphicsPipelineCreateInfo *info, unsigned char *output) {
    struct instance_encoder_t encoder={.bytes=output,.capacity=8192};
    VkGraphicsPipelineCreateInfo translated=*info;
    VkPipelineShaderStageCreateInfo stages[2]={info->pStages[0],info->pStages[1]};
    stages[0].module=(VkShaderModule)(uintptr_t)42;
    stages[1].module=(VkShaderModule)(uintptr_t)43;
    translated.pStages=stages;
    translated.layout=(VkPipelineLayout)(uintptr_t)44;
    translated.renderPass=(VkRenderPass)(uintptr_t)45;
    VkPipeline pipeline=(VkPipeline)(uintptr_t)46;
    vn_encode_vkCreateGraphicsPipelines(&encoder,1,(VkDevice)(uintptr_t)7,NULL,1,&translated,NULL,&pipeline);
    return encoder.used;
}
