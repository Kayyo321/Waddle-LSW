/** @file compute_wire_oracle.c @brief Independent pinned core command encoders, test-only. */
#include "vn_cs.h"
/** @brief Isolate upstream test ring stub when linking multiple independent oracles. */
#define vn_ring_submit_command venus_compute_oracle_unused_submit
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#include "vn_protocol_driver_command_buffer.h"
#pragma GCC diagnostic pop
/** @brief Encode pipeline binding with translated command7/pipeline42. Borrowed exclusive8192 bytes.
 * @param[in] point Core bind point0/1. @param[out] bytes Nonnull scratch, no retention.
 * @return Initialized bytes; allocation-free, sole calling test thread. */
size_t venus_compute_test_bind_pipeline(uint32_t point, unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes=bytes,.capacity=8192};
    vn_encode_vkCmdBindPipeline(&encoder,1,(VkCommandBuffer)(uintptr_t)7,point,(VkPipeline)(uintptr_t)42);
    return encoder.used;
}
/** @brief Encode descriptor binding with borrowed translated IDs and offsets.
 * @param[in] point/first Core bind point and first set. @param[in] count/ids Nonnull count<=16 array.
 * @param[in] dynamic_count/offsets Nonnull count<=1024 array. @param[out] bytes Exclusive8192 bytes.
 * @return Initialized bytes; no allocation/retention; sole calling test thread. */
size_t venus_compute_test_bind_sets(uint32_t point,uint32_t first,size_t count,const uint64_t *ids,size_t dynamic_count,const uint32_t *offsets,unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes=bytes,.capacity=8192};
    VkDescriptorSet sets[16];
    for(size_t index=0;index<count;++index) sets[index]=(VkDescriptorSet)(uintptr_t)ids[index];
    vn_encode_vkCmdBindDescriptorSets(&encoder,1,(VkCommandBuffer)(uintptr_t)7,point,(VkPipelineLayout)(uintptr_t)43,first,(uint32_t)count,sets,(uint32_t)dynamic_count,offsets);
    return encoder.used;
}
/** @brief Encode push constants with borrowed canonical values; no retention.
 * @param[in] stages/offset Core stage mask and byte offset. @param[in] size/values Nonnull size<=256.
 * @param[out] bytes Exclusive8192 scratch. @return Initialized bytes, no allocation, sole test thread. */
size_t venus_compute_test_push(uint32_t stages,uint32_t offset,size_t size,const unsigned char *values,unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes=bytes,.capacity=8192};
    vn_encode_vkCmdPushConstants(&encoder,1,(VkCommandBuffer)(uintptr_t)7,(VkPipelineLayout)(uintptr_t)43,stages,offset,(uint32_t)size,values);
    return encoder.used;
}
/** @brief Encode dispatch using borrowed three-value array.
 * @param[in] groups Nonnull three accessible values. @param[out] bytes Exclusive8192 scratch.
 * @return Initialized bytes; allocation-free, no retention, sole calling test thread. */
size_t venus_compute_test_dispatch(const uint32_t *groups,unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes=bytes,.capacity=8192};
    vn_encode_vkCmdDispatch(&encoder,1,(VkCommandBuffer)(uintptr_t)7,groups[0],groups[1],groups[2]);
    return encoder.used;
}
