/** @file dynamic_rendering_wire_oracle.c @brief Generated rendering encoder oracle. */
#include "vn_cs.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#define vn_ring_submit_command venus_dynamic_rendering_unused_ring_submit_command
#include "vn_protocol_driver_command_buffer.h"
#undef vn_ring_submit_command
#pragma GCC diagnostic pop
/** Encode borrowed native rendering fixture at command8.
 * @param[in] info Nonnull accessible record with already translated attachment handles.
 * @param[out] output Nonnull exclusive8192-byte caller storage.
 * @return Encoded bytes, no allocation; disjoint outputs are thread-safe.
 */
size_t venus_dynamic_rendering_test_begin(const VkRenderingInfo *info, unsigned char *output) {
    struct instance_encoder_t encoder = {.bytes=output,.capacity=8192};
    vn_encode_vkCmdBeginRendering(&encoder,1,(VkCommandBuffer)(uintptr_t)8,info);
    return encoder.used;
}
/** Encode render end at command8 into nonnull exclusive8192-byte output; return bytes.
 * No allocation, ownership transfer or shared state; disjoint outputs are thread-safe.
 */
size_t venus_dynamic_rendering_test_end(unsigned char *output) {
    struct instance_encoder_t encoder = {.bytes=output,.capacity=8192};
    vn_encode_vkCmdEndRendering(&encoder,1,(VkCommandBuffer)(uintptr_t)8);
    return encoder.used;
}
