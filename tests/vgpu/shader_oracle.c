/** @file shader_oracle.c @brief Independent pinned full-size shader encoder. */
#include "vn_cs.h"
/** @brief Isolate imported compile-only upstream submit hook. */
#define vn_ring_submit_command venus_shader_unused_submit
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "vn_protocol_driver_shader_module.h"
#pragma GCC diagnostic pop
/** @brief Encode canonical native shader against pinned generated code.
 * @param[in] info Nonnull borrowed accessible native shader, no ownership transfer.
 * @param[out] bytes Nonnull exclusive capacity-byte output.
 * @param[in] capacity Actual bounded output extent. @return Exact initialized size.
 * @note No allocation/retention/shared mutation; independent outputs thread safe.
 */
size_t venus_shader_test_encode(const VkShaderModuleCreateInfo *info, unsigned char *bytes,
                               size_t capacity) {
    struct instance_encoder_t encoder={.bytes=bytes,.capacity=capacity};
    VkShaderModule module=(VkShaderModule)(uintptr_t)11;
    vn_encode_vkCreateShaderModule(&encoder,1,(VkDevice)(uintptr_t)7,info,NULL,&module);
    return encoder.used;
}
