/** @file graphics_command_wire_oracle.c @brief Independent pinned graphics command encoder. */
#include "vn_cs.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#define vn_ring_submit_command venus_graphics_command_unused_ring_submit_command
#include "vn_protocol_driver_command_buffer.h"
#undef vn_ring_submit_command
#pragma GCC diagnostic pop
/** @brief Encode render-pass end with command buffer8.
 * @param[out] output Nonnull exclusive8192-byte caller storage.
 * @return Bytes written; allocation-free, thread-safe with disjoint storage.
 */
size_t venus_graphics_command_test_end(unsigned char *output) {
    struct instance_encoder_t encoder={.bytes=output,.capacity=8192};
    vn_encode_vkCmdEndRenderPass(&encoder,1,(VkCommandBuffer)(uintptr_t)8);
    return encoder.used;
}
/** @brief Encode direct draw with command buffer8.
 * @param[in] values Nonnull borrowed four scalars: vertex/instance count and first indices.
 * @param[out] output Nonnull exclusive8192-byte caller storage.
 * @return Bytes written; allocation-free, thread-safe with disjoint storage.
 */
size_t venus_graphics_command_test_draw(const uint32_t *values, unsigned char *output) {
    struct instance_encoder_t encoder={.bytes=output,.capacity=8192};
    vn_encode_vkCmdDraw(&encoder,1,(VkCommandBuffer)(uintptr_t)8,values[0],values[1],values[2],values[3]);
    return encoder.used;
}
/** @brief Encode image42 to buffer43 copies with command buffer8.
 * @param[in] layout Borrowed scalar core image layout.
 * @param[in] count Number of accessible region records,1..64.
 * @param[in] regions Nonnull borrowed accessible array of count records.
 * @param[out] output Nonnull exclusive8192-byte caller storage.
 * @return Bytes written; allocation-free, thread-safe with disjoint storage.
 */
size_t venus_graphics_command_test_copy(VkImageLayout layout, uint32_t count,
    const VkBufferImageCopy *regions, unsigned char *output) {
    struct instance_encoder_t encoder={.bytes=output,.capacity=8192};
    vn_encode_vkCmdCopyImageToBuffer(&encoder,1,(VkCommandBuffer)(uintptr_t)8,
        (VkImage)(uintptr_t)42,layout,(VkBuffer)(uintptr_t)43,count,regions);
    return encoder.used;
}
