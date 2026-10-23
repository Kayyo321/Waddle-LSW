/** @file graphics_wire_oracle.c @brief Independent pinned graphics wire encoders; test-only. */
#include "vn_cs.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#define vn_ring_submit_command venus_graphics_unused_ring_submit_command
#include "vn_protocol_driver_render_pass.h"
#include "vn_protocol_driver_framebuffer.h"
#include "vn_protocol_driver_command_buffer.h"
#undef vn_ring_submit_command
#pragma GCC diagnostic pop
/** @brief Encode single-color render pass with independent translated IDs.
 * @param[in] info Nonnull accessible native info/arrays, borrowed for call.
 * @param[out] output Nonnull exclusive8192-byte scratch.
 * @return Encoded bytes; allocation-free, thread-safe on disjoint storage.
 */
size_t venus_graphics_test_pass(const VkRenderPassCreateInfo *info, unsigned char *output) {
    struct instance_encoder_t encoder = {.bytes=output,.capacity=8192};
    VkRenderPass pass=(VkRenderPass)(uintptr_t)42;
    vn_encode_vkCreateRenderPass(&encoder,1,(VkDevice)(uintptr_t)7,info,NULL,&pass);
    return encoder.used;
}
/** @brief Encode one-view framebuffer using translated pass42/view43/output44.
 * @param[in] info Nonnull borrowed accessible native record.
 * @param[out] output Nonnull exclusive8192-byte scratch.
 * @return Encoded bytes; allocation-free, thread-safe on disjoint storage.
 */
size_t venus_graphics_test_framebuffer(const VkFramebufferCreateInfo *info, unsigned char *output) {
    struct instance_encoder_t encoder = {.bytes=output,.capacity=8192};
    VkFramebufferCreateInfo translated=*info;
    VkImageView view=(VkImageView)(uintptr_t)43;
    translated.renderPass=(VkRenderPass)(uintptr_t)42;
    translated.pAttachments=&view;
    VkFramebuffer framebuffer=(VkFramebuffer)(uintptr_t)44;
    vn_encode_vkCreateFramebuffer(&encoder,1,(VkDevice)(uintptr_t)7,&translated,NULL,&framebuffer);
    return encoder.used;
}
/** @brief Encode inline render-pass begin using translated pass42/framebuffer44.
 * @param[in] info Nonnull borrowed accessible native record/clear value.
 * @param[out] output Nonnull exclusive8192-byte scratch.
 * @return Encoded bytes; allocation-free, thread-safe on disjoint storage.
 */
size_t venus_graphics_test_begin(const VkRenderPassBeginInfo *info, unsigned char *output) {
    struct instance_encoder_t encoder = {.bytes=output,.capacity=8192};
    VkRenderPassBeginInfo translated=*info;
    translated.renderPass=(VkRenderPass)(uintptr_t)42;
    translated.framebuffer=(VkFramebuffer)(uintptr_t)44;
    vn_encode_vkCmdBeginRenderPass(&encoder,1,(VkCommandBuffer)(uintptr_t)8,&translated,VK_SUBPASS_CONTENTS_INLINE);
    return encoder.used;
}
