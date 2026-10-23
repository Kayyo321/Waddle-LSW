/** @file sampler_descriptor_wire_oracle.c @brief Generated sampler/image-write oracles. */
#include "vn_cs.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#define vn_ring_submit_command venus_sampler_descriptor_unused_ring_submit_command
#include "vn_protocol_driver_descriptor_set.h"
#include "vn_protocol_driver_descriptor_set_layout.h"
#include "vn_protocol_driver_descriptor_pool.h"
#undef vn_ring_submit_command
#pragma GCC diagnostic pop
/** Encode one borrowed descriptor write with already translated host handles.
 * @param[in] write Nonnull accessible immutable native record/payload arrays.
 * @param[out] output Nonnull exclusive8192-byte caller storage.
 * @return Bytes; no allocation/retention/shared state.
 */
size_t venus_sampler_descriptor_test_write(const VkWriteDescriptorSet *write,
    unsigned char *output) {
    struct instance_encoder_t encoder={.bytes=output,.capacity=8192};
    vn_encode_vkUpdateDescriptorSets(&encoder,1,(VkDevice)(uintptr_t)8,1,write,0,NULL);
    return encoder.used;
}
/** Encode translated native descriptor layout at device8/output42.
 * @param[in] info Nonnull borrowed record and accessible immutable sampler/flag arrays.
 * @param[out] output Nonnull exclusive8192-byte caller storage.
 * @return Bytes; no allocation/retention, disjoint outputs thread-safe.
 */
size_t venus_sampler_descriptor_test_layout(const VkDescriptorSetLayoutCreateInfo *info,
    unsigned char *output) {
    struct instance_encoder_t encoder={.bytes=output,.capacity=8192};
    VkDescriptorSetLayout layout=(VkDescriptorSetLayout)(uintptr_t)42;
    vn_encode_vkCreateDescriptorSetLayout(&encoder,1,(VkDevice)(uintptr_t)8,info,NULL,&layout);
    return encoder.used;
}
/** Encode pool at device8/output42; info and8192-byte output borrowed nonnull accessible.
 * Returns bytes, allocation-free; no ownership transfer and disjoint outputs thread-safe.
 */
size_t venus_sampler_descriptor_test_pool(const VkDescriptorPoolCreateInfo *info,
    unsigned char *output) {
    struct instance_encoder_t encoder={.bytes=output,.capacity=8192};
    VkDescriptorPool pool=(VkDescriptorPool)(uintptr_t)42;
    vn_encode_vkCreateDescriptorPool(&encoder,1,(VkDevice)(uintptr_t)8,info,NULL,&pool);
    return encoder.used;
}
/** Encode allocated sets43/44 at device8. info/reachable arrays borrowed nonnull accessible.
 * output nonnull exclusive8192 bytes; returns bytes, allocation-free, disjoint thread-safe.
 */
size_t venus_sampler_descriptor_test_allocate(const VkDescriptorSetAllocateInfo *info,
    unsigned char *output) {
    struct instance_encoder_t encoder={.bytes=output,.capacity=8192};
    VkDescriptorSet sets[2]={(VkDescriptorSet)(uintptr_t)43,(VkDescriptorSet)(uintptr_t)44};
    vn_encode_vkAllocateDescriptorSets(&encoder,1,(VkDevice)(uintptr_t)8,info,sets);
    return encoder.used;
}
