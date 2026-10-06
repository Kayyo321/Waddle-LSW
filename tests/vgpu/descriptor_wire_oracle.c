/** @file descriptor_wire_oracle.c @brief Independent pinned descriptor serializers; test-only. */
#include "vn_cs.h"
/** @brief Isolate the generated non-static ring test stub across linked oracles. */
#define vn_ring_submit_command venus_descriptor_oracle_unused_submit
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#include "vn_protocol_driver_descriptor_pool.h"
#include "vn_protocol_driver_descriptor_set.h"
#pragma GCC diagnostic pop
_Static_assert(VK_COMMAND_TYPE_vkCreateDescriptorPool_EXT == 74, "Pinned pool command");
_Static_assert(VK_COMMAND_TYPE_vkAllocateDescriptorSets_EXT == 77, "Pinned set allocation command");
_Static_assert(VK_COMMAND_TYPE_vkUpdateDescriptorSets_EXT == 79, "Pinned set update command");
/** @brief Encode a canonical pool with the independent pinned generator.
 * @param[in] info Nonnull borrowed native record/arrays, valid for synchronous call.
 * @param[out] bytes Nonnull exclusive8192-byte scratch, no retained reference.
 * @return Initialized packet bytes; allocation-free/thread-safe on disjoint inputs.
 */
size_t venus_descriptor_test_pool(const VkDescriptorPoolCreateInfo *info, unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 8192};
    VkDescriptorPool pool = (VkDescriptorPool)(uintptr_t)42;
    vn_encode_vkCreateDescriptorPool(&encoder, 1, (VkDevice)(uintptr_t)7, info, NULL, &pool);
    return encoder.used;
}
/** @brief Encode a canonical batch with translated pool42 and caller-specified identities.
 * @param[in] info Nonnull canonical borrowed native record, set count1..64.
 * @param[in] layouts/sets Nonnull borrowed arrays of exactly descriptorSetCount translated IDs.
 * @param[out] bytes Nonnull exclusive8192-byte scratch; no storage retained.
 * @return Initialized packet bytes; no allocation; thread-safe on disjoint inputs.
 */
size_t venus_descriptor_test_allocate(const VkDescriptorSetAllocateInfo *info,
                                      const uint64_t *layouts, const uint64_t *sets,
                                      unsigned char *bytes) {
    assert(info->descriptorSetCount > 0 && info->descriptorSetCount <= 64);
    VkDescriptorSetLayout native_layouts[64];
    VkDescriptorSet native_sets[64];
    for (uint32_t index = 0; index < info->descriptorSetCount; index++) {
        native_layouts[index] = (VkDescriptorSetLayout)(uintptr_t)layouts[index];
        native_sets[index] = (VkDescriptorSet)(uintptr_t)sets[index];
    }
    VkDescriptorSetAllocateInfo translated = *info;
    translated.descriptorPool = (VkDescriptorPool)(uintptr_t)42;
    translated.pSetLayouts = native_layouts;
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 8192};
    vn_encode_vkAllocateDescriptorSets(&encoder, 1, (VkDevice)(uintptr_t)7, &translated, native_sets);
    return encoder.used;
}
/** @brief Encode canonical native writes/copies with the independent pinned generator.
 * @param[in] write_count/copy_count Counts0..64, all nested borrowed arrays valid for call.
 * @param[in] writes/copies Nullable only for zero corresponding count, translated native identities.
 * @param[out] bytes Nonnull exclusive8192-byte scratch, sufficient for verified input packet.
 * @return Initialized packet bytes; allocation-free/thread-safe on disjoint inputs.
 */
size_t venus_descriptor_test_update(uint32_t write_count, const VkWriteDescriptorSet *writes,
                                    uint32_t copy_count, const VkCopyDescriptorSet *copies,
                                    unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 8192};
    vn_encode_vkUpdateDescriptorSets(&encoder, 1, (VkDevice)(uintptr_t)7,
                                    write_count, writes, copy_count, copies);
    return encoder.used;
}
