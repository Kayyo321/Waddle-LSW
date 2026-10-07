/** @file modern_sync_oracle.c @brief Independent pinned modern command encoders. */
#include "vn_cs.h"
/** @brief Isolate imported compile-only upstream submit hook. */
#define vn_ring_submit_command venus_modern_unused_submit
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wpointer-arith"
#include "vn_protocol_driver_command_buffer.h"
#include "vn_protocol_driver_queue.h"
#include "vn_protocol_driver_semaphore.h"
#include "vn_protocol_driver_buffer.h"
#include "vn_protocol_driver_query_pool.h"
#pragma GCC diagnostic pop
/** @brief Serialize one supplied canonical modern native request with pinned code.
 * @param[in] opcode One of171..175,204,206. @param[in] info Borrowed canonical
 * native request or array; opcode-specific private fixture ownership.
 * @param[in] count Array extent for206, ignored otherwise.
 * @param[out] bytes Nonnull exclusive8192-byte output.
 * @return Exact initialized bytes. @note No allocation/retention, single test thread.
 */
size_t venus_modern_sync_test_encode(uint32_t opcode, const void *info, uint32_t count,
                                    unsigned char *bytes) {
    struct instance_encoder_t encoder = {.bytes = bytes, .capacity = 8192};
    VkDevice device = (VkDevice)(uintptr_t)7;
    VkCommandBuffer command = (VkCommandBuffer)(uintptr_t)9;
    switch (opcode) {
    case 171:
        vn_encode_vkResetQueryPool(&encoder, 1, device, (VkQueryPool)(uintptr_t)11, 3, 4);
        break;
    case 172: {
        uint64_t value;
        vn_encode_vkGetSemaphoreCounterValue(&encoder, 1, device, (VkSemaphore)(uintptr_t)11, &value);
        break;
    }
    case 173:
        vn_encode_vkWaitSemaphores(&encoder, 1, device, info, UINT64_MAX);
        break;
    case 174:
        vn_encode_vkSignalSemaphore(&encoder, 1, device, info);
        break;
    case 175:
        vn_encode_vkGetBufferDeviceAddress(&encoder, 1, device, info);
        break;
    case 204:
        vn_encode_vkCmdPipelineBarrier2(&encoder, 1, command, info);
        break;
    case 206:
        vn_encode_vkQueueSubmit2(&encoder, 1, (VkQueue)(uintptr_t)9, count, info,
                                (VkFence)(uintptr_t)13);
        break;
    default:
        assert(0);
    }
    return encoder.used;
}
