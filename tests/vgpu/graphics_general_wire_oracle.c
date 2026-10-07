/** @file graphics_general_wire_oracle.c @brief Pinned full graphics encoder oracle. */
#include "vn_cs.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#define vn_ring_submit_command venus_graphics_general_unused_ring_submit_command
#include "vn_protocol_driver_pipeline.h"
#undef vn_ring_submit_command
#pragma GCC diagnostic pop
/** Encode one borrowed canonical pipeline with device8/layout44/output46.
 * @param[in] info Nonnull native record with translated host shader/pass handles.
 * @param[out] output Nonnull exclusive8192-byte storage.
 * @return Encoded bytes; no allocation/retention, disjoint outputs thread-safe.
 */
size_t venus_graphics_general_test_create(const VkGraphicsPipelineCreateInfo *info,
    unsigned char *output) {
    struct instance_encoder_t encoder = {.bytes=output,.capacity=8192};
    VkPipeline pipeline = (VkPipeline)(uintptr_t)46;
    vn_encode_vkCreateGraphicsPipelines(&encoder,1,(VkDevice)(uintptr_t)8,VK_NULL_HANDLE,
        1,info,NULL,&pipeline);
    return encoder.used;
}
