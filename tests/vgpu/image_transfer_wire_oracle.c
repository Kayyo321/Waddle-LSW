/** @file image_transfer_wire_oracle.c @brief Pinned generated transfer-byte oracle. */
#include "vn_cs.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#define vn_ring_submit_command venus_image_transfer_unused_ring_submit_command
#include "vn_protocol_driver_command_buffer.h"
#undef vn_ring_submit_command
#pragma GCC diagnostic pop
/** Encode one selected generated command with command8/source42/target43.
 * @param[in] kind Selector0 copy,1 blit,2 upload,3 color,4 depth,5 resolve.
 * @param[in] count Borrowed region array count1..64.
 * @param[in] regions Nonnull borrowed SDK array appropriate to selected command.
 * @param[in] clear Nonnull borrowed clear union/value for selected clear command.
 * @param[out] output Nonnull exclusive8192-byte storage.
 * @return Complete encoded bytes; no allocation, thread-safe for distinct storage.
 */
size_t venus_image_transfer_test_encode(uint32_t kind, uint32_t count,
    const void *regions, const void *clear, unsigned char *output) {
    struct instance_encoder_t encoder = {.bytes = output, .capacity = 8192};
    VkCommandBuffer command = (VkCommandBuffer)(uintptr_t)8;
    VkImage source = (VkImage)(uintptr_t)42, target = (VkImage)(uintptr_t)43;
    switch (kind) {
    case 0: vn_encode_vkCmdCopyImage(&encoder,1,command,source,VK_IMAGE_LAYOUT_GENERAL,
        target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,count,regions); break;
    case 1: vn_encode_vkCmdBlitImage(&encoder,1,command,source,VK_IMAGE_LAYOUT_GENERAL,
        target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,count,regions,VK_FILTER_LINEAR); break;
    case 2: vn_encode_vkCmdCopyBufferToImage(&encoder,1,command,(VkBuffer)(uintptr_t)42,
        target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,count,regions); break;
    case 3: vn_encode_vkCmdClearColorImage(&encoder,1,command,source,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,clear,count,regions); break;
    case 4: vn_encode_vkCmdClearDepthStencilImage(&encoder,1,command,source,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,clear,count,regions); break;
    case 5: vn_encode_vkCmdResolveImage(&encoder,1,command,source,VK_IMAGE_LAYOUT_GENERAL,
        target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,count,regions); break;
    default: return 0;
    }
    return encoder.used;
}
