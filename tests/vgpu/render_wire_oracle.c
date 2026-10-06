/** @file render_wire_oracle.c @brief Independent pinned core image serializers; test-only. */
#include "vn_cs.h"
/** @brief Isolate the upstream non-static test ring stub from other linked oracles. */
#define vn_ring_submit_command venus_render_oracle_unused_submit
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "vn_protocol_driver_image.h"
#include "vn_protocol_driver_shader_module.h"
#include "vn_protocol_driver_descriptor_set_layout.h"
#include "vn_protocol_driver_pipeline_layout.h"
#include "vn_protocol_driver_image_view.h"
#include "vn_protocol_driver_command_buffer.h"
#pragma GCC diagnostic pop
/** @brief Encode one valid image command using the pinned upstream generator.
 * @param[in] info Nonnull canonical borrowed record and arrays, valid for call.
 * @param[out] bytes Nonnull exclusive scratch, 8192 accessible bytes.
 * @return Initialized byte length; no allocation; sole calling test thread.
 */
size_t venus_render_test_image(const VkImageCreateInfo *info, unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 8192};
    VkImage image = (VkImage)(uintptr_t)42;
    vn_encode_vkCreateImage(&encoder, 1, (VkDevice)(uintptr_t)7, info, NULL, &image);
    return encoder.used;
}
/** @brief Encode one valid view command with translated image ID42 and view ID43.
 * @param[in] info Nonnull borrowed canonical record; no retained pointers.
 * @param[out] bytes Nonnull exclusive 8192-byte scratch.
 * @return Initialized bytes; allocation-free, sole calling test thread.
 */
size_t venus_render_test_view(const VkImageViewCreateInfo *info, unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 8192};
    VkImageView view = (VkImageView)(uintptr_t)43;
    VkImageViewCreateInfo native_info = *info;
    native_info.image = (VkImage)(uintptr_t)42;
    vn_encode_vkCreateImageView(&encoder, 1, (VkDevice)(uintptr_t)7, &native_info, NULL, &view);
    return encoder.used;
}
/** @brief Encode valid core image barrier struct with translated ID42.
 * @param[in] info Nonnull borrowed canonical record; no retained pointers.
 * @param[out] bytes Nonnull exclusive 8192-byte scratch.
 * @return Initialized bytes; allocation-free, sole calling test thread.
 */
size_t venus_render_test_barrier(const VkImageMemoryBarrier *info, unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 8192};
    VkImageMemoryBarrier native_info = *info;
    native_info.image = (VkImage)(uintptr_t)42;
    vn_encode_VkImageMemoryBarrier(&encoder, &native_info);
    return encoder.used;
}

/** @brief Encode valid shader packet using the independent pinned generator.
 * @param[in] info Nonnull borrowed canonical record/code, valid for call.
 * @param[out] bytes Nonnull exclusive 8192-byte scratch.
 * @return Initialized bytes; allocation-free, sole calling test thread.
 */
size_t venus_render_test_shader(const VkShaderModuleCreateInfo *info, unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 8192};
    VkShaderModule module = (VkShaderModule)(uintptr_t)42;
    vn_encode_vkCreateShaderModule(&encoder, 1, (VkDevice)(uintptr_t)7, info, NULL, &module);
    return encoder.used;
}

/** @brief Encode valid descriptor_layout with pinned generator.
 * @param[in] info Nonnull borrowed canonical record and arrays for call.
 * @param[out] bytes Nonnull exclusive8192-byte scratch.
 * @return Initialized bytes; allocation-free, sole calling test thread.
 */
size_t venus_render_test_descriptor_layout(const VkDescriptorSetLayoutCreateInfo *info, unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 8192};
    VkDescriptorSetLayout handle = (VkDescriptorSetLayout)(uintptr_t)42;
    vn_encode_vkCreateDescriptorSetLayout(&encoder, 1, (VkDevice)(uintptr_t)7, info, NULL, &handle);
    return encoder.used;
}

/** @brief Encode valid pipeline_layout with pinned generator.
 * @param[in] info Nonnull borrowed canonical record and arrays for call.
 * @param[out] bytes Nonnull exclusive8192-byte scratch.
 * @return Initialized bytes; allocation-free, sole calling test thread.
 */
size_t venus_render_test_pipeline_layout(const VkPipelineLayoutCreateInfo *info, unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 8192};
    VkPipelineLayout handle = (VkPipelineLayout)(uintptr_t)43;
    vn_encode_vkCreatePipelineLayout(&encoder, 1, (VkDevice)(uintptr_t)7, info, NULL, &handle);
    return encoder.used;
}
